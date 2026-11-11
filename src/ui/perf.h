/* perf.h - the quality-for-GPU-load controls (common/quality.h): a preset
 * selector that says what the viewer gives up and what the wallpapers on
 * screen would cost, and the single knobs behind it.
 *
 * Used twice: in Engine settings > Performance (the global settings) and
 * in a wallpaper's options pane (that wallpaper's own settings, which
 * replace the global ones for it; "As in Engine settings" drops them).
 * The panel talks to the daemon itself ({"cmd":"quality"} while it is
 * visible, "set" / "setquality" on a change): nothing to save. */
#pragma once
#include <gtk/gtk.h>

typedef void (*WalpePerfStatus) (const char *msg, gboolean error);

/* per_wallpaper FALSE: the global settings. TRUE: one wallpaper's own
 * (walpe_perf_panel_set_path says which; hidden while there is none) */
GtkWidget *walpe_perf_panel_new (gboolean per_wallpaper, WalpePerfStatus status);
void walpe_perf_panel_set_path (GtkWidget *panel, const char *path);
