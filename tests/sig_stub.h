/* sig_stub.h - a stand-in for the daemon's signal registry
 * (platform/signals.h) for the provider tests: it starts providers, keeps
 * what each one reports and answers the status questions, with nothing of
 * the daemon linked in. Include it in exactly one file of a test. */
#pragma once
#include <glib.h>
#include <stdio.h>
#include <string.h>
#include "platform/signals.h"

#define STUB_MAX_PROV 16
#define STUB_MAX_VAL 256

typedef struct {
    const WalpeSignalProvider *p;
    gboolean started, failed;
    char *why;
} StubProv;

typedef struct {
    const WalpeSignalProvider *p;
    WalpeSignal sig;
    guint32 output;
    gboolean on;
} StubVal;

static struct {
    StubProv prov[STUB_MAX_PROV];
    guint n_prov;
    StubVal val[STUB_MAX_VAL];
    guint n_val;
    gboolean running;
    WalpeBackend backend;
    int depth;              /* begin / end nesting */
    guint sets;             /* walpe_signal_set calls */
    guint batches;          /* outermost begin / end pairs */
    char *sources[WALPE_SIG_N], *missing[WALPE_SIG_N];
} Stub;

static void stub_start_one (StubProv *sp)
{
    const WalpeSignalProvider *p = sp->p;
    if (sp->started || sp->failed)
        return;
    if (p->backends && !(p->backends & (1u << Stub.backend)))
        return;     /* not for this backend: neither running nor missing */
    char *why = NULL;
    if (p->start (p, &why)) {
        sp->started = TRUE;
        g_free (why);
    } else {
        sp->failed = TRUE;
        sp->why = why;
    }
}

void walpe_signals_register (const WalpeSignalProvider *p)
{
    g_assert (Stub.n_prov < STUB_MAX_PROV);
    for (guint i = 0; i < Stub.n_prov; i++)
        if (Stub.prov[i].p == p)
            return;
    Stub.prov[Stub.n_prov].p = p;
    Stub.n_prov++;
    if (Stub.running)
        stub_start_one (&Stub.prov[Stub.n_prov - 1]);
}

void walpe_signals_start (WalpeBackend backend, const WalpeSignalSink *sink, gpointer user)
{
    (void) sink; (void) user;
    Stub.backend = backend;
    Stub.running = TRUE;
    for (guint i = 0; i < Stub.n_prov; i++)
        stub_start_one (&Stub.prov[i]);
}

void walpe_signals_stop (void)
{
    for (guint i = 0; i < Stub.n_prov; i++) {
        StubProv *sp = &Stub.prov[i];
        if (sp->started)
            sp->p->stop (sp->p);
        sp->started = sp->failed = FALSE;
        g_clear_pointer (&sp->why, g_free);
    }
    Stub.n_val = 0;
    Stub.running = FALSE;
}

/* forget the registered providers too (between test cases) */
static G_GNUC_UNUSED void stub_reset (void)
{
    walpe_signals_stop ();
    Stub.n_prov = 0;
    Stub.sets = Stub.batches = 0;
}

void walpe_signal_set (const WalpeSignalProvider *p, WalpeSignal sig, guint32 output, gboolean on)
{
    Stub.sets++;
    if (!(WALPE_SIG_PER_OUTPUT & WALPE_SIG_BIT (sig)))
        output = 0;
    for (guint i = 0; i < Stub.n_val; i++) {
        StubVal *v = &Stub.val[i];
        if (v->p == p && v->sig == sig && v->output == output) {
            v->on = on;
            return;
        }
    }
    g_assert (Stub.n_val < STUB_MAX_VAL);
    Stub.val[Stub.n_val++] = (StubVal) { p, sig, output, on };
}

void walpe_signals_begin (void)
{
    Stub.depth++;
}

void walpe_signals_end (void)
{
    g_assert (Stub.depth > 0);
    if (--Stub.depth == 0)
        Stub.batches++;
}

gboolean walpe_signal_get (WalpeSignal sig, guint32 output)
{
    if (!(WALPE_SIG_PER_OUTPUT & WALPE_SIG_BIT (sig)))
        output = 0;
    /* (a value given for WALPE_OUTPUT_ALL holds for every output) */
    for (guint i = 0; i < Stub.n_val; i++)
        if (Stub.val[i].sig == sig && Stub.val[i].on &&
            (Stub.val[i].output == output || Stub.val[i].output == WALPE_OUTPUT_ALL))
            return TRUE;
    return FALSE;
}

void walpe_signals_recheck (WalpeSignal sig, gboolean now)
{
    for (guint i = 0; i < Stub.n_prov; i++) {
        StubProv *sp = &Stub.prov[i];
        if (sp->started && sp->p->recheck && (sp->p->signals & WALPE_SIG_BIT (sig)))
            sp->p->recheck (sp->p, sig, now);
    }
}

const char *walpe_signal_sources (WalpeSignal sig)
{
    GString *s = g_string_new (NULL);
    for (guint i = 0; i < Stub.n_prov; i++) {
        StubProv *sp = &Stub.prov[i];
        if (sp->started && (sp->p->signals & WALPE_SIG_BIT (sig)))
            g_string_append_printf (s, "%s%s", s->len ? "," : "", sp->p->name);
    }
    g_free (Stub.sources[sig]);
    Stub.sources[sig] = g_string_free (s, FALSE);
    return Stub.sources[sig];
}

const char *walpe_signal_missing (WalpeSignal sig)
{
    GString *s = g_string_new (NULL);
    for (guint i = 0; i < Stub.n_prov; i++) {
        StubProv *sp = &Stub.prov[i];
        if (sp->failed && (sp->p->signals & WALPE_SIG_BIT (sig)))
            g_string_append_printf (s, "%s%s", s->len ? "; " : "",
                                    sp->why ? sp->why : sp->p->name);
    }
    g_free (Stub.missing[sig]);
    Stub.missing[sig] = g_string_free (s, FALSE);
    return Stub.missing[sig];
}

/* ---- helpers for the tests */

static G_GNUC_UNUSED StubProv *stub_prov (const WalpeSignalProvider *p)
{
    for (guint i = 0; i < Stub.n_prov; i++)
        if (Stub.prov[i].p == p)
            return &Stub.prov[i];
    return NULL;
}

/* the value one provider gave: 1 on, 0 off, -1 never set */
static G_GNUC_UNUSED int stub_value (const WalpeSignalProvider *p, WalpeSignal sig, guint32 output)
{
    if (!(WALPE_SIG_PER_OUTPUT & WALPE_SIG_BIT (sig)))
        output = 0;
    for (guint i = 0; i < Stub.n_val; i++)
        if (Stub.val[i].p == p && Stub.val[i].sig == sig && Stub.val[i].output == output)
            return Stub.val[i].on ? 1 : 0;
    return -1;
}

static int stub_fails;
#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            stub_fails++;                                                     \
            fprintf (stderr, "FAIL %s:%d: (%s) ", __FILE__, __LINE__, #cond); \
            fprintf (stderr, __VA_ARGS__);                                    \
            fputc ('\n', stderr);                                             \
        }                                                                     \
    } while (0)

/* run the default main context until cond () holds or ms have passed */
static G_GNUC_UNUSED gboolean stub_wait (gboolean (*cond) (gpointer), gpointer user, int ms)
{
    gint64 end = g_get_monotonic_time () + (gint64) ms * 1000;
    while (!cond (user)) {
        if (g_get_monotonic_time () > end)
            return FALSE;
        if (!g_main_context_iteration (NULL, FALSE))
            g_usleep (2000);
    }
    return TRUE;
}
