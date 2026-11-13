/* platform.h - the window system behind the daemon.
 *
 * The daemon renders with EGL into one surface per monitor and knows
 * nothing of Xlib or libwayland: outputs, the wallpaper surfaces, how a
 * frame is presented and paced, the pointer and the pause signals come
 * through this interface. Two backends implement it: platform/x11.c
 * (override-redirect windows below everything, EWMH, RandR, XInput2 - the
 * code that used to live in daemon/main.c, unchanged in what it does) and
 * platform/wayland.c (one zwlr_layer_shell_v1 background surface per
 * wl_output). Which one runs is decided once at start-up
 * (common/backend.h); a build may hold only one of them.
 *
 * Threading: unless a function says "render thread", it is called on the
 * main thread (the thread of the default GMainContext), and every event
 * callback runs there. The three presentation hooks (walpe_surface_*swap*,
 * walpe_surface_set_interval) are called on the render thread that owns
 * the surface's EGL context.
 *
 * Layout changes: surfaces are made for the outputs found at start-up.
 * When outputs come, go or change, the backend says so (outputs_changed);
 * the daemon reads the list again and, when it really differs, restarts
 * itself in place. A backend whose surfaces can stay while their output
 * changes (walpe_platform_follows_outputs: Wayland) lets the daemon follow
 * the change without that restart ("following an output in place" below);
 * X11 has none of it. */
#pragma once
#include <glib.h>
#include <math.h>
#include <string.h>
#include <json-glib/json-glib.h>
#include <epoxy/egl.h>
#include "common/backend.h"
#include "common/config.h"
#include "platform/signals.h"

/* ---------------------------------------------------------------- outputs */

/* wl_output.transform numbering; X11 reports 0 (a rotated RandR output
 * already has a rotated rectangle) */
enum {
    WALPE_TRANSFORM_NORMAL = 0, WALPE_TRANSFORM_90, WALPE_TRANSFORM_180, WALPE_TRANSFORM_270,
    WALPE_TRANSFORM_FLIPPED, WALPE_TRANSFORM_FLIPPED_90, WALPE_TRANSFORM_FLIPPED_180,
    WALPE_TRANSFORM_FLIPPED_270,
};

typedef struct {
    /* Identifies the output for as long as this layout lasts (surfaces and
     * per-output signals carry it). Never 0. X11: its index in the list
     * + 1. Wayland: the wl_registry name of its wl_output. Not stable
     * across restarts: settings are keyed by `name`. */
    guint32 id;
    /* The connector name as the window system reports it: the key of the
     * per-monitor settings. X11: the RandR 1.5 monitor whose rectangle
     * this is ("HDMI-0", "DP-2"), WALPE_MONITORS "NAME=", else "monN".
     * Wayland: wl_output.name / xdg_output.name ("DP-1", "HDMI-A-1"), else
     * "monN". Never NULL. */
    char *name;
    char *canon;            /* walpe_output_name_canon (name): the same on both
                             * backends where only the spelling differs */
    /* what the monitor says about itself; NULL = not known. Wayland:
     * wl_output.geometry make / model, wl_output.description. X11: not
     * filled yet (it would take the EDID property of the RandR output). */
    char *make, *model, *serial, *description;
    /* the rectangle the daemon renders for it, in pixels. The rectangles
     * of one list never overlap; pointer positions are given in the same
     * space. X11: root-window coordinates. Wayland: sizes are the output's
     * video mode (after its transform), positions the compositor's layout
     * scaled to pixels, or - where outputs of different scales would then
     * overlap - one row from left to right. */
    int x, y, width, height;
    /* the compositor's own layout units (X11: the same as above) */
    int logical_x, logical_y, logical_width, logical_height;
    double refresh_hz;      /* 0 = unknown */
    double scale;           /* pixels per layout unit; 1.0 on X11 */
    int transform;          /* WALPE_TRANSFORM_* */
} WalpeOutput;

typedef struct {
    int n;
    WalpeOutput *o;
} WalpeOutputList;

void walpe_output_list_free (WalpeOutputList *l);            /* clears *l, does not free l */
void walpe_output_list_copy (WalpeOutputList *dst, const WalpeOutputList *src);
/* same rectangles, names, refresh rates (within 0.5 Hz), scales and
 * transforms, in the same order */
gboolean walpe_output_list_equal (const WalpeOutputList *a, const WalpeOutputList *b);
/* the entry called `name`: exact first, then by canonical name; NULL = none */
const WalpeOutput *walpe_output_list_find (const WalpeOutputList *l, const char *name);
/* the id of the list's only output; 0 = it has none, or more than one.
 * For whoever knows "the output" without a name (a Wayland compositor
 * whose wl_output is older than version 4 tells none): with one output
 * there is no doubt which one is meant, with several nobody can tell. */
guint32 walpe_output_list_only_id (const WalpeOutputList *l);

/* What the daemon does when the window system says its outputs changed:
 * the decision as a pure function (daemon/main.c layout_check uses it,
 * tests/outputs_test.c walks it through hotplug sequences).
 *   running - the layout the daemon's surfaces were made for
 *   seen    - the outputs now
 *   pending - a different layout seen at the look before (n = 0: none)
 *   force   - a surface was taken away: `running` is not what is on
 *             screen even if `seen` reads the same
 *   too_soon - the last restart was a moment ago;  busy - an import or a
 *             download would be cut off
 * SAME: nothing to do (forget pending). WAIT: remember `seen` as pending
 * and look again (a change must hold for two looks: the window system
 * goes through passing states). RESTART: restart in place to follow it.
 * *settled (may be NULL): seen equals pending. */
typedef enum { WALPE_LAYOUT_SAME, WALPE_LAYOUT_WAIT, WALPE_LAYOUT_RESTART } WalpeLayoutVerdict;

static inline WalpeLayoutVerdict walpe_layout_verdict (const WalpeOutputList *running,
                                                       const WalpeOutputList *seen,
                                                       const WalpeOutputList *pending,
                                                       gboolean force, gboolean too_soon,
                                                       gboolean busy, gboolean *settled)
{
    gboolean held = pending->n && walpe_output_list_equal (seen, pending);
    if (settled)
        *settled = held;
    if (walpe_output_list_equal (seen, running) && !force)
        return WALPE_LAYOUT_SAME;
    if (!held || too_soon || busy)
        return WALPE_LAYOUT_WAIT;
    return WALPE_LAYOUT_RESTART;
}

/* What became of each output between two lists, for a daemon that follows
 * a change in place instead of restarting: the monitors it runs with
 * (`running`) against the outputs now (`seen`). Two entries are the same
 * output when id and name are equal - the id is the window system's own
 * handle for it and lasts as long as the output does; an output that was
 * disabled and enabled again is a new one that may carry the old name.
 * A pure function (tests/outputs_test.c). */
typedef enum {
    WALPE_FOLLOW_KEEP,      /* nothing of it changed */
    WALPE_FOLLOW_PLACE,     /* the same pixels: its position, scale, refresh rate or
                             * transform differ */
    WALPE_FOLLOW_RESIZE,    /* its size in pixels differs */
    WALPE_FOLLOW_GONE,      /* (an entry of `running`) the output is not there any more */
    WALPE_FOLLOW_NEW,       /* (an entry of `seen`) no running monitor is on it */
} WalpeFollowKind;

typedef struct {
    int n_running, n_seen;
    WalpeFollowKind *running;   /* per entry of `running`: KEEP, PLACE, RESIZE or GONE */
    int *running_to;            /* ... and its index in `seen`, -1 when GONE */
    WalpeFollowKind *seen;      /* per entry of `seen`: as its running entry, or NEW */
    int *seen_from;             /* ... and its index in `running`, -1 when NEW */
    int kept, placed, resized, gone, added;     /* how many of each */
} WalpeFollowPlan;

static inline void walpe_follow_plan (const WalpeOutputList *running, const WalpeOutputList *seen,
                                      WalpeFollowPlan *plan)
{
    memset (plan, 0, sizeof (*plan));
    plan->n_running = running->n;
    plan->n_seen = seen->n;
    plan->running = g_new0 (WalpeFollowKind, running->n + 1);
    plan->running_to = g_new0 (int, running->n + 1);
    plan->seen = g_new0 (WalpeFollowKind, seen->n + 1);
    plan->seen_from = g_new0 (int, seen->n + 1);
    for (int j = 0; j < seen->n; j++) {
        plan->seen[j] = WALPE_FOLLOW_NEW;
        plan->seen_from[j] = -1;
    }
    for (int i = 0; i < running->n; i++) {
        const WalpeOutput *x = &running->o[i];
        int at = -1;
        /* (a list never has an id twice; should one, the first entry
         * nobody has taken yet is the one) */
        for (int j = 0; j < seen->n && at < 0; j++)
            if (plan->seen_from[j] < 0 && seen->o[j].id == x->id &&
                !g_strcmp0 (seen->o[j].name, x->name))
                at = j;
        plan->running_to[i] = at;
        if (at < 0) {
            plan->running[i] = WALPE_FOLLOW_GONE;
            plan->gone++;
            continue;
        }
        const WalpeOutput *y = &seen->o[at];
        WalpeFollowKind k = WALPE_FOLLOW_KEEP;
        if (x->width != y->width || x->height != y->height)
            k = WALPE_FOLLOW_RESIZE;
        else if (x->x != y->x || x->y != y->y || fabs (x->refresh_hz - y->refresh_hz) > 0.5 ||
                 fabs (x->scale - y->scale) > 0.001 || x->transform != y->transform)
            k = WALPE_FOLLOW_PLACE;
        plan->running[i] = plan->seen[at] = k;
        plan->seen_from[at] = i;
        if (k == WALPE_FOLLOW_RESIZE)
            plan->resized++;
        else if (k == WALPE_FOLLOW_PLACE)
            plan->placed++;
        else
            plan->kept++;
    }
    for (int j = 0; j < seen->n; j++)
        plan->added += plan->seen_from[j] < 0;
}

static inline void walpe_follow_plan_clear (WalpeFollowPlan *plan)
{
    g_free (plan->running);
    g_free (plan->running_to);
    g_free (plan->seen);
    g_free (plan->seen_from);
    memset (plan, 0, sizeof (*plan));
}

/* ----------------------------------------------------------------- events */

typedef struct WalpeSurface WalpeSurface;

typedef struct {
    /* outputs were added, removed or changed (debounce and compare:
     * intermediate states are announced too) */
    void (*outputs_changed) (gpointer user);
    /* a part of the surface lost its pixels and must be drawn again (X11
     * Expose). surface_user = WalpeSurfaceSpec.user */
    void (*surface_exposed) (gpointer surface_user, gpointer user);
    /* the window system took the surface away (Wayland: layer surface
     * closed, its output gone; a --windowed window was closed). Nothing
     * can be presented on it any more. */
    void (*surface_closed) (gpointer surface_user, gpointer user);
    /* the pointer moved while motion was asked for (walpe_platform_input_select) */
    void (*pointer_motion) (gpointer user);
    /* windows were moved, mapped, restacked or closed: what is under a
     * pointer that has not moved may be something else now. Optional for
     * a backend whose pointer position already follows such changes
     * (Wayland: enter / leave events). */
    void (*windows_changed) (gpointer user);
    /* a button went down or up. button: 1 left, 2 middle, 3 right, 4-7 one
     * wheel click up / down / left / right. button_wanted is asked first
     * and can save the backend the work of finding the position.
     * x, y: as WalpePointer; on_desktop: nothing but the wallpaper is under it. */
    gboolean (*button_wanted) (int button, gpointer user);
    void (*button) (int button, gboolean down, int x, int y, gboolean on_desktop, guint32 time_ms,
                    gpointer user);
    /* the connection to the window system is gone for good. X11 never
     * reports this (Xlib ends the process; the two files named in
     * WalpePlatformOpts are removed first). */
    void (*connection_lost) (gpointer user);
} WalpePlatformEvents;

/* ------------------------------------------------------------- life cycle */

typedef struct {
    WalpeConfig *cfg;           /* read by the backend, never written */
    gboolean windowed;          /* --windowed: one plain window instead of wallpapers */
    gboolean probe;             /* a short-lived test process: no events, no signals */
    gboolean wait_for_display;  /* the display may refuse connections for a moment
                                 * (X11: right after another client of this
                                 * program left a server without -noreset) */
    int reconnect_wait_s;       /* Wayland: how long to wait for the compositor's
                                 * socket to (re)appear before giving up; 0 = not */
    /* files to remove when the connection dies under the process (X11) */
    const char *crash_marker_path, *socket_path;
    const WalpePlatformEvents *events;
    gpointer user;
} WalpePlatformOpts;

/* which backends this build holds */
gboolean walpe_platform_built (WalpeBackend b);

/* A first look at the Wayland compositor named by the environment, on a
 * short connection of its own that is closed again before this returns
 * (main thread; fine before any other thread exists; makes no EGL call).
 * The automatic backend choice and the GPU choice read it. */
typedef struct {
    gboolean connected;     /* FALSE: no connection (or no Wayland backend in this build) */
    gboolean layer_shell;   /* zwlr_layer_shell_v1 is offered: wallpapers are possible */
    gboolean xdg_shell;     /* xdg_wm_base is offered: --windowed is possible */
    /* no layer shell, and the compositor looks like GNOME's: the WalPE
     * extension for GNOME Shell answers and wallpapers are possible as
     * ordinary windows (wl_gnome.h); where it does not, the sentence that
     * says how to get it (static; NULL on other compositors) */
    gboolean desk_windows;
    const char *no_shell_hint;
    char *display_node;     /* the DRM device the compositor composes with
                             * ("/dev/dri/card1"), NULL = not told (g_free) */
} WalpePlatformProbe;
void walpe_platform_probe_wayland (WalpePlatformProbe *out);

/* The names of the RandR providers of the X server named by the
 * environment ("NVIDIA-0", "modesetting", "NVIDIA-G0" ...), asked on a
 * short connection of its own that is closed again before this returns
 * (main thread; fine before any other thread exists; makes no EGL call).
 * g_strfreev. NULL: nothing answered, the server has no RandR 1.4, or
 * this build has no X11 backend or no libXrandr. The GPU choice reads it
 * (common/gpusel.h walpe_gpu_x11_screen_is_nvidia), and only on a machine
 * where gpu = auto would otherwise start a test process.
 * WALPE_X11_PROVIDERS (tests): the names, separated by commas, in place
 * of the server's; set and empty = none. */
char **walpe_platform_x11_providers (void);

/* Connect. backend: WALPE_BACKEND_X11 or _WAYLAND (the result of
 * walpe_backend_choose). FALSE: *err says why (g_free). Must run before
 * any thread exists that touches the window system. */
gboolean walpe_platform_open (WalpeBackend backend, const WalpePlatformOpts *opts, char **err);
/* the backend that is open (WALPE_BACKEND_AUTO = none) */
WalpeBackend walpe_platform_backend (void);
/* one line about the display for the start-up log, e.g. "wm=yes" (X11) or
 * "compositor: layer-shell v4, xdg-output, viewporter" (g_free) */
char *walpe_platform_describe (void);
/* start delivering events and start the pause-signal providers; the
 * surfaces exist. layout_events: also announce output changes. */
void walpe_platform_start (gboolean layout_events, const WalpeSignalSink *sink, gpointer sink_user);
/* once, after the first wallpapers were assigned: put the surfaces where
 * they belong in the stacking order (X11: below everything) */
void walpe_platform_settle (void);
/* all surfaces are freed: disconnect */
void walpe_platform_close (void);
/* make sure the window system has processed everything sent so far */
void walpe_platform_sync (void);

/* ---------------------------------------------------------------- outputs */

/* the outputs now (a fresh look); fills *out (walpe_output_list_free) */
void walpe_platform_outputs (WalpeOutputList *out);

/* -------------------------------------------------------------------- EGL */

/* The EGLDisplay of the connection, not yet initialised (the caller calls
 * eglInitialize, and eglTerminate before walpe_platform_close).
 * EGL_NO_DISPLAY = none. Which GPU serves it was decided before, through
 * the environment the GPU choice set up; the backend adds nothing to it. */
EGLDisplay walpe_platform_egl_display (void);
/* the DRM device the window system composes with, when the open backend
 * knows it; NULL otherwise. Valid until walpe_platform_close. */
const char *walpe_platform_display_node (void);
/* text appended to the EGL line of the start-up log (", own X connection") */
const char *walpe_platform_egl_note (void);
/* EGL_NATIVE_VISUAL_ID a window config should have to be accepted by the
 * surfaces made here (X11: the root window's visual; Wayland: XRGB8888);
 * 0 = no preference */
unsigned long walpe_platform_egl_visual (void);
/* can a window surface really be made with this config? */
gboolean walpe_platform_egl_config_usable (EGLDisplay dpy, EGLConfig conf);
/* TRUE: frames are paced by the backend itself (Wayland frame callbacks),
 * so both "swap intervals" exist whatever the EGL config says */
gboolean walpe_platform_paces (void);

/* --------------------------------------------------------------- surfaces */

typedef enum {
    WALPE_SURFACE_WALLPAPER,    /* covers `output`, below every window, takes no input */
    WALPE_SURFACE_WINDOW,       /* a normal window of width x height (--windowed) */
    WALPE_SURFACE_PROBE,        /* never shown: width x height, for a test frame */
} WalpeSurfaceKind;

typedef struct {
    WalpeSurfaceKind kind;
    const WalpeOutput *output;  /* WALLPAPER: which one (copied, need not stay valid) */
    int width, height;          /* WINDOW / PROBE */
    const char *title;          /* WINDOW */
    gpointer user;              /* handed back in surface events */
} WalpeSurfaceSpec;

/* Three sizes, kept apart:
 *   output size  - WalpeOutput.width x height: the pixels the monitor shows.
 *   surface size - the size of the buffers of the EGL window surface: what
 *                  glViewport of the default framebuffer covers and what a
 *                  swap presents. After walpe_surface_new it is the output
 *                  size (WINDOW / PROBE: the size asked for, or what the
 *                  window system made of it).
 *   render size  - the size the daemon draws the wallpaper at. The
 *                  platform knows nothing of it. A daemon that renders
 *                  smaller than the surface scales its picture up itself -
 *                  or asks the backend to present smaller buffers
 *                  (walpe_surface_set_buffer_size), which a backend that
 *                  can scale on presentation does for free. */

/* Make the native window and its EGL window surface. *w, *h: the surface
 * size in pixels. NULL on failure; eglGetError () then still holds EGL's
 * reason when the EGL surface was what failed. */
WalpeSurface *walpe_surface_new (const WalpeSurfaceSpec *spec, EGLDisplay dpy, EGLConfig conf,
                                 EGLSurface *egl_surface, int *w, int *h);
/* destroys the EGL surface, then the native window. The surface's render
 * thread has ended. */
void walpe_surface_free (WalpeSurface *s);
/* the surface size now */
void walpe_surface_size (WalpeSurface *s, int *w, int *h);
/* Render thread, between two frames (after a swap, before the first GL
 * call of the next frame): from the next frame on the surface's buffers
 * are w x h pixels and the window system scales them to the output.
 * TRUE = done (walpe_surface_size answers w x h from now on). FALSE = this
 * backend cannot (X11: a window's buffers have the window's size); the
 * surface is unchanged and the caller scales its picture itself.
 * w, h < 1 = back to the output size. */
gboolean walpe_surface_set_buffer_size (WalpeSurface *s, int w, int h);
/* WalpeOutput.id of a WALLPAPER; for a WINDOW the id its per-output pause
 * signals are reported under (X11: 1), 0 = none; 0 for a PROBE */
guint32 walpe_surface_output (WalpeSurface *s);
/* a number that names the native window in logs (X11: the window id) */
unsigned long walpe_surface_native_id (WalpeSurface *s);
/* the wallpaper on this surface uses the pointer (position or clicks) or
 * not. Wayland: the surface takes pointer input only while it does; X11:
 * nothing (the pointer is read from the root window). */
void walpe_surface_want_pointer (WalpeSurface *s, gboolean want);

/* ---- following an output in place
 * TRUE: a WALLPAPER surface of this backend lasts as long as its output
 * does and can be given the output's new rectangle
 * (walpe_surface_follow_output), so the daemon need not restart when an
 * output moves, changes its scale, refresh rate or mode, goes or comes.
 * FALSE (X11, no backend): every change of the outputs is followed by a
 * restart in place. The functions of this section exist only in a build
 * that holds the Wayland backend (HAVE_WAYLAND), like the daemon's code
 * that calls them. */
gboolean walpe_platform_follows_outputs (void);
/* Main thread: `o` is what the surface's output is now (the same output:
 * the id the surface was made for). The backend takes the rectangle (the
 * pointer positions it reports lie in it), the scale and the refresh rate.
 * A new size in pixels is not applied here: the surface's render thread
 * does that with walpe_surface_fit; *w, *h (may be NULL): the surface size
 * it will then have. FALSE = the surface cannot go on (the window system
 * closed it, its output is gone, o is another output): nothing was
 * changed, *w and *h are left alone. */
gboolean walpe_surface_follow_output (WalpeSurface *s, const WalpeOutput *o, int *w, int *h);
/* Main thread: how long the WALLPAPER surfaces made from now on wait for
 * the window system's first answer (walpe_surface_new stands still that
 * long at most); 0 = the backend's own time, which is the one of the
 * start. For an output that is tried again after it got no surface. */
void walpe_platform_surface_wait (int ms);
/* Render thread, between two frames: give the surface's buffers the size
 * the output has since the last walpe_surface_follow_output. *w, *h: the
 * surface size from now on. FALSE = nothing to do or not possible; *w, *h
 * are the size it has. */
gboolean walpe_surface_fit (WalpeSurface *s, int *w, int *h);

/* ---- presentation, render thread
 * The daemon's "swap interval" (present.h): 1 = after a swap the thread
 * waits until the display took the frame, 0 = it does not. */
/* the surface is to run with interval iv from now on; returns the value
 * to hand to eglSwapInterval on this thread (X11: iv itself; Wayland: 0,
 * the wait is done in walpe_surface_swapped) */
int walpe_surface_set_interval (WalpeSurface *s, int iv);
/* right before eglSwapBuffers of the surface */
void walpe_surface_pre_swap (WalpeSurface *s);
/* right after a successful swap; may block for interval 1, but never for
 * long: a surface the window system does not show must not hold its
 * thread (the backend reports it as WALPE_SIG_COVERED instead) */
void walpe_surface_swapped (WalpeSurface *s);

/* what the window system said about the frames it was given (Wayland:
 * frame callbacks, wp_presentation feedback). Any thread. FALSE = this
 * backend has no such numbers (X11). */
typedef struct {
    guint64 committed;      /* swaps handed to the window system */
    guint64 presented;      /* of them, reported as shown on the output */
    guint64 discarded;      /* reported as never shown (replaced by a newer frame) */
    guint64 wait_timeouts;  /* interval-1 waits that ended without an answer */
    gint64 last_presented_us; /* g_get_monotonic_time () scale; 0 = none yet */
    double refresh_hz;      /* the output's refresh as the feedback gives it; 0 = unknown */
    gboolean hidden;        /* the window system is not showing the surface now */
} WalpePresentStats;
gboolean walpe_surface_present_stats (WalpeSurface *s, WalpePresentStats *out);

/* ---------------------------------------------------------------- pointer */

typedef struct {
    int x, y;               /* in the space of WalpeOutput x / y / width / height */
    gboolean inside;        /* FALSE: the pointer is where the backend cannot see it
                             * (Wayland: over another client's window); x, y are
                             * then the last position seen */
    guintptr under;         /* names what is under the pointer; changes when that changes */
} WalpePointer;

/* where the pointer is now; FALSE = unknown (nothing was ever seen, or the
 * query failed) */
gboolean walpe_platform_pointer (WalpePointer *out);
/* is `under` (from WalpePointer) the bare desktop, i.e. the wallpaper or a
 * desktop-icon window? May ask the window system once per new value. */
gboolean walpe_platform_on_desktop (guintptr under);
/* The pointer from somebody else. On KDE Plasma with the add-on for
 * Plasma (contrib/plasma) Plasma's desktop window lies over the wallpaper
 * and takes the pointer; the add-on's wallpaper type hands on what it sees
 * of it (signals/kwin.c receives it on the session bus).
 * walpe_platform_pointer_helper: such a helper can exist here and a
 * wallpaper uses the pointer - the only time the daemon listens for it.
 * *outputs (may be NULL): the names of the outputs whose wallpaper uses
 * it, one per line (g_free; NULL with FALSE).
 * walpe_platform_pointer_foreign: one event of it. output: the output's
 * name; x, y: on that output, in the units a pointer has on a surface
 * there (logical pixels from the output's top left corner); a, b: the
 * button (1 left, 2 middle, 3 right) for PRESS / RELEASE, the wheel's
 * horizontal and vertical angle in 1/8 degree (120 = one notch, as Qt
 * counts it) for WHEEL. From then on the position reads like any pointer
 * position and buttons arrive as button events. Main thread. */
typedef enum {
    WALPE_FOREIGN_MOTION = 0,
    WALPE_FOREIGN_PRESS,
    WALPE_FOREIGN_RELEASE,
    WALPE_FOREIGN_WHEEL,
    WALPE_FOREIGN_LEAVE,
} WalpeForeignPointer;
gboolean walpe_platform_pointer_helper (char **outputs);
void walpe_platform_pointer_foreign (const char *output, WalpeForeignPointer what, double x, double y,
                                     double a, double b);
/* what the daemon wants to be told about (each costs wake-ups while on):
 * buttons - button events; motion - one pointer_motion event when the
 * pointer moves (the daemon then reads positions itself until it rests). */
void walpe_platform_input_select (gboolean buttons, gboolean motion);
/* FALSE: there are no input events here at all (X11 without XInput2): the
 * daemon polls the position slowly instead */
gboolean walpe_platform_has_input_events (void);

/* ----------------------------------------------------------- presentation */

/* is something else putting the windows on screen, so that a swap that
 * waits for the display only adds a wait? (X11: a compositing manager
 * owns _NET_WM_CM_Sn. Wayland: FALSE - its frame callbacks are that wait.) */
gboolean walpe_platform_composited (void);
/* log what the display offers for presentation (start-up, once) */
void walpe_platform_present_log (void);
/* backend keys for `walpe-ctl status` (the builder is inside the "state"
 * object) */
void walpe_platform_status (JsonBuilder *b);
