/* providers.h - pause and visibility signals that do not come from the
 * window system the wallpaper is drawn on with the daemon's own
 * connection: logind (lock, suspend, another virtual terminal) and, on
 * Wayland, the compositor's idle notification and window list. Each is a
 * WalpeSignalProvider (platform/signals.h); the daemon registers them
 * with one call and otherwise only sets the few values below.
 *
 *   provider        signal(s)                        needs
 *   logind          asleep                           org.freedesktop.login1 on the system bus
 *   wl-idle         idle                             ext_idle_notifier_v1 + wl_seat
 *   wl-fullscreen   fullscreen, fullscreen_on        zwlr_foreign_toplevel_manager_v1 >= 2
 *                   (per output)
 *   wl-windows      covered, focus_app (per output)  zwlr_foreign_toplevel_manager_v1
 *   kwin            fullscreen, fullscreen_on,       KWin (KDE Plasma) on the session bus:
 *                   covered, focus_app (per output)  a script the daemon loads into it
 *
 * The Wayland providers use a connection of their own (wl_session.c), not
 * the one the wallpaper surfaces live on: a protocol error or a compositor
 * quirk on this side cannot take the wallpaper down, and the providers
 * need no object of the rendering side. Everything runs on the main
 * thread. */
#pragma once
#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include "platform/signals.h"
#include "common/config.h"

extern const WalpeSignalProvider walpe_sig_logind;
extern const WalpeSignalProvider walpe_sig_wl_idle;         /* builds with HAVE_WL_SIGNALS */
extern const WalpeSignalProvider walpe_sig_wl_fullscreen;   /* " */
extern const WalpeSignalProvider walpe_sig_wl_windows;      /* " */
extern const WalpeSignalProvider walpe_sig_kwin;            /* " */

/* Register the providers of this file's family with the registry
 * (walpe_signals_register); call once, before walpe_signals_start.
 * backend = the backend the daemon runs on (logind stays off on X11
 * unless WALPE_LOGIND asks for it: X11 has its own display-sleep
 * detection, and its behaviour does not change). */
void walpe_sigprov_register (WalpeBackend backend);

/* How a window's output is told to the registry: the daemon turns the
 * name the compositor gives the output (wl_output.name, "DP-1") into the
 * id it uses with walpe_signal_set. name = NULL asks for "the only output
 * there is" (compositors too old to name outputs): the id when the daemon
 * has exactly one output, else 0. 0 = not an output of the daemon. */
typedef guint32 (*WalpeSigOutputId) (const char *name, gpointer user);
void walpe_sigprov_set_output_lookup (WalpeSigOutputId fn, gpointer user);
/* the daemon's outputs changed: ids are looked up again, values re-sent */
void walpe_sigprov_outputs_changed (void);

/* "pause_idle": seconds without input after which WALPE_SIG_IDLE goes on;
 * 0 = the rule is off (no notification object is kept). Takes effect at
 * the provider's next start or recheck. */
void walpe_sigprov_set_idle_timeout (guint seconds);
guint walpe_sigprov_idle_timeout (void);

/* Which of the rules that need to know about other programs' windows are
 * switched on: WALPE_SIG_BIT () of WALPE_SIG_FULLSCREEN (pause_fullscreen),
 * WALPE_SIG_COVERED (pause_covered) and WALPE_SIG_FOCUS_APP (focus_action
 * other than "run"). The provider "kwin" keeps its connection to the
 * session bus and its script in KWin only while one of them is; the
 * others do not look at it. At start and whenever a setting changes. */
void walpe_sigprov_set_window_rules (guint mask);

/* The pointer from the add-on for KDE Plasma (platform.h,
 * walpe_platform_pointer_foreign has the meaning of the arguments). want:
 * listen for it - a connection to the session bus, the name
 * io.github.walpe.Pointer on it (buttons and the wheel, from the add-on's
 * wallpaper type) and a script in KWin that tells where the pointer is over
 * Plasma's desktop, for as long as want holds and the compositor is KWin.
 * outputs: the names of the outputs whose wallpaper wants the pointer, one
 * per line - the script reports on those only; ms: the least time between
 * two positions (the frame time). fn is called on the main thread per
 * event. */
typedef void (*WalpeSigPointerFn) (const char *output, guint what, double x, double y, double a,
                                   double b, gpointer user);
void walpe_sigprov_set_plasma_pointer (gboolean want, const char *outputs, guint ms, WalpeSigPointerFn fn,
                                       gpointer user);

/* "transparent_windows": comma-separated app ids that never count as
 * covering (X11 matches the same list against WM_CLASS). NULL / "" = none */
void walpe_sigprov_set_see_through (const char *app_ids);

/* status: adds the member "pause_rules" to the object being built - per
 * setting that depends on a signal: on, signal, sources, works and, when
 * nothing can report the signal, a note saying why; for the fullscreen
 * rule also when it pauses every monitor because nothing says which one
 * the window is on (rules.h) */
void walpe_sigprov_status_json (JsonBuilder *b, const WalpeConfig *cfg);

/* ---- logind (logind.c) */
/* tests: talk to this connection instead of the system bus (a private bus
 * with a stand-in org.freedesktop.login1). Before the provider starts;
 * NULL = back to the system bus. Takes a reference. */
void walpe_sig_logind_use_bus (GDBusConnection *bus);
/* what the provider knows, for the log and tests */
typedef struct {
    gboolean running;       /* started and attached to a login manager */
    gboolean have_session;  /* a session object was found (lock / VT state) */
    gboolean sleeping;      /* between PrepareForSleep (true) and (false) */
    gboolean locked;        /* the session's LockedHint */
    gboolean inactive;      /* the session is not the active one of its seat */
    char *session_path;     /* g_free */
} WalpeLogindState;
void walpe_sig_logind_state (WalpeLogindState *out);

/* ---- internal to src/signals (wl_session.c <-> wl_idle.c, wl_toplevel.c, providers.c) */
WalpeBackend walpe_sigprov_backend (void);
guint32 walpe_sigprov_output_id (const char *name);
const char *walpe_sigprov_see_through (void);
void walpe_sig_wl_outputs_changed (void);       /* no-op without HAVE_WL_SIGNALS */
/* builds with HAVE_WL_SIGNALS (wl_toplevel.c): why "wl-fullscreen" reports
 * a focused fullscreen window for every output instead of the one it is
 * on; NULL = it does not (static text) */
const char *walpe_sig_wl_fullscreen_everywhere (void);
/* how often the window list has been worked through (the tests count it) */
guint walpe_sig_wl_passes (void);

/* ---- KWin (kwin.c; with HAVE_WL_SIGNALS) */
void walpe_sig_kwin_set_rules (guint mask);
void walpe_sig_kwin_set_pointer (gboolean want, const char *outputs, guint ms, WalpeSigPointerFn fn,
                                 gpointer user);
/* for status: NULL while the pointer is not wanted from the add-on */
const char *walpe_sig_kwin_pointer_state (void);
void walpe_sig_kwin_outputs_changed (void);
/* tests: the bus to connect to instead of the session bus (an address);
 * before a rule is switched on. NULL = the session bus again. */
void walpe_sig_kwin_use_address (const char *address);
/* for status: NULL where the provider did not start (not KWin), else what
 * it is doing; and, while a rule is on and it cannot report, why */
const char *walpe_sig_kwin_state (void);
const char *walpe_sig_kwin_trouble (void);
