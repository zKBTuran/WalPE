/* xembed.c - classic freedesktop system tray icon (XEmbed)
 *
 * System Tray Protocol 0.3 (_NET_SYSTEM_TRAY_S<screen>), as served by
 * dwm's systray patch, stalonetray, trayer, xfce4-panel, polybar, ...
 *   - dock: _NET_SYSTEM_TRAY_OPCODE SYSTEM_TRAY_REQUEST_DOCK to the
 *     selection owner, _XEMBED_INFO { version 0, XEMBED_MAPPED }
 *   - _NET_SYSTEM_TRAY_VISUAL: a 32-bit visual -> real ARGB icon window
 *     (compositing trays); else the default visual with a ParentRelative
 *     background (or the tray's background pixel, dwm) and the icon
 *     alpha-blended over it with XRender
 *   - _NET_SYSTEM_TRAY_ORIENTATION: which side the tray constrains
 *   - the tray's size for us (ConfigureNotify) -> icon re-drawn at that
 *     size (vector icon, sharp at any bar height)
 *   - tray restarts (dwm restart, stalonetray killed): the manager's
 *     DestroyNotify / our window reparented back to root -> wait for the
 *     MANAGER client message on the root window and dock again
 * Uses its own Xlib connection (independent of GTK's), driven by a GSource.
 */
#include <glib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrender.h>
#include "tray.h"

#define SYSTEM_TRAY_REQUEST_DOCK 0
#define XEMBED_MAPPED (1 << 0)
/* cairo ARGB32 = native-endian 32-bit words; Xlib swaps for the server */
#define NATIVE_ORDER (G_BYTE_ORDER == G_LITTLE_ENDIAN ? LSBFirst : MSBFirst)

static struct {
    Display *dpy;
    int screen;
    Window root;
    Atom a_sel, a_opcode, a_manager, a_info, a_visual, a_orient, a_wm_name, a_utf8, a_pid;
    Window manager;
    Window icon;
    gboolean argb;
    Visual *visual;
    int depth;
    Colormap cmap;
    gboolean have_render;
    int w, h;
    int orientation;       /* 0 horizontal, 1 vertical */
    gboolean docked;
    GSource *src;
    guint serial_seen;
    TrayIconState drawn_state;
    int drawn_w, drawn_h;
    gboolean pressed1, pressed2, pressed3;
    guint hover_timer;
    gboolean hovering;
    guint redock_timer;
} X;

static int (*old_handler) (Display *, XErrorEvent *);

/* windows of a dying tray vanish under us: BadWindow & co. are expected */
static int on_xerror (Display *d, XErrorEvent *e)
{
    if (d == X.dpy) {
        char msg[128];
        XGetErrorText (d, e->error_code, msg, sizeof msg);
        g_debug ("xembed: X error %s (request %d) ignored", msg, e->request_code);
        return 0;
    }
    return old_handler ? old_handler (d, e) : 0;
}

/* ------------------------------------------------------------ drawing */

static void draw (void)
{
    if (!X.icon || X.w <= 0 || X.h <= 0)
        return;
    TrayIconState st = tray_icon_state ();
    cairo_surface_t *s = tray_icon_render_in (X.w, X.h, st);
    unsigned char *px = cairo_image_surface_get_data (s);
    int stride = cairo_image_surface_get_stride (s);
    if (X.argb) {
        /* 32-bit visual: the premultiplied ARGB pixels are the window
         * contents (the compositor blends them) */
        XImage *im = XCreateImage (X.dpy, X.visual, 32, ZPixmap, 0, (char *) px, X.w, X.h, 32, stride);
        im->byte_order = NATIVE_ORDER;
        GC gc = XCreateGC (X.dpy, X.icon, 0, NULL);
        XPutImage (X.dpy, X.icon, gc, im, 0, 0, 0, 0, X.w, X.h);
        XFreeGC (X.dpy, gc);
        im->data = NULL;
        XDestroyImage (im);
    } else if (X.have_render) {
        /* default visual: tray background first (ParentRelative or the
         * tray's own background pixel), then the icon OVER it */
        XClearWindow (X.dpy, X.icon);
        Pixmap pm = XCreatePixmap (X.dpy, X.icon, X.w, X.h, 32);
        XImage *im = XCreateImage (X.dpy, NULL, 32, ZPixmap, 0, (char *) px, X.w, X.h, 32, stride);
        im->byte_order = NATIVE_ORDER;
        GC gc = XCreateGC (X.dpy, pm, 0, NULL);
        XPutImage (X.dpy, pm, gc, im, 0, 0, 0, 0, X.w, X.h);
        XFreeGC (X.dpy, gc);
        im->data = NULL;
        XDestroyImage (im);
        XRenderPictFormat *sf = XRenderFindStandardFormat (X.dpy, PictStandardARGB32);
        XRenderPictFormat *df = XRenderFindVisualFormat (X.dpy, X.visual);
        Picture sp = XRenderCreatePicture (X.dpy, pm, sf, 0, NULL);
        Picture dp = XRenderCreatePicture (X.dpy, X.icon, df, 0, NULL);
        XRenderComposite (X.dpy, PictOpOver, sp, None, dp, 0, 0, 0, 0, 0, 0, X.w, X.h);
        XRenderFreePicture (X.dpy, sp);
        XRenderFreePicture (X.dpy, dp);
        XFreePixmap (X.dpy, pm);
    } else {
        /* no XRender: blend over a dark bar colour by hand */
        guint32 *p = (guint32 *) px;
        for (int y = 0; y < X.h; y++)
            for (int x = 0; x < X.w; x++) {
                guint32 v = p[y * (stride / 4) + x], a = v >> 24;
                guint32 bg = 0x22 * (255 - a) / 255;
                guint32 r = ((v >> 16) & 255) + bg, g = ((v >> 8) & 255) + bg, b = (v & 255) + bg;
                p[y * (stride / 4) + x] = (r << 16) | (g << 8) | b;
            }
        XImage *im = XCreateImage (X.dpy, X.visual, X.depth, ZPixmap, 0, (char *) px, X.w, X.h, 32, stride);
        im->byte_order = NATIVE_ORDER;
        GC gc = XCreateGC (X.dpy, X.icon, 0, NULL);
        XPutImage (X.dpy, X.icon, gc, im, 0, 0, 0, 0, X.w, X.h);
        XFreeGC (X.dpy, gc);
        im->data = NULL;
        XDestroyImage (im);
    }
    cairo_surface_destroy (s);
    X.drawn_state = st;
    X.drawn_w = X.w;
    X.drawn_h = X.h;
    XFlush (X.dpy);
}

/* ------------------------------------------------------------ docking */

static gboolean read_card (Window w, Atom prop, Atom type, long *out)
{
    Atom at;
    int fmt;
    unsigned long n = 0, after;
    unsigned char *data = NULL;
    gboolean ok = FALSE;
    if (XGetWindowProperty (X.dpy, w, prop, 0, 1, False, type, &at, &fmt, &n, &after, &data) == Success &&
        data && n == 1 && fmt == 32) {
        *out = *(long *) data;
        ok = TRUE;
    }
    if (data)
        XFree (data);
    return ok;
}

static void destroy_icon (void)
{
    if (X.icon) {
        XDestroyWindow (X.dpy, X.icon);
        X.icon = None;
    }
    if (X.cmap) {
        XFreeColormap (X.dpy, X.cmap);
        X.cmap = None;
    }
    X.docked = FALSE;
    tray_tooltip_hide ();
}

static void create_icon (void)
{
    /* the tray's visual choice */
    long vid = 0;
    X.argb = FALSE;
    X.visual = DefaultVisual (X.dpy, X.screen);
    X.depth = DefaultDepth (X.dpy, X.screen);
    if (read_card (X.manager, X.a_visual, XA_VISUALID, &vid) && vid) {
        XVisualInfo tmpl = { .visualid = (VisualID) vid, .screen = X.screen };
        int n = 0;
        XVisualInfo *vi = XGetVisualInfo (X.dpy, VisualIDMask | VisualScreenMask, &tmpl, &n);
        if (vi && n > 0) {
            X.visual = vi[0].visual;
            X.depth = vi[0].depth;
            X.argb = vi[0].depth == 32;
        }
        if (vi)
            XFree (vi);
    }
    long orient = 0;
    X.orientation = read_card (X.manager, X.a_orient, XA_CARDINAL, &orient) ? (int) orient : 0;

    XSetWindowAttributes a = { 0 };
    unsigned long mask = CWEventMask;
    a.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | StructureNotifyMask |
                   EnterWindowMask | LeaveWindowMask | PropertyChangeMask;
    if (X.argb) {
        X.cmap = XCreateColormap (X.dpy, X.root, X.visual, AllocNone);
        a.colormap = X.cmap;
        a.background_pixel = 0;
        a.border_pixel = 0;
        mask |= CWColormap | CWBackPixel | CWBorderPixel;
    } else {
        a.background_pixmap = ParentRelative;
        mask |= CWBackPixmap;
    }
    /* a first size; the tray sets the real one */
    X.w = X.h = 22;
    X.icon = XCreateWindow (X.dpy, X.root, -100, -100, X.w, X.h, 0, X.depth, InputOutput, X.visual,
                            mask, &a);
    XClassHint ch = { (char *) "walpe-tray", (char *) "WalPE" };
    XSetClassHint (X.dpy, X.icon, &ch);
    XStoreName (X.dpy, X.icon, "WalPE");
    const char *nm = "WalPE";
    XChangeProperty (X.dpy, X.icon, X.a_wm_name, X.a_utf8, 8, PropModeReplace,
                     (const unsigned char *) nm, strlen (nm));
    long pid = getpid ();
    XChangeProperty (X.dpy, X.icon, X.a_pid, XA_CARDINAL, 32, PropModeReplace, (unsigned char *) &pid, 1);
    long info[2] = { 0, XEMBED_MAPPED };
    XChangeProperty (X.dpy, X.icon, X.a_info, X.a_info, 32, PropModeReplace, (unsigned char *) info, 2);
    XSizeHints *sh = XAllocSizeHints ();
    sh->flags = PMinSize | PBaseSize;
    sh->min_width = sh->min_height = 8;
    sh->base_width = sh->base_height = X.w;
    XSetWMNormalHints (X.dpy, X.icon, sh);
    XFree (sh);
    g_message ("xembed: tray 0x%lx, %s visual (depth %d), %s", X.manager,
               X.argb ? "ARGB" : "default", X.depth, X.orientation ? "vertical" : "horizontal");
}

static void dock (void)
{
    XGrabServer (X.dpy);
    X.manager = XGetSelectionOwner (X.dpy, X.a_sel);
    if (X.manager)
        XSelectInput (X.dpy, X.manager, StructureNotifyMask | PropertyChangeMask);
    XUngrabServer (X.dpy);
    XFlush (X.dpy);
    if (!X.manager) {
        g_message ("xembed: no system tray yet; waiting for one");
        return;
    }
    destroy_icon ();
    create_icon ();
    XEvent e = { 0 };
    e.xclient.type = ClientMessage;
    e.xclient.window = X.manager;
    e.xclient.message_type = X.a_opcode;
    e.xclient.format = 32;
    e.xclient.data.l[0] = CurrentTime;
    e.xclient.data.l[1] = SYSTEM_TRAY_REQUEST_DOCK;
    e.xclient.data.l[2] = X.icon;
    XSendEvent (X.dpy, X.manager, False, NoEventMask, &e);
    XSync (X.dpy, False);
}

static gboolean redock_cb (gpointer u)
{
    (void) u;
    X.redock_timer = 0;
    dock ();
    return G_SOURCE_REMOVE;
}

static void manager_lost (const char *why)
{
    g_message ("xembed: tray gone (%s); waiting for it to come back", why);
    X.manager = None;
    destroy_icon ();
    /* a tray that restarts in place may already own the selection again */
    if (!X.redock_timer)
        X.redock_timer = g_timeout_add (300, redock_cb, NULL);
}

/* ------------------------------------------------------------ events */

static gboolean hover_cb (gpointer u)
{
    (void) u;
    X.hover_timer = 0;
    if (!X.hovering || !X.icon || tray_popup_visible ())
        return G_SOURCE_REMOVE;
    int rx, ry;
    Window child;
    XTranslateCoordinates (X.dpy, X.icon, X.root, 0, 0, &rx, &ry, &child);
    tray_tooltip_show (rx, ry, X.w, X.h);
    return G_SOURCE_REMOVE;
}

static void handle (XEvent *e)
{
    switch (e->type) {
    case ClientMessage:
        if (e->xclient.window == X.root && e->xclient.message_type == X.a_manager &&
            (Atom) e->xclient.data.l[1] == X.a_sel) {
            g_message ("xembed: a system tray appeared (MANAGER)");
            if (X.redock_timer) {
                g_source_remove (X.redock_timer);
                X.redock_timer = 0;
            }
            dock ();
        }
        break;
    case DestroyNotify:
        if (e->xdestroywindow.window == X.manager && X.manager)
            manager_lost ("tray window destroyed");
        else if (e->xdestroywindow.window == X.icon) {
            X.icon = None; /* destroyed along with the tray */
            X.docked = FALSE;
        }
        break;
    case ReparentNotify:
        if (e->xreparent.window == X.icon) {
            if (e->xreparent.parent == X.root) {
                /* the tray let go of us (shutting down, save-set) */
                if (X.docked)
                    manager_lost ("icon returned to the root window");
            } else {
                X.docked = TRUE;
                g_message ("xembed: docked");
            }
        }
        break;
    case ConfigureNotify:
        if (e->xconfigure.window == X.icon) {
            int w = e->xconfigure.width, h = e->xconfigure.height;
            if (w != X.w || h != X.h) {
                X.w = w;
                X.h = h;
                draw ();
            }
        }
        break;
    case Expose:
        if (e->xexpose.window == X.icon && e->xexpose.count == 0)
            draw ();
        break;
    case MapNotify:
        if (e->xmap.window == X.icon)
            draw ();
        break;
    case EnterNotify:
        if (e->xcrossing.window == X.icon) {
            X.hovering = TRUE;
            if (!X.hover_timer)
                X.hover_timer = g_timeout_add (650, hover_cb, NULL);
        }
        break;
    case LeaveNotify:
        if (e->xcrossing.window == X.icon) {
            X.hovering = FALSE;
            if (X.hover_timer) {
                g_source_remove (X.hover_timer);
                X.hover_timer = 0;
            }
            tray_tooltip_hide ();
        }
        break;
    case ButtonPress:
        if (e->xbutton.window == X.icon) {
            tray_tooltip_hide ();
            if (e->xbutton.button == 1) X.pressed1 = TRUE;
            if (e->xbutton.button == 2) X.pressed2 = TRUE;
            if (e->xbutton.button == 3) X.pressed3 = TRUE;
        }
        break;
    case ButtonRelease:
        /* act on release: the implicit grab of the press is over, so the
         * menu can take its own pointer grab */
        if (e->xbutton.window == X.icon) {
            gboolean inside = e->xbutton.x >= 0 && e->xbutton.y >= 0 && e->xbutton.x < X.w &&
                              e->xbutton.y < X.h;
            if (e->xbutton.button == 1 && X.pressed1 && inside) {
                tray_popup_hide ();
                tray_open_gui ();
            } else if (e->xbutton.button == 2 && X.pressed2 && inside) {
                tray_dispatch ("pause-toggle", NULL);
            } else if (e->xbutton.button == 3 && X.pressed3 && inside) {
                tray_popup_show (e->xbutton.x_root, e->xbutton.y_root);
            }
            if (e->xbutton.button == 1) X.pressed1 = FALSE;
            if (e->xbutton.button == 2) X.pressed2 = FALSE;
            if (e->xbutton.button == 3) X.pressed3 = FALSE;
        }
        break;
    case PropertyNotify:
        /* a tray changing its visual: dock again with the new one */
        if (e->xproperty.window == X.manager && e->xproperty.atom == X.a_visual) {
            g_message ("xembed: tray visual changed; re-docking");
            dock ();
        }
        break;
    default:
        break;
    }
}

/* GSource: the X fd plus events Xlib already queued during round trips */
typedef struct { GSource base; gpointer tag; } XSrc;

static gboolean xs_prepare (GSource *s, gint *timeout)
{
    (void) s;
    *timeout = -1;
    return X.dpy && XEventsQueued (X.dpy, QueuedAlready) > 0;
}

static gboolean xs_check (GSource *s)
{
    XSrc *xs = (XSrc *) s;
    return X.dpy && ((g_source_query_unix_fd (s, xs->tag) & G_IO_IN) ||
                     XEventsQueued (X.dpy, QueuedAlready) > 0);
}

static gboolean xs_dispatch (GSource *s, GSourceFunc cb, gpointer u)
{
    (void) s; (void) cb; (void) u;
    while (X.dpy && XPending (X.dpy)) {
        XEvent e;
        XNextEvent (X.dpy, &e);
        handle (&e);
    }
    return G_SOURCE_CONTINUE;
}

static GSourceFuncs xs_funcs = { xs_prepare, xs_check, xs_dispatch, NULL, NULL, NULL };

/* ------------------------------------------------------------ backend */

static gboolean xe_start (void)
{
    if (X.dpy)
        return TRUE;
    X.dpy = XOpenDisplay (NULL);
    if (!X.dpy) {
        g_message ("xembed: cannot open the X display");
        return FALSE;
    }
    old_handler = XSetErrorHandler (on_xerror);
    X.screen = DefaultScreen (X.dpy);
    X.root = RootWindow (X.dpy, X.screen);
    char sel[64];
    g_snprintf (sel, sizeof sel, "_NET_SYSTEM_TRAY_S%d", X.screen);
    X.a_sel = XInternAtom (X.dpy, sel, False);
    X.a_opcode = XInternAtom (X.dpy, "_NET_SYSTEM_TRAY_OPCODE", False);
    X.a_manager = XInternAtom (X.dpy, "MANAGER", False);
    X.a_info = XInternAtom (X.dpy, "_XEMBED_INFO", False);
    X.a_visual = XInternAtom (X.dpy, "_NET_SYSTEM_TRAY_VISUAL", False);
    X.a_orient = XInternAtom (X.dpy, "_NET_SYSTEM_TRAY_ORIENTATION", False);
    X.a_wm_name = XInternAtom (X.dpy, "_NET_WM_NAME", False);
    X.a_utf8 = XInternAtom (X.dpy, "UTF8_STRING", False);
    X.a_pid = XInternAtom (X.dpy, "_NET_WM_PID", False);
    int ev, er;
    X.have_render = XRenderQueryExtension (X.dpy, &ev, &er);
    /* MANAGER broadcasts arrive on the root window */
    XSelectInput (X.dpy, X.root, StructureNotifyMask);
    XSrc *s = (XSrc *) g_source_new (&xs_funcs, sizeof (XSrc));
    s->tag = g_source_add_unix_fd ((GSource *) s, ConnectionNumber (X.dpy), G_IO_IN);
    g_source_set_name ((GSource *) s, "walpe-tray xembed");
    g_source_attach ((GSource *) s, NULL);
    X.src = (GSource *) s;
    dock ();
    return TRUE;
}

static void xe_stop (void)
{
    if (!X.dpy)
        return;
    if (X.hover_timer)
        g_source_remove (X.hover_timer);
    if (X.redock_timer)
        g_source_remove (X.redock_timer);
    X.hover_timer = X.redock_timer = 0;
    destroy_icon ();
    XSync (X.dpy, False);
    g_source_destroy (X.src);
    g_source_unref (X.src);
    X.src = NULL;
    XCloseDisplay (X.dpy);
    X.dpy = NULL;
    X.manager = None;
}

static void xe_update (void)
{
    if (!X.dpy || !X.icon)
        return;
    if (X.drawn_state != tray_icon_state () || X.drawn_w != X.w || X.drawn_h != X.h)
        draw ();
}

const TrayBackend tray_backend_xembed = { "XEmbed", xe_start, xe_stop, xe_update };
