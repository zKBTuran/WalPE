/* winstate.h - is a window on screen at all?
 *
 * GTK keeps the widgets of a window "mapped" when a window manager takes
 * the window off the display itself (another workspace, an icon) or a
 * compositor says it cannot be seen. What only matters while somebody can
 * look at the window - previews that play, numbers that are asked for,
 * pictures kept in memory for a quick return - asks here. One rule for all
 * of them (thumbs.c: the previews; perf.c: the quality panel's timer).
 *
 * Not on screen = the toplevel is minimised or suspended (GTK 4.12: the
 * compositor's "cannot be seen"), or, on X11, its window manager unmapped
 * the window or moved it wholly off the screen without saying "minimised":
 * what i3, bspwm, awesome and dwm do with the windows of a workspace or
 * tag that is not shown (walpe_winstate_watch, winstate.c). A window that
 * is on screen WITHOUT the focus - the usual state in a tiled layout - is
 * on screen. */
#pragma once
#include <gtk/gtk.h>

static inline gboolean walpe_toplevel_state_hidden (GdkToplevelState st)
{
    GdkToplevelState hidden = GDK_TOPLEVEL_STATE_MINIMIZED;
#if GTK_CHECK_VERSION (4, 12, 0)
    hidden |= GDK_TOPLEVEL_STATE_SUSPENDED;
#endif
    return (st & hidden) != 0;
}

/* X11, a window manager that hides without unmapping the window itself:
 * the value of the window's WM_STATE property (ICCCM: 0 withdrawn, 1
 * normal, 3 iconic). i3 unmaps the frame it put around the window and
 * says "withdrawn" here. */
static inline gboolean walpe_wm_state_gone (long wm_state)
{
    return wm_state == 0 || wm_state == 3;
}

/* X11: the marks winstate.c keeps on a toplevel that its window manager
 * hid without saying "minimised": the window itself unmapped, parked
 * outside the screen, WM_STATE withdrawn / iconic. Each is taken away by
 * the event that undoes it - and WM_STATE's also by a newer word that is
 * no PropertyNotify:
 *  - the window itself is mapped (MapNotify): whoever mapped it wants it
 *    shown, whatever the property still says;
 *  - the window is put back under the root (ReparentNotify): the frame
 *    WM_STATE spoke of is gone.
 * Both happen when a window manager ends while the window is on a
 * workspace it does not show: the X server reparents the window to the
 * root and maps it, it is on screen, and nobody writes WM_STATE again.
 * Without them it counted as hidden for good: no numbers, no previews. A
 * window manager that maps a window without writing WM_STATE normal is
 * the other case. */
#define WALPE_WM_UNMAPPED  1u
#define WALPE_WM_OFFSCREEN 2u
#define WALPE_WM_WITHDRAWN 4u

static inline guint walpe_wm_marks_mapped (guint marks)
{
    return marks & ~(WALPE_WM_UNMAPPED | WALPE_WM_WITHDRAWN);
}

static inline guint walpe_wm_marks_unframed (guint marks)
{
    return marks & ~WALPE_WM_WITHDRAWN;
}

/* X11: the window lies wholly outside the screen (sw x sh), where a window
 * manager that never unmaps (dwm) parks the windows of tags not shown. A
 * position inside a frame - what a reparenting window manager's own events
 * carry - is never that far out and reads as on screen: the safe side. */
static inline gboolean walpe_wm_offscreen (int x, int y, int w, int h, int sw, int sh)
{
    return x + w <= 0 || y + h <= 0 || x >= sw || y >= sh;
}

/* set on a toplevel by walpe_winstate_watch: hidden by the window manager */
#define WALPE_WM_HIDDEN_KEY "walpe-wm-hidden"

/* NULL (no window yet): not hidden */
static inline gboolean walpe_surface_hidden (GdkSurface *sf)
{
    if (!sf || !GDK_IS_TOPLEVEL (sf))
        return FALSE;
    return g_object_get_data (G_OBJECT (sf), WALPE_WM_HIDDEN_KEY) != NULL ||
           walpe_toplevel_state_hidden (gdk_toplevel_get_state (GDK_TOPLEVEL (sf)));
}

/* X11: follow what the window manager does to this display's toplevels and
 * announce a change as a change of the toplevel's "state" (notify::state),
 * so one handler serves every case. No timer, no request: the events
 * arrive anyway. Once per display; nothing to do on Wayland, whose
 * compositor says "suspended". */
void walpe_winstate_watch (GdkDisplay *d);

/* the toplevel surface a widget is shown on, NULL while it has none */
static inline GdkSurface *walpe_widget_toplevel_surface (GtkWidget *w)
{
    GtkRoot *root = w ? gtk_widget_get_root (w) : NULL;
    return root ? gtk_native_get_surface (GTK_NATIVE (root)) : NULL;
}
