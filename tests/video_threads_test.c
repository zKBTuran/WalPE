/* video_threads_test.c - decoder threads by need (src/engine/video_threads.h)
 *
 * No decoder and no display: the rule is run against a model of the frame
 * queue of video_custom.c (the decoder may be two frames ahead of the
 * presentation clock, a frame queued more than 100 ms late moves the clock).
 * What is checked: a clip never gets more threads than version 0.12 gave it and
 * starts with fewer only where the work per second allows it; a clip that
 * keeps up keeps its count; a clip that does not gets what its rate asks
 * for in one step, also when it is only a little too slow (the earlier build's rule
 * is run next to it and misses that case); one stall is not a reason. */
#include <glib.h>
#include <math.h>
#include "engine/video_threads.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; g_printerr ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* version 0.12, video_custom.c line 849: the count by picture size */
static int r12_threads (gint64 px, int cores)
{
    int want = px > 2560 * 1440 ? 4 : px > 1920 * 1080 ? 3 : px > 1280 * 720 ? 2 : 1;
    return CLAMP (MIN (want, MAX (1, cores - 1)), 1, 16);
}

typedef struct {
    double fps;             /* the clip */
    double per_thread;      /* frames a second one thread decodes */
    int threads, max;
    double stall_at, stall_s;   /* one stall of the whole machine (0 = none) */
    gboolean old_rule;             /* the earlier build's rule decides instead (one thread per round) */
    /* results */
    int raises, first_to;
    double first_at;
    double slipped;
} Sim;

/* seconds of playback through the queue model, the rule asked at every
 * frame as the decode loop does */
static void run (Sim *s, double seconds)
{
    double iv = 1.0 / s->fps, t = 0, clk = 0.040;
    guint frames = 0, late_n = 0;
    WalpeVLag lag;
    WalpeVLagOld old = { -1, 2.0 };
    gboolean stalled = FALSE;
    walpe_vlag_restart (&lag, 0);
    s->raises = s->first_to = 0;
    s->first_at = -1;
    s->slipped = 0;
    for (guint k = 0; t < seconds; k++) {
        double pts = k * iv;
        t += 1.0 / (s->per_thread * s->threads);    /* decoding it */
        if (s->stall_s > 0 && !stalled && t >= s->stall_at) {
            t += s->stall_s;
            stalled = TRUE;
        }
        /* a slot is free when the frame two before this one is due */
        t = MAX (t, clk + pts - 2 * iv);
        double late = t - (clk + pts);
        frames++;
        if (late > 0)
            late_n++;
        if (late > 0.1) {
            clk += late;
            s->slipped += late;
        }
        int to = 0;
        if (!s->old_rule) {
            to = walpe_vlag_step (&lag, t, frames, late_n, s->fps, s->threads, s->max);
        } else if (s->threads < s->max && walpe_vlag_old (&old, t, s->slipped)) {
            to = s->threads + 1;
            old.mark = -1;      /* as its decoder reopen did */
            old.mark_at = t + 2.0;
        }
        if (to > 0) {
            CHECK (to > s->threads && to <= s->max);
            if (!s->raises) {
                s->first_at = t;
                s->first_to = to;
            }
            s->raises++;
            s->threads = to;
            walpe_vlag_restart (&lag, t);
        }
    }
}

int main (void)
{
    /* ---- the count by size: never more than version 0.12, the same maximum */
    static const int sizes[][2] = { { 640, 360 }, { 1280, 720 }, { 1920, 1080 }, { 2560, 1440 },
                                    { 3440, 1440 }, { 3840, 2160 }, { 7680, 4320 } };
    for (guint i = 0; i < G_N_ELEMENTS (sizes); i++)
        for (int cores = 1; cores <= 32; cores++) {
            gint64 px = (gint64) sizes[i][0] * sizes[i][1];
            int max = -1;
            CHECK (walpe_vthreads_max (px, cores) == r12_threads (px, cores));
            for (int f = 0; f < 5; f++) {
                static const double fpss[] = { 0, 23.976, 30, 60, 144 };
                int st = walpe_vthreads_start (px, fpss[f], 8e6, cores, &max);
                CHECK (max == r12_threads (px, cores));
                CHECK (st >= 1 && st <= max);
                if (fpss[f] <= 0)
                    CHECK (st == max);      /* rate unknown: version 0.12's count */
            }
        }
    {
        int max = 0;
        /* 1080p60, 8 Mbit/s: on the decode thread itself */
        CHECK (walpe_vthreads_start (1920 * 1080, 60, 8e6, 16, &max) == 1 && max == 2);
        /* 4K60, 17.7 Mbit/s (workshop 2891310910): 3 of 4 on a 16-thread CPU; on the
         * 4-core test machine 3 of 3, which is version 0.12's count there */
        CHECK (walpe_vthreads_start (3840 * 2160, 60, 17.7e6, 16, &max) == 3 && max == 4);
        CHECK (walpe_vthreads_start (3840 * 2160, 60, 17.7e6, 4, &max) == 3 && max == 3);
        CHECK (walpe_vthreads_start (3840 * 2160, 30, 9e6, 16, &max) == 2 && max == 4);
        CHECK (walpe_vthreads_start (2560 * 1440, 144, 10.8e6, 16, &max) == 3 && max == 3);
        CHECK (walpe_vthreads_start (3840 * 2160, 60, 17.7e6, 1, &max) == 1 && max == 1);
        /* a sample table that claims an absurd rate: still a count, not an overflow */
        CHECK (walpe_vthreads_start (3840 * 2160, 1e300, 1e300, 16, &max) == 4 && max == 4);
        CHECK (walpe_vthreads_start (3840 * 2160, 1e-300, 0, 16, &max) == 1);
    }

    /* ---- a clip that keeps up keeps its (smaller) count */
    {
        Sim s = { .fps = 60, .per_thread = 80, .threads = 1, .max = 2 };
        run (&s, 120);
        CHECK (s.raises == 0 && s.threads == 1 && s.slipped == 0);
    }
    /* ---- 5 % too slow: the earlier build's rule never noticed (one 100 ms slip every
     * two seconds stays under its 300 ms in 4 s), the clip stuttered for
     * good with a thread to spare. Now it gets the thread within seconds. */
    {
        Sim s = { .fps = 60, .per_thread = 57, .threads = 1, .max = 2 };
        Sim o = { .fps = 60, .per_thread = 57, .threads = 1, .max = 2, .old_rule = TRUE };
        run (&o, 120);
        CHECK (o.raises == 0 && o.threads == 1 && o.slipped > 5.0);     /* 6 s lost in 2 minutes */
        run (&s, 120);
        CHECK (s.raises == 1 && s.threads == 2);
        CHECK (s.first_at > WALPE_VLAG_GRACE_S && s.first_at < 8.0);
        /* and from then on it keeps up */
        double before = s.slipped;
        Sim again = { .fps = 60, .per_thread = 57, .threads = 2, .max = 2 };
        run (&again, 60);
        CHECK (again.raises == 0 && again.slipped == 0);
        CHECK (before < 0.5);       /* what it cost: under half a second in total */
    }
    /* ---- 1 % too slow is found too */
    {
        Sim s = { .fps = 30, .per_thread = 29.7, .threads = 1, .max = 3 };
        Sim o = { .fps = 30, .per_thread = 29.7, .threads = 1, .max = 3, .old_rule = TRUE };
        run (&o, 120);
        CHECK (o.raises == 0);
        run (&s, 120);
        CHECK (s.raises == 1 && s.threads == 2 && s.first_at < 30.0);
    }
    /* ---- far too slow: one step to what the rate asks for (the earlier build: one
     * thread per round, each with a decoder restart at the last keyframe) */
    {
        Sim s = { .fps = 60, .per_thread = 24, .threads = 1, .max = 4 };
        Sim o = { .fps = 60, .per_thread = 24, .threads = 1, .max = 4, .old_rule = TRUE };
        run (&o, 60);
        CHECK (o.raises == 2 && o.threads == 3);    /* two restarts to get there */
        run (&s, 60);
        CHECK (s.raises == 1 && s.first_to == 3 && s.threads == 3);     /* 60 / 24 * 1.2 = 3 */
        CHECK (s.first_at < 6.0);
    }
    {
        /* what even the maximum cannot do still ends at the maximum = version 0.12 */
        Sim s = { .fps = 60, .per_thread = 10, .threads = 2, .max = 4 };
        run (&s, 60);
        CHECK (s.raises == 1 && s.threads == 4);
    }
    /* ---- one stall of the machine (0.5 s) is not a reason */
    {
        Sim s = { .fps = 60, .per_thread = 90, .threads = 1, .max = 2, .stall_at = 10, .stall_s = 0.5 };
        run (&s, 60);
        CHECK (s.slipped > 0.3 && s.raises == 0 && s.threads == 1);
    }
    /* ---- already at version 0.12's count: nothing to raise, never asked */
    {
        Sim s = { .fps = 60, .per_thread = 10, .threads = 3, .max = 3 };
        run (&s, 30);
        CHECK (s.raises == 0);
    }
    /* ---- the first two seconds after the decoder was opened are not judged */
    {
        WalpeVLag l;
        walpe_vlag_restart (&l, 100.0);
        CHECK (walpe_vlag_step (&l, 100.5, 30, 30, 60, 1, 4) == 0);
        CHECK (walpe_vlag_step (&l, 101.9, 110, 110, 60, 1, 4) == 0);
        CHECK (walpe_vlag_step (&l, 102.1, 120, 120, 60, 1, 4) == 0);      /* window opens */
        CHECK (walpe_vlag_step (&l, 103.0, 150, 150, 60, 1, 4) == 0);      /* 0.9 s: too short */
        CHECK (walpe_vlag_step (&l, 104.2, 183, 183, 60, 1, 4) == 3);      /* 30 of 60 fps, all late */
        /* a crawling clip: the window waits for eight frames */
        walpe_vlag_restart (&l, 0);
        CHECK (walpe_vlag_step (&l, 2.0, 0, 0, 60, 1, 4) == 0);
        CHECK (walpe_vlag_step (&l, 4.5, 5, 5, 60, 1, 4) == 0);
        CHECK (walpe_vlag_step (&l, 6.0, 8, 8, 60, 1, 4) == 4);
        /* half of the frames late is not "behind" */
        walpe_vlag_restart (&l, 0);
        CHECK (walpe_vlag_step (&l, 2.0, 0, 0, 60, 1, 4) == 0);
        CHECK (walpe_vlag_step (&l, 4.1, 120, 60, 60, 1, 4) == 0);
    }
    if (fails)
        g_printerr ("%d check(s) failed\n", fails);
    else
        g_print ("video-threads: ok\n");
    return fails ? 1 : 0;
}
