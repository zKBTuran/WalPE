/* uistats.h - opt-in GUI responsiveness probe.
 *
 * WALPE_UI_STATS=<file> records frame intervals (after-paint to
 * after-paint), per-frame work (before-paint to after-paint: update,
 * layout incl. list-item binds, paint) and main-loop lateness (a 5 ms
 * watchdog timer: how late it fires = how long the main thread was
 * blocked). SIGUSR1 resets the counters, SIGUSR2 appends a percentile
 * summary to the file. Off (no timers, no handlers) when unset. */
#pragma once
#include <gtk/gtk.h>

void walpe_ui_stats_attach (GtkWidget *win);
