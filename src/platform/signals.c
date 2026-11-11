/* signals.c - the registry of pause-signal providers (signals.h).
 * Main thread only; no locks. */
#include "platform/signals.h"
#include <string.h>

#define MAX_PROVIDERS 16

typedef struct {
    const WalpeSignalProvider *p;
    gboolean fits;                      /* its backends mask holds the running backend */
    gboolean running;                   /* start () answered TRUE */
    gboolean tried;                     /* start () was called */
    char *why;                          /* start () answered FALSE: its reason (may be NULL) */
    gboolean global[WALPE_SIG_N];       /* what it reported last */
    GHashTable *per_output[WALPE_SIG_N]; /* output id -> set: ids it reports TRUE for */
} Slot;

static struct {
    Slot slot[MAX_PROVIDERS];
    int n;
    gboolean started;
    WalpeBackend backend;
    const WalpeSignalSink *sink;
    gpointer sink_user;
    int batch;                          /* walpe_signals_begin depth */
    guint dirty;                        /* signals whose combined value changed in the batch */
    gboolean in_sink;                   /* the sink is running: changes made meanwhile wait */
    char *sources[WALPE_SIG_N], *missing[WALPE_SIG_N];
} R;

static Slot *slot_of (const WalpeSignalProvider *p)
{
    for (int i = 0; i < R.n; i++)
        if (R.slot[i].p == p)
            return &R.slot[i];
    return NULL;
}

static gboolean combined (WalpeSignal sig, guint32 output)
{
    gboolean per_output = (WALPE_SIG_PER_OUTPUT & WALPE_SIG_BIT (sig)) != 0;
    for (int i = 0; i < R.n; i++) {
        Slot *s = &R.slot[i];
        if (!s->running)
            continue;
        if (!per_output) {
            if (s->global[sig])
                return TRUE;
        } else if (s->per_output[sig] &&
                   g_hash_table_contains (s->per_output[sig], GUINT_TO_POINTER (output))) {
            return TRUE;
        }
    }
    return FALSE;
}

/* tell the daemon what changed, unless a batch is open */
static void flush (void)
{
    if (R.batch > 0 || R.in_sink)
        return;
    while (R.dirty) {
        guint mask = R.dirty;
        R.dirty = 0;
        if (R.sink && R.sink->changed) {
            R.in_sink = TRUE;
            R.sink->changed (mask, R.sink_user);
            R.in_sink = FALSE;      /* what a provider reported from inside it: next turn */
        }
    }
}

static void slot_start (Slot *s)
{
    if (s->tried || !s->fits)
        return;
    s->tried = TRUE;
    /* What it reported before it ran never counted and is forgotten here.
     * Kept, it would swallow the first real report: "a value the provider
     * already has" is not passed on, and the daemon would never hear of a
     * signal that was on from the start. */
    for (int k = 0; k < WALPE_SIG_N; k++) {
        s->global[k] = FALSE;
        g_clear_pointer (&s->per_output[k], g_hash_table_unref);
    }
    /* running while it starts: its first reports count */
    s->running = TRUE;
    char *why = NULL;
    gboolean ok = s->p->start ? s->p->start (s->p, &why) : TRUE;
    if (!ok) {
        s->running = FALSE;
        /* whatever it reported before giving up is void */
        for (int k = 0; k < WALPE_SIG_N; k++) {
            if (s->global[k] || (s->per_output[k] && g_hash_table_size (s->per_output[k])))
                R.dirty |= WALPE_SIG_BIT (k);
            s->global[k] = FALSE;
            g_clear_pointer (&s->per_output[k], g_hash_table_unref);
        }
        s->why = why;
        g_message ("pause signals: %s is not available here%s%s", s->p->name ? s->p->name : "?",
                   why && *why ? ": " : "", why && *why ? why : "");
        return;
    }
    g_free (why);
}

void walpe_signals_register (const WalpeSignalProvider *p)
{
    g_return_if_fail (p != NULL);
    if (slot_of (p))
        return;
    if (R.n >= MAX_PROVIDERS) {
        g_warning ("pause signals: too many providers, %s is left out", p->name ? p->name : "?");
        return;
    }
    Slot *s = &R.slot[R.n++];
    memset (s, 0, sizeof (*s));
    s->p = p;
    if (R.started) {
        s->fits = !p->backends || (p->backends & (1u << R.backend));
        walpe_signals_begin ();
        slot_start (s);
        walpe_signals_end ();
    }
}

void walpe_signals_start (WalpeBackend backend, const WalpeSignalSink *sink, gpointer user)
{
    if (R.started)
        return;
    R.started = TRUE;
    R.backend = backend;
    R.sink = sink;
    R.sink_user = user;
    walpe_signals_begin ();
    /* (a provider may register another one while it starts: R.n is read again) */
    for (int i = 0; i < R.n; i++) {
        Slot *s = &R.slot[i];
        s->fits = !s->p->backends || (s->p->backends & (1u << backend));
        slot_start (s);
    }
    walpe_signals_end ();
}

void walpe_signals_stop (void)
{
    /* Nobody is told about what happens from here on. A provider may take
     * its values back while it stops (they are dropped below anyway), and
     * the daemon stops the registry last of all, with
     * walpe_platform_close: by then it has freed its monitors, which its
     * sink walks through. */
    R.sink = NULL;
    for (int i = R.n - 1; i >= 0; i--) {
        Slot *s = &R.slot[i];
        if (s->running && s->p->stop)
            s->p->stop (s->p);
        for (int k = 0; k < WALPE_SIG_N; k++)
            g_clear_pointer (&s->per_output[k], g_hash_table_unref);
        g_free (s->why);
    }
    for (int k = 0; k < WALPE_SIG_N; k++) {
        g_clear_pointer (&R.sources[k], g_free);
        g_clear_pointer (&R.missing[k], g_free);
    }
    memset (&R, 0, sizeof (R));
}

void walpe_signal_set (const WalpeSignalProvider *p, WalpeSignal sig, guint32 output, gboolean on)
{
    Slot *s = slot_of (p);
    if (!s || (int) sig < 0 || sig >= WALPE_SIG_N)
        return;
    on = on ? TRUE : FALSE;
    gboolean per_output = (WALPE_SIG_PER_OUTPUT & WALPE_SIG_BIT (sig)) != 0;
    gboolean before = s->running ? combined (sig, per_output ? output : 0) : FALSE;
    if (!per_output) {
        if (s->global[sig] == on)
            return;
        s->global[sig] = on;
    } else {
        if (!s->per_output[sig])
            s->per_output[sig] = g_hash_table_new (NULL, NULL);
        gboolean had = g_hash_table_contains (s->per_output[sig], GUINT_TO_POINTER (output));
        if (had == on)
            return;
        if (on)
            g_hash_table_add (s->per_output[sig], GUINT_TO_POINTER (output));
        else
            g_hash_table_remove (s->per_output[sig], GUINT_TO_POINTER (output));
    }
    if (!s->running)
        return;         /* kept for nothing: a provider that is not running does not count */
    if (combined (sig, per_output ? output : 0) != before) {
        R.dirty |= WALPE_SIG_BIT (sig);
        flush ();
    }
}

void walpe_signals_begin (void)
{
    R.batch++;
}

void walpe_signals_end (void)
{
    if (R.batch > 0)
        R.batch--;
    flush ();
}

gboolean walpe_signal_get (WalpeSignal sig, guint32 output)
{
    if ((int) sig < 0 || sig >= WALPE_SIG_N)
        return FALSE;
    return combined (sig, output);
}

void walpe_signals_recheck (WalpeSignal sig, gboolean now)
{
    if ((int) sig < 0 || sig >= WALPE_SIG_N)
        return;
    for (int i = 0; i < R.n; i++) {
        Slot *s = &R.slot[i];
        if (s->running && s->p->recheck && (s->p->signals & WALPE_SIG_BIT (sig)))
            s->p->recheck (s->p, sig, now);
    }
}

const char *walpe_signal_sources (WalpeSignal sig)
{
    if ((int) sig < 0 || sig >= WALPE_SIG_N)
        return "";
    GString *g = g_string_new (NULL);
    for (int i = 0; i < R.n; i++) {
        Slot *s = &R.slot[i];
        if (s->running && (s->p->signals & WALPE_SIG_BIT (sig)))
            g_string_append_printf (g, "%s%s", g->len ? "," : "", s->p->name ? s->p->name : "?");
    }
    g_free (R.sources[sig]);
    R.sources[sig] = g_string_free (g, FALSE);
    return R.sources[sig];
}

const char *walpe_signal_missing (WalpeSignal sig)
{
    if ((int) sig < 0 || sig >= WALPE_SIG_N)
        return "";
    GString *g = g_string_new (NULL);
    for (int i = 0; i < R.n; i++) {
        Slot *s = &R.slot[i];
        if (s->tried && !s->running && (s->p->signals & WALPE_SIG_BIT (sig)))
            g_string_append_printf (g, "%s%s%s%s", g->len ? "; " : "", s->p->name ? s->p->name : "?",
                                    s->why && *s->why ? ": " : "", s->why && *s->why ? s->why : "");
    }
    g_free (R.missing[sig]);
    R.missing[sig] = g_string_free (g, FALSE);
    return R.missing[sig];
}
