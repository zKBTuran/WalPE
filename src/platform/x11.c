/* x11.c - the X11 backend (platform/impl.h).
 *
 * One override-redirect window per monitor at the bottom of the stacking
 * order, input shaped away; each window is the EGL surface its monitor's
 * render thread draws on. Around that: the monitor layout (Xinerama,
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
    gboolean has_wm;         /* EWMH manager present (checked at startup) */
    GPtrArray *surfs;        /* X11Surface *: the wallpaper windows, in creation order */
    gboolean fs_active;      /* fullscreen window has focus */
    Window fs_win;           /* that window (None while fs_active is off) */
    Window fs_placed;        /* the window the monitors' fs_here was worked out for */
    gboolean fs_everywhere;  /* ... and where it is could not be found out: it
                              * counts for every monitor */
    gboolean asleep;         /* DPMS power-save or screensaver active */
    Window watched_win;      /* active window we subscribed to */
    guint cover_timer_id;    /* debounced coverage recompute */
    gboolean stack_dirty;    /* a window may have got below a wallpaper window */
    GHashTable *watched_clients; /* client windows with PropertyChangeMask selected */
    Atom cm_atom;            /* _NET_WM_CM_S<screen> (x11_present_log) */
    gboolean sleep_watching; /* sleep_watch_init ran */
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
static gboolean window_is_normal_visible (Window w)
{
    static Atom a_type, a_normal, a_dialog, a_state, a_hidden, a_wmstate;
    if (!a_type) {
        a_type = XInternAtom (X.xd, "_NET_WM_WINDOW_TYPE", False);
        a_normal = XInternAtom (X.xd, "_NET_WM_WINDOW_TYPE_NORMAL", False);
        a_dialog = XInternAtom (X.xd, "_NET_WM_WINDOW_TYPE_DIALOG", False);
        a_state = XInternAtom (X.xd, "_NET_WM_STATE", False);
        a_hidden = XInternAtom (X.xd, "_NET_WM_STATE_HIDDEN", False);
        a_wmstate = XInternAtom (X.xd, "WM_STATE", False);
    }
    guchar *d = NULL;
    gulong n = window_prop32 (w, a_type, XA_ATOM, 16, &d);
    if (d) {
        gboolean normal = FALSE;
        for (gulong i = 0; i < n; i++)
            normal |= ((Atom *) d)[i] == a_normal || ((Atom *) d)[i] == a_dialog;
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
    Window *tree_clients = NULL;
    if (!clients) {
        Window r2, parent;
        guint tn = 0;
        if (XQueryTree (X.xd, X.root, &r2, &parent, &tree_clients, &tn) && tree_clients) {
            clients = tree_clients;
            n = tn;
        }
    }

    /* each client is asked about once per scan (attributes, root position,
     * type / state / opacity), not once per monitor: the windows that can
     * hide a wallpaper, as rectangles in root coordinates */
    XRect *wins = g_new (XRect, n ? n : 1);
    guint nw = 0;
    XRect focus = { 0, 0, 0, 0 };   /* the focused application window */
    gboolean have_focus = FALSE;
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
        if (!XTranslateCoordinates (X.xd, clients[i], X.root, 0, 0, &ox, &oy, &junk0))
            continue;
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
        if (!window_is_normal_visible (clients[i]))
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
    if (data)
        XFree (data);
    if (tree_clients)
        XFree (tree_clients);
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

/* the wallpaper window: override-redirect so no WM (dwm tiles root children,
 * desktops restack) can manage, tile or raise it; input shaped away so
 * clicks/keys reach whatever is below. This window IS the EGL surface. */
static Window create_desktop_window (const XRect *g, gboolean or_mode)
{
    XSetWindowAttributes swa = {
        .override_redirect = or_mode ? True : False,
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
    if (or_mode) {
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

/* keep the wallpaper window at the bottom of the stack; re-assert often */
static void sink_desktop_window (X11Surface *m)
{
    if (m->xid == None)
        return;
    shape_away_input (m->xid);
    XLowerWindow (X.xd, m->xid);
}

/* are the wallpaper windows the lowest children of the root already?
 * (XQueryTree lists them bottom first) */
static gboolean wallpapers_at_bottom (void)
{
    Window r, p, *kids = NULL;
    guint n = 0, nm = X.surfs->len;
    if (!XQueryTree (X.xd, X.root, &r, &p, &kids, &n))
        return FALSE;
    gboolean ok = n >= nm;
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
    WalpeFsRect *mons = g_new0 (WalpeFsRect, n + 1);
    gboolean *on = g_new0 (gboolean, n + 1);
    for (guint k = 0; k < n; k++) {
        const XRect *g = &SURF (k)->geom;
        mons[k] = (WalpeFsRect) { g->x, g->y, g->width, g->height };
    }
    walpe_fs_decide (X.fs_active, known ? &win : NULL, mons, (int) n, on, &everywhere);
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
    gboolean fs = window_is_fullscreen (active);
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
            if (e.xproperty.window == X.root) {
                if (at == a_active)
                    update_active_window ();
                else if (at == a_clients)
                    cover_schedule (); /* clients appeared/vanished */
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
 * clicks on windows (and on bars/panels) do not. */
static gboolean pointer_on_desktop (Window child)
{
    if (child == None || child == X.root)
        return TRUE;
    for (guint i = 0; i < X.surfs->len; i++)
        if (SURF (i)->xid == child)
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
    g_free (geoms);
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
        s->xid = create_desktop_window (&s->geom, TRUE);
    } else if (spec->kind == WALPE_SURFACE_WINDOW) {
        /* a normal window: the window manager places (and tiles) it */
        s->geom = (XRect) { 0, 0, spec->width, spec->height };
        s->out_id = 1;
        s->index = 0;
        s->xid = create_desktop_window (&s->geom, FALSE);
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
    int shape_ev, shape_err;
    have_shape = XShapeQueryExtension (X.xd, &shape_ev, &shape_err);
    Atom wmcheck = XInternAtom (X.xd, "_NET_SUPPORTING_WM_CHECK", True);
    if (wmcheck != None) {
        guchar *data = NULL;
        gulong n = window_prop32 (X.root, wmcheck, XA_WINDOW, 1, &data);
        if (data) {
            Window w = n == 1 ? *(Window *) data : None;
            XFree (data);
            XWindowAttributes wa;
            X.has_wm = (w != None && XGetWindowAttributes (X.xd, w, &wa));
        }
    }
    return TRUE;
}

static char *x11_describe (void)
{
    return g_strdup_printf ("wm=%s", X.has_wm ? "yes" : "no");
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
}

/* requests the main thread has sent to the X server since start (its own
 * connection; a status call adds a few) */
static void x11_status (JsonBuilder *b)
{
    json_builder_set_member_name (b, "x_requests");
    json_builder_add_int_value (b, (gint64) NextRequest (X.xd) - 1);
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
