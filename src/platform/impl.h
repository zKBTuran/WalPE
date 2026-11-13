/* impl.h - what a backend implements. platform.c holds the public
 * functions of platform.h and forwards each one to the table of the
 * backend that is open; a backend is one such table plus whatever files
 * it needs. Nothing but platform.c and the backends includes this file.
 *
 * Every member has the meaning, the thread and the return value of the
 * platform.h function of the same name (surface_new = walpe_surface_new
 * and so on); what is said here is only what a backend has to know
 * beyond that. A member may be NULL where "optional" says what then
 * happens.
 *
 * Rules for a backend
 *  - One connection, opened by open () and closed by close (); nothing
 *    is connected, bound or allocated before open () (except probe).
 *  - Events (WalpePlatformEvents of the opts given to open) are delivered
 *    on the main thread, from a GSource the backend attaches to the
 *    default GMainContext in start (), never before start () and never
 *    from inside a call the daemon makes into the backend - with two
 *    exceptions: pause signals may be reported (walpe_signal_set) from
 *    inside start () and from a provider's recheck (now = TRUE).
 *  - The three presentation hooks, set_buffer_size and surface_fit run on the render
 *    thread of the surface, with its EGL context current. They must not
 *    wait for the main thread to do something (the main thread may be
 *    joining that very render thread). swapped () is the only one that
 *    may block, and at most for about a quarter of a second.
 *  - surface_free () is called on the main thread after the surface's
 *    render thread has ended; close () after every surface was freed and
 *    eglTerminate was called on the display.
 *  - No setenv / putenv / setlocale after open () returns (other threads
 *    read the environment), no exit (): a connection that dies is
 *    reported with connection_lost and the daemon shuts down in order.
 *    (X11 is the exception the X library forces: its I/O error handler
 *    must not return, so it removes the two files named in the opts and
 *    ends the process.)
 *  - Descriptors the backend opens are close-on-exec: the daemon
 *    restarts itself in place with execve.
 *  - The private surface type is the backend's own struct; it is handed
 *    out and taken back as the opaque WalpeSurface pointer (a cast). */
#pragma once
#include "platform/platform.h"

typedef struct {
    const char *name;                   /* "x11" / "wayland" */
    WalpeBackend backend;

    gboolean (*open) (const WalpePlatformOpts *opts, char **err);
    void (*close) (void);
    char *(*describe) (void);
    /* Attach the event source, select what is needed for the events and
     * register + report the backend's own pause signals. sink / user: to
     * be handed to walpe_signals_start when the backend's start-up
     * sequence has reached the point where the providers may run
     * (platform.c does not call walpe_signals_start itself). */
    void (*start) (gboolean layout_events, const WalpeSignalSink *sink, gpointer sink_user);
    void (*settle) (void);              /* optional: nothing */
    void (*sync) (void);                /* optional: nothing */

    void (*outputs) (WalpeOutputList *out);

    EGLDisplay (*egl_display) (void);
    const char *(*egl_note) (void);     /* optional: "" */
    unsigned long (*egl_visual) (void); /* optional: 0 */
    gboolean (*egl_config_usable) (EGLDisplay dpy, EGLConfig conf);   /* optional: TRUE */
    const char *(*display_node) (void); /* optional: NULL */
    gboolean paces;                     /* walpe_platform_paces () */

    WalpeSurface *(*surface_new) (const WalpeSurfaceSpec *spec, EGLDisplay dpy, EGLConfig conf,
                                  EGLSurface *egl_surface, int *w, int *h);
    void (*surface_free) (WalpeSurface *s);
    void (*surface_size) (WalpeSurface *s, int *w, int *h);
    gboolean (*surface_set_buffer_size) (WalpeSurface *s, int w, int h);   /* optional: FALSE */
    guint32 (*surface_output) (WalpeSurface *s);
    unsigned long (*surface_native_id) (WalpeSurface *s);
    void (*surface_want_pointer) (WalpeSurface *s, gboolean want);         /* optional: nothing */
    int (*surface_set_interval) (WalpeSurface *s, int iv);                 /* optional: iv */
    void (*surface_pre_swap) (WalpeSurface *s);                            /* optional: nothing */
    void (*surface_swapped) (WalpeSurface *s);                             /* optional: nothing */
    gboolean (*surface_present_stats) (WalpeSurface *s, WalpePresentStats *out);   /* optional: FALSE */

    gboolean (*pointer) (WalpePointer *out);
    gboolean (*on_desktop) (guintptr under);                               /* optional: TRUE */
    gboolean (*pointer_helper) (char **outputs);                           /* optional: FALSE */
    void (*pointer_foreign) (const char *output, WalpeForeignPointer what, double x, double y,
                             double a, double b);                          /* optional: nothing */
    void (*input_select) (gboolean buttons, gboolean motion);              /* optional: nothing */
    gboolean (*has_input_events) (void);                                   /* optional: TRUE */

    gboolean (*composited) (void);                                         /* optional: FALSE */
    void (*present_log) (void);                                            /* optional: nothing */
    void (*status) (JsonBuilder *b);                                       /* optional: nothing */
} WalpePlatformOps;

/* the tables (each exists only in a build that holds its backend) */
extern const WalpePlatformOps walpe_x11_ops;
extern const WalpePlatformOps walpe_wayland_ops;

/* X11 only, callable without an open backend: the names of the X
 * server's RandR providers behind walpe_platform_x11_providers (a
 * connection of its own: connect, a handful of requests, disconnect; no
 * EGL). g_strfreev; NULL = none or not known. */
char **walpe_x11_provider_names (void);

/* Wayland only, callable without an open backend: the look at the
 * compositor behind walpe_platform_probe_wayland (a connection of its own:
 * connect, one or two round trips over the registry, disconnect; no EGL).
 * out is zeroed by the caller. */
void walpe_wayland_probe (WalpePlatformProbe *out);
/* Wayland only: the probe may have had to keep its connection for open ()
 * (a socket handed over in WAYLAND_SOCKET can be connected to once).
 * Closes it when it is still there; called when another backend was
 * opened. */
void walpe_wayland_probe_release (void);
/* Wayland only: walpe_surface_follow_output and walpe_surface_fit of
 * platform.h ("following an output in place"). Not members of the table:
 * the X11 backend has no such thing, and a build without the Wayland
 * backend has neither these nor the daemon's code that calls them. */
gboolean walpe_wayland_surface_follow_output (WalpeSurface *s, const WalpeOutput *o, int *w, int *h);
gboolean walpe_wayland_surface_fit (WalpeSurface *s, int *w, int *h);
/* ... and walpe_platform_surface_wait */
void walpe_wayland_surface_wait (int ms);
gboolean walpe_wayland_display_gone (void);
gboolean walpe_wayland_display_refused (void);

/* helpers of platform.c for the backends */
/* a filled-in output record: name copied, canon derived; the other
 * strings copied when not NULL */
void walpe_output_set_names (WalpeOutput *o, const char *name, const char *make, const char *model,
                             const char *serial, const char *description);
