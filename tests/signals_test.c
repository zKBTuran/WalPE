/* signals_test.c - the registry of pause-signal providers
 * (platform/signals.c): which providers start on which backend, a
 * provider that is "not available" and its reason, values of two
 * providers combined, per-output values, a value for every output (the
 * fullscreen window whose monitor nobody can name), batches, what the
 * daemon is told and when, asking providers to look again, late
 * registration, stop.
 * Made-up providers; no display, no daemon. */
#include <glib.h>
#include <string.h>
#include "platform/signals.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; g_printerr ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ---- the daemon's side: counts what it is told */
static int told;
static guint told_mask, told_last;
static GString *order;      /* provider names in the order they started */

static void sink_changed (guint mask, gpointer user)
{
    CHECK (user == &told);
    told++;
    told_mask |= mask;
    told_last = mask;
}

static const WalpeSignalSink sink = { sink_changed };

static void told_reset (void)
{
    told = 0;
    told_mask = told_last = 0;
}

/* ---- providers */
static int rechecks[3];
static WalpeSignal recheck_sig;
static gboolean recheck_now;

static gboolean start_ok (const WalpeSignalProvider *self, char **why)
{
    (void) why;
    g_string_append_printf (order, "%s ", self->name);
    return TRUE;
}

/* reports while it starts: the first state counts */
static gboolean start_reporting (const WalpeSignalProvider *self, char **why)
{
    (void) why;
    g_string_append_printf (order, "%s ", self->name);
    walpe_signal_set (self, WALPE_SIG_FULLSCREEN, 0, TRUE);
    walpe_signal_set (self, WALPE_SIG_COVERED, 7, TRUE);
    return TRUE;
}

/* reports something, then finds it cannot work here */
static gboolean start_fails (const WalpeSignalProvider *self, char **why)
{
    g_string_append_printf (order, "%s ", self->name);
    walpe_signal_set (self, WALPE_SIG_IDLE, 0, TRUE);
    *why = g_strdup ("the compositor has no idle notifier");
    return FALSE;
}

static int stops;
static void stop_count (const WalpeSignalProvider *self)
{
    (void) self;
    stops++;
}
/* takes its value back while it stops, as a tidy provider does */
static void stop_reporting (const WalpeSignalProvider *self)
{
    stops++;
    walpe_signal_set (self, WALPE_SIG_ASLEEP, 0, FALSE);
}

static void recheck0 (const WalpeSignalProvider *self, WalpeSignal sig, gboolean now)
{
    (void) self;
    rechecks[0]++;
    recheck_sig = sig;
    recheck_now = now;
}

static void recheck1 (const WalpeSignalProvider *self, WalpeSignal sig, gboolean now)
{
    (void) self; (void) sig; (void) now;
    rechecks[1]++;
}

#define BIT WALPE_SIG_BIT
static const WalpeSignalProvider p_x = {
    .name = "x", .backends = 1u << WALPE_BACKEND_X11,
    .signals = BIT (WALPE_SIG_FULLSCREEN) | BIT (WALPE_SIG_ASLEEP) | BIT (WALPE_SIG_COVERED) |
               BIT (WALPE_SIG_FOCUS_APP),
    .start = start_ok, .stop = stop_count, .recheck = recheck0,
};
static const WalpeSignalProvider p_any = {
    .name = "any", .backends = 0,
    .signals = BIT (WALPE_SIG_ASLEEP) | BIT (WALPE_SIG_COVERED),
    .start = start_ok, .stop = stop_count, .recheck = recheck1,
};
static const WalpeSignalProvider p_wl = {
    .name = "wl", .backends = 1u << WALPE_BACKEND_WAYLAND,
    .signals = BIT (WALPE_SIG_FULLSCREEN) | BIT (WALPE_SIG_COVERED),
    .start = start_reporting, .stop = stop_count,
};
static const WalpeSignalProvider p_idle = {
    .name = "idle", .backends = 1u << WALPE_BACKEND_WAYLAND,
    .signals = BIT (WALPE_SIG_IDLE),
    .start = start_fails, .stop = stop_count,
};
/* for another desktop than this one: declines with the empty reason */
static gboolean start_elsewhere (const WalpeSignalProvider *self, char **why)
{
    g_string_append_printf (order, "%s ", self->name);
    *why = g_strdup ("");
    return FALSE;
}
static const WalpeSignalProvider p_elsewhere = {
    .name = "elsewhere", .backends = 1u << WALPE_BACKEND_WAYLAND,
    .signals = BIT (WALPE_SIG_IDLE) | BIT (WALPE_SIG_FULLSCREEN) | BIT (WALPE_SIG_FOCUS_APP),
    .start = start_elsewhere, .stop = stop_count,
};
static guint elsewhere_lines;
static GLogWriterOutput count_elsewhere (GLogLevelFlags level, const GLogField *fields, gsize n, gpointer user)
{
    (void) user;
    for (gsize i = 0; i < n; i++)
        if (!strcmp (fields[i].key, "MESSAGE") && strstr (fields[i].value, "elsewhere"))
            elsewhere_lines++;
    return g_log_writer_default (level, fields, n, NULL);
}
static const WalpeSignalProvider p_tidy = {
    .name = "tidy", .backends = 0, .signals = BIT (WALPE_SIG_ASLEEP),
    .start = start_ok, .stop = stop_reporting,
};
static const WalpeSignalProvider p_late = {
    .name = "late", .backends = 0, .signals = BIT (WALPE_SIG_IDLE),
    .start = NULL, .stop = NULL,
};

/* an X11 session: the X provider and the one that fits any backend run,
 * the Wayland ones are never started */
static void test_x11 (void)
{
    order = g_string_new (NULL);
    told_reset ();
    stops = 0;
    walpe_signals_register (&p_x);
    walpe_signals_register (&p_wl);
    walpe_signals_register (&p_any);
    walpe_signals_register (&p_idle);
    walpe_signals_register (&p_x);              /* twice: once */
    walpe_signals_start (WALPE_BACKEND_X11, &sink, &told);
    CHECK (!strcmp (order->str, "x any "));
    CHECK (told == 0);                          /* nothing reported, nothing told */
    CHECK (!strcmp (walpe_signal_sources (WALPE_SIG_FULLSCREEN), "x"));
    CHECK (!strcmp (walpe_signal_sources (WALPE_SIG_ASLEEP), "x,any"));
    CHECK (!strcmp (walpe_signal_sources (WALPE_SIG_IDLE), ""));    /* nobody here can tell */
    CHECK (!strcmp (walpe_signal_missing (WALPE_SIG_IDLE), ""));    /* and nobody tried */
    CHECK (!walpe_signal_get (WALPE_SIG_FULLSCREEN, 0) && !walpe_signal_get (WALPE_SIG_COVERED, 1));

    /* one value, told once, with its bit */
    walpe_signal_set (&p_x, WALPE_SIG_FULLSCREEN, 0, TRUE);
    CHECK (told == 1 && told_last == BIT (WALPE_SIG_FULLSCREEN));
    CHECK (walpe_signal_get (WALPE_SIG_FULLSCREEN, 0));
    walpe_signal_set (&p_x, WALPE_SIG_FULLSCREEN, 0, TRUE);     /* the same again: free */
    CHECK (told == 1);
    walpe_signal_set (&p_x, WALPE_SIG_FULLSCREEN, 0, 5);        /* any non-zero is TRUE */
    CHECK (told == 1);
    walpe_signal_set (&p_x, WALPE_SIG_FULLSCREEN, 0, FALSE);
    CHECK (told == 2 && !walpe_signal_get (WALPE_SIG_FULLSCREEN, 0));

    /* two providers of one signal: on while either says so */
    told_reset ();
    walpe_signal_set (&p_x, WALPE_SIG_ASLEEP, 0, TRUE);
    CHECK (told == 1 && walpe_signal_get (WALPE_SIG_ASLEEP, 0));
    walpe_signal_set (&p_any, WALPE_SIG_ASLEEP, 0, TRUE);
    CHECK (told == 1);                          /* already on: no news */
    walpe_signal_set (&p_x, WALPE_SIG_ASLEEP, 0, FALSE);
    CHECK (told == 1 && walpe_signal_get (WALPE_SIG_ASLEEP, 0));    /* the other still says so */
    walpe_signal_set (&p_any, WALPE_SIG_ASLEEP, 0, FALSE);
    CHECK (told == 2 && !walpe_signal_get (WALPE_SIG_ASLEEP, 0));

    /* per output: each id on its own */
    told_reset ();
    walpe_signal_set (&p_x, WALPE_SIG_COVERED, 1, TRUE);
    CHECK (told == 1 && walpe_signal_get (WALPE_SIG_COVERED, 1) && !walpe_signal_get (WALPE_SIG_COVERED, 2));
    walpe_signal_set (&p_any, WALPE_SIG_COVERED, 2, TRUE);
    CHECK (told == 2 && walpe_signal_get (WALPE_SIG_COVERED, 2));
    walpe_signal_set (&p_x, WALPE_SIG_COVERED, 1, FALSE);
    CHECK (told == 3 && !walpe_signal_get (WALPE_SIG_COVERED, 1) && walpe_signal_get (WALPE_SIG_COVERED, 2));
    walpe_signal_set (&p_any, WALPE_SIG_COVERED, 2, FALSE);
    CHECK (told == 4);
    /* a global signal ignores the output argument */
    walpe_signal_set (&p_x, WALPE_SIG_FULLSCREEN, 9, TRUE);
    CHECK (walpe_signal_get (WALPE_SIG_FULLSCREEN, 0) && walpe_signal_get (WALPE_SIG_FULLSCREEN, 3));
    walpe_signal_set (&p_x, WALPE_SIG_FULLSCREEN, 0, FALSE);

    /* a batch (one scan of all windows): told once, with every bit; a
     * batch that changes nothing in the end tells nothing */
    told_reset ();
    walpe_signals_begin ();
    walpe_signal_set (&p_x, WALPE_SIG_COVERED, 1, TRUE);
    walpe_signal_set (&p_x, WALPE_SIG_COVERED, 2, TRUE);
    walpe_signal_set (&p_x, WALPE_SIG_FOCUS_APP, 2, TRUE);
    CHECK (told == 0);
    CHECK (walpe_signal_get (WALPE_SIG_COVERED, 2));    /* readable inside the batch */
    walpe_signals_end ();
    CHECK (told == 1 && told_last == (BIT (WALPE_SIG_COVERED) | BIT (WALPE_SIG_FOCUS_APP)));
    walpe_signals_begin ();
    walpe_signals_begin ();                     /* nested */
    walpe_signal_set (&p_x, WALPE_SIG_COVERED, 1, FALSE);
    walpe_signals_end ();
    CHECK (told == 1);
    walpe_signals_end ();
    CHECK (told == 2);
    walpe_signals_begin ();
    walpe_signals_end ();
    CHECK (told == 2);

    /* a provider that never started does not count, whatever it says */
    told_reset ();
    walpe_signal_set (&p_wl, WALPE_SIG_FULLSCREEN, 0, TRUE);
    CHECK (told == 0 && !walpe_signal_get (WALPE_SIG_FULLSCREEN, 0));
    /* nor does one nobody registered, or a signal that does not exist */
    walpe_signal_set (&p_late, WALPE_SIG_IDLE, 0, TRUE);
    walpe_signal_set (&p_x, (WalpeSignal) 99, 0, TRUE);
    CHECK (told == 0 && !walpe_signal_get (WALPE_SIG_IDLE, 0) && !walpe_signal_get ((WalpeSignal) 99, 0));

    /* asking the providers of a signal to look again */
    walpe_signals_recheck (WALPE_SIG_COVERED, FALSE);
    CHECK (rechecks[0] == 1 && rechecks[1] == 1 && recheck_sig == WALPE_SIG_COVERED && !recheck_now);
    walpe_signals_recheck (WALPE_SIG_FULLSCREEN, TRUE);         /* only "x" reports that one */
    CHECK (rechecks[0] == 2 && rechecks[1] == 1 && recheck_sig == WALPE_SIG_FULLSCREEN && recheck_now);
    walpe_signals_recheck (WALPE_SIG_IDLE, TRUE);               /* nobody */
    CHECK (rechecks[0] == 2 && rechecks[1] == 1);

    /* registered after the start: started at once (no start function = it
     * simply runs) */
    walpe_signals_register (&p_late);
    CHECK (!strcmp (walpe_signal_sources (WALPE_SIG_IDLE), "late"));
    walpe_signal_set (&p_late, WALPE_SIG_IDLE, 0, TRUE);
    CHECK (told == 1 && told_last == BIT (WALPE_SIG_IDLE) && walpe_signal_get (WALPE_SIG_IDLE, 0));

    walpe_signals_stop ();
    CHECK (stops == 2);                         /* the two that ran */
    CHECK (!walpe_signal_get (WALPE_SIG_IDLE, 0) && !walpe_signal_get (WALPE_SIG_COVERED, 2));
    CHECK (!strcmp (walpe_signal_sources (WALPE_SIG_ASLEEP), ""));
    g_string_free (order, TRUE);
}

/* a Wayland session: a provider reports while it starts, another one is
 * not available and says why */
static void test_wayland (void)
{
    order = g_string_new (NULL);
    told_reset ();
    stops = 0;
    walpe_signals_register (&p_wl);             /* the backend's own first */
    walpe_signals_register (&p_x);
    walpe_signals_register (&p_idle);
    walpe_signals_register (&p_elsewhere);
    walpe_signals_register (&p_any);
    /* said before the registry runs: does not count, and does not hide
     * the same value reported when the provider starts (start_reporting) */
    walpe_signal_set (&p_wl, WALPE_SIG_FULLSCREEN, 0, TRUE);
    walpe_signal_set (&p_wl, WALPE_SIG_COVERED, 7, TRUE);
    walpe_signal_set (&p_wl, WALPE_SIG_COVERED, 8, TRUE);
    CHECK (told == 0 && !walpe_signal_get (WALPE_SIG_FULLSCREEN, 0));
    walpe_signals_start (WALPE_BACKEND_WAYLAND, &sink, &told);
    CHECK (!strcmp (order->str, "wl idle elsewhere any "));     /* registration order; "x" does not fit */
    /* the one for another desktop was asked, and is named nowhere: not in
     * the log, not among what is missing for a signal nobody else reports */
    CHECK (elsewhere_lines == 0);
    CHECK (!strstr (walpe_signal_missing (WALPE_SIG_IDLE), "elsewhere"));
    CHECK (!strcmp (walpe_signal_missing (WALPE_SIG_FOCUS_APP), ""));
    /* what the providers found while starting arrives as one piece of news
     * (the mask may also name the signal of the provider that reported
     * and then gave up: its value is read as off again) */
    CHECK (told == 1 && (told_mask & (BIT (WALPE_SIG_FULLSCREEN) | BIT (WALPE_SIG_COVERED))) ==
                            (BIT (WALPE_SIG_FULLSCREEN) | BIT (WALPE_SIG_COVERED)));
    CHECK (walpe_signal_get (WALPE_SIG_FULLSCREEN, 0) && walpe_signal_get (WALPE_SIG_COVERED, 7));
    CHECK (!walpe_signal_get (WALPE_SIG_COVERED, 8));
    /* the one that gave up: its report is void, its reason is kept */
    CHECK (!walpe_signal_get (WALPE_SIG_IDLE, 0));
    CHECK (!strcmp (walpe_signal_sources (WALPE_SIG_IDLE), ""));
    CHECK (strstr (walpe_signal_missing (WALPE_SIG_IDLE), "idle: the compositor has no idle notifier"));
    CHECK (!strcmp (walpe_signal_missing (WALPE_SIG_FULLSCREEN), ""));
    CHECK (!strcmp (walpe_signal_sources (WALPE_SIG_COVERED), "wl,any"));
    CHECK (!strcmp (walpe_signal_sources (WALPE_SIG_FOCUS_APP), ""));   /* nobody: the rule cannot fire */
    /* later reports of the failed one do not count either */
    told_reset ();
    walpe_signal_set (&p_idle, WALPE_SIG_IDLE, 0, FALSE);
    walpe_signal_set (&p_idle, WALPE_SIG_IDLE, 0, TRUE);
    CHECK (told == 0 && !walpe_signal_get (WALPE_SIG_IDLE, 0));
    /* started once only */
    walpe_signals_start (WALPE_BACKEND_WAYLAND, &sink, &told);
    CHECK (!strcmp (order->str, "wl idle elsewhere any "));
    walpe_signals_stop ();
    CHECK (stops == 2);                         /* "wl" and "any"; not the two that declined */
    /* after a stop the registry is empty: the same providers can be used again */
    walpe_signals_register (&p_any);
    walpe_signals_start (WALPE_BACKEND_WAYLAND, NULL, NULL);    /* no sink: values can still be read */
    walpe_signal_set (&p_any, WALPE_SIG_ASLEEP, 0, TRUE);
    CHECK (walpe_signal_get (WALPE_SIG_ASLEEP, 0) && told == 0);
    walpe_signals_stop ();
    /* a provider that reports while it is being stopped: the daemon is not
     * told (it stops the registry when its monitors are freed already) */
    told_reset ();
    stops = 0;
    walpe_signals_register (&p_tidy);
    walpe_signals_start (WALPE_BACKEND_WAYLAND, &sink, &told);
    walpe_signal_set (&p_tidy, WALPE_SIG_ASLEEP, 0, TRUE);
    CHECK (told == 1 && walpe_signal_get (WALPE_SIG_ASLEEP, 0));
    walpe_signals_stop ();
    CHECK (stops == 1 && told == 1 && !walpe_signal_get (WALPE_SIG_ASLEEP, 0));
    g_string_free (order, TRUE);
}

/* the fullscreen rule per monitor: "the focused fullscreen window is on
 * this output" next to the global "there is one", and WALPE_OUTPUT_ALL for
 * a provider that knows of the window but not of its output */
static void test_every_output (void)
{
    const WalpeSignal ON = WALPE_SIG_FULLSCREEN_ON;
    order = g_string_new (NULL);
    told_reset ();
    stops = 0;
    walpe_signals_register (&p_x);
    walpe_signals_register (&p_any);
    walpe_signals_start (WALPE_BACKEND_X11, &sink, &told);
    CHECK (!walpe_signal_get (ON, 1) && !walpe_signal_get (ON, WALPE_OUTPUT_ALL));

    /* a fullscreen window gets the focus on output 2: the daemon hears of
     * the window and of its output in one piece of news */
    walpe_signals_begin ();
    walpe_signal_set (&p_x, WALPE_SIG_FULLSCREEN, 0, TRUE);
    walpe_signal_set (&p_x, ON, 2, TRUE);
    walpe_signals_end ();
    CHECK (told == 1 && told_last == (BIT (WALPE_SIG_FULLSCREEN) | BIT (ON)));
    CHECK (walpe_signal_get (WALPE_SIG_FULLSCREEN, 0));
    CHECK (walpe_signal_get (ON, 2) && !walpe_signal_get (ON, 1));
    /* (asking for "every output" is not asking for "any output") */
    CHECK (!walpe_signal_get (ON, WALPE_OUTPUT_ALL));

    /* the window is moved to output 1 */
    walpe_signals_begin ();
    walpe_signal_set (&p_x, ON, 2, FALSE);
    walpe_signal_set (&p_x, ON, 1, TRUE);
    walpe_signals_end ();
    CHECK (told == 2 && told_last == BIT (ON));
    CHECK (walpe_signal_get (ON, 1) && !walpe_signal_get (ON, 2));
    CHECK (walpe_signal_get (WALPE_SIG_FULLSCREEN, 0));

    /* where it is can no longer be told: every output, also one that was
     * never named */
    walpe_signal_set (&p_x, ON, WALPE_OUTPUT_ALL, TRUE);
    CHECK (told == 3 && told_last == BIT (ON));
    CHECK (walpe_signal_get (ON, 1) && walpe_signal_get (ON, 2) && walpe_signal_get (ON, 77));
    CHECK (walpe_signal_get (ON, WALPE_OUTPUT_ALL));
    /* an id set or taken back meanwhile changes nothing anybody can read:
     * no news */
    walpe_signal_set (&p_x, ON, 1, FALSE);
    walpe_signal_set (&p_x, ON, 3, TRUE);
    CHECK (told == 3 && walpe_signal_get (ON, 1) && walpe_signal_get (ON, 3));
    /* known again: what is left is the id set meanwhile */
    walpe_signal_set (&p_x, ON, WALPE_OUTPUT_ALL, FALSE);
    CHECK (told == 4 && told_last == BIT (ON));
    CHECK (!walpe_signal_get (ON, 1) && !walpe_signal_get (ON, 2) && walpe_signal_get (ON, 3));
    CHECK (!walpe_signal_get (ON, 77) && !walpe_signal_get (ON, WALPE_OUTPUT_ALL));

    /* two providers: "every output" from one, an id from the other */
    walpe_signal_set (&p_any, ON, WALPE_OUTPUT_ALL, TRUE);
    CHECK (told == 5 && walpe_signal_get (ON, 9) && walpe_signal_get (ON, 3));
    walpe_signal_set (&p_x, ON, 3, FALSE);          /* hidden under the other's value */
    CHECK (told == 5 && walpe_signal_get (ON, 3));
    walpe_signal_set (&p_any, ON, WALPE_OUTPUT_ALL, FALSE);
    CHECK (told == 6 && !walpe_signal_get (ON, 3) && !walpe_signal_get (ON, 9));

    /* no focused fullscreen window any more */
    walpe_signals_begin ();
    walpe_signal_set (&p_x, ON, 4, TRUE);
    walpe_signals_end ();
    told_reset ();
    walpe_signals_begin ();
    walpe_signal_set (&p_x, WALPE_SIG_FULLSCREEN, 0, FALSE);
    walpe_signal_set (&p_x, ON, 4, FALSE);
    walpe_signals_end ();
    CHECK (told == 1 && told_last == (BIT (WALPE_SIG_FULLSCREEN) | BIT (ON)));
    CHECK (!walpe_signal_get (WALPE_SIG_FULLSCREEN, 0) && !walpe_signal_get (ON, 4));

    /* the other per-output signals: an id is that output's alone, as ever
     * (no provider reports them for every output) */
    walpe_signal_set (&p_x, WALPE_SIG_COVERED, 1, TRUE);
    CHECK (walpe_signal_get (WALPE_SIG_COVERED, 1) && !walpe_signal_get (WALPE_SIG_COVERED, 2));
    CHECK (!walpe_signal_get (WALPE_SIG_COVERED, WALPE_OUTPUT_ALL));

    /* a provider that stops takes "every output" with it */
    walpe_signal_set (&p_x, ON, WALPE_OUTPUT_ALL, TRUE);
    CHECK (walpe_signal_get (ON, 5));
    walpe_signals_stop ();
    CHECK (!walpe_signal_get (ON, 5) && !walpe_signal_get (ON, WALPE_OUTPUT_ALL));
    g_string_free (order, TRUE);
}

static void test_names (void)
{
    CHECK (!strcmp (walpe_signal_name (WALPE_SIG_FULLSCREEN), "fullscreen"));
    CHECK (!strcmp (walpe_signal_name (WALPE_SIG_ASLEEP), "asleep"));
    CHECK (!strcmp (walpe_signal_name (WALPE_SIG_IDLE), "idle"));
    CHECK (!strcmp (walpe_signal_name (WALPE_SIG_COVERED), "covered"));
    CHECK (!strcmp (walpe_signal_name (WALPE_SIG_FOCUS_APP), "focus_app"));
    CHECK (!strcmp (walpe_signal_name (WALPE_SIG_FULLSCREEN_ON), "fullscreen_on"));
    CHECK (!strcmp (walpe_signal_name (-1), "?") && !strcmp (walpe_signal_name (WALPE_SIG_N), "?"));
    CHECK ((WALPE_SIG_PER_OUTPUT & BIT (WALPE_SIG_COVERED)) && (WALPE_SIG_PER_OUTPUT & BIT (WALPE_SIG_FOCUS_APP)));
    /* "a fullscreen window has the focus" stays one value for the session;
     * which monitor it is on is the per-output one */
    CHECK (WALPE_SIG_PER_OUTPUT & BIT (WALPE_SIG_FULLSCREEN_ON));
    CHECK (WALPE_OUTPUT_ALL == 0);      /* never an output's id (platform.h) */
    CHECK (!(WALPE_SIG_PER_OUTPUT & (BIT (WALPE_SIG_FULLSCREEN) | BIT (WALPE_SIG_ASLEEP) | BIT (WALPE_SIG_IDLE))));
}

int main (void)
{
    /* the "not available here" lines are expected */
    g_log_set_always_fatal (G_LOG_LEVEL_ERROR);
    g_log_set_writer_func (count_elsewhere, NULL, NULL);
    test_names ();
    test_x11 ();
    test_wayland ();
    test_every_output ();
    if (fails)
        g_printerr ("signals-test: %d check(s) failed\n", fails);
    else
        g_print ("signals-test: ok\n");
    return fails ? 1 : 0;
}
