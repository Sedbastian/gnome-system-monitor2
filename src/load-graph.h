#ifndef _GSM_LOAD_GRAPH_H_
#define _GSM_LOAD_GRAPH_H_

#include <glib.h>
#include <glibtop/cpu.h>

#include "gsm-graph.h"
#include "legacy/gsm_color_button.h"
#include "util.h"
#include "settings-keys.h"

enum
{
  LOAD_GRAPH_CPU,
  LOAD_GRAPH_MEM,
  LOAD_GRAPH_NET,
  LOAD_GRAPH_DISK
};

enum
{
  CPU_TOTAL,
  CPU_USED,
  N_CPU_STATES
};

struct LoadGraphLabels
{
  GtkLabel *cpu[GLIBTOP_NCPU];
  GtkLabel *memory;
  GtkLabel *swap;
  GtkLabel *net_in;
  GtkLabel *net_in_total;
  GtkLabel *net_out;
  GtkLabel *net_out_total;
  GtkLabel *disk_read;
  GtkLabel *disk_read_total;
  GtkLabel *disk_write;
  GtkLabel *disk_write_total;
};

struct LoadGraph;

/* Where the Resources page is looking in the graphs' history. There is one of
   these for the whole page, shared by all its graphs and by the scrollbar
   under them. */
struct LoadGraphHistory
{
  GtkAdjustment *adj;
  GtkWidget *scrollbar;
  std::vector<LoadGraph*> graphs;
  /* The graph whose samples the adjustment counts */
  LoadGraph *reference;
  /* Following the newest sample, at the right end of the scrollbar */
  bool live;
  /* When not live: the sample at the right edge of the graphs, as a count of
     the samples taken since the graphs started */
  guint64 anchor;
  /* Set while the adjustment is being moved along with the data, rather
     than by the user */
  bool updating;
};

struct LoadGraph
  : private procman::NonCopyable
{
  LoadGraph (guint type);
  ~LoadGraph();

  unsigned get_num_bars (int height) const;
  void     clear_background ();
  bool     is_logarithmic_scale () const;
  char *   get_caption (guint index);
  float    translate_to_log_partial_if_needed (float position_partial);
  guint    scroll_back () const;

  double indent;

  guint n;
  gint type;
  guint speed;
  /* The samples kept, of which gsm_graph_get_num_points () are on screen */
  guint num_points;
  guint latest;
  /* Samples taken since the graph started, and how many of them are kept */
  guint64 samples;
  guint filled;
  guint graph_dely;
  guint num_bars;
  guint real_draw_height;

  std::vector<GdkRGBA> colors;

  std::vector<double> data_block;
  std::vector<double*> data;

  GtkBox *main_widget;
  GsmGraph *disp;
  /* The x axis shared by every graph on the page, if this graph draws it */
  GtkWidget *time_axis;
  LoadGraphHistory *history;

  LoadGraphLabels labels;
  GsmColorButton *mem_color_picker;
  GsmColorButton *swap_color_picker;

  Glib::RefPtr<Gio::Settings> font_settings;

  /* union { */
  struct CPU
  {
    guint now;     /* 0 -> current, 1 -> last
                      now ^ 1 each time */
    /* times[now], times[now ^ 1] is last */
    guint64 times[2][GLIBTOP_NCPU][N_CPU_STATES];
  } cpu;

  struct NET
  {
    guint64 last_in, last_out;
    guint64 last_hash;
    guint64 time;
    guint64 max;
    std::vector<unsigned> values;
  } net;

  struct DISK
  {
    guint64 last_read, last_write;
    guint64 time;
    guint64 max;
    std::vector<unsigned> values;
  } disk;
  /* }; */
};

/* Force a drawing update */
void
load_graph_queue_draw (LoadGraph *g);

/* Start load graph. */
void
load_graph_start (LoadGraph *g);

/* Stop load graph. */
void
load_graph_stop (LoadGraph *g);

/* Change load graph speed and restart it if it has been previously started */
void
load_graph_change_speed (LoadGraph *g,
                         guint      new_speed);

/* Change how many data points load graph keeps, and how many of them it shows */
void
load_graph_change_num_points (LoadGraph *g,
                              guint      new_num_points,
                              guint      new_visible_points);

/* Clear the history data. */
void
load_graph_reset (LoadGraph *g);

LoadGraphLabels*
load_graph_get_labels (LoadGraph *g) G_GNUC_CONST;

GtkBox*
load_graph_get_widget (LoadGraph *g) G_GNUC_CONST;

/* Create the x axis shared by every graph on the page, drawn to g's grid. */
GtkWidget*
load_graph_create_time_axis (LoadGraph *g);

/* Create the scrollbar that scrolls every graph in graphs back through its
   history. reference is the graph that owns the time axis. */
GtkWidget*
load_graph_create_history_scrollbar (LoadGraph                     *reference,
                                     const std::vector<LoadGraph*> &graphs);

GsmColorButton*
load_graph_get_mem_color_picker (LoadGraph *g) G_GNUC_CONST;

GsmColorButton*
load_graph_get_swap_color_picker (LoadGraph *g) G_GNUC_CONST;

#endif /* _GSM_LOAD_GRAPH_H_ */
