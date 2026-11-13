/* signals.h - the conditions under which wallpapers pause, and who reports
 * them.
 *
 * The daemon pauses a wallpaper while nobody can see it or the user asked
 * for quiet: a fullscreen window has the focus on its monitor, the displays
 * sleep, windows cover the monitor, an application has the focus on it. How such a
 * condition is found out depends on the window system and on what the
 * session offers (EWMH properties and the DPMS extension on X11; on
 * Wayland a protocol the compositor may or may not have, logind, ...).
 * Each way of finding out is a provider: a small object with a name, the
 * signals it can report and a start function that answers "not available
 * here" with the reason. The registry below starts the providers that fit
 * the running backend, combines what they report (a signal is on while
 * any provider says so) and tells the daemon when a combined value
 * changes. The daemon never asks where a value came from; `status` does,
 * so that a user can see which pause rules can work in the session.
 *
 * Threading: everything here runs on the main thread (the thread of the
 * default GMainContext). A provider that learns something on another
 * thread hands it over with g_idle_add / g_main_context_invoke. */
#pragma once
#include <glib.h>
#include "common/backend.h"

typedef enum {
    WALPE_SIG_FULLSCREEN = 0,   /* global: a fullscreen window has the focus (on whatever
                                 * monitor, also one the daemon shows nothing on) */
    WALPE_SIG_ASLEEP,           /* global: the displays are in a power-save state, the
                                 * screensaver runs, or the session is locked / suspended */
    WALPE_SIG_IDLE,             /* global: no user input for the time the provider was given */
    WALPE_SIG_COVERED,          /* per output: windows hide (nearly) all of the wallpaper,
                                 * or the window system does not show its surface */
    WALPE_SIG_FOCUS_APP,        /* per output: an application's window on it has the focus */
    WALPE_SIG_FULLSCREEN_ON,    /* per output: the window of WALPE_SIG_FULLSCREEN is on it.
                                 * Whoever reports that signal reports this one with it; a
                                 * provider that cannot tell where the window is reports it
                                 * for WALPE_OUTPUT_ALL */
    WALPE_SIG_N
} WalpeSignal;

#define WALPE_SIG_BIT(s) (1u << (s))
#define WALPE_SIG_PER_OUTPUT                                                              \
    (WALPE_SIG_BIT (WALPE_SIG_COVERED) | WALPE_SIG_BIT (WALPE_SIG_FOCUS_APP) |           \
     WALPE_SIG_BIT (WALPE_SIG_FULLSCREEN_ON))

/* In place of an output's id (a WalpeOutput.id is never 0), for a
 * per-output signal: "every output". A provider sets it when it knows the
 * condition holds but not on which output (a compositor that does not say
 * where a window is); the value then reads as on for each output asked
 * about, until the provider takes it back. */
#define WALPE_OUTPUT_ALL ((guint32) 0)

static inline const char *walpe_signal_name (int s)
{
    static const char *const names[WALPE_SIG_N] = { "fullscreen", "asleep", "idle", "covered",
                                                    "focus_app", "fullscreen_on" };
    return s >= 0 && s < WALPE_SIG_N ? names[s] : "?";
}

typedef struct WalpeSignalProvider WalpeSignalProvider;

struct WalpeSignalProvider {
    const char *name;       /* short, for status and the log: "x11", "wl-frame", "logind" */
    guint signals;          /* WALPE_SIG_BIT () of every signal it reports once started */
    guint backends;         /* 1u << WalpeBackend it works with; 0 = any backend */
    /* Bind what it needs and report the current state (walpe_signal_set).
     * FALSE = not available in this session: *why (g_free, may be left
     * NULL) names what is missing, e.g. "the compositor has no
     * ext_idle_notifier_v1". A provider that fails to start is not asked
     * again and reports nothing. *why set to an empty string: the provider
     * is for another desktop than this one, where nothing is missing -
     * the log has no line about it and status does not name it. */
    gboolean (*start) (const WalpeSignalProvider *self, char **why);
    /* release everything; values it reported are dropped by the registry
     * (it may also take them back itself: the sink is not told any more) */
    void (*stop) (const WalpeSignalProvider *self);
    /* optional: a setting the provider reads changed, or the daemon wants
     * the state looked at again. now = TRUE: before this returns; FALSE:
     * soon (debounced). sig: the signal concerned. */
    void (*recheck) (const WalpeSignalProvider *self, WalpeSignal sig, gboolean now);
};

/* the daemon's side */
typedef struct {
    /* a combined value changed: mask = WALPE_SIG_BIT () of the signals
     * concerned; read the values with walpe_signal_get. Called once per
     * walpe_signal_set outside a batch, once at the end of a batch. (The
     * mask may name a signal whose value went back and forth meanwhile
     * and reads as before: act on the values, not on the bits.) */
    void (*changed) (guint mask, gpointer user);
} WalpeSignalSink;

/* ---- registry (platform/signals.c) */

/* Add a provider. Before walpe_signals_start: it is started with the
 * others; afterwards: started at once. The struct must stay valid. */
void walpe_signals_register (const WalpeSignalProvider *p);
/* Start every registered provider that fits `backend`, in the order of
 * registration. sink may be NULL (nothing is told; values can be read). */
void walpe_signals_start (WalpeBackend backend, const WalpeSignalSink *sink, gpointer user);
/* stops the running providers, last registered first, and empties the
 * registry. The sink is not called from here on, whatever a provider
 * reports while it stops: the daemon may have freed what the sink reads. */
void walpe_signals_stop (void);

/* provider -> registry. output: a WalpeOutput.id (or WALPE_OUTPUT_ALL)
 * for a per-output signal, ignored (pass 0) for a global one. Setting a
 * value a provider already has is free. */
void walpe_signal_set (const WalpeSignalProvider *p, WalpeSignal sig, guint32 output, gboolean on);
/* several values that belong together (one scan of all windows): the sink
 * is told once, after the last walpe_signals_end. May nest. */
void walpe_signals_begin (void);
void walpe_signals_end (void);

/* daemon -> registry. A per-output signal is on for an output while a
 * provider reports it for that output or for WALPE_OUTPUT_ALL. */
gboolean walpe_signal_get (WalpeSignal sig, guint32 output);
/* ask the providers of sig to look again (WalpeSignalProvider.recheck) */
void walpe_signals_recheck (WalpeSignal sig, gboolean now);
/* for status: the running providers of sig, comma separated ("" = none:
 * nothing in this session can report it), and why providers that could
 * have reported it did not start ("" = none failed). Static storage,
 * valid until the next call with the same sig. */
const char *walpe_signal_sources (WalpeSignal sig);
const char *walpe_signal_missing (WalpeSignal sig);
