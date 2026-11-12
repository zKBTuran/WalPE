/* video_threads.h - how many libavcodec threads a clip decodes with
 *
 * Pure arithmetic, no decoder in it: video_custom.c applies it and
 * tests/video_threads_test.c runs it against a simulated decoder.
 *
 * Version 0.12 sized the thread count by the picture alone (1 up to 720p, 2 up
 * to 1080p, 3 up to 1440p, 4 above, never more than cores - 1). Version 0.12.1
 * starts with what the clip is expected to need (pixels and bits per
 * second) and keeps version 0.12's count as the maximum. The rule that makes
 * this safe: a clip that does not keep up on the smaller count gets more,
 * so that no clip ends up with less than version 0.12 gave it when it needs
 * it. That rule was too weak in the earlier build (walpe_vlag_old below):
 *  - it looked at the presentation clock's slips only (one slip = the
 *    decoder 100 ms late), wanted 300 ms of them inside a fixed 4 s
 *    window, and so never fired for a decoder up to 7.5 % too slow: such a
 *    clip stuttered for good with threads to spare;
 *  - it added one thread per round, and every round reopened the decoder
 *    and decoded the clip again from the last keyframe (a freeze that
 *    grows with the keyframe distance), up to three times in a row.
 * Now: the share of frames that were queued after their presentation time
 * decides (a decoder that keeps up queues every frame ahead of time), the
 * count goes straight to what the measured rate asks for, and the
 * decoder is reopened where the next sequence starts (video_custom.c:
 * nothing is decoded twice, nothing freezes). */
#pragma once
#include <glib.h>
#include <math.h>

/* seconds after the decoder was (re)opened that are not judged */
#define WALPE_VLAG_GRACE_S 2.0
/* one judgement covers at least this long ... */
#define WALPE_VLAG_WINDOW_S 2.0
/* ... and this many frames (a clip that crawls gets a longer window, up
 * to WALPE_VLAG_WINDOW_MAX_S) */
#define WALPE_VLAG_MIN_FRAMES 8
#define WALPE_VLAG_WINDOW_MAX_S 8.0

/* version 0.12's count for a picture of px pixels: the most a clip may get */
static inline int walpe_vthreads_max (gint64 px, int cores)
{
    int most = px > 2560 * 1440 ? 4 : px > 1920 * 1080 ? 3 : px > 1280 * 720 ? 2 : 1;
    return MIN (most, MAX (1, cores - 1));
}

/* what a clip starts with: sized by the work per second, pixels and bits,
 * at rates a slow core reaches (2.5 ns per pixel, 6.7 ns per bit: the
 * measured cost on the loaded 2.1 GHz test machine; a current desktop
 * core is 2-3 times faster), with half as much again in reserve; never
 * more than version 0.12's count (*max). fps <= 0 (unknown) = that count. */
static inline int walpe_vthreads_start (gint64 px, double fps, double bitrate, int cores, int *max)
{
    int most = walpe_vthreads_max (px, cores);
    if (max)
        *max = most;
    if (fps <= 0 || most <= 1)
        return most;
    double load = fps * (double) px * 2.5e-9 + bitrate * 6.7e-9;    /* cores of work */
    /* (limited as a double: a damaged sample table can claim any rate) */
    double want = ceil (load * 1.5);
    return want >= most ? most : want >= 1 ? (int) want : 1;
}

/* is the decoder keeping up? Fed with two running totals: frames queued,
 * and frames queued after their presentation time. */
typedef struct {
    double from;                /* the window's start; before it nothing is judged */
    gboolean armed;             /* frames0 / late0 hold the totals at `from` */
    guint frames0, late0;
} WalpeVLag;

/* the decoder was (re)opened at `now`: judge again after the grace time */
static inline void walpe_vlag_restart (WalpeVLag *l, double now)
{
    l->from = now + WALPE_VLAG_GRACE_S;
    l->armed = FALSE;
    l->frames0 = l->late0 = 0;
}

/* Called now and then (every packet) with the totals. Returns the thread
 * count to go to when the clip did not keep up during the last window
 * (three of four frames were late), else 0. need_fps = the clip's rate. */
static inline int walpe_vlag_step (WalpeVLag *l, double now, guint frames, guint late,
                                   double need_fps, int threads, int max)
{
    if (threads >= max || now < l->from)
        return 0;
    if (!l->armed) {
        l->armed = TRUE;
        l->from = now;
        l->frames0 = frames;
        l->late0 = late;
        return 0;
    }
    double dt = now - l->from;
    guint nf = frames - l->frames0, nl = late - l->late0;
    if (dt < WALPE_VLAG_WINDOW_S || (nf < WALPE_VLAG_MIN_FRAMES && dt < WALPE_VLAG_WINDOW_MAX_S))
        return 0;
    l->from = now;              /* the next window starts here */
    l->frames0 = frames;
    l->late0 = late;
    if (nf < 2 || nl * 4 < nf * 3)
        return 0;
    /* threads scale the rate about linearly: what the clip's rate needs,
     * a fifth more in reserve, at least one more, at most version 0.12's */
    double got = nf / dt;
    double want = need_fps > got ? ceil (threads * (need_fps / got) * 1.2 - 1e-9) : threads + 1;
    return want >= max ? max : want > threads + 1 ? (int) want : threads + 1;
}

/* ---- the other direction (asked for with WALPE_VIDEO_THREADS_LOWER=1; off
 * otherwise, and then nothing below is ever called).
 *
 * The count a clip starts with is sized for a slow core (above). On a
 * faster one the decoder then runs with frame threads it does not need:
 * each holds a picture pool of its own (measured: 20 to 47 MB for
 * 3840x2160, 10 to 22 for 2560x1440, 5 to 13 for 1920x1080) and costs
 * thread wake-ups for every picture, while the CPU time per picture stays
 * the same.
 *
 * What tells is the CPU time the decoder really used: the decode thread's
 * own and that of libavcodec's frame threads, over a window of ten seconds
 * in which no frame was late. (How long the decode thread waits for a free
 * queue slot does NOT tell: with frame threads it waits while the threads
 * still work.) When that work would keep each of threads - 1 threads busy
 * no more than half of the time, one thread goes - where the next sequence
 * starts, exactly as threads are added - and the decoder is asked again
 * with the smaller count. A clip that falls behind afterwards gets its
 * threads back by the rule above and keeps them (video_custom.c: such a
 * clip is not asked again). */
#define WALPE_VIDLE_WINDOW_S 10.0
#define WALPE_VIDLE_MIN_FRAMES 120
/* the share of its time each of the remaining threads may be busy, at most */
#define WALPE_VIDLE_BUSY_MAX 0.5
/* windows judged after the decoder was opened; then the answer stands
 * (a clip does not change) */
#define WALPE_VIDLE_WINDOWS 6

typedef struct {
    double from;                /* the window's start; before it nothing is judged */
    gboolean armed;             /* the totals below are those at `from` */
    double cpu0;
    guint frames0, late0;
    int windows;                /* judged so far */
} WalpeVIdle;

/* the decoder was (re)opened, or playback continues after a pause, at `now` */
static inline void walpe_vidle_restart (WalpeVIdle *l, double now)
{
    l->from = now + WALPE_VLAG_GRACE_S;
    l->armed = FALSE;
    l->cpu0 = 0;
    l->frames0 = l->late0 = 0;
    l->windows = 0;
}

/* Asked now and then (every packet), with the frames queued so far: is the
 * CPU time wanted now - to open a window or to judge one? (Reading it
 * costs a look at every thread of the process: only then.) */
static inline gboolean walpe_vidle_wants (const WalpeVIdle *l, double now, guint frames, int threads)
{
    if (threads <= 1 || l->windows >= WALPE_VIDLE_WINDOWS || now < l->from)
        return FALSE;
    return !l->armed || (now - l->from >= WALPE_VIDLE_WINDOW_S &&
                         frames - l->frames0 >= WALPE_VIDLE_MIN_FRAMES);
}

/* ... and when it is: cpu = CPU seconds the decoder has used in total,
 * frames / late = frames queued, and those queued after their presentation
 * time. Returns threads - 1 when the decoder would keep up with one thread
 * less, else 0. *busy (may be NULL): the cores of work in the window that
 * was just judged (CPU seconds per second), -1 = none was judged. */
static inline int walpe_vidle_step (WalpeVIdle *l, double now, double cpu, guint frames, guint late,
                                    int threads, double *busy)
{
    if (busy)
        *busy = -1;
    if (!walpe_vidle_wants (l, now, frames, threads))
        return 0;
    const gboolean judge = l->armed && cpu >= l->cpu0;  /* (less than before: threads of another
                                                         * decoder ended meanwhile - no answer) */
    const double dt = now - l->from, work = judge ? (cpu - l->cpu0) / dt : 0;
    const guint nl = late - l->late0;
    l->armed = TRUE;
    l->from = now;              /* the next window starts here */
    l->cpu0 = cpu;
    l->frames0 = frames;
    l->late0 = late;
    if (!judge)
        return 0;
    l->windows++;
    if (busy)
        *busy = work;
    if (nl > 0 || work > WALPE_VIDLE_BUSY_MAX * (threads - 1))
        return 0;
    return threads - 1;
}

/* the earlier build's rule, kept for the test that shows what it missed: `slipped`
 * = seconds the presentation clock was moved back in total; TRUE = one
 * more thread */
typedef struct {
    double mark, mark_at;       /* mark < 0: no window yet */
} WalpeVLagOld;

static inline gboolean walpe_vlag_old (WalpeVLagOld *l, double now, double slipped)
{
    if (now < l->mark_at)
        return FALSE;
    if (l->mark < 0 || now - l->mark_at > 4.0) {
        l->mark = slipped;
        l->mark_at = now;
        return FALSE;
    }
    return slipped - l->mark >= 0.3;
}
