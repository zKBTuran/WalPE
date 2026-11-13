/* x11.c - the X11 backend (platform/impl.h).
 *
 * One override-redirect window per monitor at the bottom of the stacking
 * order, input shaped away; each window is the EGL surface its monitor's
 * render thread draws on. On a desktop with a desktop window of its own,
 * and under a window manager that paints such a window above everything,
 * the window is a child of that desktop window or a managed one of the
 * desktop type instead (x11desk.h says when, docs/DESKTOPS.md what was
 * run). Around that: the monitor layout (Xinerama,
 * RandR names and refresh rates), what makes a wallpaper pause (EWMH
 * properties for fullscreen / covered / focus, DPMS and the screensaver
 * extension for a sleeping display), and the pointer (XQueryPointer,
 * XInput2 raw events). Two connections: X.xd for the main thread (events,
 * queries, the windows) and X.egl_xd for EGL and the render threads.
 *
 * Main thread only, except x11_surface_swapped (render threads; it only
 * looks at the EGL connection's event queue). */
#include "walpe-build-config.h"
#include <glib.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h> /* XGetClassHint */
#include <X11/extensions/shape.h>
#include <X11/extensions/Xinerama.h>
#include <X11/extensions/dpms.h>
#ifdef HAVE_XRANDR
#include <X11/extensions/Xrandr.h>   /* output names of the monitors */
#endif
#ifdef HAVE_XI2
#include <X11/extensions/XInput2.h>
#endif
#include <epoxy/egl.h>
#include <dlfcn.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "platform/impl.h"
#include "platform/fsplace.h"
#include "platform/x11desk.h"
#include "common/config.h"
#include "common/pace.h"

typedef struct { int x, y, width, height; } XRect;

/* a wallpaper window (or the --windowed window) and its EGL surface */
typedef struct {
    WalpeSurfaceKind kind;
    Window xid;              /* override-redirect window = render target */
    EGLDisplay egl_dpy;
    EGLSurface egl;
    guint32 out_id;          /* WalpeOutput.id: per-output signals are reported under it */
    int index;               /* the monitor's index, for log lines */
    XRect geom;              /* the monitor in root coordinates */
    gpointer user;           /* handed back in surface events */
    gboolean covered;        /* client windows hide this monitor */
    gboolean focus_app;      /* the focused window is an application's, on this monitor */
    gboolean fs_here;        /* the focused fullscreen window is on this monitor */
    gboolean destroyed;      /* another client destroyed the window (reported once) */
    gboolean managed;        /* a window the window manager manages, of the desktop type
                              * (x11desk.h: Mutter and its forks); else override-redirect */
    Window host;             /* the desktop window this window is a child of (x11desk.h);
                              * None: it is a child of the root */
    Window host_want;        /* ... the one it is to be a child of (not host: on its way) */
    int host_x, host_y;      /* where in it */
    guint adopt_id;          /* the timer of that way (desk_adopt_cb) */
} X11Surface;

static struct {
    Display *xd;             /* main thread: events, queries, the windows */
    Display *egl_xd;         /* EGL / the render threads (= xd with WALPE_ONE_XCONN) */
    Window root;
    WalpeConfig *cfg;        /* the daemon's settings (read only) */
    const WalpePlatformEvents *ev;
    gpointer user;
    gboolean windowed;       /* --windowed */
    gboolean probe;          /* a test child: one connection, no events */
    gboolean has_wm;         /* EWMH manager present (at start-up, and when one came or went) */
    GPtrArray *surfs;        /* X11Surface *: the wallpaper windows, in creation order */
    gboolean fs_active;      /* fullscreen window has focus */
    Window fs_win;           /* that window (None while fs_active is off) */
    Window fs_placed;        /* the window the monitors' fs_here was worked out for */
    gboolean fs_everywhere;  /* ... and where it is could not be found out: it
                              * counts for every monitor */
    XRect *layout;           /* every monitor of the screen as x11_outputs last listed
                              * them, also those this daemon does not draw on */
    int n_layout;
    gboolean asleep;         /* DPMS power-save or screensaver active */
    Window watched_win;      /* active window we subscribed to */
    guint cover_timer_id;    /* debounced coverage recompute */
    gboolean stack_dirty;    /* a window may have got below a wallpaper window */
    GHashTable *watched_clients; /* client windows with PropertyChangeMask selected */
    GHashTable *client_props;    /* Window -> ClientProps: what the cover scan keeps of them */
    guint top_probe_wait;        /* scans left until the root's children are listed again */
    Atom cm_atom;            /* _NET_WM_CM_S<screen> (x11_present_log) */
    gboolean sleep_watching; /* sleep_watch_init ran */
    WalpeX11WinMode win_mode;    /* WALPE_X11_WINDOW */
    gboolean managed;        /* the wallpaper windows are managed ones (x11desk.h) */
    char *wm_name;           /* the window manager's _NET_WM_NAME (NULL: none, or it has none) */
    Atom a_wmcheck;          /* _NET_SUPPORTING_WM_CHECK */
    guint wm_timer_id;       /* the window manager came or went: one look at its name */
    WalpeX11DeskMode desk_mode;  /* WALPE_X11_DESKTOP */
    WalpeDeskWin *desks;     /* the desktop windows the last scan of the clients found */
    guint n_desks;
    gboolean desk_tops_stale;    /* a window was reparented since their frames were asked for */
    Atom a_supported;        /* _NET_SUPPORTED (None until a desktop window made it matter) */
    int wm_desk;             /* the window manager lists the desktop type there: 1 / 0;
                              * -1: not asked since it came (wm_knows_desktop) */
    gboolean desk_hidden_said;   /* the line about a wallpaper behind a desktop window */
} X;

#define SURF(i) ((X11Surface *) g_ptr_array_index (X.surfs, (i)))

/* the pause signals this backend reports (defined with the table at the end) */
static const WalpeSignalProvider x11_provider;

/* XInput2 raw events on the root, selected only while something needs
 * them: desktop clicks (cursor scripts, web pages), pointer motion (a
 * wallpaper that follows the pointer, pointer at rest), any input (display
 * asleep by DPMS on a server without DPMS events: the wake-up) */
static gboolean xi_want_buttons, xi_want_motion, xi_want_wake;
static int xi2_opcode = -1;
static void xi2_apply_mask (void);
#ifdef HAVE_XRANDR
static int rr_event_base = -1;
#endif

static void outputs_changed (void)
{
    if (X.ev && X.ev->outputs_changed)
        X.ev->outputs_changed (X.user);
}

/* --------------------------------------------------- display sleep checks */

/* Display sleep is noticed by events where the server has them, not by
 * asking it every second:
 *  - screensaver (blank / lock): MIT-SCREEN-SAVER ScreenSaverNotify;
 *  - DPMS: DPMSInfoNotify (DPMS 1.2: newer servers + libXext >= 1.3.6,
 *    looked up at run time). Older servers have no DPMS event at all:
 *    there the state is asked every 10 s (`xset dpms force off` and the
 *    idle timeout are both noticed within that), and while the display is
 *    off raw input events are selected (XInput2), so the wake-up - which
 *    only input causes - is seen at once instead of at the next poll.
 * WALPE_FAKE_DPMS=<file> (tests; Xvfb has no DPMS): the DPMS state is the
 * file's content ("off..." = asleep), handled like a server without DPMS
 * events. */
static int ss_event_base = -1;      /* ScreenSaverNotify selected: first event code */
static int dpms_opcode = -1;        /* DPMSInfoNotify selected: the extension's opcode */
static gboolean dpms_poll;          /* DPMS without events: slow poll + wake watch */
static guint sleep_poll_id, sleep_poll_secs;
static guint sleep_kick_id;
static int sleep_kick_n;
static const char *fake_dpms;

/* DPMS (libXext, already linked): power-save states standby/suspend/off */
static gboolean dpms_asleep (void)
{
    static int probed = FALSE, ok = FALSE, evb = 0, errb = 0;
    if (fake_dpms) {
        char *s = NULL;
        gboolean off = g_file_get_contents (fake_dpms, &s, NULL, NULL) && s &&
                       g_str_has_prefix (s, "off");
        g_free (s);
        return off;
    }
    if (!probed) {
        probed = TRUE;
        ok = DPMSQueryExtension (X.xd, &evb, &errb);
        if (ok)
            g_message ("DPMS extension present");
    }
    if (!ok)
        return FALSE;
    CARD16 level = DPMSModeOn;
    BOOL enabled = False;
    DPMSInfo (X.xd, &level, &enabled);
    return enabled && level != DPMSModeOn;
}

/* screensaver active = screen locked/blanked. libXss is optional here:
 * dlopened at runtime (no build dep); silently unavailable if missing. */
typedef struct {
    Window window;
    int state, kind;
    unsigned long til_or_since, idle, eventMask;
} XScreenSaverInfo; /* layout-identical to <X11/extensions/scrnsaver.h> */
#define SCREENSAVER_ON 1 /* ScreenSaverOn in scrnsaver.h */
#define SCREENSAVER_NOTIFY_MASK 0x00000001L /* ScreenSaverNotifyMask */

static struct {
    int probed, ok, evb, errb;
    Bool (*qext) (Display *, int *, int *);
    XScreenSaverInfo *(*alloc_info) (void);
    Bool (*query_info) (Display *, Drawable, XScreenSaverInfo *);
    void (*select_input) (Display *, Drawable, unsigned long);
    XScreenSaverInfo *info;
} Xss;

static gboolean screensaver_probe (void)
{
    if (!Xss.probed) {
        Xss.probed = TRUE;
        void *lib = dlopen ("libXss.so.1", RTLD_LAZY);
        if (lib) {
            Xss.qext = (Bool (*)(Display *, int *, int *))
                dlsym (lib, "XScreenSaverQueryExtension");
            Xss.alloc_info = (XScreenSaverInfo * (*) (void))
                dlsym (lib, "XScreenSaverAllocInfo");
            Xss.query_info = (Bool (*) (Display *, Drawable, XScreenSaverInfo *))
                dlsym (lib, "XScreenSaverQueryInfo");
            Xss.select_input = (void (*) (Display *, Drawable, unsigned long))
                dlsym (lib, "XScreenSaverSelectInput");
            Xss.ok = Xss.qext && Xss.alloc_info && Xss.query_info &&
                     Xss.qext (X.xd, &Xss.evb, &Xss.errb);
        }
        if (Xss.ok)
            g_message ("XScreenSaver extension present (libXss dlopened)");
    }
    return Xss.ok;
}

static gboolean screensaver_active (void)
{
    if (!screensaver_probe ())
        return FALSE;
    if (!Xss.info)
        Xss.info = Xss.alloc_info ();
    if (!Xss.info || !Xss.query_info (X.xd, X.root, Xss.info))
        return FALSE;
    return Xss.info->state == SCREENSAVER_ON;
}

/* once at start-up: ask for the events */
static void sleep_watch_init (void)
{
    fake_dpms = g_getenv ("WALPE_FAKE_DPMS");
    if (screensaver_probe () && Xss.select_input) {
        Xss.select_input (X.xd, X.root, SCREENSAVER_NOTIFY_MASK);
        ss_event_base = Xss.evb;
    }
    int evb = 0, errb = 0;
    if (fake_dpms) {
        dpms_poll = TRUE;
    } else if (DPMSQueryExtension (X.xd, &evb, &errb)) {
        int maj = 0, min = 0, op = 0, e1 = 0, e2 = 0;
        void (*sel) (Display *, unsigned long) = NULL;
        if (DPMSGetVersion (X.xd, &maj, &min) && (maj > 1 || (maj == 1 && min >= 2)))
            sel = (void (*) (Display *, unsigned long)) dlsym (RTLD_DEFAULT, "DPMSSelectInput");
        if (sel && XQueryExtension (X.xd, "DPMS", &op, &e1, &e2)) {
            sel (X.xd, 1UL /* DPMSInfoNotifyMask */);
            dpms_opcode = op;
        } else {
            dpms_poll = TRUE;
        }
    }
    g_message ("display sleep: screensaver %s, DPMS %s",
               ss_event_base >= 0 ? "events" : Xss.ok ? "present, no events (not followed)" : "absent",
               dpms_opcode >= 0 ? "events" : dpms_poll ? "polled every 10 s (no DPMS events here)"
                                                       : "absent");
}

static void sleep_arm (void);

static void sleep_recheck (void)
{
    gboolean asleep = dpms_asleep () || screensaver_active ();
    if (asleep != X.asleep) {
        X.asleep = asleep;
        if (asleep) {
            g_message ("display asleep (dpms/screensaver): pausing render + decode");
            malloc_trim (0);
        } else {
            g_message ("display awake: resuming");
        }
        walpe_signal_set (&x11_provider, WALPE_SIG_ASLEEP, 0, asleep);
    }
    sleep_arm ();
}

static gboolean sleep_tick (gpointer user)
{
    (void) user;
    sleep_recheck ();
    return G_SOURCE_CONTINUE;
}

/* the poll (servers without DPMS events only) and the wake watch follow
 * the state */
static void sleep_arm (void)
{
    gboolean poll = dpms_poll && X.cfg->pause_dpms;
    /* asleep without XInput2: nothing announces the wake-up */
    guint secs = !poll ? 0 : (X.asleep && xi2_opcode < 0) ? 2 : 10;
    if (secs != sleep_poll_secs) {
        if (sleep_poll_id)
            g_source_remove (sleep_poll_id);
        sleep_poll_secs = secs;
        sleep_poll_id = secs ? g_timeout_add_seconds (secs, sleep_tick, NULL) : 0;
    }
    xi_want_wake = poll && X.asleep && !sleep_kick_id;
    xi2_apply_mask ();
}

static gboolean sleep_kick_cb (gpointer user)
{
    (void) user;
    sleep_kick_id = 0;
    /* the server may report the display on a moment after the input that
     * woke it: look again, then wait for the next input */
    if (X.asleep && ++sleep_kick_n < 3)
        sleep_kick_id = g_timeout_add (1000, sleep_kick_cb, NULL);
    sleep_recheck ();
    return G_SOURCE_REMOVE;
}

/* raw input arrived while the display was off */
static void sleep_wake_kick (void)
{
    if (sleep_kick_id)
        return;
    sleep_kick_n = 0;
    sleep_kick_id = g_timeout_add (150, sleep_kick_cb, NULL);
    sleep_recheck ();   /* also drops the raw input selection (sleep_arm) */
}

/* -------------------------------------------------- covered detection
 * dwm has no MAXIMIZED EWMH atoms and tiled layouts cover a monitor even
 * when the active window is only half of it, so "covered" is decided by
 * GEOMETRY: union of all visible client windows rasterized on a coarse
 * grid per monitor (exact enough, no rectangle-union algebra). Event
 * driven only — recomputed after map/unmap/configure/property changes,
 * debounced. */

#define COVER_GRID 24           /* raster cells per axis per monitor */
#define COVER_THRESHOLD 0.85f   /* covered area fraction that pauses */

static gboolean window_is_wallpaper (Window w)
{
    for (guint i = 0; i < X.surfs->len; i++)
        if (SURF (i)->xid == w)
            return TRUE;
    return FALSE;
}

/* windows that never count as covering: the wallpaper is visible through
 * them (WM_CLASS matched against cfg->transparent_windows, e.g. "st" —
 * suckless terminal's alpha patch shows the wallpaper behind it) */
static gboolean window_is_translucent (Window w)
{
    if (!X.cfg->transparent_windows || !*X.cfg->transparent_windows)
        return FALSE;
    XClassHint ch = { NULL, NULL };
    if (!XGetClassHint (X.xd, w, &ch))
        return FALSE;
    gboolean hit = FALSE;
    char **tokens = g_strsplit (X.cfg->transparent_windows, ",", -1);
    for (guint i = 0; tokens && tokens[i]; i++) {
        const char *t = g_strstrip (tokens[i]);
        if (*t && ((ch.res_name && !g_ascii_strcasecmp (t, ch.res_name)) ||
                   (ch.res_class && !g_ascii_strcasecmp (t, ch.res_class)))) {
            hit = TRUE;
            break;
        }
    }
    g_strfreev (tokens);
    if (ch.res_name)
        XFree (ch.res_name);
    if (ch.res_class)
        XFree (ch.res_class);
    return hit;
}

/* property helper: up to max 32-bit items of type `type` (XFree *out).
 * Every property the daemon reads as an array of longs goes through
 * here: any X client can set e.g. _NET_CLIENT_LIST with format 8, Xlib
 * then hands back nitems BYTES, and indexing that as Window[] read far
 * past the buffer (ASan: heap-buffer-overflow in the cover scan). */
static gulong window_prop32 (Window w, Atom prop, Atom type, gulong max, guchar **out)
{
    Atom rt = None;
    int fmt = 0;
    gulong n = 0, left = 0;
    *out = NULL;
    if (prop == None ||
        XGetWindowProperty (X.xd, w, prop, 0, (long) max, False, type, &rt, &fmt, &n, &left,
                            out) != Success || !*out)
        return 0;
    if (fmt != 32 || rt != type) {
        XFree (*out);
        *out = NULL;
        return 0;
    }
    return MIN (n, max);
}

/* a window that can hide the wallpaper: a normal (or dialog / untyped)
 * client that is not minimized. Docks, desktop icon windows, menus,
 * tooltips, notifications and splash screens never count; neither do
 * iconic (WM_STATE) or _NET_WM_STATE_HIDDEN windows. dwm keeps the windows
 * of other tags mapped but moves them off-screen, so the geometry test
 * already ignores them. */
static Atom a_type, a_normal, a_dialog, a_desktop, a_state, a_hidden, a_wmstate;

static void normal_visible_atoms (void)
{
    if (!a_type) {
        a_type = XInternAtom (X.xd, "_NET_WM_WINDOW_TYPE", False);
        a_normal = XInternAtom (X.xd, "_NET_WM_WINDOW_TYPE_NORMAL", False);
        a_dialog = XInternAtom (X.xd, "_NET_WM_WINDOW_TYPE_DIALOG", False);
        a_desktop = XInternAtom (X.xd, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
        a_state = XInternAtom (X.xd, "_NET_WM_STATE", False);
        a_hidden = XInternAtom (X.xd, "_NET_WM_STATE_HIDDEN", False);
        a_wmstate = XInternAtom (X.xd, "WM_STATE", False);
    }
}

/* *desktop: the window's type is _NET_WM_WINDOW_TYPE_DESKTOP (a file
 * manager's desktop, x11desk.h; known from the same read) */
static gboolean window_is_normal_visible (Window w, gboolean *desktop)
{
    normal_visible_atoms ();
    guchar *d = NULL;
    gulong n = window_prop32 (w, a_type, XA_ATOM, 16, &d);
    *desktop = FALSE;
    if (d) {
        gboolean normal = FALSE;
        for (gulong i = 0; i < n; i++) {
            normal |= ((Atom *) d)[i] == a_normal || ((Atom *) d)[i] == a_dialog;
            *desktop |= ((Atom *) d)[i] == a_desktop;
        }
        XFree (d);
        if (n > 0 && !normal)
            return FALSE;
    }
    n = window_prop32 (w, a_state, XA_ATOM, 32, &d);
    if (d) {
        gboolean hidden = FALSE;
        for (gulong i = 0; i < n; i++)
            hidden |= ((Atom *) d)[i] == a_hidden;
        XFree (d);
        if (hidden)
            return FALSE;
    }
    n = window_prop32 (w, a_wmstate, a_wmstate, 2, &d);
    if (d) {
        gboolean iconic = n >= 1 && ((long *) d)[0] == IconicState;
        XFree (d);
        if (iconic)
            return FALSE;
    }
    return TRUE;
}

/* is a compositing manager running? (_NET_WM_CM_Sn selection owner) */
static gboolean compositor_running (void)
{
    static Atom a_cm;
    if (!a_cm) {
        char name[32];
        g_snprintf (name, sizeof name, "_NET_WM_CM_S%d", DefaultScreen (X.xd));
        a_cm = XInternAtom (X.xd, name, False);
    }
    return XGetSelectionOwner (X.xd, a_cm) != None;
}

/* see-through: listed in transparent_windows, a compositor opacity below
 * 100 % (_NET_WM_WINDOW_OPACITY, set by transset / some picom set-ups), or
 * - under a compositing manager - a 32-bit (ARGB) window that does not
 * declare itself opaque: toolkits set _NET_WM_OPAQUE_REGION on windows
 * they paint solid, and leave it off (or small) on translucent ones (a
 * terminal with background alpha, a GTK window with a translucent theme).
 * picom's own opacity rules leave no trace on the window: such windows
 * need their WM_CLASS in transparent_windows.
 * depth / w / h: the window's (already asked for by the scan); cm: a
 * compositing manager runs. */
static gboolean window_is_see_through (Window w, int depth, int ww, int wh, gboolean cm)
{
    if (window_is_translucent (w))
        return TRUE;
    static Atom a_op, a_opaque;
    if (!a_op) {
        a_op = XInternAtom (X.xd, "_NET_WM_WINDOW_OPACITY", False);
        a_opaque = XInternAtom (X.xd, "_NET_WM_OPAQUE_REGION", False);
    }
    guchar *d = NULL;
    gulong n = window_prop32 (w, a_op, XA_CARDINAL, 1, &d);
    gboolean see = d && n == 1 && (((unsigned long *) d)[0] & 0xffffffffUL) < 0xffffffffUL;
    if (d)
        XFree (d);
    if (see || !cm || depth != 32 || ww < 1 || wh < 1)
        return see;
    /* opaque part of the window (rectangles x, y, w, h; overlap between
     * them is rare and only errs towards "opaque") */
    n = window_prop32 (w, a_opaque, XA_CARDINAL, 4 * 64, &d);
    double opaque = 0.0;
    for (gulong i = 0; d && i + 3 < n; i += 4) {
        long x = (long) ((unsigned long *) d)[i], y = (long) ((unsigned long *) d)[i + 1];
        long rw = (long) ((unsigned long *) d)[i + 2], rh = (long) ((unsigned long *) d)[i + 3];
        long x1 = CLAMP (x, 0, ww), y1 = CLAMP (y, 0, wh);
        long x2 = CLAMP (x + rw, 0, ww), y2 = CLAMP (y + rh, 0, wh);
        if (x2 > x1 && y2 > y1)
            opaque += (double) (x2 - x1) * (double) (y2 - y1);
    }
    if (d)
        XFree (d);
    return opaque < 0.85 * (double) ww * (double) wh;
}

/* fraction of monitor g hidden by the given window rectangles (root px) */
static float monitor_coverage (const XRect *g, const XRect *wins, guint n)
{
    guchar grid[COVER_GRID * COVER_GRID] = { 0 };
    float cw = (float) g->width / COVER_GRID;
    float ch = (float) g->height / COVER_GRID;
    guint marked = 0;

    for (guint i = 0; i < n && marked < COVER_GRID * COVER_GRID; i++) {
        const XRect *r = &wins[i];
        if (r->x >= g->x + g->width || r->y >= g->y + g->height || r->x + r->width <= g->x ||
            r->y + r->height <= g->y)
            continue;   /* no overlap with this monitor */
        for (int cy = 0; cy < COVER_GRID; cy++)
            for (int cx = 0; cx < COVER_GRID; cx++) {
                if (grid[cy * COVER_GRID + cx])
                    continue;
                float px = g->x + (cx + 0.5f) * cw;
                float py = g->y + (cy + 0.5f) * ch;
                if (px >= r->x && px < r->x + r->width && py >= r->y && py < r->y + r->height) {
                    grid[cy * COVER_GRID + cx] = 1;
                    marked++;
                }
            }
    }
    return (float) marked / (COVER_GRID * COVER_GRID);
}

/* What the cover scan keeps of a client between two scans: the answer of
 * window_is_normal_visible, which is made of three of the window's
 * properties (type, _NET_WM_STATE, WM_STATE). The scan read them anew for
 * every window every time - three round trips per window, for a focus
 * change or for each step of another window being dragged - although the
 * server tells this connection of every change of them: the clients of
 * _NET_CLIENT_LIST have PropertyChangeMask selected (clients_watch).
 * The answer is kept while that holds, exactly:
 *  - `since` is the number of the first request that read the properties.
 *    A PropertyNotify carries the number of the last request the server
 *    had worked off when it made the event: an event numbered below
 *    `since` is older than the read and in its answer, any other one ends
 *    the kept answer (client_props_event).
 *  - a scan begins with a round trip (the client list). When its reply is
 *    there, every event the server made before it is in Xlib's queue; the
 *    scan looks through that queue before it uses a kept answer
 *    (cover_look_ahead), so a kept answer is the server's state at that
 *    moment - what the three reads would have returned then.
 *  - the answer of a window is used only while the window still has this
 *    connection's selection (XGetWindowAttributes, asked for anyway, says
 *    so: your_event_mask). Another window that was given the number of one
 *    that is gone has none: it is selected and read. */
typedef struct {
    gulong since;
    gboolean valid;
    gboolean normal;
    gboolean desktop;       /* its type is _NET_WM_WINDOW_TYPE_DESKTOP */
} ClientProps;

static void client_props_event (const XPropertyEvent *e)
{
    if (!X.client_props || !a_type ||
        (e->atom != a_type && e->atom != a_state && e->atom != a_wmstate))
        return;
    ClientProps *p = g_hash_table_lookup (X.client_props, GSIZE_TO_POINTER (e->window));
    if (p && e->serial >= p->since)
        p->valid = FALSE;
}

/* XCheckIfEvent predicate that takes nothing out of the queue */
static Bool cover_look_ahead (Display *d, XEvent *e, XPointer arg)
{
    (void) d; (void) arg;
    if (e->type == PropertyNotify)
        client_props_event (&e->xproperty);
    return False;
}

/* the kept answers of the clients that are still listed (the others go) */
static void client_props_keep (const Window *w, guint n)
{
    GHashTable *now = g_hash_table_new_full (NULL, NULL, NULL, g_free);
    for (guint i = 0; X.client_props && i < n; i++) {
        gpointer key = GSIZE_TO_POINTER (w[i]), val = NULL;
        if (g_hash_table_steal_extended (X.client_props, key, NULL, &val))
            g_hash_table_insert (now, key, val);
    }
    if (X.client_props)
        g_hash_table_unref (X.client_props);
    X.client_props = now;
}

/* Property changes of the clients (state: hidden / iconic / fullscreen,
 * opacity, type) decide whether they cover: the daemon asks for them on
 * every client of _NET_CLIENT_LIST once, when it first sees it, instead
 * of re-scanning all windows every 2 s "in case" one changed. A window
 * that is gone needs no clean-up (its event selection went with it). */
static void clients_watch (const Window *w, guint n)
{
    GHashTable *now = g_hash_table_new (NULL, NULL);
    for (guint i = 0; i < n; i++) {
        if (w[i] == None || w[i] == X.root || window_is_wallpaper (w[i]))
            continue;
        g_hash_table_add (now, GSIZE_TO_POINTER (w[i]));
        if (!X.watched_clients || !g_hash_table_contains (X.watched_clients, GSIZE_TO_POINTER (w[i])))
            XSelectInput (X.xd, w[i], PropertyChangeMask);
    }
    if (X.watched_clients)
        g_hash_table_unref (X.watched_clients);
    X.watched_clients = now;
}

static gboolean client_is_watched (Window w)
{
    return X.watched_clients && g_hash_table_contains (X.watched_clients, GSIZE_TO_POINTER (w));
}

static void fs_place_report (void);
static void update_active_window (void);

/* ------------------------------------- a desktop's own desktop window
 * (x11desk.h: what it is, and why the wallpaper window goes into an
 * opaque one). The scan of the clients below finds them - it reads every
 * client's type anyway - so a session without one (a bare window manager)
 * pays nothing for this: no request, no event, no timer. */

/* the child of the root w is in: w itself, or the window manager's frame
 * around it (None: the tree could not be asked, the window is going) */
static Window window_top (Window w)
{
    for (int up = 0; up < 16; up++) {
        Window r, parent = None, *kids = NULL;
        guint n = 0;
        if (!XQueryTree (X.xd, w, &r, &parent, &kids, &n))
            return None;
        if (kids)
            XFree (kids);
        if (parent == X.root)
            return w;
        if (parent == None)
            return None;
        w = parent;
    }
    return None;
}

/* WM_CLASS of a window, for a log line ("Xfdesktop"; g_free) */
static char *window_class_name (Window w)
{
    XClassHint ch = { NULL, NULL };
    char *name = NULL;
    if (XGetClassHint (X.xd, w, &ch)) {
        name = g_strdup (ch.res_class && *ch.res_class ? ch.res_class : ch.res_name);
        if (ch.res_name)
            XFree (ch.res_name);
        if (ch.res_class)
            XFree (ch.res_class);
    }
    return name ? name : g_strdup ("?");
}

/* Does the window manager say that it knows the desktop type (x11desk.h:
 * walpe_x11_wm_knows_desktop)? Asked when the first window of that type
 * that lies over a monitor is found, and again after the window manager
 * came, went or changed what it lists - never in a session without such a
 * window: no atom, no request there. */
static gboolean wm_knows_desktop (void)
{
    if (X.wm_desk < 0) {
        if (X.a_supported == None)
            X.a_supported = XInternAtom (X.xd, "_NET_SUPPORTED", False);
        normal_visible_atoms ();
        guchar *d = NULL;
        gulong n = window_prop32 (X.root, X.a_supported, XA_ATOM, 1024, &d);
        X.wm_desk = walpe_x11_wm_knows_desktop ((const unsigned long *) d, n, a_desktop) ? 1 : 0;
        if (d)
            XFree (d);
    }
    return X.wm_desk > 0;
}

/* is this window one of the desktop windows of the last scan, under a
 * window manager to which that type is the desktop? */
static gboolean window_is_desk (Window w)
{
    for (guint k = 0; w != None && k < X.n_desks; k++)
        if ((Window) X.desks[k].client == w)
            return wm_knows_desktop ();
    return FALSE;
}

/* a wallpaper window back to where it was made: the lowest child of the
 * root, mapped, only its Expose events asked for */
static void desk_to_root (X11Surface *s)
{
    XSelectInput (X.xd, s->xid, ExposureMask);
    XReparentWindow (X.xd, s->xid, X.root, s->geom.x, s->geom.y);
    XMapWindow (X.xd, s->xid);
    XLowerWindow (X.xd, s->xid);
    X.stack_dirty = TRUE;
    s->host = None;
}

static void cover_schedule (void);

/* second half of a wallpaper window's way into a desktop window
 * (desk_apply unmapped it a moment ago).
 * The desktop window may be gone by now (a file manager that makes its
 * window twice when it starts, one that was closed in this fifth of a
 * second): the request to become its child fails then, and the window is
 * still where it was. So the server is asked where it is before anything
 * is said or believed - one request, on a desktop with a desktop window
 * only - and the window is mapped without being raised: a window that is
 * made a child of another one is on top of that one's children as it is
 * (the X protocol's ReparentWindow), and one that stayed on the root was
 * raised above every window of the screen, where it lay until the next
 * scan of the windows put it back - a tenth of a second of wallpaper over
 * everything, with a line in the log that said it was in the desktop
 * window. */
static gboolean desk_adopt_cb (gpointer user)
{
    X11Surface *s = user;
    s->adopt_id = 0;
    if (s->destroyed || s->xid == None || s->host_want == None)
        return G_SOURCE_REMOVE;
    const Window host = s->host_want;
    /* its own DestroyNotify: it dies with the window it is a child of */
    XSelectInput (X.xd, s->xid, ExposureMask | StructureNotifyMask);
    XReparentWindow (X.xd, s->xid, host, s->host_x, s->host_y);
    Window r, parent = None, *kids = NULL;
    guint n = 0;
    if (!XQueryTree (X.xd, s->xid, &r, &parent, &kids, &n))
        parent = None;
    if (kids)
        XFree (kids);
    if (parent != host) {
        g_debug ("mon%d: the desktop window 0x%lx went before the wallpaper window was in it: "
                 "the wallpaper window stays the lowest window of the root", s->index,
                 (unsigned long) host);
        desk_to_root (s);
        s->host_want = None;
        XFlush (X.xd);
        cover_schedule ();  /* (what is there now) */
        return G_SOURCE_REMOVE;
    }
    XMapWindow (X.xd, s->xid);
    XFlush (X.xd);
    s->host = host;
    char *cls = window_class_name (host);
    g_message ("mon%d: the desktop's own desktop window 0x%lx (%s) lies over this monitor and "
               "nothing is seen through it: the wallpaper window is a child of it now - above "
               "the desktop's picture and icons, below every other window; clicks go through to "
               "the desktop. WALPE_X11_DESKTOP=root keeps it the lowest window of the root, behind "
               "that window", s->index, (unsigned long) host, cls);
    g_free (cls);
    return G_SOURCE_REMOVE;
}

/* Every wallpaper window is put where x11desk.h says: into the desktop
 * window that lies over its monitor, or back at the bottom of the root.
 * Called after a scan found the desktop windows; changes something only
 * when one appeared, went or moved.
 * The way into a desktop window has two steps, a fifth of a second
 * apart: the window is unmapped, then made a child of the desktop window
 * and mapped. A window manager that has taken note of the window as a
 * child of the root is told by the unmapping and has let go of it by the
 * second step. KWin does not hear of a mapped window that is reparented
 * in one step: it goes on painting it, above everything, as the window it
 * knew (seen with KWin 5.27); and one that manages the window puts it
 * back on the root when it is withdrawn, which must be over first.
 * As a child of another client's window the wallpaper window dies with
 * it (the file manager is closed, or restarted): its own DestroyNotify
 * is asked for while it is one, and the daemon makes its windows anew
 * (surface_closed), in the next desktop window or on the root. */
static void desk_apply (void)
{
    if (X.windowed || X.probe || !X.surfs)
        return;
    gboolean any = X.n_desks > 0;
    for (guint i = 0; i < X.surfs->len && !any; i++)
        any = SURF (i)->host != None || SURF (i)->host_want != None;
    if (!any)
        return;         /* no desktop window, none left behind: a bare window manager */
    const gboolean cm = walpe_x11_desk_see_through (X.wm_name, X.n_desks > 0 && compositor_running ());
    /* (asked only when it decides something: `auto`, and a desktop window is there) */
    const gboolean allowed = walpe_x11_desk_allowed (X.desk_mode, X.desk_mode == WALPE_X11_DESK_AUTO &&
                                                     X.n_desks > 0 && wm_knows_desktop ());
    const WalpeX11DeskMode mode = allowed ? X.desk_mode : WALPE_X11_DESK_ROOT;
    for (guint i = 0; i < X.surfs->len; i++) {
        X11Surface *s = SURF (i);
        if (s->kind != WALPE_SURFACE_WALLPAPER || s->xid == None || s->destroyed)
            continue;
        const WalpeDeskRect mon = { s->geom.x, s->geom.y, s->geom.width, s->geom.height };
        const int k = walpe_x11_desk_host (mode, &mon, X.desks, (int) X.n_desks, cm);
        const Window host = k >= 0 ? (Window) X.desks[k].client : None;
        const int hx = s->geom.x - (k >= 0 ? X.desks[k].r.x : 0);
        const int hy = s->geom.y - (k >= 0 ? X.desks[k].r.y : 0);
        if (host == s->host_want) {
            if (host != None && (hx != s->host_x || hy != s->host_y)) {
                s->host_x = hx;
                s->host_y = hy;
                if (!s->adopt_id)
                    XMoveWindow (X.xd, s->xid, hx, hy);     /* the desktop window was moved */
            }
        } else {
            if (s->adopt_id)
                g_source_remove (s->adopt_id);
            s->adopt_id = 0;
            s->host_want = host;
            s->host_x = hx;
            s->host_y = hy;
            if (host != None) {
                XUnmapWindow (X.xd, s->xid);
                s->adopt_id = g_timeout_add (200, desk_adopt_cb, s);
            } else {
                if (s->host != None)
                    g_message ("mon%d: the desktop window 0x%lx the wallpaper window was a child of "
                               "is not over this monitor any more: the wallpaper window is the "
                               "lowest window of the root again", s->index, (unsigned long) s->host);
                desk_to_root (s);
            }
        }
        /* asked to stay on the root, behind a window nothing is seen
         * through: said once (the wallpaper is drawn and not seen) */
        const int hid = host == None ? walpe_x11_desk_hides (&mon, X.desks, (int) X.n_desks, cm) : -1;
        if (hid >= 0 && !X.desk_hidden_said) {
            X.desk_hidden_said = TRUE;
            char *cls = window_class_name ((Window) X.desks[hid].client);
            if (X.desk_mode == WALPE_X11_DESK_ROOT)
                g_message ("mon%d: the desktop's own desktop window 0x%lx (%s) lies over this monitor "
                           "and nothing is seen through it: the wallpaper is behind it "
                           "(WALPE_X11_DESKTOP=root). Without that variable the wallpaper window is "
                           "put into the desktop window", s->index, X.desks[hid].client, cls);
            else
                g_message ("mon%d: a window of the desktop type, 0x%lx (%s), lies over this monitor "
                           "and nothing is seen through it: the wallpaper is behind it. The window "
                           "manager does not list that type in _NET_SUPPORTED - to it this is a "
                           "window like any other -, so the wallpaper window stays the lowest window "
                           "of the root. WALPE_X11_DESKTOP=window puts it into that window",
                           s->index, X.desks[hid].client, cls);
            g_free (cls);
        }
    }
    XFlush (X.xd);
}

/* the desktop windows of a scan (taken over; NULL / 0 = none) */
static void desk_set (GArray *found)
{
    g_free (X.desks);
    X.n_desks = found ? found->len : 0;
    X.desks = found ? (WalpeDeskWin *) (gpointer) g_array_free (found, FALSE) : NULL;
    X.desk_tops_stale = FALSE;
    desk_apply ();
}

static void recompute_covered (void)
{
    /* client list: dwm maintains _NET_CLIENT_LIST; XQueryTree fallback */
    Window *clients = NULL;
    guint n = 0;
    Atom prop = XInternAtom (X.xd, "_NET_CLIENT_LIST", True);
    guchar *data = NULL;
    if (prop != None) {
        gulong cnt = window_prop32 (X.root, prop, XA_WINDOW, 1024, &data);
        if (data && cnt > 0) {
            clients = (Window *) data;
            n = (guint) cnt;
        }
    }
    clients_watch (clients, n);
    /* listed clients are watched: what was read of them is kept (ClientProps) */
    const gboolean listed = clients != NULL;
    client_props_keep (clients, n);
    if (listed) {
        XEvent ev;
        XCheckIfEvent (X.xd, &ev, cover_look_ahead, NULL);
    }
    Window *tree_clients = NULL;
    if (!clients) {
        Window r2, parent;
        guint tn = 0;
        if (XQueryTree (X.xd, X.root, &r2, &parent, &tree_clients, &tn) && tree_clients) {
            clients = tree_clients;
            n = tn;
        }
    }
    /* A client that is a child of the root (no frame window around it: dwm
     * and the other window managers that do not reparent) has its place on
     * the root in its own geometry, which is asked for anyway: no
     * XTranslateCoordinates round trip for it. Which ones are: the root's
     * children, one request for all of them (the fallback list above is
     * that list). Under a window manager that puts every client into a
     * frame the request finds none and is left out for a while. */
    GHashTable *top = NULL;
    const gboolean all_top = tree_clients != NULL;
    if (listed && n > 0) {
        if (X.top_probe_wait > 0) {
            X.top_probe_wait--;
        } else {
            Window r2, parent, *kids = NULL;
            guint kn = 0;
            if (XQueryTree (X.xd, X.root, &r2, &parent, &kids, &kn)) {
                top = g_hash_table_new (NULL, NULL);
                for (guint k = 0; k < kn; k++)
                    g_hash_table_add (top, GSIZE_TO_POINTER (kids[k]));
                if (kids)
                    XFree (kids);
                guint n_top = 0;
                for (guint i = 0; i < n; i++)
                    n_top += g_hash_table_contains (top, GSIZE_TO_POINTER (clients[i])) ? 1 : 0;
                if (!n_top)
                    X.top_probe_wait = 63;
            }
        }
    }

    /* each client is asked about once per scan (attributes, root position,
     * type / state / opacity), not once per monitor: the windows that can
     * hide a wallpaper, as rectangles in root coordinates */
    XRect *wins = g_new (XRect, n ? n : 1);
    guint nw = 0;
    XRect focus = { 0, 0, 0, 0 };   /* the focused application window */
    gboolean have_focus = FALSE;
    GArray *desks = NULL;           /* WalpeDeskWin: the desktop windows among them */
    gboolean cm = !X.cfg->cover_transparent && compositor_running ();
    for (guint i = 0; i < n; i++) {
        XWindowAttributes wa;
        if (!XGetWindowAttributes (X.xd, clients[i], &wa))
            continue;
        if (wa.map_state != IsViewable || wa.class == InputOnly ||
            wa.override_redirect || window_is_wallpaper (clients[i]))
            continue;
        Window junk0;
        int ox, oy;     /* root coords (attrs x/y are parent-relative) */
        if (all_top || (top && g_hash_table_contains (top, GSIZE_TO_POINTER (clients[i])))) {
            /* the parent is the root: x / y are the outer corner there, the
             * window's own origin lies inside its border */
            ox = wa.x + wa.border_width;
            oy = wa.y + wa.border_width;
        } else if (!XTranslateCoordinates (X.xd, clients[i], X.root, 0, 0, &ox, &oy, &junk0)) {
            continue;
        }
        /* cheap rejection first: on no monitor at all (dwm parks the
         * windows of other tags off-screen) */
        gboolean on_any = FALSE;
        for (guint k = 0; k < X.surfs->len && !on_any; k++) {
            const XRect *g = &SURF (k)->geom;
            on_any = !(ox >= g->x + g->width || oy >= g->y + g->height || ox + wa.width <= g->x ||
                       oy + wa.height <= g->y);
        }
        if (!on_any)
            continue;
        gboolean normal, desktop = FALSE;
        ClientProps *cp = listed ? g_hash_table_lookup (X.client_props, GSIZE_TO_POINTER (clients[i]))
                                 : NULL;
        if (cp && cp->valid && (wa.your_event_mask & PropertyChangeMask)) {
            normal = cp->normal;
            desktop = cp->desktop;
        } else {
            if (listed && !(wa.your_event_mask & PropertyChangeMask))
                XSelectInput (X.xd, clients[i], PropertyChangeMask);   /* (not the window it was) */
            normal_visible_atoms ();
            const gulong since = NextRequest (X.xd);
            normal = window_is_normal_visible (clients[i], &desktop);
            if (listed) {
                if (!cp) {
                    cp = g_new0 (ClientProps, 1);
                    g_hash_table_insert (X.client_props, GSIZE_TO_POINTER (clients[i]), cp);
                }
                cp->since = since;
                cp->valid = TRUE;
                cp->normal = normal;
                cp->desktop = desktop;
            }
        }
        if (desktop) {
            /* a desktop's own desktop window (x11desk.h). The child of the
             * root it is in is asked for once (and again after a window
             * was reparented: a window manager came or went) */
            WalpeDeskWin dw = { clients[i], 0, { ox, oy, wa.width, wa.height }, wa.depth };
            if (all_top || (top && g_hash_table_contains (top, GSIZE_TO_POINTER (clients[i]))))
                dw.top = clients[i];
            for (guint k = 0; !dw.top && !X.desk_tops_stale && k < X.n_desks; k++)
                if (X.desks[k].client == clients[i])
                    dw.top = X.desks[k].top;
            if (!dw.top)
                dw.top = window_top (clients[i]);
            if (!desks)
                desks = g_array_new (FALSE, FALSE, sizeof (WalpeDeskWin));
            g_array_append_val (desks, dw);
        }
        if (!normal)
            continue;
        if (clients[i] == X.watched_win) {
            focus = (XRect) { ox, oy, wa.width, wa.height };
            have_focus = TRUE;
        }
        if (!X.cfg->cover_transparent &&
            window_is_see_through (clients[i], wa.depth, wa.width, wa.height, cm))
            continue;
        wins[nw++] = (XRect) { ox, oy, wa.width, wa.height };
    }

    /* what changed goes to the registry as one batch: the daemon applies
     * the pause state once per scan, when anything is different */
    walpe_signals_begin ();
    for (guint k = 0; k < X.surfs->len; k++) {
        X11Surface *m = SURF (k);
        float cov = nw > 0 ? monitor_coverage (&m->geom, wins, nw) : 0.0f;
        gboolean now = cov >= COVER_THRESHOLD;
        if (now != m->covered) {
            m->covered = now;
            g_message ("mon%d %s by windows (coverage %.2f) -> %s",
                       m->index, now ? "covered" : "exposed", cov,
                       now ? "pausing" : "resuming");
            walpe_signal_set (&x11_provider, WALPE_SIG_COVERED, m->out_id, now);
        }
        /* an application's window on this monitor has the focus (the
         * focus_action rule; the desktop itself focused = none) */
        const XRect *g = &m->geom;
        gboolean fa = have_focus &&
                      !(focus.x >= g->x + g->width || focus.y >= g->y + g->height ||
                        focus.x + focus.width <= g->x || focus.y + focus.height <= g->y);
        if (fa != m->focus_app) {
            m->focus_app = fa;
            if (X.cfg->focus_action != WALPE_FOCUS_RUN)
                g_debug ("mon%d: %s", m->index, fa ? "an application has the focus"
                                                   : "no application focused");
            walpe_signal_set (&x11_provider, WALPE_SIG_FOCUS_APP, m->out_id, fa);
        }
    }
    g_free (wins);
    if (top)
        g_hash_table_unref (top);
    if (data)
        XFree (data);
    if (tree_clients)
        XFree (tree_clients);
    desk_set (desks);   /* the wallpaper windows follow the desktop windows found */
    /* The window that has the focus and says it is fullscreen is a desktop
     * window, which is known only now: the daemon started while it had the
     * focus (the first look at the focus comes before the first scan), or
     * the window was made with both. What its state meant is asked again;
     * until the focus moved the monitor stayed paused for "a fullscreen
     * window". */
    if (X.fs_active && window_is_desk (X.fs_win))
        update_active_window ();
    /* the focused fullscreen window may have been moved to another monitor
     * (the scan runs after every change of a window's place) */
    if (X.fs_active)
        fs_place_report ();

    walpe_signals_end ();
}

static void restack_check (void);

static gboolean cover_recompute_tick (gpointer user)
{
    (void) user;
    X.cover_timer_id = 0;
    restack_check ();
    recompute_covered ();
    /* another window may be under a resting pointer now */
    if (X.ev && X.ev->windows_changed)
        X.ev->windows_changed (X.user);
    return G_SOURCE_REMOVE;
}

/* coalesce bursts (window drags fire ConfigureNotify continuously). There
 * is no periodic re-scan any more: geometry / visibility changes arrive as
 * SubstructureNotify events of the root, state / opacity / type changes as
 * PropertyNotify of the watched clients (clients_watch). */
static void cover_schedule (void)
{
    if (!X.cover_timer_id)
        X.cover_timer_id = g_timeout_add (80, cover_recompute_tick, NULL);
}

/* ------------------------------------------------------------- X11 */

#ifdef HAVE_XI2
static void xi2_event (XEvent *e);
#endif

static gboolean xdata_ready (gint fd, GIOCondition cond, gpointer user);

/* the X server went away (logging out, X killed): Xlib calls this and
 * exits the process. That end is not the wallpaper's fault: remove the
 * crash-loop marker and the socket first, or the next session would start
 * without wallpaper / find a dead socket. Only async-signal-safe calls
 * (this may run on any thread, inside Xlib); the paths are prepared at
 * start-up. GDK installs its own handler when the web module starts:
 * the X event source puts this one back (xsrc_prepare). */
static char *xio_marker_path, *xio_sock_path;

static int walpe_xio_error (Display *d)
{
    (void) d;
    static const char msg[] = "walpe-daemon: lost the X server connection; exit\n";
    if (xio_marker_path)
        unlink (xio_marker_path);
    if (xio_sock_path)
        unlink (xio_sock_path);
    if (write (2, msg, sizeof (msg) - 1) < 0) { /* nothing to do about it */ }
    _exit (1);
    return 0;
}

/* non-fatal X error handler: BadWindow races (selecting input on a window
 * that just died) must never take the daemon down */
static int walpe_xerror (Display *d, XErrorEvent *e)
{
    g_debug ("X error %d on resource 0x%lx (request %lu)",
             e->error_code, (unsigned long) e->resourceid, (unsigned long) e->serial);
    return 0;
}

static gboolean have_shape = FALSE; /* probed at startup */

static void shape_away_input (Window w)
{
    if (have_shape)
        XShapeCombineRectangles (X.xd, w, ShapeInput, 0, 0, NULL, 0, ShapeSet, 0);
}

/* The properties of a wallpaper window the window manager manages
 * (x11desk.h: Mutter and its forks), set before it is mapped: the desktop
 * type - the window manager's lowest layer -, no focus, no decorations, on
 * every workspace, in no window list. WM_DELETE_WINDOW is declared so that
 * a "close" is a message (which nothing here acts on) and not the end of
 * this program's connection. */
static void wallpaper_window_managed_props (Window w, const XRect *g)
{
    Atom type = XInternAtom (X.xd, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
    XChangeProperty (X.xd, w, XInternAtom (X.xd, "_NET_WM_WINDOW_TYPE", False), XA_ATOM, 32,
                     PropModeReplace, (guchar *) &type, 1);
    Atom state[3] = { XInternAtom (X.xd, "_NET_WM_STATE_SKIP_TASKBAR", False),
                      XInternAtom (X.xd, "_NET_WM_STATE_SKIP_PAGER", False),
                      XInternAtom (X.xd, "_NET_WM_STATE_STICKY", False) };
    XChangeProperty (X.xd, w, XInternAtom (X.xd, "_NET_WM_STATE", False), XA_ATOM, 32,
                     PropModeReplace, (guchar *) state, 3);
    long all = 0xffffffffL;
    XChangeProperty (X.xd, w, XInternAtom (X.xd, "_NET_WM_DESKTOP", False), XA_CARDINAL, 32,
                     PropModeReplace, (guchar *) &all, 1);
    long motif[5] = { 2 /* decorations */, 0, 0 /* none */, 0, 0 };
    Atom mh = XInternAtom (X.xd, "_MOTIF_WM_HINTS", False);
    XChangeProperty (X.xd, w, mh, mh, 32, PropModeReplace, (guchar *) motif, 5);
    XWMHints hints = { .flags = InputHint, .input = False };
    XSetWMHints (X.xd, w, &hints);
    XSizeHints size = { .flags = USPosition | USSize | PMinSize | PMaxSize, .x = g->x, .y = g->y,
                        .width = g->width, .height = g->height, .min_width = g->width,
                        .min_height = g->height, .max_width = g->width, .max_height = g->height };
    XSetWMNormalHints (X.xd, w, &size);
    char res_name[] = "walpe-wallpaper", res_class[] = "Walpe";
    XClassHint cls = { res_name, res_class };
    XSetClassHint (X.xd, w, &cls);
    XStoreName (X.xd, w, "WalPE wallpaper");
    Atom del = XInternAtom (X.xd, "WM_DELETE_WINDOW", False);
    XSetWMProtocols (X.xd, w, &del, 1);
}

typedef enum {
    X11_WIN_PREVIEW,    /* --windowed: a normal window */
    X11_WIN_OVERRIDE,   /* a wallpaper window no window manager touches */
    X11_WIN_MANAGED,    /* a wallpaper window of the desktop type, managed (x11desk.h) */
} X11WinKind;

/* the wallpaper window: override-redirect so no WM (dwm tiles root children,
 * desktops restack) can manage, tile or raise it - or, under a window
 * manager that paints such a window above everything (x11desk.h), a
 * managed one of the desktop type; input shaped away so clicks/keys reach
 * whatever is below. This window IS the EGL surface. */
static Window create_desktop_window (const XRect *g, X11WinKind kind)
{
    XSetWindowAttributes swa = {
        .override_redirect = kind == X11_WIN_OVERRIDE ? True : False,
        .background_pixel = 0x000000, /* black until the first frame lands */
        /* Expose: without a compositor the server paints a part that
         * comes into view with the background; a scene that skips
         * unchanged frames (or has no frame clock) must draw it again */
        .event_mask = ExposureMask,
        .save_under = False,
        .backing_store = NotUseful,
    };
    Window w = XCreateWindow (X.xd, X.root, g->x, g->y, g->width, g->height, 0,
                              CopyFromParent, InputOutput, CopyFromParent,
                              CWOverrideRedirect | CWBackPixel | CWEventMask |
                              CWSaveUnder | CWBackingStore, &swa);
    if (kind != X11_WIN_PREVIEW) {
        if (kind == X11_WIN_MANAGED)
            wallpaper_window_managed_props (w, g);
        shape_away_input (w);
        XMapWindow (X.xd, w);
        XLowerWindow (X.xd, w);
    } else {
        XStoreName (X.xd, w, "walpe (preview)");
        XMapWindow (X.xd, w);
    }
    XFlush (X.xd);
    return w;
}

/* A wallpaper window becomes the other kind (the window manager came,
 * went or was replaced by another one after the windows were made): the
 * same window - its EGL surface stays -, taken off the screen for the
 * change, as the override-redirect flag counts when a window is mapped. */
static void wallpaper_window_set_kind (X11Surface *s, gboolean managed)
{
    if (s->kind != WALPE_SURFACE_WALLPAPER || s->xid == None || s->destroyed || s->managed == managed)
        return;
    s->managed = managed;
    XUnmapWindow (X.xd, s->xid);
    XSetWindowAttributes swa = { .override_redirect = managed ? False : True };
    XChangeWindowAttributes (X.xd, s->xid, CWOverrideRedirect, &swa);
    if (managed) {
        wallpaper_window_managed_props (s->xid, &s->geom);
    } else {
        XDeleteProperty (X.xd, s->xid, XInternAtom (X.xd, "_NET_WM_WINDOW_TYPE", False));
        XDeleteProperty (X.xd, s->xid, XInternAtom (X.xd, "_NET_WM_STATE", False));
    }
    shape_away_input (s->xid);
    if (s->adopt_id)
        return;     /* on its way into a desktop window: mapped there (desk_adopt_cb) */
    XMapWindow (X.xd, s->xid);
    if (s->host == None) {
        /* (a window manager may have moved a window it managed) */
        XMoveResizeWindow (X.xd, s->xid, s->geom.x, s->geom.y, (guint) s->geom.width,
                           (guint) s->geom.height);
        XLowerWindow (X.xd, s->xid);
    } else {
        XRaiseWindow (X.xd, s->xid);
    }
}

/* the window manager's name: _NET_WM_NAME of the window that
 * _NET_SUPPORTING_WM_CHECK on the root names (g_free; NULL: no such
 * window - no window manager that follows EWMH -, or it has no name).
 * *present: that window exists */
static char *wm_name_read (gboolean *present)
{
    *present = FALSE;
    if (X.a_wmcheck == None)
        return NULL;
    guchar *data = NULL;
    gulong n = window_prop32 (X.root, X.a_wmcheck, XA_WINDOW, 1, &data);
    Window w = data && n == 1 ? *(Window *) data : None;
    if (data)
        XFree (data);
    XWindowAttributes wa;
    if (w == None || !XGetWindowAttributes (X.xd, w, &wa))
        return NULL;
    *present = TRUE;
    Atom rt = None;
    int fmt = 0;
    gulong len = 0, left = 0;
    guchar *txt = NULL;
    char *name = NULL;
    if (XGetWindowProperty (X.xd, w, XInternAtom (X.xd, "_NET_WM_NAME", False), 0, 64, False,
                            AnyPropertyType, &rt, &fmt, &len, &left, &txt) == Success && txt) {
        if (fmt == 8 && len > 0)
            name = g_utf8_make_valid ((const char *) txt, (gssize) len);
        XFree (txt);
    }
    return name;
}

/* The kind of window the wallpaper windows are follows the window manager
 * (x11desk.h). Once at start-up, and when the window manager came, went
 * or was replaced (_NET_SUPPORTING_WM_CHECK on the root changed). */
static void wm_look (void)
{
    gboolean present = FALSE;
    char *name = wm_name_read (&present);
    X.has_wm = present;     /* ("wm=yes" of walpe-ctl diag) */
    X.wm_desk = -1;         /* (what it lists in _NET_SUPPORTED: asked when it matters) */
    const gboolean managed = walpe_x11_win_managed (X.win_mode, name);
    const gboolean changed = managed != X.managed || g_strcmp0 (name, X.wm_name) != 0;
    g_free (X.wm_name);
    X.wm_name = name;
    X.managed = managed;
    if (!changed)
        return;
    if (managed)
        g_message ("x11: the wallpaper windows are managed windows of the desktop type%s%s%s: this "
                   "window manager paints an override-redirect window above every other window. "
                   "WALPE_X11_WINDOW=override makes them override-redirect as on other window "
                   "managers", X.win_mode == WALPE_X11_WIN_MANAGED ? " (WALPE_X11_WINDOW=managed)" : "",
                   name ? "; window manager: " : "", name ? name : "");
    for (guint i = 0; X.surfs && i < X.surfs->len; i++)
        wallpaper_window_set_kind (SURF (i), managed);
    if (X.surfs && X.surfs->len) {
        X.stack_dirty = TRUE;
        XFlush (X.xd);
        cover_schedule ();  /* (what a desktop window counts as goes by the name too) */
    }
}

static gboolean wm_look_cb (gpointer user)
{
    (void) user;
    X.wm_timer_id = 0;
    wm_look ();
    return G_SOURCE_REMOVE;
}

/* keep the wallpaper window at the bottom of the stack; re-assert often */
static void sink_desktop_window (X11Surface *m)
{
    if (m->xid == None || m->host != None)  /* (in a desktop window: nothing to lower) */
        return;
    shape_away_input (m->xid);
    XLowerWindow (X.xd, m->xid);
}

/* are the wallpaper windows the lowest children of the root already?
 * (XQueryTree lists them bottom first) */
static gboolean wallpapers_at_bottom (void)
{
    Window r, p, *kids = NULL;
    guint n = 0, nm = 0;
    /* (those that are children of a desktop window are not on the root) */
    for (guint i = 0; i < X.surfs->len; i++)
        nm += SURF (i)->host == None ? 1 : 0;
    if (nm == 0)
        return TRUE;
    if (!XQueryTree (X.xd, X.root, &r, &p, &kids, &n))
        return FALSE;
    gboolean ok = n >= nm;
    if (X.managed) {
        /* Managed wallpaper windows (x11desk.h): the window manager keeps
         * them in its lowest layer, and may have windows of its own below
         * them (Mutter's guard window). What must not be below them is a
         * file manager's desktop window - its icons belong above the
         * wallpaper */
        gboolean desk_below = FALSE;
        ok = TRUE;
        for (guint i = 0; ok && i < n; i++) {
            if (walpe_x11_desk_is_top (kids[i], X.desks, (int) X.n_desks))
                desk_below = TRUE;
            else if (desk_below && window_is_wallpaper (kids[i]))
                ok = FALSE;
        }
        nm = 0;
    }
    for (guint i = 0; ok && i < nm; i++)
        ok = window_is_wallpaper (kids[i]);
    if (kids)
        XFree (kids);
    return ok;
}

/* Lowering unconditionally restacked for nothing: with two monitors each
 * XLowerWindow puts its window below the other one, so every tick both
 * monitor-sized windows changed places, the WM / a compositor / our own
 * cover scan were woken by the ConfigureNotify events, and the input
 * shape was re-sent. Now only when something got below a wallpaper - and
 * that is looked at (one XQueryTree) only after an event that can mean it
 * (stack_event), not every 5 s. */
static void restack_check (void)
{
    if (!X.stack_dirty)
        return;
    X.stack_dirty = FALSE;
    if (wallpapers_at_bottom ())
        return;
    g_debug ("a window got below the wallpaper: lowering the wallpaper windows");
    for (guint i = 0; i < X.surfs->len; i++)
        sink_desktop_window (SURF (i));
}

/* a root child was restacked: can a window be below a wallpaper window
 * now? `above` = the sibling directly below the window (None: lowest).
 * Our own lowering (wallpaper above None / above a wallpaper) is not. */
static void stack_event (Window w, Window above)
{
    gboolean wp = window_is_wallpaper (w);
    if (wp ? (above != None && !window_is_wallpaper (above))
           : (above == None || window_is_wallpaper (above)))
        X.stack_dirty = TRUE;
}

static gboolean window_is_fullscreen (Window w)
{
    if (w == None)
        return FALSE;
    Atom type = XInternAtom (X.xd, "_NET_WM_STATE", True);
    if (type == None)
        return FALSE;
    Atom fs = XInternAtom (X.xd, "_NET_WM_STATE_FULLSCREEN", True);
    guchar *data = NULL;
    gboolean found = FALSE;
    gulong n = window_prop32 (w, type, XA_ATOM, 64, &data);
    if (data) {
        Atom *atoms = (Atom *) data;
        for (gulong i = 0; i < n; i++)
            if (atoms[i] == fs)
                found = TRUE;
        XFree (data);
    }
    return found;
}

/* Which monitors the focused fullscreen window is on
 * (WALPE_SIG_FULLSCREEN_ON; the rule is fsplace.h's). "Pause for
 * fullscreen apps" pauses those and leaves the others running: a film or
 * a game in full screen on one monitor used to stop the wallpaper of the
 * monitor next to it.
 * The window is the client _NET_ACTIVE_WINDOW names. Its rectangle is
 * asked for in root coordinates (under a reparenting window manager the
 * client sits inside a frame, and its own position is the one in that
 * frame) and without its border. The server is asked only while such a
 * window exists (three requests, when the focus or its state changes and
 * after a scan of the windows): without one this costs no request, and
 * nothing here runs on a timer.
 * A window whose rectangle cannot be asked for stays on the monitors it
 * was last seen on (it is going away: the window manager moves the focus
 * on in a moment); one that was never seen counts for every monitor, as
 * the rule did before. So does every window for the --windowed preview,
 * which has no monitor rectangle. */
static void fs_place_report (void)
{
    guint n = X.surfs ? X.surfs->len : 0;
    WalpeFsRect win = { 0, 0, 0, 0 };
    gboolean known = FALSE, everywhere = FALSE;
    if (X.fs_active && X.fs_win != None && !X.windowed) {
        XWindowAttributes wa;
        Window junk;
        int ox = 0, oy = 0;
        if (XGetWindowAttributes (X.xd, X.fs_win, &wa) &&
            XTranslateCoordinates (X.xd, X.fs_win, X.root, 0, 0, &ox, &oy, &junk)) {
            win = (WalpeFsRect) { ox, oy, wa.width, wa.height };
            known = TRUE;
        } else if (X.fs_placed == X.fs_win) {
            return;     /* as it was when the window could still be asked */
        }
    }
    X.fs_placed = known ? X.fs_win : None;
    guint all = n;
    WalpeFsRect *mons = g_new0 (WalpeFsRect, n + (guint) X.n_layout + 1);
    gboolean *on = g_new0 (gboolean, n + (guint) X.n_layout + 1);
    for (guint k = 0; k < n; k++) {
        const XRect *g = &SURF (k)->geom;
        mons[k] = (WalpeFsRect) { g->x, g->y, g->width, g->height };
    }
    /* "The monitor that holds most of the window" is found among all
     * monitors of the screen, also those this daemon does not draw on
     * (--monitor N): a fullscreen window on one of those whose edge
     * reaches a few pixels into ours is not on ours. They follow ours in
     * the list; only ours are reported. */
    for (int i = 0; i < X.n_layout; i++) {
        const XRect *g = &X.layout[i];
        gboolean ours = FALSE;
        for (guint k = 0; k < n && !ours; k++) {
            const XRect *o = &SURF (k)->geom;
            ours = o->x == g->x && o->y == g->y && o->width == g->width && o->height == g->height;
        }
        if (!ours)
            mons[all++] = (WalpeFsRect) { g->x, g->y, g->width, g->height };
    }
    walpe_fs_decide (X.fs_active, known ? &win : NULL, mons, (int) all, on, &everywhere);
    walpe_signals_begin ();
    if (everywhere != X.fs_everywhere) {
        X.fs_everywhere = everywhere;
        if (everywhere)
            g_debug ("fullscreen: where the window is is not known: every monitor");
        walpe_signal_set (&x11_provider, WALPE_SIG_FULLSCREEN_ON, WALPE_OUTPUT_ALL, everywhere);
    }
    for (guint k = 0; k < n; k++) {
        X11Surface *m = SURF (k);
        if (on[k] == m->fs_here)
            continue;
        m->fs_here = on[k];
        g_debug ("mon%d: %s (window %dx%d+%d+%d)", m->index,
                 on[k] ? "the focused fullscreen window is on it" : "no focused fullscreen window",
                 win.width, win.height, win.x, win.y);
        walpe_signal_set (&x11_provider, WALPE_SIG_FULLSCREEN_ON, m->out_id, on[k]);
    }
    walpe_signals_end ();
    g_free (mons);
    g_free (on);
}

static void update_active_window (void)
{
    Atom prop = XInternAtom (X.xd, "_NET_ACTIVE_WINDOW", True);
    if (prop == None)
        return;
    guchar *data = NULL;
    Window active = None;
    gulong n = window_prop32 (X.root, prop, XA_WINDOW, 1, &data);
    if (data) {
        if (n == 1)
            active = *(Window *) data;
        XFree (data);
    }
    /* Not an application's window: the root, or one of this daemon's own
     * (any client can write the property, and the --windowed preview gets
     * the focus from the window manager like any other window). Asking
     * for such a window's property events below would replace the events
     * this connection asked for on it - Expose: what another window had
     * covered was never drawn again - and no wallpaper pauses for itself. */
    if (active == X.root || window_is_wallpaper (active))
        active = None;
    gboolean changed = active != X.watched_win;
    if (changed) {
        /* The window's property events are asked for before its state is
         * read. The other way round, a state change that reached the
         * server between the read and the XSelectInput was told to nobody:
         * a window that turned fullscreen right after it got the focus (an
         * application asks for that as soon as its window is mapped) stayed
         * "not fullscreen" here until the focus moved again, and its
         * monitor was not paused.
         * (clients of _NET_CLIENT_LIST are watched anyway: clients_watch) */
        if (X.watched_win != None && !client_is_watched (X.watched_win))
            XSelectInput (X.xd, X.watched_win, 0);
        if (active != None && active != X.root && !client_is_watched (active))
            XSelectInput (X.xd, active, PropertyChangeMask);
        X.watched_win = active;
    }
    /* (a desktop's own desktop window has the focus after a click on the
     * desktop; whatever state it carries, it is no fullscreen application -
     * where the window manager knows such a window as the desktop: under
     * dwm or i3 a fullscreen window of that type is one more fullscreen
     * client, and pauses as before) */
    gboolean fs = window_is_fullscreen (active);
    if (fs && window_is_desk (active))
        fs = FALSE;
    /* one batch: the daemon hears of the window and of its monitor at once */
    walpe_signals_begin ();
    if (fs != X.fs_active) {
        X.fs_active = fs;
        changed = TRUE;
        g_debug ("fullscreen %s", fs ? "engaged -> pausing" : "left -> resuming");
        walpe_signal_set (&x11_provider, WALPE_SIG_FULLSCREEN, 0, fs);
    }
    /* its monitor, also when another fullscreen window took the focus */
    gboolean placed = fs || X.fs_win != None || X.fs_everywhere;
    X.fs_win = fs ? active : None;
    if (placed)
        fs_place_report ();
    walpe_signals_end ();
    /* focus changes usually accompany stack/geometry churn (a property of
     * the same window that changed nothing here does not). The scan also
     * looks at a resting pointer again: window managers that warp the
     * pointer to the focused window send no raw motion for it */
    if (changed)
        cover_schedule ();
}

static gboolean xdata_ready (gint fd, GIOCondition cond, gpointer user)
{
    (void) fd; (void) cond; (void) user;
    while (XPending (X.xd)) {
        XEvent e;
        XNextEvent (X.xd, &e);
        if (e.type == PropertyNotify) {
            static Atom a_active, a_clients, a_state, a_wmstate, a_opacity, a_wtype, a_opaque;
            if (!a_active) {
                a_active = XInternAtom (X.xd, "_NET_ACTIVE_WINDOW", False);
                a_clients = XInternAtom (X.xd, "_NET_CLIENT_LIST", False);
                a_state = XInternAtom (X.xd, "_NET_WM_STATE", False);
                a_wmstate = XInternAtom (X.xd, "WM_STATE", False);
                a_opacity = XInternAtom (X.xd, "_NET_WM_WINDOW_OPACITY", False);
                a_wtype = XInternAtom (X.xd, "_NET_WM_WINDOW_TYPE", False);
                a_opaque = XInternAtom (X.xd, "_NET_WM_OPAQUE_REGION", False);
            }
            Atom at = e.xproperty.atom;
            client_props_event (&e.xproperty);   /* what the cover scan keeps of the window */
            if (e.xproperty.window == X.root) {
                if (at == a_active)
                    update_active_window ();
                else if (at == a_clients)
                    cover_schedule (); /* clients appeared/vanished */
                else if (at == X.a_supported && X.a_supported != None) {
                    /* (the window manager's list of what it knows: asked
                     * for again when a desktop window is looked at) */
                    X.wm_desk = -1;
                    if (X.n_desks > 0)
                        cover_schedule ();
                }
                else if (at == X.a_wmcheck && X.a_wmcheck != None && !X.windowed) {
                    /* the window manager came, went or was replaced: its
                     * name a moment later (it names itself after this) */
                    if (!X.wm_timer_id)
                        X.wm_timer_id = g_timeout_add (500, wm_look_cb, NULL);
                }
                /* anything else on the root (a status bar text set with
                 * xsetroot -name every second, ...) costs this wake-up and
                 * nothing more */
            } else if (at == a_state || at == a_wmstate) {
                /* a client's state: fullscreen (the focused window), hidden,
                 * iconic. Titles, _NET_WM_USER_TIME (set on every key press
                 * / click), icons ... are ignored: no round trip, no scan */
                if (e.xproperty.window == X.watched_win)
                    update_active_window ();
                cover_schedule ();
            } else if (at == a_opacity || at == a_wtype || at == a_opaque) {
                cover_schedule ();
            }
        }
        else if (ss_event_base >= 0 && e.type == ss_event_base) {
            sleep_recheck ();       /* ScreenSaverNotify: blanked / unblanked */
        }
        else if (e.type == GenericEvent) {
            if (dpms_opcode >= 0 && e.xcookie.extension == dpms_opcode)
                sleep_recheck ();   /* DPMSInfoNotify */
#ifdef HAVE_XI2
            else
                xi2_event (&e);
#endif
        }
#ifdef HAVE_XRANDR
        else if (rr_event_base >= 0 && e.type == rr_event_base + RRScreenChangeNotify) {
            XRRUpdateConfiguration (&e);
            outputs_changed ();     /* monitors changed? */
        }
#endif
        else if (e.type == Expose) {
            /* a part of a wallpaper window lost its pixels: its next frame
             * is drawn whatever the content thinks of it (a follower of a
             * shared scene presents the leader's newest frame again). A
             * window dragged across the wallpaper sends these at the
             * pointer's rate: the daemon answers once per frame interval of
             * the fps setting. */
            for (guint k = 0; k < X.surfs->len; k++)
                if (SURF (k)->xid == e.xexpose.window && X.ev && X.ev->surface_exposed)
                    X.ev->surface_exposed (SURF (k)->user, X.user);
        }
        else if (e.type == MapNotify || e.type == UnmapNotify ||
                   e.type == ConfigureNotify || e.type == CirculateNotify ||
                   e.type == DestroyNotify || e.type == ReparentNotify) {
            if (e.type == ConfigureNotify && e.xconfigure.window == X.root)
                outputs_changed (); /* the screen was resized */
            /* One of the wallpaper windows is gone: another client destroyed
             * it (the daemon destroys its windows only when it ends, after
             * its last look at the events). The render thread goes on
             * swapping into a drawable that no longer exists - nothing fails
             * loudly, the monitor shows the root window from then on, and
             * with shared rendering the monitors that follow this one get
             * its undefined frames. The surface is reported closed, as a
             * Wayland compositor's layer surface is: the daemon restarts in
             * place and makes its windows anew. */
            /* (a wallpaper window that is a child of a desktop window is
             * not the root's: its own DestroyNotify was asked for, and it
             * goes when the file manager's window does) */
            if (e.type == DestroyNotify &&
                (e.xdestroywindow.event == X.root ||
                 e.xdestroywindow.event == e.xdestroywindow.window))
                for (guint k = 0; k < X.surfs->len; k++)
                    if (SURF (k)->xid == e.xdestroywindow.window && !SURF (k)->destroyed) {
                        SURF (k)->destroyed = TRUE;
                        if (X.ev && X.ev->surface_closed)
                            X.ev->surface_closed (SURF (k)->user, X.user);
                        break;
                    }
            /* a window manager put a frame around a window, or took it
             * away: which child of the root a desktop window is in */
            if (e.type == ReparentNotify && !window_is_wallpaper (e.xreparent.window))
                X.desk_tops_stale = TRUE;
            if (e.type == ConfigureNotify && e.xconfigure.event == X.root)
                stack_event (e.xconfigure.window, e.xconfigure.above);
            else if (e.type == CirculateNotify &&
                     (e.xcirculate.place == PlaceOnBottom ||
                      window_is_wallpaper (e.xcirculate.window)))
                X.stack_dirty = TRUE;
            cover_schedule (); /* window geometry/visibility changed */
        }
    }
    return G_SOURCE_CONTINUE;
}

/* The X connection as a main-loop source that also looks at Xlib's own
 * queue. A plain fd watch is not enough: every round trip on the Display
 * (XQueryPointer in the pointer poll, the cover queries, a render thread's
 * eglSwapBuffers) reads whatever the server sent and parks the events
 * that came before its reply in Xlib's memory. The socket is empty then,
 * an fd watch never fires, and those events - desktop clicks above all -
 * waited for the next unrelated X event: releases seconds late, double
 * clicks lost, drags stuck. prepare / check ask Xlib (as GDK's event
 * source does), so a parked event is dispatched in the same main-loop
 * iteration when the main thread's own round trip parked it. (Render
 * threads no longer use this connection: EGL has its own, X.egl_xd.) */
typedef struct {
    GSource src;
    GPollFD pfd;
} XEventSource;

static gboolean xsrc_prepare (GSource *src, gint *timeout)
{
    (void) src;
    *timeout = -1;
    /* GDK (the web module) installs its own I/O error handler when it
     * opens its display. Putting ours back here - every main-loop
     * iteration, i.e. right after GDK did it - costs no request and no
     * timer (it used to ride on a 1 s tick) */
    XSetIOErrorHandler (walpe_xio_error);
    return XEventsQueued (X.xd, QueuedAlready) > 0;
}

static gboolean xsrc_check (GSource *src)
{
    XEventSource *x = (XEventSource *) src;
    return (x->pfd.revents & (G_IO_IN | G_IO_HUP | G_IO_ERR)) != 0 ||
           XEventsQueued (X.xd, QueuedAlready) > 0;
}

static gboolean xsrc_dispatch (GSource *src, GSourceFunc cb, gpointer user)
{
    (void) src; (void) cb; (void) user;
    return xdata_ready (0, 0, NULL);
}

static GSourceFuncs xsrc_funcs = { xsrc_prepare, xsrc_check, xsrc_dispatch, NULL, NULL, NULL };

static void x_event_source_add (void)
{
    XEventSource *x = (XEventSource *) g_source_new (&xsrc_funcs, sizeof (XEventSource));
    x->pfd.fd = ConnectionNumber (X.xd);
    x->pfd.events = G_IO_IN | G_IO_HUP | G_IO_ERR;
    g_source_add_poll (&x->src, &x->pfd);
    g_source_set_name (&x->src, "walpe X events");
    g_source_attach (&x->src, NULL);
}

/* ------------------------------------------------------------- pointer */

/* is the pointer on the desktop, i.e. over no client window? child = the
 * root child XQueryPointer reports (input shapes respected: our wallpaper
 * windows are input-shaped away, so the bare desktop gives None); desktop
 * icon windows (_NET_WM_WINDOW_TYPE_DESKTOP: pcmanfm, xfdesktop, ...)
 * count as desktop. Like WE: clicks on the desktop reach the wallpaper,
 * clicks on windows (and on bars/panels) do not.
 * A window manager that puts a frame around every client (xfwm4, openbox,
 * kwin) reports that frame as the child, and a frame has no type: the
 * desktop windows the scan of the clients found are known with the child
 * of the root they are in (x11desk.h). */
static gboolean pointer_on_desktop (Window child)
{
    if (child == None || child == X.root)
        return TRUE;
    for (guint i = 0; i < X.surfs->len; i++)
        if (SURF (i)->xid == child)
            return TRUE;
    if (walpe_x11_desk_is_top (child, X.desks, (int) X.n_desks) && wm_knows_desktop ())
        return TRUE;
    static Window last_w = None;
    static gboolean last_r = FALSE;
    if (child == last_w)
        return last_r;
    gboolean desk = FALSE;
    Atom type = XInternAtom (X.xd, "_NET_WM_WINDOW_TYPE", True);
    Atom dt = XInternAtom (X.xd, "_NET_WM_WINDOW_TYPE_DESKTOP", True);
    if (type != None && dt != None) {
        guchar *data = NULL;
        gulong n = window_prop32 (child, type, XA_ATOM, 16, &data);
        if (data) {
            for (gulong i = 0; i < n; i++)
                if (((Atom *) data)[i] == dt)
                    desk = TRUE;
            XFree (data);
        }
    }
    last_w = child;
    last_r = desk;
    return desk;
}

#ifdef HAVE_XI2
/* raw button events on the root: any client may select them (a plain
 * ButtonPress on the root is owned by the window manager).
 * XInput 2.1, not 2.0: a 2.0 client gets no raw events while another
 * client holds a grab, and a window manager that selects ButtonPress on
 * the root (dwm does, to focus the monitor clicked on) holds the implicit
 * grab from every desktop press to its release - the daemon saw the press
 * and never the release, so no click ever completed (cursorClick fires on
 * the release). From 2.1 on raw events are sent whatever the grab state. */
static void xi2_init (void)
{
    int ev, err, maj = 2, min = 1;
    if (!XQueryExtension (X.xd, "XInputExtension", &xi2_opcode, &ev, &err) ||
        XIQueryVersion (X.xd, &maj, &min) != Success) {
        xi2_opcode = -1;
        g_message ("pointer: no XInput2, desktop clicks do not reach wallpapers");
        return;
    }
}

/* Raw events are selected only while something needs them (each one wakes
 * the daemon): buttons while a wallpaper takes clicks (SceneScript cursor
 * events, or a web page), motion while a wallpaper follows a pointer that
 * is at rest (pointer_set_mode), any input while the display is off on a
 * server without DPMS events (sleep_arm). One request per change. */
static void xi2_apply_mask (void)
{
    static int cur = -1;
    if (xi2_opcode < 0)
        return;
    int want = (xi_want_buttons ? 1 : 0) | (xi_want_motion ? 2 : 0) | (xi_want_wake ? 4 : 0);
    if (want == cur)
        return;
    gboolean clicks_changed = cur < 0 || ((cur ^ want) & 1);
    cur = want;
    unsigned char mask[XIMaskLen (XI_LASTEVENT)] = { 0 };
    if (want & 1) {
        XISetMask (mask, XI_RawButtonPress);
        XISetMask (mask, XI_RawButtonRelease);
    }
    if (want & 2)
        XISetMask (mask, XI_RawMotion);
    if (want & 4) {
        XISetMask (mask, XI_RawMotion);
        XISetMask (mask, XI_RawKeyPress);
        XISetMask (mask, XI_RawButtonPress);
    }
    XIEventMask em = { XIAllMasterDevices, sizeof (mask), mask };
    XISelectEvents (X.xd, X.root, &em, 1);
    XFlush (X.xd);
    if (clicks_changed)
        g_message ("pointer: desktop clicks %s",
                   (want & 1) ? "selected (cursor events / web input)" : "not selected");
}

static void xi2_event (XEvent *e)
{
    if (e->xcookie.extension != xi2_opcode)
        return;
    int t = e->xcookie.evtype;
    if (xi_want_wake && (t == XI_RawMotion || t == XI_RawKeyPress || t == XI_RawButtonPress))
        sleep_wake_kick ();     /* input while the display was off */
    if (t == XI_RawMotion) {
        if (X.ev && X.ev->pointer_motion)
            X.ev->pointer_motion (X.user);   /* the pointer left its resting place */
        return;                 /* the event's data is not needed */
    }
    if (t != XI_RawButtonPress && t != XI_RawButtonRelease)
        return;
    if (!xi_want_buttons || !XGetEventData (X.xd, &e->xcookie))
        return;
    XIRawEvent *re = e->xcookie.data;
    /* which buttons go anywhere is the daemon's business (scenes: the
     * left one; web pages: left / middle / right and the wheel, 4-7): it is
     * asked before the server is asked where the pointer is */
    if (X.ev && X.ev->button && X.ev->button_wanted &&
        X.ev->button_wanted (re->detail, X.user)) {
        Window r, c;
        int rx, ry, wx, wy;
        unsigned mask = 0;
        if (XQueryPointer (X.xd, X.root, &r, &c, &rx, &ry, &wx, &wy, &mask)) {
            gboolean desk = pointer_on_desktop (c);
            X.ev->button (re->detail, t == XI_RawButtonPress, rx, ry, desk, (guint32) re->time,
                          X.user);
        }
    }
    XFreeEventData (X.xd, &e->xcookie);
}
#else
static void xi2_apply_mask (void)
{
}
#endif

/* ------------------------------------------------------------- outputs */

#ifdef HAVE_XRANDR
/* refresh rate of the output(s) lit at a monitor's rectangle: the CRTC
 * whose area is that rectangle (else the one containing its centre); the
 * fastest when several show it (mirrored outputs). 0 = none found. */
static double randr_refresh (const XRect *g)
{
    int ev = 0, er = 0;
    if (!XRRQueryExtension (X.xd, &ev, &er))
        return 0.0;
    XRRScreenResources *res = XRRGetScreenResourcesCurrent (X.xd, X.root);
    if (!res)
        return 0.0;
    double exact = 0.0, inside = 0.0;
    int cx = g->x + g->width / 2, cy = g->y + g->height / 2;
    for (int c = 0; c < res->ncrtc; c++) {
        XRRCrtcInfo *ci = XRRGetCrtcInfo (X.xd, res, res->crtcs[c]);
        if (!ci)
            continue;
        if (ci->mode != None && ci->noutput > 0) {
            double hz = 0.0;
            for (int k = 0; k < res->nmode; k++)
                if (res->modes[k].id == ci->mode)
                    hz = walpe_mode_refresh (res->modes[k].dotClock, res->modes[k].hTotal,
                                             res->modes[k].vTotal,
                                             (res->modes[k].modeFlags & RR_Interlace) != 0,
                                             (res->modes[k].modeFlags & RR_DoubleScan) != 0);
            if (ci->x == g->x && ci->y == g->y && (int) ci->width == g->width &&
                (int) ci->height == g->height)
                exact = MAX (exact, hz);
            else if (cx >= ci->x && cx < ci->x + (int) ci->width && cy >= ci->y &&
                     cy < ci->y + (int) ci->height)
                inside = MAX (inside, hz);
        }
        XRRFreeCrtcInfo (ci);
    }
    XRRFreeScreenResources (res);
    return exact > 0.0 ? exact : inside;
}
#endif

static void x11_outputs (WalpeOutputList *out)
{
    XRect *geoms = NULL;
    int nmon = 0;
    int evb = 0, errb = 0;
    if (XineramaQueryExtension (X.xd, &evb, &errb)) {
        int nscr = 0;
        XineramaScreenInfo *scr = XineramaQueryScreens (X.xd, &nscr);
        if (scr && nscr > 0) {
            nmon = nscr;
            geoms = g_new0 (XRect, nmon);
            for (int i = 0; i < nmon; i++) {
                geoms[i].x = scr[i].x_org;
                geoms[i].y = scr[i].y_org;
                geoms[i].width = scr[i].width;
                geoms[i].height = scr[i].height;
            }
        }
        if (scr)
            XFree (scr);
    }
    /* WALPE_MONITORS="WxH+X+Y,...": override the monitor layout (testing on
     * a server without Xinerama, e.g. Xvfb with one wide screen) */
    const char *mon_env = g_getenv ("WALPE_MONITORS");
    char *mon_file = NULL;
    if (mon_env && mon_env[0] == '@') {
        /* WALPE_MONITORS=@FILE: the list is read from FILE at every
         * detection, so a test can change the layout under a running
         * daemon (then any RandR / root resize event makes it look) */
        if (g_file_get_contents (mon_env + 1, &mon_file, NULL, NULL))
            mon_env = g_strstrip (mon_file);
        else
            mon_env = NULL;
    }
    char **env_names = NULL;   /* "NAME=WxH+X+Y" entries name their monitor */
    double *env_hz = NULL;     /* "...@HZ": its refresh rate (testing the pacing) */
    if (mon_env && *mon_env) {
        char **parts = g_strsplit (mon_env, ",", -1);
        int n = 0;
        XRect *g = g_new0 (XRect, g_strv_length (parts) + 1);
        env_names = g_new0 (char *, g_strv_length (parts) + 1);
        env_hz = g_new0 (double, g_strv_length (parts) + 1);
        for (int i = 0; parts[i]; i++) {
            const char *eq = strchr (parts[i], '=');
            const char *geo = eq ? eq + 1 : parts[i];
            if (sscanf (geo, "%dx%d+%d+%d", &g[n].width, &g[n].height, &g[n].x,
                        &g[n].y) == 4 && g[n].width > 0 && g[n].height > 0) {
                env_names[n] = eq ? g_strndup (parts[i], (gsize) (eq - parts[i])) : NULL;
                env_hz[n] = walpe_monitor_spec_hz (geo);
                n++;
            }
        }
        g_strfreev (parts);
        if (n > 0) {
            g_free (geoms);
            geoms = g;
            nmon = n;
        } else {
            g_free (g);
            g_clear_pointer (&env_names, g_free);
            g_clear_pointer (&env_hz, g_free);
        }
    }
    if (nmon < 1) {
        nmon = 1;
        g_free (geoms);
        geoms = g_new0 (XRect, 1);
        Window rw;
        int wx, wy;
        unsigned ww = 0, wh = 0, wb, wd;
        XGetGeometry (X.xd, X.root, &rw, &wx, &wy, &ww, &wh, &wb, &wd);
        geoms[0].width = (int) ww;
        geoms[0].height = (int) wh;
    }

    /* output names (config keys for per-monitor wallpapers): RandR 1.5
     * monitors matched by geometry ("HDMI-0", "DP-2"), else "monN" */
    char **names = g_new0 (char *, nmon + 1);
    for (int i = 0; i < nmon; i++)
        names[i] = env_names ? env_names[i] : NULL;   /* ownership moves */
    g_free (env_names);     /* the array only: unnamed entries are NULL holes */
#ifdef HAVE_XRANDR
    {
        int rev = 0, rerr = 0, nm = 0;
        XRRMonitorInfo *mi = XRRQueryExtension (X.xd, &rev, &rerr)
                                 ? XRRGetMonitors (X.xd, X.root, True, &nm) : NULL;
        for (int i = 0; i < nmon; i++)
            for (int k = 0; mi && k < nm && !names[i]; k++)
                if (mi[k].x == geoms[i].x && mi[k].y == geoms[i].y &&
                    mi[k].width == geoms[i].width && mi[k].height == geoms[i].height) {
                    char *an = XGetAtomName (X.xd, mi[k].name);
                    if (an) {
                        names[i] = g_strdup (an);
                        XFree (an);
                    }
                }
        if (mi)
            XRRFreeMonitors (mi);
    }
#endif
    for (int i = 0; i < nmon; i++)
        if (!names[i])
            names[i] = g_strdup_printf ("mon%d", i);
    /* refresh rates (pacing, shared-rendering leader): WALPE_MONITORS
     * "@HZ", else the RandR mode of the output */
    double *hzs = g_new0 (double, nmon + 1);
    for (int i = 0; i < nmon; i++) {
        hzs[i] = env_hz ? env_hz[i] : 0.0;
#ifdef HAVE_XRANDR
        if (hzs[i] <= 0.0 && !env_hz)
            hzs[i] = randr_refresh (&geoms[i]);
#endif
    }
    g_free (env_hz);
    g_free (mon_file);
    out->n = nmon;
    out->o = g_new0 (WalpeOutput, nmon + 1);
    for (int i = 0; i < nmon; i++) {
        WalpeOutput *o = &out->o[i];
        o->id = (guint32) i + 1;
        walpe_output_set_names (o, names[i], NULL, NULL, NULL, NULL);
        o->x = o->logical_x = geoms[i].x;
        o->y = o->logical_y = geoms[i].y;
        o->width = o->logical_width = geoms[i].width;
        o->height = o->logical_height = geoms[i].height;
        o->refresh_hz = hzs[i];
        o->scale = 1.0;
        o->transform = WALPE_TRANSFORM_NORMAL;
        g_free (names[i]);
    }
    g_free (names);
    /* the fullscreen rule places a window among all of them (fs_place_report) */
    g_free (X.layout);
    X.layout = geoms;
    X.n_layout = nmon;
    g_free (hzs);
}

/* ----------------------------------------------------------------- EGL */

/* can a window of the root visual get a surface with this config? */
static gboolean x11_egl_config_usable (EGLDisplay dpy, EGLConfig c)
{
    XSetWindowAttributes swa = { .override_redirect = True };
    Window w = XCreateWindow (X.xd, X.root, 0, 0, 1, 1, 0, CopyFromParent, InputOutput,
                              CopyFromParent, CWOverrideRedirect, &swa);
    if (w == None)
        return FALSE;
    EGLSurface sf = eglCreateWindowSurface (dpy, c, (EGLNativeWindowType) w, NULL);
    gboolean ok = sf != EGL_NO_SURFACE;
    if (ok)
        eglDestroySurface (dpy, sf);
    else
        (void) eglGetError ();
    XDestroyWindow (X.xd, w);
    XSync (X.xd, False);
    return ok;
}

/* the visual the windows are created with: the root window's */
static unsigned long x11_egl_visual (void)
{
    XWindowAttributes ra;
    return XGetWindowAttributes (X.xd, X.root, &ra) && ra.visual
               ? XVisualIDFromVisual (ra.visual) : 0;
}

static EGLDisplay x11_egl_display (void)
{
    if (X.probe) {
        /* the test child: is there an EGL library to ask at all? Then the
         * default display, on a connection EGL opens itself */
        if (eglGetDisplay ((EGLNativeDisplayType) X.xd) == EGL_NO_DISPLAY)
            return EGL_NO_DISPLAY;
        return eglGetDisplay ((EGLNativeDisplayType) NULL);
    }
    /* EGL and the render threads have a connection of their own
     * (X.egl_xd; WALPE_ONE_XCONN=1 = shared with the main thread) */
    return eglGetDisplay ((EGLNativeDisplayType) X.egl_xd);
}

static const char *x11_egl_note (void)
{
    return X.egl_xd != X.xd ? ", own X connection" : "";
}

/* -------------------------------------------------------- presentation */

static gboolean x11_composited (void)
{
    return X.cm_atom != None && XGetSelectionOwner (X.xd, X.cm_atom) != None;
}

/* the compositing manager and the X server's presentation-related
 * extensions, once at start-up */
static void x11_present_log (void)
{
    char an[32];
    g_snprintf (an, sizeof an, "_NET_WM_CM_S%d", DefaultScreen (X.xd));
    X.cm_atom = XInternAtom (X.xd, an, False);
    static const char *const xext[] = { "NV-GLX", "NV-CONTROL", "DRI3", "Present", "Composite" };
    GString *s = g_string_new (NULL);
    for (guint i = 0; i < G_N_ELEMENTS (xext); i++) {
        int op = 0, ev = 0, er = 0;
        g_string_append_printf (s, "%s%s %s", i ? ", " : "", xext[i],
                                XQueryExtension (X.xd, xext[i], &op, &ev, &er) ? "yes" : "no");
    }
    g_message ("present: X server: %s; compositing manager: %s", s->str,
               x11_composited () ? "running" : "none");
    g_string_free (s, TRUE);
}

/* XCheckIfEvent predicate: any event (x11_surface_swapped throws them all
 * away) */
static Bool egl_any_event (Display *d, XEvent *e, XPointer arg)
{
    (void) d; (void) e; (void) arg;
    return True;
}

/* render thread, after a swap */
static void x11_surface_swapped (WalpeSurface *s)
{
    (void) s;
    /* the EGL connection has no event loop: whatever the server sent it
     * anyway (MappingNotify, ...) would pile up in Xlib's queue. No
     * request, no system call: the queue is only looked at */
    if (X.egl_xd != X.xd && XEventsQueued (X.egl_xd, QueuedAlready) > 0) {
        /* Every monitor's thread comes by here on the one EGL connection.
         * Looking at the queue and taking an event were two calls
         * (XEventsQueued, XNextEvent): two threads that both saw the last
         * event both asked for it, and the one that came second waited
         * inside XNextEvent for the next event of a connection that hardly
         * ever gets one - its monitor stood still, and the daemon's quit
         * waited for the thread. XCheckIfEvent takes an event if there is
         * one and returns if there is none, in one call under Xlib's lock.
         * The look above is what a swap cost before; only when something
         * is queued does the last call here read the connection once
         * (without waiting). */
        XEvent e;
        while (XCheckIfEvent (X.egl_xd, &e, egl_any_event, NULL)) {
        }
    }
}

/* ------------------------------------------------------------ surfaces */

static WalpeSurface *x11_surface_new (const WalpeSurfaceSpec *spec, EGLDisplay dpy, EGLConfig conf,
                                      EGLSurface *egl_surface, int *w, int *h)
{
    X11Surface *s = g_new0 (X11Surface, 1);
    s->kind = spec->kind;
    s->egl_dpy = dpy;
    s->user = spec->user;
    if (spec->kind == WALPE_SURFACE_WALLPAPER && spec->output) {
        s->geom = (XRect) { spec->output->x, spec->output->y, spec->output->width,
                            spec->output->height };
        s->out_id = spec->output->id;
        s->index = (int) spec->output->id - 1;
        s->managed = X.managed;
        s->xid = create_desktop_window (&s->geom, X.managed ? X11_WIN_MANAGED : X11_WIN_OVERRIDE);
    } else if (spec->kind == WALPE_SURFACE_WINDOW) {
        /* a normal window: the window manager places (and tiles) it */
        s->geom = (XRect) { 0, 0, spec->width, spec->height };
        s->out_id = 1;
        s->index = 0;
        s->xid = create_desktop_window (&s->geom, X11_WIN_PREVIEW);
    } else {
        /* a test frame: a window with the root's visual as the wallpaper
         * windows have it, never mapped */
        XSetWindowAttributes swa = { .override_redirect = True };
        s->geom = (XRect) { 0, 0, MAX (1, spec->width), MAX (1, spec->height) };
        s->xid = XCreateWindow (X.xd, X.root, 0, 0, (unsigned) s->geom.width,
                                (unsigned) s->geom.height, 0, CopyFromParent, InputOutput,
                                CopyFromParent, CWOverrideRedirect, &swa);
        XSync (X.xd, False);
    }
    if (spec->kind != WALPE_SURFACE_PROBE && X.egl_xd != X.xd)
        XSync (X.xd, False);    /* EGL names the window on its own connection */
    s->egl = eglCreateWindowSurface (dpy, conf, (EGLNativeWindowType) s->xid, NULL);
    if (s->egl == EGL_NO_SURFACE) {
        /* (eglGetError () stays what it is for the caller's message: no
         * EGL call follows) */
        XDestroyWindow (X.xd, s->xid);
        g_free (s);
        return NULL;
    }
    if (spec->kind != WALPE_SURFACE_PROBE)
        g_ptr_array_add (X.surfs, s);
    *egl_surface = s->egl;
    *w = s->geom.width;
    *h = s->geom.height;
    return (WalpeSurface *) s;
}

static void x11_surface_free (WalpeSurface *ws)
{
    X11Surface *s = (X11Surface *) ws;
    if (s->adopt_id)
        g_source_remove (s->adopt_id);
    eglDestroySurface (s->egl_dpy, s->egl);
    XDestroyWindow (X.xd, s->xid);
    g_ptr_array_remove (X.surfs, s);
    g_free (s);
}

static void x11_surface_size (WalpeSurface *ws, int *w, int *h)
{
    X11Surface *s = (X11Surface *) ws;
    *w = s->geom.width;
    *h = s->geom.height;
}

static guint32 x11_surface_output (WalpeSurface *ws)
{
    return ((X11Surface *) ws)->out_id;
}

static unsigned long x11_surface_native_id (WalpeSurface *ws)
{
    return (unsigned long) ((X11Surface *) ws)->xid;
}

/* ------------------------------------------------------- pointer, input */

/* one XQueryPointer on the root */
static gboolean x11_pointer (WalpePointer *out)
{
    Window r, c;
    int rx, ry, wx, wy;
    unsigned mask = 0;
    if (!XQueryPointer (X.xd, X.root, &r, &c, &rx, &ry, &wx, &wy, &mask))
        return FALSE;
    out->x = rx;
    out->y = ry;
    out->inside = TRUE;
    out->under = (guintptr) c;
    return TRUE;
}

static gboolean x11_on_desktop (guintptr under)
{
    return pointer_on_desktop ((Window) under);
}

static void x11_input_select (gboolean buttons, gboolean motion)
{
    xi_want_buttons = buttons;
    xi_want_motion = motion;
    xi2_apply_mask ();
}

static gboolean x11_has_input_events (void)
{
    return xi2_opcode >= 0;
}

/* ---------------------------------------------------------- life cycle */

static gboolean x11_open (const WalpePlatformOpts *o, char **err)
{
    memset (&X, 0, sizeof (X));
    X.cfg = o->cfg;
    X.ev = o->events;
    X.user = o->user;
    X.windowed = o->windowed;
    X.probe = o->probe;
    X.surfs = g_ptr_array_new ();
    if (o->probe) {
        /* a test child: one connection and Xlib's own error handlers (an
         * X error ends the child, which is the answer of the test) */
        X.xd = XOpenDisplay (NULL);
        if (!X.xd) {
            *err = g_strdup ("cannot open the X display");
            return FALSE;
        }
        X.root = DefaultRootWindow (X.xd);
        return TRUE;
    }
    /* render threads present through EGL while the main thread polls its
     * Display (XPending / XQueryPointer): Xlib must be thread-safe */
    XInitThreads ();
    XSetErrorHandler (walpe_xerror); /* never let a BadWindow kill us */
    xio_sock_path = g_strdup (o->socket_path);
    xio_marker_path = g_strdup (o->crash_marker_path);
    XSetIOErrorHandler (walpe_xio_error);
    X.xd = XOpenDisplay (NULL);
    /* a test child of this program may just have been an X client of its
     * own. Where it was the only one (nothing else running yet, Xvfb in
     * tests), a server without -noreset resets itself when that client
     * leaves and refuses connections for a moment */
    for (int i = 0; !X.xd && o->wait_for_display && i < 30; i++) {
        g_usleep (100 * 1000);
        X.xd = XOpenDisplay (NULL);
    }
    if (!X.xd) {
        *err = g_strdup ("cannot open X display");
        return FALSE;
    }
    X.root = DefaultRootWindow (X.xd);
    /* EGL and the render threads get an X connection of their own. On a
     * shared one every buffer swap's traffic (replies, events of the GL
     * driver) made the socket readable and woke the main loop, which then
     * found nothing of its own: ~200 wake-ups a second with one scene at
     * 60 fps on two monitors, and the swaps' round trips parked main-loop
     * events in Xlib's queue (late clicks). WALPE_ONE_XCONN=1 = one
     * connection for everything, as before. */
    X.egl_xd = g_getenv ("WALPE_ONE_XCONN") ? NULL : XOpenDisplay (NULL);
    if (!X.egl_xd)
        X.egl_xd = X.xd;
    /* where the wallpaper windows go on a desktop with a desktop window of
     * its own (x11desk.h) */
    const char *desk = g_getenv ("WALPE_X11_DESKTOP");
    if (walpe_x11_desk_parse (desk, &X.desk_mode) < 0)
        g_warning ("x11: WALPE_X11_DESKTOP=%s: not a word this program knows (auto, root, "
                   "window); taken as auto", desk);
    else if (X.desk_mode != WALPE_X11_DESK_AUTO)
        g_message ("x11: WALPE_X11_DESKTOP=%s", walpe_x11_desk_mode_name (X.desk_mode));
    int shape_ev, shape_err;
    have_shape = XShapeQueryExtension (X.xd, &shape_ev, &shape_err);
    /* what kind of window the wallpaper windows are (x11desk.h) */
    const char *win = g_getenv ("WALPE_X11_WINDOW");
    if (walpe_x11_win_parse (win, &X.win_mode) < 0)
        g_warning ("x11: WALPE_X11_WINDOW=%s: not a word this program knows (auto, override, "
                   "managed); taken as auto", win);
    else if (X.win_mode != WALPE_X11_WIN_AUTO)
        g_message ("x11: WALPE_X11_WINDOW=%s", walpe_x11_win_mode_name (X.win_mode));
    /* the window manager: is there one, and its name (the windows are
     * made after this) */
    X.a_wmcheck = XInternAtom (X.xd, "_NET_SUPPORTING_WM_CHECK", False);
    X.wm_desk = -1;
    X.wm_name = wm_name_read (&X.has_wm);
    X.managed = !X.windowed && walpe_x11_win_managed (X.win_mode, X.wm_name);
    if (X.managed)
        g_message ("x11: the wallpaper windows are managed windows of the desktop type%s%s%s: this "
                   "window manager paints an override-redirect window above every other window. "
                   "WALPE_X11_WINDOW=override makes them override-redirect as on other window "
                   "managers", X.win_mode == WALPE_X11_WIN_MANAGED ? " (WALPE_X11_WINDOW=managed)" : "",
                   X.wm_name ? "; window manager: " : "", X.wm_name ? X.wm_name : "");
    return TRUE;
}

/* "wm=yes"; on a desktop with a desktop window of its own (x11desk.h) also
 * that window and where the wallpaper windows are because of it - the
 * line of a bug report ("walpe-ctl diag") that says why the icons are
 * gone, or why no wallpaper is seen */
static char *x11_describe (void)
{
    GString *s = g_string_new (NULL);
    g_string_append_printf (s, "wm=%s", X.has_wm ? "yes" : "no");
    for (guint i = 0; i < X.n_desks; i++) {
        char *cls = window_class_name ((Window) X.desks[i].client);
        guint in = 0, n = 0;
        for (guint k = 0; X.surfs && k < X.surfs->len; k++) {
            n += SURF (k)->kind == WALPE_SURFACE_WALLPAPER ? 1 : 0;
            in += SURF (k)->host == (Window) X.desks[i].client ? 1 : 0;
        }
        g_string_append_printf (s, "; desktop window 0x%lx (%s, %dx%d+%d+%d, %d-bit): %u of %u "
                                "wallpaper window(s) in it", X.desks[i].client, cls,
                                X.desks[i].r.width, X.desks[i].r.height, X.desks[i].r.x,
                                X.desks[i].r.y, X.desks[i].depth, in, n);
        g_free (cls);
    }
    if (X.n_desks || X.desk_mode != WALPE_X11_DESK_AUTO)
        g_string_append_printf (s, " [WALPE_X11_DESKTOP: %s%s]", walpe_x11_desk_mode_name (X.desk_mode),
                                X.n_desks && X.desk_mode == WALPE_X11_DESK_AUTO && !wm_knows_desktop ()
                                    ? "; the window manager does not list the desktop type in "
                                      "_NET_SUPPORTED: the wallpaper windows stay on the root" : "");
    if (X.managed || X.win_mode != WALPE_X11_WIN_AUTO)
        g_string_append_printf (s, "; wallpaper windows: %s%s%s%s [WALPE_X11_WINDOW: %s]",
                                X.managed ? "managed, desktop type" : "override-redirect",
                                X.wm_name ? " (window manager: " : "", X.wm_name ? X.wm_name : "",
                                X.wm_name ? ")" : "", walpe_x11_win_mode_name (X.win_mode));
    return g_string_free (s, FALSE);
}

static void x11_start (gboolean layout_events, const WalpeSignalSink *sink, gpointer sink_user)
{
    /* StructureNotify: the root's own ConfigureNotify (screen resized) */
    XSelectInput (X.xd, X.root, PropertyChangeMask | SubstructureNotifyMask | StructureNotifyMask);
#ifdef HAVE_XRANDR
    {
        int rerr = 0;
        if (layout_events && XRRQueryExtension (X.xd, &rr_event_base, &rerr))
            XRRSelectInput (X.xd, X.root, RRScreenChangeNotifyMask);
        else
            rr_event_base = -1;
    }
#else
    (void) layout_events;
#endif
#ifdef HAVE_XI2
    xi2_init ();
#endif
    x_event_source_add ();   /* socket + Xlib's queue (not a bare fd watch) */
    /* the providers first (this backend's own does nothing when it
     * starts), then the first look at the windows: each of the two scans
     * tells the daemon by itself what it found */
    walpe_signals_register (&x11_provider);
    walpe_signals_start (WALPE_BACKEND_X11, sink, sink_user);
    update_active_window ();
    recompute_covered ();
}

/* No periodic timer runs once the daemon is up (there were four: pointer
 * recheck 1 s, display sleep 1 s, cover re-scan 2 s, re-lower 5 s): the
 * stacking order is looked at once now, then after events that can mean a
 * window got below a wallpaper. */
static void x11_settle (void)
{
    X.stack_dirty = !X.windowed;
    restack_check ();
}

static void x11_sync (void)
{
    XSync (X.xd, False);
}

static void x11_close (void)
{
    if (X.egl_xd && X.egl_xd != X.xd)
        XCloseDisplay (X.egl_xd);
    if (X.xd)
        XCloseDisplay (X.xd);
    X.xd = X.egl_xd = NULL;
    if (X.surfs)
        g_ptr_array_unref (X.surfs);
    X.surfs = NULL;
    g_clear_pointer (&X.layout, g_free);
    X.n_layout = 0;
    g_clear_pointer (&X.desks, g_free);
    X.n_desks = 0;
    g_clear_pointer (&X.wm_name, g_free);
    if (X.wm_timer_id)
        g_source_remove (X.wm_timer_id);
    X.wm_timer_id = 0;
}

/* requests the main thread has sent to the X server since start (its own
 * connection; a status call adds a few) */
static void x11_status (JsonBuilder *b)
{
    json_builder_set_member_name (b, "x_requests");
    json_builder_add_int_value (b, (gint64) NextRequest (X.xd) - 1);
    /* a desktop's own desktop window (x11desk.h): the setting, the windows
     * of that kind the last scan found, and per wallpaper window the
     * window it is a child of ("root", or the desktop window's id) */
    /* the kind of the wallpaper windows and the window manager it follows */
    json_builder_set_member_name (b, "x11_window");
    json_builder_add_string_value (b, X.managed ? "managed" : "override");
    json_builder_set_member_name (b, "x11_window_manager");
    json_builder_add_string_value (b, X.wm_name ? X.wm_name : "");
    json_builder_set_member_name (b, "x11_desktop");
    json_builder_add_string_value (b, walpe_x11_desk_mode_name (X.desk_mode));
    json_builder_set_member_name (b, "x11_desktop_windows");
    json_builder_begin_array (b);
    for (guint i = 0; i < X.n_desks; i++) {
        char *d = g_strdup_printf ("0x%lx %dx%d+%d+%d depth %d", X.desks[i].client,
                                   X.desks[i].r.width, X.desks[i].r.height, X.desks[i].r.x,
                                   X.desks[i].r.y, X.desks[i].depth);
        json_builder_add_string_value (b, d);
        g_free (d);
    }
    json_builder_end_array (b);
    json_builder_set_member_name (b, "x11_wallpaper_parent");
    json_builder_begin_array (b);
    for (guint i = 0; X.surfs && i < X.surfs->len; i++) {
        if (SURF (i)->host == None) {
            json_builder_add_string_value (b, "root");
        } else {
            char *d = g_strdup_printf ("0x%lx", (unsigned long) SURF (i)->host);
            json_builder_add_string_value (b, d);
            g_free (d);
        }
    }
    json_builder_end_array (b);
}

/* -------------------------------------------------------- pause signals
 * The signals X11 can tell (signals.h). The scans run when their
 * events arrive (xdata_ready); the registry only asks for another look
 * when a setting they read was changed. Display sleep is watched from the
 * first time it is asked about: the daemon does that at the point of its
 * start-up where the watch was always set up, and asks for the first
 * state at the end of it. */
static gboolean x11_provider_start (const WalpeSignalProvider *self, char **why)
{
    (void) self; (void) why;
    return TRUE;
}

static void x11_provider_recheck (const WalpeSignalProvider *self, WalpeSignal sig, gboolean now)
{
    (void) self;
    if (!X.xd || X.probe)
        return;
    switch (sig) {
    case WALPE_SIG_FULLSCREEN:
    case WALPE_SIG_FULLSCREEN_ON:
        if (now)
            update_active_window ();
        break;
    case WALPE_SIG_ASLEEP:
        if (!X.sleep_watching) {
            X.sleep_watching = TRUE;
            sleep_watch_init ();
        }
        if (now)
            sleep_recheck ();
        break;
    case WALPE_SIG_COVERED:
        if (now)
            recompute_covered ();
        else
            cover_schedule ();
        break;
    default:
        break;
    }
}

static const WalpeSignalProvider x11_provider = {
    .name = "x11",
    .signals = WALPE_SIG_BIT (WALPE_SIG_FULLSCREEN) | WALPE_SIG_BIT (WALPE_SIG_ASLEEP) |
               WALPE_SIG_BIT (WALPE_SIG_COVERED) | WALPE_SIG_BIT (WALPE_SIG_FOCUS_APP) |
               WALPE_SIG_BIT (WALPE_SIG_FULLSCREEN_ON),
    .backends = 1u << WALPE_BACKEND_X11,
    .start = x11_provider_start,
    .stop = NULL,
    .recheck = x11_provider_recheck,
};

const WalpePlatformOps walpe_x11_ops = {
    .name = "x11",
    .backend = WALPE_BACKEND_X11,
    .open = x11_open,
    .close = x11_close,
    .describe = x11_describe,
    .start = x11_start,
    .settle = x11_settle,
    .sync = x11_sync,
    .outputs = x11_outputs,
    .egl_display = x11_egl_display,
    .egl_note = x11_egl_note,
    .egl_visual = x11_egl_visual,
    .egl_config_usable = x11_egl_config_usable,
    .paces = FALSE,
    .surface_new = x11_surface_new,
    .surface_free = x11_surface_free,
    .surface_size = x11_surface_size,
    .surface_output = x11_surface_output,
    .surface_native_id = x11_surface_native_id,
    .surface_swapped = x11_surface_swapped,
    .pointer = x11_pointer,
    .on_desktop = x11_on_desktop,
    .input_select = x11_input_select,
    .has_input_events = x11_has_input_events,
    .composited = x11_composited,
    .present_log = x11_present_log,
    .status = x11_status,
};
