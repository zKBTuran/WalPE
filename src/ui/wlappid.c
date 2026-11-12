/* wlappid.c - one application id for every window of the GUI on Wayland.
 *
 * On X11 every window of a program carries the same WM_CLASS ("walpe",
 * the desktop entry's StartupWMClass). On Wayland a window says an
 * application id instead, and a desktop looks for the desktop entry of
 * that name to find the window's icon and to group it in the task bar.
 * GTK gives the id of the GtkApplication, io.github.walpe.ui, to the
 * windows that belong to it - the library window - and the program's
 * name, "walpe", to every other toplevel: "Engine settings", "Monitor
 * resolution", the Workshop install and the delete dialogs. No desktop
 * entry is called walpe.desktop: on KDE Plasma those windows had the
 * generic Wayland icon in their title bars, next to a main window with
 * the right one.
 *
 * Making them windows of the application (gtk_window_set_application)
 * would also give them its accelerators - Ctrl+Q in the settings window
 * would quit - on X11 as well. So the id alone is set, where GTK allows
 * it: on the Wayland toplevel, once it exists. Nothing is done on X11. */
#include "wlappid.h"
#include <gtk/gtk.h>

#ifdef WALPE_UI_WAYLAND
#include <gdk/wayland/gdkwayland.h>

static const char *the_id;

/* (after GTK's own handler: the xdg_toplevel is made when the window is
 * mapped, and GTK names it then) */
static void on_map (GtkWidget *w, gpointer u)
{
    (void) u;
    GdkSurface *sf = gtk_native_get_surface (GTK_NATIVE (w));
    if (sf && walpe_wl_app_id_wanted (GDK_IS_WAYLAND_TOPLEVEL (sf),
                                      gtk_window_get_application (GTK_WINDOW (w)) != NULL))
        gdk_wayland_toplevel_set_application_id (GDK_TOPLEVEL (sf), the_id);
}

static void on_toplevels (GListModel *m, guint pos, guint removed, guint added, gpointer u)
{
    (void) removed; (void) u;
    for (guint i = pos; i < pos + added; i++) {
        GtkWidget *w = g_list_model_get_item (m, i);
        if (w && GTK_IS_WINDOW (w) &&
            !g_signal_handler_find (w, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, on_map, NULL)) {
            g_signal_connect_after (w, "map", G_CALLBACK (on_map), NULL);
            if (gtk_widget_get_mapped (w))
                on_map (w, NULL);
        }
        g_clear_object (&w);
    }
}

void walpe_wl_app_id_follow (const char *app_id)
{
    GdkDisplay *d = gdk_display_get_default ();
    if (the_id || !app_id || !d || !GDK_IS_WAYLAND_DISPLAY (d))
        return;
    the_id = app_id;
    GListModel *tl = gtk_window_get_toplevels ();
    g_signal_connect (tl, "items-changed", G_CALLBACK (on_toplevels), NULL);
    on_toplevels (tl, 0, 0, g_list_model_get_n_items (tl), NULL);
}

#else

void walpe_wl_app_id_follow (const char *app_id)
{
    (void) app_id;
}

#endif
