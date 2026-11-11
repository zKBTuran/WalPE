/* monlist.h - the monitors as the toolkit names them, for the GUI while no
 * engine runs.
 *
 * The GUI's monitor list (selector, diagram, "Showing ...") is the
 * engine's: status "monitors", with the engine's indices and names, so
 * the two can never disagree. Without an engine there is no such list;
 * all the GUI can say then is which outputs GDK sees. GDK reports the
 * connector ("DP-1", "HDMI-A-1"; RandR output names on X11), which is the
 * string the engine uses as a monitor's name on both backends, so what is
 * shown here is what the list will show once the engine is up. Indices
 * are deliberately not shown: the engine's order is its own.
 * Header only; tests/ui_session_test.c covers the formatting. */
#pragma once
#include <gtk/gtk.h>

/* one entry: "DP-1 2560×1440"; a monitor without a connector name is
 * "Monitor"; scale: device pixels = logical size x scale (what the engine
 * reports as a monitor's size) */
static inline char *walpe_ui_monitor_text (const char *connector, int logical_w, int logical_h,
                                           double scale)
{
    if (scale < 1.0)
        scale = 1.0;
    int w = (int) (logical_w * scale + 0.5), h = (int) (logical_h * scale + 0.5);
    return g_strdup_printf ("%s %d×%d", connector && *connector ? connector : "Monitor", w, h);
}

/* "Monitors: DP-1 2560×1440, HDMI-A-1 1920×1080"; NULL without a display
 * or without monitors. g_free. */
static inline char *walpe_ui_monitors_line (GdkDisplay *d)
{
    if (!d)
        return NULL;
    GListModel *ms = gdk_display_get_monitors (d);
    guint n = ms ? g_list_model_get_n_items (ms) : 0;
    if (!n)
        return NULL;
    GString *s = g_string_new (n == 1 ? "Monitor: " : "Monitors: ");
    for (guint i = 0; i < n; i++) {
        GdkMonitor *m = g_list_model_get_item (ms, i);
        GdkRectangle g;
        gdk_monitor_get_geometry (m, &g);
        double scale = gdk_monitor_get_scale_factor (m);
#if GTK_CHECK_VERSION (4, 14, 0)
        scale = gdk_monitor_get_scale (m);  /* fractional where the compositor says so */
#endif
        char *t = walpe_ui_monitor_text (gdk_monitor_get_connector (m), g.width, g.height, scale);
        g_string_append_printf (s, "%s%s", i ? ", " : "", t);
        g_free (t);
        g_object_unref (m);
    }
    return g_string_free (s, FALSE);
}
