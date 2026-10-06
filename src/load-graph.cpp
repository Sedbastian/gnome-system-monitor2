#include <config.h>

#include <math.h>

#include <glib/gi18n.h>

#include <glibtop.h>
#include <glibtop/cpu.h>
#include <glibtop/disk.h>
#include <glibtop/mem.h>
#include <glibtop/swap.h>
#include <glibtop/netload.h>
#include <glibtop/netlist.h>

#include "application.h"
#include "load-graph.h"
#include "util.h"
#include "legacy/gsm_color_button.h"

constexpr double BORDER_ALPHA = 0.7;
constexpr double GRID_ALPHA = BORDER_ALPHA / 2.0;
/* Breathing room around the grid, top and bottom of every graph */
constexpr int FRAME_WIDTH = 1;
constexpr unsigned GRAPH_MIN_HEIGHT = 40;

void
LoadGraph::clear_background ()
{
  gsm_graph_clear_background (GSM_GRAPH (disp));
}

bool
LoadGraph::is_logarithmic_scale () const
{
  // logarithmic scale is used only for memory graph
  return this->type == LOAD_GRAPH_MEM && GsmApplication::get ()->config.logarithmic_scale;
}

/*
 Returns Y scale caption based on give index of the label.
 Takes into account whether the scale should be logarithmic for memory graph.
 */
char*
LoadGraph::get_caption (guint index)
{
  char *caption;
  guint64 max_value;

  if (this->type == LOAD_GRAPH_NET)
    max_value = this->net.max;
  else if (this->type == LOAD_GRAPH_DISK)
    max_value = this->disk.max;
  else
    max_value = 100;

  // operation orders matters so it's 0 if index == num_bars
  float caption_percentage = (float)max_value - index * (float)max_value / this->num_bars;

  if (this->is_logarithmic_scale ())
    {
      float caption_value = caption_percentage == 0 ? 0 : pow (100, caption_percentage / max_value);
      // Translators: loadgraphs y axis percentage labels: 0 %, 50%, 100%
      caption = g_strdup_printf (_("%.0f %%"), caption_value);
    }
  else if (this->type == LOAD_GRAPH_NET)
    {
      const std::string captionstr (procman::format_network_rate ((guint64)caption_percentage));
      caption = g_strdup (captionstr.c_str ());
    }
  else if (this->type == LOAD_GRAPH_DISK)
    {
      const std::string captionstr (procman::format_rate ((guint64)caption_percentage));
      caption = g_strdup (captionstr.c_str ());
    }
  else
    {
      // Translators: loadgraphs y axis percentage labels: 0 %, 50%, 100%
      caption = g_strdup_printf (_("%.0f %%"), caption_percentage);
    }

  return caption;
}

/*
 Translates y partial position to logarithmic position if set to logarithmic scale.
*/
float
LoadGraph::translate_to_log_partial_if_needed (float position_partial)
{
  if (this->is_logarithmic_scale ())
    position_partial = position_partial == 0 ? 0 : log10 (position_partial * 100) / 2;

  return position_partial;
}

/*
 How many samples back from the newest one the right edge of the graph is.
 Each graph counts from its own samples, not the reference graph's: their
 timers tick at slightly different moments, and a shared count would make
 every graph but one jump back and forth by a sample on every tick.
*/
guint
LoadGraph::scroll_back () const
{
  if (this->history == NULL || this->history->live)
    return 0;

  guint visible = gsm_graph_get_num_points (this->disp);
  guint max_back = this->num_points > visible ? this->num_points - visible : 0;
  guint64 back = this->samples > this->history->anchor ? this->samples - this->history->anchor : 0;

  return MIN (back, max_back);
}

/* Number of vertical grid lines, and so of captions on the time axis. */
static const guint TIME_AXIS_SECTIONS = 7;

static gchar*
format_duration (unsigned seconds)
{
  gchar *caption = NULL;

  unsigned minutes = seconds / 60;
  unsigned hours = seconds / 3600;

  if (hours != 0)
    {
      if (minutes % 60 == 0)
        {
          // If minutes mod 60 is 0 set it to 0, to prevent it from showing full hours in
          // minutes in addition to hours.
          minutes = 0;
        }
      else
        {
          // Round minutes as seconds wont get shown if neither hours nor minutes are 0.
          minutes = int(rint (seconds / 60.0)) % 60;
          if (minutes == 0)
            {
              // Increase hours if rounding minutes results in 0, because that would be
              // what it would be rounded to.
              hours++;
              // Set seconds to hours * 3600 to prevent seconds from being drawn.
              seconds = hours * 3600;
            }
        }
    }

  gchar*captionH = g_strdup_printf (dngettext (GETTEXT_PACKAGE, "%u hr", "%u hrs", hours), hours);
  gchar*captionM = g_strdup_printf (dngettext (GETTEXT_PACKAGE, "%u min", "%u mins", minutes),
                                    minutes);
  gchar*captionS = g_strdup_printf (dngettext (GETTEXT_PACKAGE, "%u sec", "%u secs", seconds % 60),
                                    seconds % 60);

  caption = g_strjoin (" ", hours > 0 ? captionH : "",
                       minutes > 0 ? captionM : "",
                       seconds % 60 > 0 ? captionS : "",
                       NULL);
  g_free (captionH);
  g_free (captionM);
  g_free (captionS);

  return caption;
}

static int time_axis_height (LoadGraph *graph);
static void history_set_margins (LoadGraphHistory *history);
static void rescale_net_or_disk (LoadGraph *graph);

static void
load_graph_rescale (LoadGraph *graph)
{
  ///org/gnome/desktop/interface/text-scaling-factor
  gsm_graph_set_font_size (GSM_GRAPH (graph->disp), 8 * graph->font_settings->get_double ("text-scaling-factor"));

  if (graph->time_axis != NULL)
    gtk_widget_set_size_request (graph->time_axis, -1, time_axis_height (graph));

  /* The right margin of the graphs follows the font size */
  if (graph->history != NULL && graph->history->reference == graph)
    history_set_margins (graph->history);
}

static cairo_surface_t*
create_background (LoadGraph *graph,
                   int        width,
                   int        height)
{
  GdkRGBA fg_color;
  GdkRGBA label_color;
  GdkRGBA grid_color;
  GtkAllocation allocation;
  PangoContext *pango_context;
  PangoFontDescription *font_desc;
  PangoLayout *layout;
  cairo_t *cr;
  cairo_surface_t *surface;
  double fontsize = gsm_graph_get_font_size (graph->disp);
  double rmargin = gsm_graph_get_right_margin (graph->disp);
  guint num_bars = gsm_graph_get_num_bars (graph->disp, height);
  const guint num_sections = TIME_AXIS_SECTIONS;

  guint indent = gsm_graph_get_indent (graph->disp);

  gtk_widget_get_allocation (GTK_WIDGET (graph->disp), &allocation);
  surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32,
                                        allocation.width,
                                        allocation.height);
  cr = cairo_create (surface);

  /* Create grid label layout */
  layout = pango_cairo_create_layout (cr);

  /* Set font for graph labels */
  pango_context = gtk_widget_get_pango_context (GTK_WIDGET (graph->disp));
  font_desc = pango_context_get_font_description (pango_context);
  pango_font_description_set_size (font_desc, 0.8 * fontsize * PANGO_SCALE);
  pango_layout_set_font_description (layout, font_desc);

  /* draw frame */
  cairo_translate (cr, FRAME_WIDTH, FRAME_WIDTH);

  /* Draw background rectangle */
  /* When a user uses a dark theme, the hard-coded
   * white background in GSM is a lone white on the
   * display, which makes the user unhappy. To fix
   * this, here we offer the user a chance to set
   * his favorite background color. */
  GtkStyleContext *context = gtk_widget_get_style_context (GTK_WIDGET (graph->disp));

  /* The axis labels sit in the margin beside the graph, on the window's own
   * background, so they keep the theme's text color. */
  gtk_style_context_get_color (context, &label_color);

  gtk_style_context_save (context);

  /* Here we specify the name of the class. Now in
   * the theme's CSS we can specify the own colors
   * for this class. */
  gtk_style_context_add_class (context, "loadgraph");

  /* Get foreground color */
  gtk_style_context_get_color (context, &fg_color);
  /* The grid color is the same as the foreground color but has sometimes
   * different alpha. Keep it separate. */
  grid_color = fg_color;

  /* Why not use the new features of the
   * GTK instead of cairo_rectangle ?! :) */
  gtk_render_background (context, cr, indent, 0.0,
                         width - rmargin - indent,
                         graph->real_draw_height);

  gtk_style_context_restore (context);

  cairo_set_line_width (cr, 0.25);

  /* Horizontal grid lines */
  for (guint i = 0; i <= num_bars; i++)
    {
      PangoRectangle extents;

      /* Label alignment */
      double y;
      if (i == 0)
        /* Below the line */
        y = 0.5 + fontsize / 2.0;
      else if (i == num_bars)
        /* Above the line */
        y = i * graph->graph_dely + 0.5;
      else
        /* Next to the line */
        y = i * graph->graph_dely + fontsize / 2.0;

      /* Draw the label */
      /* Prepare the text */
      gchar *caption = graph->get_caption (i);
      pango_layout_set_text (layout, caption, -1);
      pango_layout_set_alignment (layout, PANGO_ALIGN_LEFT);
      pango_layout_get_extents (layout, NULL, &extents);

      /* Create y axis position modifier */
      double label_y_offset_modifier = i == 0 ? 0.5
                                : i == num_bars
                                    ? 1.0
                                    : 0.85;

      /* Set the label position */
      cairo_move_to (cr,
                     width - indent - 23,
                     y - label_y_offset_modifier * extents.height / PANGO_SCALE);

      /* Set the color */
      gdk_cairo_set_source_rgba (cr, &label_color);

      /* Paint the grid label */
      pango_cairo_show_layout (cr, layout);
      g_free (caption);

      /* Set the grid line alpha */
      if (i == 0 || i == num_bars)
        grid_color.alpha = BORDER_ALPHA;
      else
        grid_color.alpha = GRID_ALPHA;

      /* Draw the line */
      /* Set the color */
      gdk_cairo_set_source_rgba (cr, &grid_color);

      /* Set the grid line path */
      cairo_move_to (cr, indent, i * graph->graph_dely);
      cairo_line_to (cr,
                     width - rmargin + 4,
                     i * graph->graph_dely);
    }

  /* Vertical grid lines. The durations they stand for are written once, at
     the bottom of the page, by the shared time axis -- not under every
     graph, where the same seven captions were repeated four times over. */
  for (unsigned int i = 0; i < num_sections; i++)
    {
      /* Prepare the x position */
      double x = ceil (i * (width - rmargin - indent) / (num_sections - 1));

      /* Set the grid line alpha */
      if (i == 0 || i == (num_sections - 1))
        grid_color.alpha = BORDER_ALPHA;
      else
        grid_color.alpha = GRID_ALPHA;

      /* Draw the line */
      /* Set the color */
      gdk_cairo_set_source_rgba (cr, &grid_color);

      /* Set the grid line path */
      cairo_move_to (cr, x + indent, 0);
      cairo_line_to (cr, x + indent, graph->real_draw_height + 4);
    }

  /* Paint */
  cairo_stroke (cr);

  /* The top and bottom lines (the graph's maximum and its zero) are drawn again
   * over the grid, solid and a full pixel wide, so they stand out from it. Each
   * is nudged half a pixel into the graph so it covers one row of pixels
   * instead of being blurred across two. */
  grid_color.alpha = 1.0;
  gdk_cairo_set_source_rgba (cr, &grid_color);
  cairo_set_line_width (cr, 1.0);
  cairo_move_to (cr, indent, 0.5);
  cairo_line_to (cr, width - rmargin + 4, 0.5);
  cairo_move_to (cr, indent, graph->real_draw_height - 0.5);
  cairo_line_to (cr, width - rmargin + 4, graph->real_draw_height - 0.5);
  cairo_stroke (cr);

  g_object_unref (layout);
  cairo_destroy (cr);

  return surface;
}

/* Every graph on the Resources page shares one x axis: they all run at the
   same speed, over the same number of points, at the same width. Drawing the
   durations under each of them repeated the same seven captions four times
   over, so they are drawn once, here, below the last graph. */

static int
time_axis_height (LoadGraph *graph)
{
  PangoContext *pango_context;
  PangoFontDescription *font_desc;
  PangoLayout *layout;
  int height;

  pango_context = gtk_widget_get_pango_context (GTK_WIDGET (graph->time_axis));
  font_desc = pango_font_description_copy (pango_context_get_font_description (pango_context));
  pango_font_description_set_size (font_desc,
                                   0.8 * gsm_graph_get_font_size (graph->disp) * PANGO_SCALE);

  layout = pango_layout_new (pango_context);
  pango_layout_set_font_description (layout, font_desc);
  pango_layout_set_text (layout, "0", -1);
  pango_layout_get_pixel_size (layout, NULL, &height);

  g_object_unref (layout);
  pango_font_description_free (font_desc);

  return height;
}

static void
time_axis_draw (GtkDrawingArea *area,
                cairo_t        *cr,
                int             width,
                int             height,
                gpointer        data_ptr)
{
  LoadGraph * const graph = static_cast<LoadGraph*>(data_ptr);
  GtkWidget *self = GTK_WIDGET (area);
  GdkRGBA fg_color;
  PangoContext *pango_context;
  PangoFontDescription *font_desc;
  PangoLayout *layout;
  guint frames_per_unit = gsm_graph_get_frames_per_unit (graph->disp);
  double rmargin = gsm_graph_get_right_margin (graph->disp);
  guint indent = gsm_graph_get_indent (graph->disp);
  /* Read the speed and the point count back off the graph: LoadGraph::speed
     keeps its initial value when the update interval is changed. */
  guint speed = gsm_graph_get_speed (graph->disp);
  guint num_points = gsm_graph_get_num_points (graph->disp);
  /* Multiply before dividing: at short intervals speed * points / 1000 on its
     own truncates, and 15 seconds of graph read as 10. */
  const unsigned total_seconds = speed * frames_per_unit * (num_points - 2) / 1000;
  /* How long ago the right edge is, when scrolled back through the history */
  const unsigned back_seconds = speed * frames_per_unit * graph->scroll_back () / 1000;

  /* The graphs are siblings of this widget in the same vertical box, so they
     span exactly the same columns; the grid lines are where create_background ()
     puts them, on a width that excludes the frame it draws inside. */
  double graph_width = width - 2 * FRAME_WIDTH;

  if (graph_width <= rmargin + indent)
    return;

  pango_context = gtk_widget_get_pango_context (self);
  font_desc = pango_font_description_copy (pango_context_get_font_description (pango_context));
  pango_font_description_set_size (font_desc,
                                   0.8 * gsm_graph_get_font_size (graph->disp) * PANGO_SCALE);

  layout = pango_layout_new (pango_context);
  pango_layout_set_font_description (layout, font_desc);

  gtk_widget_get_color (self, &fg_color);
  gdk_cairo_set_source_rgba (cr, &fg_color);

  for (guint i = 0; i < TIME_AXIS_SECTIONS; i++)
    {
      PangoRectangle extents;

      double x = ceil (i * (graph_width - rmargin - indent) / (TIME_AXIS_SECTIONS - 1));

      gchar *caption = format_duration (back_seconds + total_seconds
                                        - i * total_seconds / (TIME_AXIS_SECTIONS - 1));

      pango_layout_set_text (layout, caption, -1);
      pango_layout_get_extents (layout, NULL, &extents);

      /* The first caption hangs off the line to the right, the last one to the
         left, so that neither runs past the end of the grid. */
      double label_x_offset_modifier = i == 0 ? 0
                                       : i == (TIME_AXIS_SECTIONS - 1)
                                         ? 1.0
                                         : 0.5;

      cairo_move_to (cr,
                     FRAME_WIDTH + x + indent
                     - label_x_offset_modifier * extents.width / PANGO_SCALE + 1.0,
                     height - extents.height / PANGO_SCALE);

      pango_cairo_show_layout (cr, layout);
      g_free (caption);
    }

  g_object_unref (layout);
  pango_font_description_free (font_desc);
}

GtkWidget *
load_graph_create_time_axis (LoadGraph *graph)
{
  GtkWidget *area = gtk_drawing_area_new ();

  graph->time_axis = area;

  gtk_widget_set_hexpand (area, TRUE);
  gtk_widget_set_size_request (area, -1, time_axis_height (graph));
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (area), time_axis_draw, graph, NULL);

  return area;
}

/* The history scrollbar counts in samples of the reference graph. Its page
   is the samples on screen; its range, the samples kept so far, so that it
   never scrolls to where there is no data yet.
   The value is the left edge of the page, counted from the oldest sample. */

static void
history_set_margins (LoadGraphHistory *history)
{
  GsmGraph *disp = history->reference->disp;

  /* Span the plotted area only, like the time axis captions */
  gtk_widget_set_margin_start (history->scrollbar,
                               FRAME_WIDTH + gsm_graph_get_indent (disp));
  gtk_widget_set_margin_end (history->scrollbar,
                             FRAME_WIDTH + (int) ceil (gsm_graph_get_right_margin (disp)));
}

/* Move the scrollbar along with the data: keep it at the right end when live,
   and on the same sample otherwise. */
static void
history_sync (LoadGraphHistory *history)
{
  LoadGraph *reference = history->reference;
  const guint visible = gsm_graph_get_num_points (reference->disp);
  const double page = visible - 2;
  const double upper = MAX (double (MIN (reference->filled, reference->num_points - 2)), page);
  double value = upper - page;

  if (!history->live)
    {
      const guint64 back = reference->samples > history->anchor
                           ? reference->samples - history->anchor
                           : 0;

      value -= back;
      if (value < 0)
        {
          /* The samples that were on screen are gone from the history:
             stay on the oldest ones left. */
          value = 0;
          history->anchor = reference->samples - guint64 (upper - page);
        }
    }

  history->updating = true;
  gtk_adjustment_configure (history->adj, value, 0, upper, 1, page, page);
  history->updating = false;

  gtk_widget_set_visible (history->scrollbar,
                          reference->num_points > visible
                          || GsmApplication::get ()->config.graph_keep_all_history);

  /* When live, the time axis stays the same; when not, the right edge just
     got one sample older. The value does not always change with it: not
     while the history is still filling up. */
  if (!history->live && reference->time_axis != NULL)
    gtk_widget_queue_draw (reference->time_axis);
}

static void
history_value_changed (GtkAdjustment    *adj,
                       LoadGraphHistory *history)
{
  /* Only follow the user; history_sync () takes care of the rest */
  if (history->updating || history->reference == NULL)
    return;

  const double top = gtk_adjustment_get_upper (adj) - gtk_adjustment_get_page_size (adj);
  const guint64 back = llround (top - gtk_adjustment_get_value (adj));

  history->live = back == 0;
  history->anchor = history->reference->samples - back;

  for (LoadGraph *graph : history->graphs)
    {
      rescale_net_or_disk (graph);
      gtk_widget_queue_draw (GTK_WIDGET (graph->disp));
    }

  if (history->reference->time_axis != NULL)
    gtk_widget_queue_draw (history->reference->time_axis);
}

static void
history_free (GtkWidget*,
              gpointer data_ptr)
{
  LoadGraphHistory * const history = static_cast<LoadGraphHistory*>(data_ptr);

  for (LoadGraph *graph : history->graphs)
    graph->history = NULL;

  g_signal_handlers_disconnect_by_data (history->adj, history);
  g_object_unref (history->adj);

  delete history;
}

GtkWidget *
load_graph_create_history_scrollbar (LoadGraph                     *reference,
                                     const std::vector<LoadGraph*> &graphs)
{
  LoadGraphHistory *history = new LoadGraphHistory ();

  history->adj = GTK_ADJUSTMENT (g_object_ref_sink (gtk_adjustment_new (0, 0, 0, 1, 0, 0)));
  history->scrollbar = gtk_scrollbar_new (GTK_ORIENTATION_HORIZONTAL, history->adj);
  history->graphs = graphs;
  history->reference = reference;
  history->live = true;
  history->anchor = 0;
  history->updating = false;

  for (LoadGraph *graph : graphs)
    graph->history = history;

  gtk_widget_set_hexpand (history->scrollbar, TRUE);
  history_set_margins (history);

  g_signal_connect (history->adj, "value-changed",
                    G_CALLBACK (history_value_changed), history);
  g_signal_connect (history->scrollbar, "destroy",
                    G_CALLBACK (history_free), history);

  history_sync (history);

  return history->scrollbar;
}

static void
load_graph_draw (GtkDrawingArea* area,
                 cairo_t *cr,
                 int      width,
                 int      height,
                 gpointer data_ptr)
{
  LoadGraph * const graph = static_cast<LoadGraph*>(data_ptr);
  cairo_surface_t * background;
  guint frames_per_unit = gsm_graph_get_frames_per_unit (GSM_GRAPH (area));
  guint render_counter = gsm_graph_get_render_counter (GSM_GRAPH (area));
  double rmargin = gsm_graph_get_right_margin (GSM_GRAPH (area));
  guint num_points = gsm_graph_get_num_points (GSM_GRAPH (area));
  guint indent = gsm_graph_get_indent (GSM_GRAPH (area));
  /* The newest sample drawn: data[back] */
  guint back = graph->scroll_back ();
  graph->num_bars = gsm_graph_get_num_bars (GSM_GRAPH (area), height);

  /* Initialize graph dimensions */
  width -= 2 * FRAME_WIDTH;
  height -= 2 * FRAME_WIDTH;

  /* The x axis captions used to live in a 15px strip below the grid; they are
     drawn once for the whole page now, so the grid gets that height back. */
  graph->graph_dely = height / graph->num_bars;   /* round to int to avoid AA blur */
  graph->real_draw_height = graph->graph_dely * graph->num_bars;

  /* Number of pixels wide for one sample point */
  const double x_step = double(width - rmargin - graph->indent) / (num_points - 2);

  /* Lines start at the right edge of the drawing,
   * a bit outside the clip rectangle. */
  /* Adjustment for smooth movement between samples */
  double x_offset = width - rmargin + FRAME_WIDTH;

  /* Shift the x position of the most recent (shown rightmost) value outside of the clip area in order
     to be able to simulate continuous, smooth movement without the line being cut off at its ends.
     Scrolled back into the history, the graph holds still, with data[back] on the right edge. */
  if (back == 0)
    x_offset += x_step * (1 - render_counter / double(frames_per_unit));

  /* Draw background */
  if (!gsm_graph_is_background_set (GSM_GRAPH (graph->disp))) {
    background = create_background (graph, width, height);
    gsm_graph_set_background (GSM_GRAPH (graph->disp), background);
  } else {
    background = gsm_graph_get_background (GSM_GRAPH (graph->disp));
  }

  cairo_set_source_surface (cr, background, 0, 0);
  cairo_paint (cr);

  /* Set the drawing style */
  cairo_set_line_width (cr, 1);
  cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join (cr, CAIRO_LINE_JOIN_ROUND);

  /* Clip the drawing area to the inside of the drawn background */
  cairo_rectangle (cr,
                   indent + FRAME_WIDTH,
                   FRAME_WIDTH,
                   width - rmargin - indent,
                   graph->real_draw_height);
  cairo_clip (cr);

  bool drawStacked = graph->type == LOAD_GRAPH_CPU && GsmApplication::get ()->config.draw_stacked;
  bool drawSmooth = GsmApplication::get ()->config.draw_smooth;
  gsm_graph_set_smooth_chart (GSM_GRAPH (area), drawSmooth);
  gsm_graph_set_stacked_chart (GSM_GRAPH (area), drawStacked);
  

  const double y_base = FRAME_WIDTH;

  /* Draw every graph */
  for (gint j = graph->n - 1; j >= 0; j--)
    {
      /* Set the color of the currently drawn graph */
      gdk_cairo_set_source_rgba (cr, &(graph->colors [j]));

      /* Start drawing on the right at the correct height */
      cairo_move_to (cr, x_offset, y_base + (1.0f - graph->data[back][j]) * graph->real_draw_height);

      /* Draw the path of the line
         Loop starts at 1 because the curve accesses the 0th data point */
      for (guint i = 1; i < num_points; i++)
        {
          const double *sample = graph->data[back + i];
          const double *next_sample = graph->data[back + i - 1];

          if (sample[j] == -1.0f)
            continue;

          if (drawSmooth)
            cairo_curve_to (cr,
                            x_offset - ((i - 0.5f) * x_step),
                            y_base + (1.0 - next_sample[j]) * graph->real_draw_height,
                            x_offset - ((i - 0.5f) * x_step),
                            y_base + (1.0 - sample[j]) * graph->real_draw_height,
                            x_offset - (i * x_step),
                            y_base + (1.0 - sample[j]) * graph->real_draw_height);
          else
            cairo_line_to (cr,
                           x_offset - (i * x_step),
                           y_base + (1.0 - sample[j]) * graph->real_draw_height);
        }

      if (drawStacked)
        {
          /* Draw the remaining outline of the area */
          /* Left bottom corner */
          cairo_rel_line_to (cr, 0, y_base + graph->real_draw_height);
          /* Right bottom corner.
             It's drawn far outside the visible area to avoid a weird bug
             where it's not filling the area it should completely */
          cairo_rel_line_to (cr, x_offset * 2, 0);

          cairo_close_path (cr);
          cairo_fill (cr);
        }
      else
        {
          cairo_stroke (cr);
        }
    }
}

void
load_graph_reset (LoadGraph *graph)
{
  std::fill (graph->data_block.begin (), graph->data_block.end (), -1.0);
  graph->filled = 0;
}

static void
get_load (LoadGraph *graph)
{
  guint i;
  glibtop_cpu cpu;

  glibtop_get_cpu (&cpu);

  auto NOW = [&]() -> guint64 (&)[GLIBTOP_NCPU][N_CPU_STATES] {
               return graph->cpu.times[graph->cpu.now];
             };
  auto LAST = [&]() -> guint64 (&)[GLIBTOP_NCPU][N_CPU_STATES] {
                return graph->cpu.times[graph->cpu.now ^ 1];
              };

  if (graph->n == 1)
    {
      NOW ()[0][CPU_TOTAL] = cpu.total;
      NOW ()[0][CPU_USED] = cpu.user + cpu.nice + cpu.sys;
    }
  else
    {
      for (i = 0; i < graph->n; i++)
        {
          NOW ()[i][CPU_TOTAL] = cpu.xcpu_total[i];
          NOW ()[i][CPU_USED] = cpu.xcpu_user[i] + cpu.xcpu_nice[i]
                                + cpu.xcpu_sys[i];
        }
    }

  // on the first call, LAST is 0
  // which means data is set to the average load since boot
  // that value has no meaning, we just want all the
  // graphs to be aligned, so the CPU graph needs to start
  // immediately
  bool drawStacked = graph->type == LOAD_GRAPH_CPU && GsmApplication::get ()->config.draw_stacked;

  for (i = 0; i < graph->n; i++)
    {
      float load;
      float total, used;
      gchar *text;

      total = NOW ()[i][CPU_TOTAL] - LAST ()[i][CPU_TOTAL];
      used = NOW ()[i][CPU_USED] - LAST ()[i][CPU_USED];

      load = used / MAX (total, 1.0f);
      graph->data[0][i] = load;
      if (drawStacked)
        {
          graph->data[0][i] /= graph->n;
          if (i > 0)
            graph->data[0][i] += graph->data[0][i - 1];
        }

      /* Update label */
      // Translators: CPU usage percentage label: 95.7%
      text = g_strdup_printf (_("%.1f%%"), load * 100.0f);
      gtk_label_set_text (GTK_LABEL (graph->labels.cpu[i]), text);
      g_free (text);
    }

  graph->cpu.now ^= 1;
}

static void
set_memory_label_and_picker (GtkLabel      *label,
                             GsmColorButton*picker,
                             guint64        used,
                             guint64        cached,
                             guint64        total,
                             double         percent)
{
  char*used_text;
  char*cached_text;
  char*cached_label;
  char*total_text;
  char*text;

  used_text = format_byte_size (used, GsmApplication::get ()->config.resources_memory_in_iec);
  cached_text = format_byte_size (cached, GsmApplication::get ()->config.resources_memory_in_iec);
  total_text = format_byte_size (total, GsmApplication::get ()->config.resources_memory_in_iec);
  if (total == 0)
    {
      text = g_strdup (_("not available"));
    }
  else
    {
      // xgettext: "540MiB (53 %) of 1.0 GiB" or "540MB (53 %) of 1.0 GB"
      text = g_strdup_printf (_("%s (%.1f%%) of %s"), used_text, 100.0 * percent, total_text);

      if (cached != 0)
        {
          char*used_label = text;

          // xgettext: Used cache string, e.g.: "Cache 2.4GiB" or "Cache 2.4GB"
          cached_label = g_strdup_printf (_("Cache %s"), cached_text);
          // The label sits on the section title row, so keep it to one line.
          text = g_strdup_printf ("%s \xc2\xb7 %s", used_label, cached_label);
          g_free (used_label);
          g_free (cached_label);
        }
    }

  gtk_label_set_text (label, text);
  g_free (used_text);
  g_free (cached_text);
  g_free (total_text);
  g_free (text);

  if (picker)
    gsm_color_button_set_fraction (picker, percent);
}

static void
get_memory (LoadGraph *graph)
{
  float mempercent, swappercent;

  glibtop_mem mem;
  glibtop_swap swap;

  glibtop_get_mem (&mem);
  glibtop_get_swap (&swap);

  /* There's no swap on LiveCD : 0.0f is better than NaN :) */
  swappercent = (swap.total ? (float)swap.used / (float)swap.total : 0.0f);
  mempercent = (float)mem.user / (float)mem.total;
  set_memory_label_and_picker (GTK_LABEL (graph->labels.memory),
                               GSM_COLOR_BUTTON (graph->mem_color_picker),
                               mem.user, mem.cached, mem.total, mempercent);

  set_memory_label_and_picker (GTK_LABEL (graph->labels.swap),
                               GSM_COLOR_BUTTON (graph->swap_color_picker),
                               swap.used, 0, swap.total, swappercent);

  gtk_widget_set_sensitive (GTK_WIDGET (graph->swap_color_picker), swap.total > 0);

  graph->data[0][0] = graph->translate_to_log_partial_if_needed (mempercent);
  graph->data[0][1] = swap.total > 0 ? graph->translate_to_log_partial_if_needed (swappercent) : -1.0;
}

/* Nice Numbers for Graph Labels after Paul Heckbert
   nicenum: find a "nice" number approximately equal to x.
   Round the number if round=1, take ceiling if round=0    */

static double
nicenum (double x,
         int    round)
{
  int expv;                  /* exponent of x */
  double f;                  /* fractional part of x */
  double nf;                  /* nice, rounded fraction */

  expv = floor (log10 (x));
  f = x / pow (10.0, expv);       /* between 1 and 10 */
  if (round)
    {
      if (f < 1.5)
        nf = 1.0;
      else if (f < 3.0)
        nf = 2.0;
      else if (f < 7.0)
        nf = 5.0;
      else
        nf = 10.0;
    }
  else
    {
      if (f <= 1.0)
        nf = 1.0;
      else if (f <= 2.0)
        nf = 2.0;
      else if (f <= 5.0)
        nf = 5.0;
      else
        nf = 10.0;
    }
  return nf * pow (10.0, expv);
}

/* values is a ring buffer, the newest sample at latest - 1; this is the one
   `back` samples before that. */
static unsigned &
value_at (LoadGraph             *graph,
          std::vector<unsigned> *values,
          guint                  back)
{
  const guint n = graph->num_points;

  return values->at ((graph->latest + n - 1 - back % n) % n);
}

/* Fit the scale to the samples on screen, rather than to the whole history:
   a spike an hour ago must not flatten the graph of the last minute. */
static void
rescale_to_window (LoadGraph             *graph,
                   std::vector<unsigned> *values,
                   guint64               *max,
                   gboolean               in_bits)
{
  const guint back = graph->scroll_back ();
  const guint visible = gsm_graph_get_num_points (graph->disp);

  guint64 new_max = 0;
  for (guint i = back; i < back + visible && i < graph->num_points; i++)
    new_max = std::max (new_max, guint64 (value_at (graph, values, i)));

  //
  // Round maximum
  //

  const guint64 bak_max (new_max);

  if (in_bits)
    {
      // nice number is for the ticks
      unsigned ticks = graph->num_bars;

      if (graph->num_bars == 0)
        return;

      // gets messy at low values due to division by 8
      guint64 bit_max = std::max (new_max * 8, G_GUINT64_CONSTANT (10000));

      // our tick size leads to max
      double d = nicenum (bit_max / ticks, 0);
      bit_max = ticks * d;
      new_max = bit_max / 8;

      procman_debug ("bak*8 %" G_GUINT64_FORMAT ", ticks %d, d %f"
                     ", bit_max %" G_GUINT64_FORMAT ", new_max %" G_GUINT64_FORMAT,
                     bak_max * 8, ticks, d, bit_max, new_max);
    }
  else
    {
      // round up to get some extra space
      // yes, it can overflow
      new_max = 1.1 * new_max;
      // make sure max is not 0 to avoid / 0
      // default to 1 KiB
      new_max = std::max (new_max, G_GUINT64_CONSTANT (1024));

      // decompose new_max = coef10 * 2**(base10 * 10)
      // where coef10 and base10 are integers and coef10 < 2**10
      //
      // e.g: ceil(100.5 KiB) = 101 KiB = 101 * 2**(1 * 10)
      //      where base10 = 1, coef10 = 101, pow2 = 16

      guint64 pow2 = std::floor (log2 (new_max));
      guint64 base10 = pow2 / 10.0;
      guint64 coef10 = std::ceil (new_max / double (G_GUINT64_CONSTANT (1) << (base10 * 10)));
      g_assert (new_max <= (coef10 * (G_GUINT64_CONSTANT (1) << (base10 * 10))));

      // then decompose coef10 = x * 10**factor10
      // where factor10 is integer and x < 10
      // so we new_max has only 1 significant digit

      guint64 factor10 = std::pow (10.0, std::floor (std::log10 (coef10)));
      coef10 = std::ceil (coef10 / double (factor10)) * factor10;

      new_max = coef10 * (G_GUINT64_CONSTANT (1) << guint64 (base10 * 10));
      procman_debug ("bak %" G_GUINT64_FORMAT " new_max %" G_GUINT64_FORMAT
                     "pow2 %" G_GUINT64_FORMAT " coef10 %" G_GUINT64_FORMAT,
                     bak_max, new_max, pow2, coef10);
    }

  // if max is the same or has decreased but not so much, don't
  // do anything to avoid rescaling
  if ((0.8 * *max) < new_max && new_max <= *max)
    return;

  const double scale = 1.0f * *max / new_max;

  for (size_t i = 0; i < graph->num_points; i++)
    if (graph->data[i][0] >= 0.0f)
      {
        graph->data[i][0] *= scale;
        graph->data[i][1] *= scale;
      }

  procman_debug ("rescale max = %" G_GUINT64_FORMAT
                 " new_max = %" G_GUINT64_FORMAT,
                 *max, new_max);

  *max = new_max;

  // force the graph background to be redrawn now that scale has changed
  graph->clear_background ();
}

static void
dynamic_scale (LoadGraph             *graph,
               std::vector<unsigned> *values,
               guint64               *max,
               guint64                din,
               guint64                dout,
               gboolean               in_bits)
{
  graph->data[0][0] = 1.0f * din / *max;
  graph->data[0][1] = 1.0f * dout / *max;

  value_at (graph, values, 0) = std::max (din, dout);

  rescale_to_window (graph, values, max, in_bits);
}

/* The history scrollbar moved: net and disk rescale to what is now on screen. */
static void
rescale_net_or_disk (LoadGraph *graph)
{
  if (graph->type == LOAD_GRAPH_NET)
    rescale_to_window (graph, &graph->net.values, &graph->net.max,
                       GsmApplication::get ()->config.network_in_bits);
  else if (graph->type == LOAD_GRAPH_DISK)
    rescale_to_window (graph, &graph->disk.values, &graph->disk.max, FALSE);
}

static guint64
get_hash64 (const gchar*c_str)
{
  // Fowler–Noll–Vo FNV-1 64-bit hash:

  guint64 hash = 0xcbf29ce484222325L;

  while (gchar c = *c_str++)
    {
      hash = (hash * 0x00000100000001B3L) ^ c;
    }

  return hash;
}

static void
handle_dynamic_max_value (LoadGraph             *graph,
                          std::vector<unsigned> *values,
                          guint64               *max,
                          guint64               *last_in,
                          guint64               *last_out,
                          guint64                in,
                          guint64                out,
                          guint64               *graph_time,
                          guint64                hash,
                          guint64               *graph_hash,
                          gboolean               in_bits,
                          gboolean               totals_in_bits,
                          GtkLabel              *label_in,
                          GtkLabel              *label_out,
                          GtkLabel              *label_in_total,
                          GtkLabel              *label_out_total)
{
  guint64 time = g_get_monotonic_time ();
  guint64 din, dout;

  if (in >= *last_in && out >= *last_out &&
      (graph_hash == NULL || hash == *graph_hash) &&
      *graph_time != 0)
    {
      float dtime = ((double) (time - *graph_time)) / G_USEC_PER_SEC;
      din = static_cast<guint64>((in - *last_in) / dtime);
      dout = static_cast<guint64>((out - *last_out) / dtime);
    }
  else
    {
      /* Don't calc anything if new data is less than old (interface
         removed, counters reset, ...) or if it is the first time */
      din = 0;
      dout = 0;
    }

  *last_in = in;
  *last_out = out;
  *graph_time = time;
  if (graph_hash != NULL)
    *graph_hash = hash;

  dynamic_scale (graph, values, max, din, dout, in_bits);

  char*total_text;

  gtk_label_set_text (GTK_LABEL (label_in), procman::format_rate (din, in_bits).c_str ());
  // The running total follows the rate on the section title row: "1.2 MiB/s (3.4 GiB)".
  total_text = g_strdup_printf ("(%s)", procman::format_volume (in, totals_in_bits).c_str ());
  gtk_label_set_text (GTK_LABEL (label_in_total), total_text);
  g_free (total_text);

  gtk_label_set_text (GTK_LABEL (label_out), procman::format_rate (dout, in_bits).c_str ());
  total_text = g_strdup_printf ("(%s)", procman::format_volume (out, totals_in_bits).c_str ());
  gtk_label_set_text (GTK_LABEL (label_out_total), total_text);
  g_free (total_text);
}

static void
get_net (LoadGraph *graph)
{
  glibtop_netlist netlist;
  guint32 i;
  guint64 in = 0, out = 0;
  guint64 hash = 1;
  char **ifnames;

  ifnames = glibtop_get_netlist (&netlist);

  for (i = 0; i < netlist.number; ++i)
    {
      glibtop_netload netload;
      glibtop_get_netload (&netload, ifnames[i]);

      if (netload.if_flags & (1 << GLIBTOP_IF_FLAGS_LOOPBACK))
        continue;

      /* Skip interfaces without any IPv4/IPv6 address (or
         those with only a LINK ipv6 addr) However we need to
         be able to exclude these while still keeping the
         value so when they get online (with NetworkManager
         for example) we don't get a sudden peak.  Once we're
         able to get this, ignoring down interfaces will be
         possible too.  */
      if (not ((netload.flags & (1 << GLIBTOP_NETLOAD_ADDRESS6))
               and netload.scope6 != GLIBTOP_IF_IN6_SCOPE_LINK)
          and not (netload.flags & (1 << GLIBTOP_NETLOAD_ADDRESS)))
        continue;

      /* Don't skip interfaces that are down (GLIBTOP_IF_FLAGS_UP)
         to avoid spikes when they are brought up */

      in += netload.bytes_in;
      out += netload.bytes_out;
      hash += get_hash64 (ifnames[i]);
    }

  g_strfreev (ifnames);

  handle_dynamic_max_value (graph, &graph->net.values, &graph->net.max, &graph->net.last_in,
                            &graph->net.last_out, in, out, &graph->net.time,
                            hash, &graph->net.last_hash,
                            GsmApplication::get ()->config.network_in_bits,
                            GsmApplication::get ()->config.network_total_in_bits,
                            graph->labels.net_in, graph->labels.net_out,
                            graph->labels.net_in_total, graph->labels.net_out_total);
}

static void
get_disk (LoadGraph *graph)
{
  glibtop_disk disk;
  gint32 i;
  guint64 read = 0, write = 0;

  glibtop_get_disk (&disk);

  for (i = 0; i < glibtop_global_server->ndisk; i++)
    {
      read += disk.xdisk_sectors_read[i];
      write += disk.xdisk_sectors_write[i];
    }

  // These sectors are standard unix 512 byte sectors, not the actual disk sectors.
  read *= 512;
  write *= 512;

  handle_dynamic_max_value (graph, &graph->disk.values, &graph->disk.max, &graph->disk.last_read,
                            &graph->disk.last_write, read, write, &graph->disk.time, 0, NULL,
                            FALSE, FALSE, graph->labels.disk_read, graph->labels.disk_write,
                            graph->labels.disk_read_total, graph->labels.disk_write_total);
}

int
load_graph_update_data (LoadGraph *graph)
{
  // Keeping all the history, make room rather than drop the oldest sample.
  // Doubling keeps the copies this takes few and far between.
  if (GsmApplication::get ()->config.graph_keep_all_history
      && graph->filled >= graph->num_points - 2)
    load_graph_change_num_points (graph, graph->num_points * 2,
                                  gsm_graph_get_num_points (graph->disp));

  // Rotate data one element down.
  std::rotate (graph->data.begin (),
               graph->data.end () - 1,
               graph->data.end ());

  // Update rotation counter.
  graph->latest = (graph->latest + 1) % graph->num_points;
  graph->samples++;
  graph->filled = MIN (graph->filled + 1, graph->num_points);

  // Replace the 0th element
  switch (graph->type)
    {
      case LOAD_GRAPH_CPU:
        get_load (graph);
        break;

      case LOAD_GRAPH_MEM:
        get_memory (graph);
        break;

      case LOAD_GRAPH_NET:
        get_net (graph);
        break;

      case LOAD_GRAPH_DISK:
        get_disk (graph);
        break;

      default:
        g_assert_not_reached ();
    }

  if (graph->history != NULL && graph->history->reference == graph)
    history_sync (graph->history);

  return 0;
}

static void
load_graph_destroy (GtkWidget*,
                    gpointer data_ptr)
{
  LoadGraph * const graph = static_cast<LoadGraph*>(data_ptr);

  /* The time axis outlives its reference graph in teardown, and its draw
     function reads through this pointer. */
  if (graph->time_axis != NULL)
    gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (graph->time_axis), NULL, NULL, NULL);

  /* Likewise the history, which the scrollbar owns */
  if (graph->history != NULL)
    {
      std::vector<LoadGraph*> &graphs = graph->history->graphs;

      graphs.erase (std::remove (graphs.begin (), graphs.end (), graph), graphs.end ());
      if (graph->history->reference == graph)
        graph->history->reference = NULL;
    }

  delete graph;
}

LoadGraph::LoadGraph(guint type)
  :
  indent (18.0),
  n (0),
  type (type),
  speed (GsmApplication::get ()->config.graph_update_interval),
  num_points (std::max (GsmApplication::get ()->config.graph_history_points,
                         GsmApplication::get ()->config.graph_data_points) + 2),
  latest (0),
  samples (0),
  filled (0),
  graph_dely (0),
  num_bars (0),
  real_draw_height (0),
  colors (),
  data_block (),
  data (),
  main_widget (NULL),
  disp (NULL),
  time_axis (NULL),
  history (NULL),
  labels (),
  mem_color_picker (NULL),
  swap_color_picker (NULL),
  font_settings (Gio::Settings::create (FONT_SETTINGS_SCHEMA)),
  cpu (),
  net (),
  disk ()
{
  font_settings->signal_changed (FONT_SETTING_SCALING).connect ([this](const Glib::ustring&) {
    load_graph_rescale (this);
  });

  switch (type)
    {
      case LOAD_GRAPH_CPU:
        cpu = CPU {};
        n = GsmApplication::get ()->config.num_cpus;

        for (guint i = 0; i < G_N_ELEMENTS (labels.cpu); ++i)
          labels.cpu[i] = make_tnum_label ();

        break;

      case LOAD_GRAPH_MEM:
        n = 2;
        labels.memory = init_tnum_label (30, GTK_ALIGN_START);
        labels.swap = init_tnum_label (30, GTK_ALIGN_START);
        break;

      case LOAD_GRAPH_NET:
        net = NET {};
        n = 2;
        net.max = 1;
        labels.net_in = init_tnum_label (11, GTK_ALIGN_END);
        labels.net_in_total = init_tnum_label (12, GTK_ALIGN_START);
        labels.net_out = init_tnum_label (11, GTK_ALIGN_END);
        labels.net_out_total = init_tnum_label (12, GTK_ALIGN_START);
        break;

      case LOAD_GRAPH_DISK:
        disk = DISK {};
        n = 2;
        disk.max = 1;
        labels.disk_read = init_tnum_label (11, GTK_ALIGN_END);
        labels.disk_read_total = init_tnum_label (12, GTK_ALIGN_START);
        labels.disk_write = init_tnum_label (11, GTK_ALIGN_END);
        labels.disk_write_total = init_tnum_label (12, GTK_ALIGN_START);
        break;
    }

  colors.resize (n);

  disp = GSM_GRAPH (gsm_graph_new ());
  gsm_graph_set_speed (disp, speed);
  gsm_graph_set_data_function (disp, (GSourceFunc)load_graph_update_data, this);
  gsm_graph_set_num_points (disp, GsmApplication::get ()->config.graph_data_points + 2);

  switch (type)
    {
      case LOAD_GRAPH_CPU:
        memcpy (&colors[0], GsmApplication::get ()->config.cpu_color,
                n * sizeof colors[0]);
        gsm_graph_set_max_value (disp, 100);
        break;

      case LOAD_GRAPH_MEM:
        colors[0] = GsmApplication::get ()->config.mem_color;
        colors[1] = GsmApplication::get ()->config.swap_color;
        mem_color_picker = gsm_color_button_new (&colors[0],
                                                 GSMCP_TYPE_CPU);
        swap_color_picker = gsm_color_button_new (&colors[1],
                                                  GSMCP_TYPE_CPU);
        gsm_graph_set_max_value (disp, 100);
        break;

      case LOAD_GRAPH_NET:
        net.values = std::vector<unsigned>(num_points);
        colors[0] = GsmApplication::get ()->config.net_in_color;
        colors[1] = GsmApplication::get ()->config.net_out_color;
        gsm_graph_set_max_value (disp, this->net.max);
        break;

      case LOAD_GRAPH_DISK:
        disk.values = std::vector<unsigned>(num_points);
        colors[0] = GsmApplication::get ()->config.disk_read_color;
        colors[1] = GsmApplication::get ()->config.disk_write_color;
        gsm_graph_set_max_value (disp, this->disk.max);
        break;
    }

  main_widget = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 6));
  gtk_widget_set_size_request (GTK_WIDGET (main_widget), -1, GRAPH_MIN_HEIGHT);

  gtk_widget_set_vexpand (GTK_WIDGET (disp), TRUE);
  gtk_widget_set_hexpand (GTK_WIDGET (disp), TRUE);
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (disp),
                                  load_graph_draw,
                                  this,
                                  NULL);
  g_signal_connect (G_OBJECT (disp), "destroy",
                    G_CALLBACK (load_graph_destroy), this);
  gtk_box_prepend (main_widget, GTK_WIDGET (disp));

  data = std::vector<double*>(num_points);
  /* Allocate data in a contiguous block */
  data_block = std::vector<double>(n * num_points, -1.0);

  for (guint i = 0; i < num_points; ++i)
    data[i] = &data_block[0] + i * n;

}

LoadGraph::~LoadGraph()
{
  load_graph_stop (this);
}

void
load_graph_start (LoadGraph *graph)
{
  gsm_graph_start (GSM_GRAPH (graph->disp));
}

void
load_graph_stop (LoadGraph *graph)
{
  /* don't draw anymore, but continue to poll */
  gsm_graph_stop (GSM_GRAPH (graph->disp));
}

void
load_graph_change_speed (LoadGraph *graph,
                         guint      new_speed)
{
  gsm_graph_set_speed (GSM_GRAPH (graph->disp), new_speed);

  /* The shared time axis spells out the length of the graph, which just moved */
  if (graph->time_axis != NULL)
    gtk_widget_queue_draw (graph->time_axis);
}

/* Put a ring buffer of values back in order for latest == 0, sized for the
   new amount of data points, keeping the newest ones. */
static void
reorder_values (LoadGraph             *graph,
                std::vector<unsigned> *values,
                guint                  new_num_points)
{
  std::vector<unsigned> reordered (new_num_points);

  for (guint back = 0; back < MIN (graph->num_points, new_num_points); back++)
    reordered[new_num_points - 1 - back] = value_at (graph, values, back);

  *values = std::move (reordered);
}

void
load_graph_change_num_points (LoadGraph *graph,
                              guint      new_num_points,
                              guint      new_visible_points)
{
  gsm_graph_set_num_points (graph->disp, new_visible_points);

  // Keeping all the history, it never gets shorter.
  if (GsmApplication::get ()->config.graph_keep_all_history)
    new_num_points = MAX (new_num_points, graph->num_points);

  // Nothing more to do if the history keeps its length.
  if (graph->num_points == new_num_points)
    {
      if (graph->history != NULL && graph->history->reference == graph)
        history_sync (graph->history);

      if (graph->time_axis != NULL)
        gtk_widget_queue_draw (graph->time_axis);
      return;
    }

  // The net and disk values are indexed by latest, which is about to be reset.
  if (graph->type == LOAD_GRAPH_NET)
    reorder_values (graph, &graph->net.values, new_num_points);
  else if (graph->type == LOAD_GRAPH_DISK)
    reorder_values (graph, &graph->disk.values, new_num_points);

  // Sort the values in the data_block vector in the order they were accessed in by the pointers in data.
  std::rotate (graph->data_block.begin (),
               graph->data_block.begin () + (graph->num_points - graph->latest) * graph->n,
               graph->data_block.end ());

  // Reset rotation counter.
  graph->latest = 0;

  // Resize the vectors to the new amount of data points.
  // Fill the new values with -1.
  graph->data.resize (new_num_points);
  graph->data_block.resize (graph->n * new_num_points, -1.0);

  // Replace the pointers in data, to match the new data_block values.
  for (guint i = 0; i < new_num_points; ++i)
    graph->data[i] = &graph->data_block[0] + i * graph->n;

  graph->num_points = new_num_points;
  graph->filled = MIN (graph->filled, new_num_points);

  if (graph->history != NULL && graph->history->reference == graph)
    history_sync (graph->history);

  // Force the scale to be redrawn.
  graph->clear_background ();

  if (graph->time_axis != NULL)
    gtk_widget_queue_draw (graph->time_axis);
}

LoadGraphLabels*
load_graph_get_labels (LoadGraph *graph)
{
  return &graph->labels;
}

GtkBox*
load_graph_get_widget (LoadGraph *graph)
{
  return graph->main_widget;
}

GsmColorButton*
load_graph_get_mem_color_picker (LoadGraph *graph)
{
  return graph->mem_color_picker;
}

GsmColorButton*
load_graph_get_swap_color_picker (LoadGraph *graph)
{
  return graph->swap_color_picker;
}
