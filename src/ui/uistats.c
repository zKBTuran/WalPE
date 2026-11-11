/* uistats.c - opt-in GUI responsiveness probe (see uistats.h) */
#include "uistats.h"
#include <glib-unix.h>
#include <signal.h>
#include <stdio.h>

static struct {
    char *out;
    GArray *intervals;   /* after-paint to after-paint, us */
    GArray *paints;      /* before-paint to after-paint, us */
    GArray *stalls;      /* main-loop watchdog lateness, us */
    gint64 t_before, t_last_after, t_tick;
    gboolean attached;
} S;

#define WD_MS 5

static int cmp64 (gconstpointer a, gconstpointer b)
{
    gint64 x = *(const gint64 *) a, y = *(const gint64 *) b;
    return x < y ? -1 : x > y;
}

static void pct (FILE *f, const char *name, GArray *a)
{
    if (!a->len) {
        fprintf (f, "%s: n=0\n", name);
        return;
    }
    GArray *c = g_array_sized_new (FALSE, FALSE, sizeof (gint64), a->len);
    g_array_append_vals (c, a->data, a->len);
    g_array_sort (c, cmp64);
    gint64 *v = (gint64 *) c->data;
    guint n = c->len;
    double sum = 0;
    for (guint i = 0; i < n; i++)
        sum += v[i];
    fprintf (f, "%s: n=%u mean=%.2fms p50=%.2fms p95=%.2fms p99=%.2fms max=%.2fms\n", name, n,
             sum / n / 1000.0, v[n / 2] / 1000.0, v[(n * 95) / 100] / 1000.0,
             v[MIN (n - 1, (n * 99) / 100)] / 1000.0, v[n - 1] / 1000.0);
    g_array_free (c, TRUE);
}

static guint count_over (GArray *a, gint64 us)
{
    guint k = 0;
    for (guint i = 0; i < a->len; i++)
        if (g_array_index (a, gint64, i) > us)
            k++;
    return k;
}

static gboolean on_reset (gpointer u)
{
    (void) u;
    g_array_set_size (S.intervals, 0);
    g_array_set_size (S.paints, 0);
    g_array_set_size (S.stalls, 0);
    S.t_last_after = 0;
    return G_SOURCE_CONTINUE;
}

static gboolean on_dump (gpointer u)
{
    (void) u;
    FILE *f = fopen (S.out, "a");
    if (!f)
        return G_SOURCE_CONTINUE;
    fprintf (f, "--- walpe ui stats ---\n");
    pct (f, "frame_interval", S.intervals);
    pct (f, "frame_work", S.paints);
    pct (f, "mainloop_lateness", S.stalls);
    fprintf (f, "frames_over_33ms=%u frames_over_100ms=%u stalls_over_50ms=%u stalls_over_250ms=%u\n",
             count_over (S.intervals, 33000), count_over (S.intervals, 100000),
             count_over (S.stalls, 50000), count_over (S.stalls, 250000));
    fclose (f);
    return G_SOURCE_CONTINUE;
}

static void before_paint (GdkFrameClock *fc, gpointer u)
{
    (void) fc; (void) u;
    S.t_before = g_get_monotonic_time ();
}

static void after_paint (GdkFrameClock *fc, gpointer u)
{
    (void) fc; (void) u;
    gint64 now = g_get_monotonic_time ();
    if (S.t_before) {
        gint64 d = now - S.t_before;
        g_array_append_val (S.paints, d);
    }
    /* intervals only inside a burst: a gap > 1 s is idle, not a frame */
    if (S.t_last_after && now - S.t_last_after < G_USEC_PER_SEC) {
        gint64 d = now - S.t_last_after;
        g_array_append_val (S.intervals, d);
    }
    S.t_last_after = now;
}

static gboolean watchdog (gpointer u)
{
    (void) u;
    gint64 now = g_get_monotonic_time ();
    if (S.t_tick) {
        gint64 late = now - S.t_tick - WD_MS * 1000;
        if (late < 0)
            late = 0;
        g_array_append_val (S.stalls, late);
    }
    S.t_tick = now;
    return G_SOURCE_CONTINUE;
}

static void on_realize (GtkWidget *w, gpointer u)
{
    (void) u;
    GdkFrameClock *fc = gtk_widget_get_frame_clock (w);
    if (!fc || S.attached)
        return;
    S.attached = TRUE;
    g_signal_connect (fc, "before-paint", G_CALLBACK (before_paint), NULL);
    g_signal_connect (fc, "after-paint", G_CALLBACK (after_paint), NULL);
}

void walpe_ui_stats_attach (GtkWidget *win)
{
    const char *out = g_getenv ("WALPE_UI_STATS");
    if (!out || !*out || S.out)
        return;
    S.out = g_strdup (out);
    S.intervals = g_array_new (FALSE, FALSE, sizeof (gint64));
    S.paints = g_array_new (FALSE, FALSE, sizeof (gint64));
    S.stalls = g_array_new (FALSE, FALSE, sizeof (gint64));
    g_timeout_add (WD_MS, watchdog, NULL);
    g_unix_signal_add (SIGUSR1, on_reset, NULL);
    g_unix_signal_add (SIGUSR2, on_dump, NULL);
    if (gtk_widget_get_realized (win))
        on_realize (win, NULL);
    else
        g_signal_connect (win, "realize", G_CALLBACK (on_realize), NULL);
}
