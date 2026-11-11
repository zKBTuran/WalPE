/* cpuphase.c - see cpuphase.h */
#include "cpuphase.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

gint walpe_cph_level = -1;

typedef struct {
    WalpeCphStats st;
    int cur;
    gint64 last_ns;
} CphThread;

static __thread CphThread cph = { .cur = -1 };

static gint64 cph_now (void)
{
    struct timespec ts;
    clock_gettime (CLOCK_THREAD_CPUTIME_ID, &ts);
    return (gint64) ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static gboolean cph_on (void)
{
    gint lv = g_atomic_int_get (&walpe_cph_level);
    if (lv < 0) {
        const char *e = g_getenv ("WALPE_CPU_PHASES");
        int n = e ? atoi (e) : 0;
        lv = CLAMP (n, 0, 2);
        g_atomic_int_set (&walpe_cph_level, lv);
    }
    return lv > 0;
}

int walpe_cph_switch (int phase)
{
    if (!cph_on ())
        return phase;
    gint64 now = cph_now ();
    int prev = cph.cur;
    if (cph.last_ns && prev >= 0)
        cph.st.ms[prev] += (double) (now - cph.last_ns) / 1e6;
    cph.last_ns = now;
    cph.cur = phase < 0 ? -1 : MIN (phase, WALPE_CPH_N - 1);
    return prev;
}

WalpeCphStats *walpe_cph_stats (void)
{
    return cph_on () ? &cph.st : NULL;
}

void walpe_cph_get (WalpeCphStats *out, gboolean reset)
{
    if (!cph_on ()) {
        memset (out, 0, sizeof (*out));
        return;
    }
    walpe_cph_switch (cph.cur);   /* the running phase's time up to now */
    *out = cph.st;
    if (reset)
        memset (&cph.st, 0, sizeof (cph.st));
}

guint64 walpe_cph_fnv (guint64 h, const void *p, gsize n)
{
    const guint8 *b = p;
    if (!h)
        h = 1469598103934665603ULL;
    for (gsize i = 0; i < n; i++)
        h = (h ^ b[i]) * 1099511628211ULL;
    return h;
}
