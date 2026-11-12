/* winstate.c - what a tiling window manager does to a window it hides
 * (winstate.h: is a window on screen at all?) */
#include "winstate.h"

#ifdef WALPE_UI_X11
#include <gdk/x11/gdkx.h>
#include <X11/Xlib.h>

/* three ways to hide a window that GDK does not call "minimised"
 * (the marks of winstate.h) */
#define WM_UNMAPPED  WALPE_WM_UNMAPPED      /* the window itself is unmapped (bspwm, awesome, xmonad) */
#define WM_OFFSCREEN WALPE_WM_OFFSCREEN     /* parked outside the screen (dwm) */
#define WM_WITHDRAWN WALPE_WM_WITHDRAWN     /* its frame is unmapped: WM_STATE says so (i3) */

static void wm_hidden_store (GdkSurface *sf, guint was, guint now)
{
    if (now == was)
        return;
    g_object_set_data (G_OBJECT (sf), WALPE_WM_HIDDEN_KEY, GUINT_TO_POINTER (now));
    /* who follows the toplevel's state looks again (walpe_surface_hidden) */
    if (!was != !now)
        g_object_notify (G_OBJECT (sf), "state");
}

static guint wm_hidden_get (GdkSurface *sf)
{
    return GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (sf), WALPE_WM_HIDDEN_KEY));
}

static void wm_hidden_set (GdkSurface *sf, guint bit, gboolean on)
{
    guint was = wm_hidden_get (sf);
    wm_hidden_store (sf, was, on ? was | bit : was & ~bit);
}

static gboolean on_xevent (GdkDisplay *d, gpointer xevent, gpointer u)
{
    (void) u;
    const XEvent *e = xevent;
    GdkSurface *sf;
    switch (e->type) {
    case UnmapNotify:
    case MapNotify:
        /* same layout up to .window; not the copies a parent gets */
        if (e->xmap.event != e->xmap.window)
            break;
        sf = gdk_x11_surface_lookup_for_display (d, e->xmap.window);
        if (!sf || !GDK_IS_TOPLEVEL (sf))
            break;
        if (e->type == UnmapNotify)
            wm_hidden_set (sf, WM_UNMAPPED, TRUE);
        else    /* mapped: also the newer word than what WM_STATE said (winstate.h) */
            wm_hidden_store (sf, wm_hidden_get (sf), walpe_wm_marks_mapped (wm_hidden_get (sf)));
        break;
    case ReparentNotify:
        /* back under the root: the frame WM_STATE spoke of is gone */
        if (e->xreparent.event != e->xreparent.window ||
            e->xreparent.parent != DefaultRootWindow (e->xreparent.display))
            break;
        sf = gdk_x11_surface_lookup_for_display (d, e->xreparent.window);
        if (sf && GDK_IS_TOPLEVEL (sf))
            wm_hidden_store (sf, wm_hidden_get (sf), walpe_wm_marks_unframed (wm_hidden_get (sf)));
        break;
    case ConfigureNotify: {
        const XConfigureEvent *c = &e->xconfigure;
        if (c->event != c->window)
            break;
        sf = gdk_x11_surface_lookup_for_display (d, c->window);
        if (!sf || !GDK_IS_TOPLEVEL (sf))
            break;
        Screen *s = DefaultScreenOfDisplay (c->display);
        gboolean off = walpe_wm_offscreen (c->x, c->y, c->width, c->height,
                                           WidthOfScreen (s), HeightOfScreen (s));
        wm_hidden_set (sf, WM_OFFSCREEN, off);
        break;
    }
    case PropertyNotify: {
        const XPropertyEvent *pe = &e->xproperty;
        Atom wm_state = gdk_x11_get_xatom_by_name_for_display (d, "WM_STATE");
        if (pe->atom != wm_state)
            break;
        sf = gdk_x11_surface_lookup_for_display (d, pe->window);
        if (!sf || !GDK_IS_TOPLEVEL (sf))
            break;
        /* one round trip per change of the property, none otherwise */
        gboolean gone = FALSE;
        if (pe->state == PropertyNewValue) {
            Atom type = None;
            int fmt = 0;
            unsigned long n = 0, left = 0;
            unsigned char *data = NULL;
            gdk_x11_display_error_trap_push (d);
            if (XGetWindowProperty (pe->display, pe->window, wm_state, 0, 2, False, wm_state,
                                    &type, &fmt, &n, &left, &data) == Success &&
                type == wm_state && fmt == 32 && n >= 1 && data) {
                gone = walpe_wm_state_gone (*(long *) data);
            }
            gdk_x11_display_error_trap_pop_ignored (d);
            if (data)
                XFree (data);
        }
        wm_hidden_set (sf, WM_WITHDRAWN, gone);
        break;
    }
    default:
        break;
    }
    return FALSE;   /* GDK handles the event as always */
}
#endif

void walpe_winstate_watch (GdkDisplay *d)
{
#ifdef WALPE_UI_X11
    if (!d || !GDK_IS_X11_DISPLAY (d) || g_object_get_data (G_OBJECT (d), "walpe-winstate"))
        return;
    g_object_set_data (G_OBJECT (d), "walpe-winstate", GINT_TO_POINTER (1));
    g_signal_connect (d, "xevent", G_CALLBACK (on_xevent), NULL);
#else
    (void) d;
#endif
}
