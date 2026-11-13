/* scenesound_server_test.c - scene sound and the sound server
 * (src/engine/scenesound.c, out_connect).
 *
 * No sound server is contacted and nothing is played: the five functions
 * of libpulse-simple are replaced by the stand-ins below
 * (engine/scenesound_server.h) before the first stream is opened.
 *
 *  1. The players of a scene start together, each with a connection of its
 *     own. A server takes connections from a short queue, and what does
 *     not fit is refused: streams are opened one at a time, and no player
 *     is left to play into nothing meanwhile.
 *  2. A server that does not answer the stream being opened holds that one
 *     player. The others do not wait behind it without a limit: they play
 *     into nothing, as after a refusal, and their group can be freed.
 *  3. A connection the server refuses (its queue is full of other clients)
 *     is asked for again in the same turn, and heard; one it refuses five
 *     times running plays into nothing, and the status line says what the
 *     server said.
 *  4. A server that is slow but answers - more time for all the streams
 *     together than one turn may last - loses nobody: the limit is on the
 *     turn, not on the queue.
 *  5. A stream the server stops serving while the server itself is there
 *     (the calls come back with "Timeout": scenesound_pulse.c gives each a
 *     time limit; pipewire-pulse 1.0.5 does this to a stream now and then
 *     when it is drained): a drain that is not answered when the wallpaper
 *     is paused - the stream is given up without another call, and the
 *     resume plays through a new one; a write that is not taken after a
 *     resume - a new stream is asked for with the next block, not five
 *     seconds later. In both cases the player is heard again. */
#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include <pulse/def.h>
#include "engine/scenesound.h"
#include "engine/scenesound_server.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf ("FAIL %s:%d: ", __FILE__, __LINE__); printf (__VA_ARGS__); printf ("\n"); } } while (0)
/* waits up to ms milliseconds for cond */
#define WAIT(cond, ms) do { for (int w_ = 0; w_ < (ms) && !(cond); w_++) g_usleep (1000); } while (0)

/* ---- the stand-in server ---- */
typedef struct { guint bytes_per_s; } Stream;
static gint inside;          /* calls of new_stream that have not returned */
static gint most_inside;     /* the largest number of them at one time */
static gint opened, closed;
static gint hang;            /* 1: the next new_stream does not return until this is 0 again */
static gint hanging;         /* a new_stream is held */
static gint refuse_first;    /* so many of a player's first calls are refused */
static gint refused;         /* calls that were refused */
static GPrivate asked;       /* calls of this thread (one player) so far */
static gint open_us = 3000;  /* the round trips of opening a stream */
/* the lines the players print when they are left without an output */
static gint said_nothing;    /* "... playing into nothing ..." */
static gint said_lines;      /* all of them */

static void log_count (const gchar *domain, GLogLevelFlags level, const gchar *msg, gpointer user)
{
    (void) domain; (void) level; (void) user;
    if (msg && strstr (msg, "playing into nothing"))
        g_atomic_int_inc (&said_nothing);
    g_atomic_int_inc (&said_lines);
}

static gint stall_drain;     /* 1: drains are not answered (they fail with Timeout, as after the limit) */
static gint stall_write;     /* so many writes are not taken (the same) */
static gint drains, flushes; /* calls counted */
static gint after_stall;     /* calls into a stream after it failed with Timeout, other than free */
typedef struct { Stream st; gint stalled; } StreamS;   /* (Stream is its first member: what srv_new makes) */

static pa_simple *srv_new (const char *server, const char *app, pa_stream_direction_t dir, const char *device,
                           const char *stream, const pa_sample_spec *spec, const pa_channel_map *map,
                           const pa_buffer_attr *attr, int *error)
{
    (void) server; (void) app; (void) dir; (void) device; (void) stream; (void) map; (void) attr;
    const gint now = g_atomic_int_add (&inside, 1) + 1;
    for (;;) {
        const gint most = g_atomic_int_get (&most_inside);
        if (now <= most || g_atomic_int_compare_and_exchange (&most_inside, most, now))
            break;
    }
    const int mine = GPOINTER_TO_INT (g_private_get (&asked));
    g_private_set (&asked, GINT_TO_POINTER (mine + 1));
    if (mine < g_atomic_int_get (&refuse_first)) {
        /* the queue is full: a connect () fails at once */
        g_atomic_int_inc (&refused);
        g_atomic_int_add (&inside, -1);
        if (error)
            *error = PA_ERR_CONNECTIONREFUSED;
        return NULL;
    }
    if (g_atomic_int_compare_and_exchange (&hang, 1, 2)) {
        g_atomic_int_set (&hanging, 1);
        while (g_atomic_int_get (&hang))
            g_usleep (1000);
        g_atomic_int_set (&hanging, 0);
    } else {
        g_usleep ((gulong) g_atomic_int_get (&open_us));
    }
    Stream *st = (Stream *) g_new0 (StreamS, 1);
    st->bytes_per_s = MAX (spec->rate * spec->channels * 2u, 1u);
    g_atomic_int_inc (&opened);
    g_atomic_int_add (&inside, -1);
    if (error)
        *error = 0;
    return (pa_simple *) st;
}

static void srv_free (pa_simple *s)
{
    g_atomic_int_inc (&closed);
    g_free (s);
}

/* blocks for as long as the data sounds, like a stream whose buffer is full */
static int srv_write (pa_simple *s, const void *data, size_t bytes, int *error)
{
    (void) data;
    if (g_atomic_int_get (&((StreamS *) s)->stalled))
        g_atomic_int_inc (&after_stall);
    for (;;) {
        const gint n = g_atomic_int_get (&stall_write);
        if (n <= 0)
            break;
        if (g_atomic_int_compare_and_exchange (&stall_write, n, n - 1)) {
            g_atomic_int_set (&((StreamS *) s)->stalled, 1);
            if (error)
                *error = PA_ERR_TIMEOUT;
            return -1;
        }
    }
    g_usleep ((gulong) ((guint64) bytes * G_USEC_PER_SEC / ((Stream *) s)->bytes_per_s));
    return 0;
}

static int srv_drain (pa_simple *s, int *error)
{
    g_atomic_int_inc (&drains);
    if (g_atomic_int_get (&((StreamS *) s)->stalled))
        g_atomic_int_inc (&after_stall);
    if (g_atomic_int_get (&stall_drain)) {
        g_atomic_int_set (&((StreamS *) s)->stalled, 1);
        if (error)
            *error = PA_ERR_TIMEOUT;
        return -1;
    }
    return 0;
}

static int srv_flush (pa_simple *s, int *error)
{
    (void) error;
    g_atomic_int_inc (&flushes);
    if (g_atomic_int_get (&((StreamS *) s)->stalled))
        g_atomic_int_inc (&after_stall);
    return 0;
}

static const WalpeSoundServerCalls srv = { srv_new, srv_free, srv_write, srv_drain, srv_flush };

/* ---- helpers ---- */
static void put32 (guint8 *p, guint32 v) { p[0] = v & 255; p[1] = (v >> 8) & 255; p[2] = (v >> 16) & 255; p[3] = (guint8) (v >> 24); }

/* one second of a triangle wave, 48000 Hz mono 16 bit */
static char *write_wav (const char *dir)
{
    enum { FRAMES = 48000 };
    guint8 *b = g_malloc0 (44 + FRAMES * 2);
    memcpy (b, "RIFF", 4);
    put32 (b + 4, 36 + FRAMES * 2);
    memcpy (b + 8, "WAVEfmt ", 8);
    put32 (b + 16, 16);
    b[20] = 1;
    b[22] = 1;
    put32 (b + 24, 48000);
    put32 (b + 28, 96000);
    b[32] = 2;
    b[34] = 16;
    memcpy (b + 36, "data", 4);
    put32 (b + 40, FRAMES * 2);
    for (int i = 0; i < FRAMES; i++) {
        const int v = (i % 100 < 50 ? i % 100 : 100 - i % 100) * 200 - 5000;
        b[44 + i * 2] = (guint8) (v & 255);
        b[45 + i * 2] = (guint8) ((v >> 8) & 255);
    }
    char *path = g_build_filename (dir, "tone.wav", NULL);
    if (!g_file_set_contents (path, (const char *) b, 44 + FRAMES * 2, NULL))
        g_clear_pointer (&path, g_free);
    g_free (b);
    return path;
}

static WalpeSound *player (WalpeSoundGroup *g, const char *file)
{
    const char *files[1] = { file };
    WalpeSoundSpec spec = { .files = files, .nfiles = 1, .volume = 1.0f, .mode = WALPE_SOUND_LOOP, .name = "layer" };
    WalpeSound *s = walpe_sound_new (g, &spec);
    walpe_sound_play (s);
    return s;
}

static gint64 frames_of (WalpeSound *s)
{
    WalpeSoundStats st;
    walpe_sound_stats (s, &st);
    return st.frames;
}

static gboolean threads_gone (void)
{
    WAIT (walpe_sound_threads () == 0, 10000);
    return walpe_sound_threads () == 0;
}

/* ---- 1 ---- */
static void test_one_at_a_time (const char *wav)
{
    enum { N = WALPE_SOUND_MAX_STREAMS };
    WalpeSoundGroup *g = walpe_sound_group_new ("/w/server-a", 1);
    WalpeSound *p[N];
    for (int i = 0; i < N; i++)
        p[i] = player (g, wav);
    walpe_sound_group_set_active (g, TRUE);   /* all of them start now */
    WAIT (g_atomic_int_get (&opened) == N, 10000);
    CHECK (g_atomic_int_get (&opened) == N, "%d players: %d streams were opened", N, g_atomic_int_get (&opened));
    CHECK (g_atomic_int_get (&most_inside) == 1, "streams are opened one at a time: %d at once",
           g_atomic_int_get (&most_inside));
    /* (a player that gave up its turn and came back five seconds later
     * has its stream as well by now: the wait above does not tell) */
    CHECK (g_atomic_int_get (&said_nothing) == 0, "none of them was left to play into nothing meanwhile: %d were",
           g_atomic_int_get (&said_nothing));
    int sounding = 0;
    for (int w = 0; w < 5000 && sounding < N; w++) {
        g_usleep (1000);
        sounding = 0;
        for (int i = 0; i < N; i++)
            sounding += frames_of (p[i]) > 0;
    }
    char *line = walpe_sound_status ();
    CHECK (sounding == N && line && strstr (line, "through PulseAudio") && !strstr (line, "into nothing"),
           "all %d are on the server (%d sound): %s", N, sounding, line ? line : "(null)");
    g_free (line);
    walpe_sound_group_free (g);
    CHECK (threads_gone (), "the threads end: %d left", walpe_sound_threads ());
    CHECK (g_atomic_int_get (&closed) == g_atomic_int_get (&opened), "every stream was closed: %d of %d",
           g_atomic_int_get (&closed), g_atomic_int_get (&opened));
}

/* ---- 2 ---- */
static void test_server_does_not_answer (const char *wav)
{
    enum { N = 4 };
    const gint opened0 = g_atomic_int_get (&opened);
    g_atomic_int_set (&hang, 1);
    WalpeSoundGroup *g = walpe_sound_group_new ("/w/server-b", 1);
    WalpeSound *p[N];
    for (int i = 0; i < N; i++)
        p[i] = player (g, wav);
    walpe_sound_group_set_active (g, TRUE);
    WAIT (g_atomic_int_get (&hanging), 5000);
    CHECK (g_atomic_int_get (&hanging), "one stream is being opened and gets no answer");
    /* the other three give up their turn and play into nothing */
    int going = 0;
    for (int w = 0; w < 5000 && going < N - 1; w++) {
        g_usleep (1000);
        going = 0;
        for (int i = 0; i < N; i++)
            going += frames_of (p[i]) > 0;
    }
    char *line = walpe_sound_status ();
    CHECK (going == N - 1 && g_atomic_int_get (&hanging) && g_atomic_int_get (&opened) == opened0 && line &&
           strstr (line, "3 stream(s) play into nothing"),
           "behind a server that does not answer, the other players go on without it (%d of %d): %s", going,
           N - 1, line ? line : "(null)");
    g_free (line);
    /* their group is freed meanwhile: three threads end, the held one stays */
    walpe_sound_group_free (g);
    WAIT (walpe_sound_threads () == 1, 10000);
    CHECK (walpe_sound_threads () == 1, "freed with one stream still being opened: %d thread(s) left",
           walpe_sound_threads ());
    g_atomic_int_set (&hang, 0);   /* the server answers after all */
    CHECK (threads_gone (), "the last thread ends: %d left", walpe_sound_threads ());
    CHECK (g_atomic_int_get (&closed) == g_atomic_int_get (&opened), "every stream was closed: %d of %d",
           g_atomic_int_get (&closed), g_atomic_int_get (&opened));
}

/* ---- 3 ---- */
static void test_refused_asks_again (const char *wav)
{
    enum { N = 6, FIRST = 2 };
    const gint opened0 = g_atomic_int_get (&opened), nothing0 = g_atomic_int_get (&said_nothing);
    g_atomic_int_set (&most_inside, 0);
    g_atomic_int_set (&refused, 0);
    g_atomic_int_set (&refuse_first, FIRST);
    WalpeSoundGroup *g = walpe_sound_group_new ("/w/server-c", 1);
    for (int i = 0; i < N; i++)
        player (g, wav);
    walpe_sound_group_set_active (g, TRUE);
    /* (three seconds: less than the five after which a layer that plays
     * into nothing asks the server again) */
    WAIT (g_atomic_int_get (&opened) == opened0 + N, 3000);
    CHECK (g_atomic_int_get (&opened) == opened0 + N && g_atomic_int_get (&refused) == N * FIRST,
           "%d players, each refused %d times first: %d streams are open after %d refusals", N, FIRST,
           g_atomic_int_get (&opened) - opened0, g_atomic_int_get (&refused));
    CHECK (g_atomic_int_get (&said_nothing) == nothing0 && g_atomic_int_get (&most_inside) == 1,
           "... in the same turn: %d played into nothing meanwhile, %d asked at a time",
           g_atomic_int_get (&said_nothing) - nothing0, g_atomic_int_get (&most_inside));
    walpe_sound_group_free (g);
    CHECK (threads_gone (), "the threads end: %d left", walpe_sound_threads ());

    /* refused every time (no server is there): the first attempt and
     * four more for every one of them, then into nothing - with the
     * server's reason, also for those that were far back in the queue */
    enum { M = WALPE_SOUND_MAX_STREAMS, EVERY = 1000 };
    g_atomic_int_set (&refused, 0);
    g_atomic_int_set (&refuse_first, EVERY);
    const gint opened1 = g_atomic_int_get (&opened);
    WalpeSound *q[M];
    g = walpe_sound_group_new ("/w/server-d", 1);
    for (int i = 0; i < M; i++)
        q[i] = player (g, wav);
    walpe_sound_group_set_active (g, TRUE);
    int going = 0;
    for (int w = 0; w < 4000 && (going < M || g_atomic_int_get (&refused) < M * 5); w++) {
        g_usleep (1000);
        going = 0;
        for (int i = 0; i < M; i++)
            going += frames_of (q[i]) > 0;
    }
    char *line = walpe_sound_status (), *want = g_strdup_printf ("%d stream(s) play into nothing", M);
    CHECK (going == M && g_atomic_int_get (&refused) == M * 5 && g_atomic_int_get (&opened) == opened1 && line &&
           strstr (line, want) && strstr (line, "Connection refused"),
           "refused every time: %d attempts for %d players, then into nothing: %s", g_atomic_int_get (&refused), M,
           line ? line : "(null)");
    g_free (want);
    g_free (line);
    CHECK (g_atomic_int_get (&said_nothing) == nothing0 + M, "... and each of them says so once: %d line(s)",
           g_atomic_int_get (&said_nothing) - nothing0);
    walpe_sound_group_free (g);
    g_atomic_int_set (&refuse_first, 0);
    CHECK (threads_gone (), "the threads end: %d left", walpe_sound_threads ());
    CHECK (g_atomic_int_get (&closed) == g_atomic_int_get (&opened), "every stream was closed: %d of %d",
           g_atomic_int_get (&closed), g_atomic_int_get (&opened));
}

/* ---- 4 ---- */
static void test_slow_server (const char *wav)
{
    /* 40 ms a stream: the sixteenth has waited 0.6 s when its turn comes,
     * more than twice the time one turn may last */
    enum { N = WALPE_SOUND_MAX_STREAMS };
    const gint opened0 = g_atomic_int_get (&opened), nothing0 = g_atomic_int_get (&said_nothing);
    g_atomic_int_set (&most_inside, 0);
    g_atomic_int_set (&open_us, 40000);
    WalpeSoundGroup *g = walpe_sound_group_new ("/w/server-e", 1);
    for (int i = 0; i < N; i++)
        player (g, wav);
    walpe_sound_group_set_active (g, TRUE);
    WAIT (g_atomic_int_get (&opened) == opened0 + N, 4000);
    CHECK (g_atomic_int_get (&opened) == opened0 + N && g_atomic_int_get (&most_inside) == 1,
           "a server that takes 40 ms a stream: %d of %d streams are open, %d asked at a time",
           g_atomic_int_get (&opened) - opened0, N, g_atomic_int_get (&most_inside));
    CHECK (g_atomic_int_get (&said_nothing) == nothing0,
           "... and nobody was left to play into nothing behind the others: %d were",
           g_atomic_int_get (&said_nothing) - nothing0);
    g_atomic_int_set (&open_us, 3000);
    walpe_sound_group_free (g);
    CHECK (threads_gone (), "the threads end: %d left", walpe_sound_threads ());
    CHECK (g_atomic_int_get (&closed) == g_atomic_int_get (&opened), "every stream was closed: %d of %d",
           g_atomic_int_get (&closed), g_atomic_int_get (&opened));
}

/* ---- 5 ---- */
static void test_stalled_stream (const char *wav)
{
    const gint opened0 = g_atomic_int_get (&opened), closed0 = g_atomic_int_get (&closed);
    g_atomic_int_set (&after_stall, 0);
    WalpeSoundGroup *g = walpe_sound_group_new ("/w/server-e", 1);
    WalpeSound *p = player (g, wav);
    walpe_sound_group_set_active (g, TRUE);
    WAIT (frames_of (p) > 4800, 5000);
    CHECK (g_atomic_int_get (&opened) == opened0 + 1 && frames_of (p) > 4800, "a player is heard through one stream");
    /* paused; the drain is not answered */
    g_atomic_int_set (&stall_drain, 1);
    const gint d0 = g_atomic_int_get (&drains), f0 = g_atomic_int_get (&flushes);
    walpe_sound_group_set_active (g, FALSE);
    WAIT (g_atomic_int_get (&closed) == closed0 + 1, 3000);
    g_usleep (50000);
    CHECK (g_atomic_int_get (&drains) == d0 + 1 && g_atomic_int_get (&closed) == closed0 + 1 &&
           g_atomic_int_get (&flushes) == f0 && g_atomic_int_get (&after_stall) == 0,
           "paused, the drain not answered: the stream is given up at once (closed %d of 1), with %d more "
           "call(s) into it (wanted: 0)", g_atomic_int_get (&closed) - closed0, g_atomic_int_get (&after_stall));
    g_atomic_int_set (&stall_drain, 0);
    gint64 f = frames_of (p);
    gint64 t0 = g_get_monotonic_time ();
    walpe_sound_group_set_active (g, TRUE);
    WAIT (frames_of (p) > f + 4800, 3000);
    double ms = (g_get_monotonic_time () - t0) / 1e3;
    CHECK (frames_of (p) > f + 4800 && g_atomic_int_get (&opened) == opened0 + 2 && ms < 1000.0,
           "resumed: heard again through a new stream (%d opened, wanted 2; %" G_GINT64_FORMAT " frames more "
           "after %.0f ms)", g_atomic_int_get (&opened) - opened0, frames_of (p) - f, ms);
    /* paused and resumed within the two seconds a silent stream is kept;
     * the drain was answered, the write after the resume is not taken */
    walpe_sound_group_set_active (g, FALSE);
    g_usleep (300000);
    CHECK (g_atomic_int_get (&closed) == closed0 + 1 && g_atomic_int_get (&opened) == opened0 + 2,
           "paused, the drain answered: the stream is kept");
    f = frames_of (p);
    g_atomic_int_set (&stall_write, 1);
    g_atomic_int_set (&said_nothing, 0);
    t0 = g_get_monotonic_time ();
    walpe_sound_group_set_active (g, TRUE);
    WAIT (g_atomic_int_get (&opened) == opened0 + 3 && frames_of (p) > f + 9600, 3000);
    ms = (g_get_monotonic_time () - t0) / 1e3;
    CHECK (g_atomic_int_get (&opened) == opened0 + 3 && g_atomic_int_get (&closed) == closed0 + 2 &&
           frames_of (p) > f + 9600 && ms < 1500.0 && g_atomic_int_get (&after_stall) == 0,
           "resumed, the write not taken: a new stream with the next block (%d opened, wanted 3; %d closed, "
           "wanted 2; heard again after %.0f ms; %d call(s) into the stalled one)",
           g_atomic_int_get (&opened) - opened0, g_atomic_int_get (&closed) - closed0, ms,
           g_atomic_int_get (&after_stall));
    char *line = walpe_sound_status ();
    CHECK (line && strstr (line, "1 stream(s) through") && !g_atomic_int_get (&said_nothing),
           "... and nothing is said to play into nothing: %s", line ? line : "(null)");
    g_free (line);
    /* the new stream is not taken either, nor the next: it is not the
     * stream (a suspended sink). No stream after stream: into nothing, the
     * next one after the usual five seconds; said once. */
    const gint o3 = g_atomic_int_get (&opened);
    g_atomic_int_set (&stall_write, 1);
    WAIT (g_atomic_int_get (&closed) == closed0 + 3, 3000);
    f = frames_of (p);
    g_usleep (2000000);
    CHECK (g_atomic_int_get (&closed) == closed0 + 3 && g_atomic_int_get (&opened) == o3 &&
           g_atomic_int_get (&said_nothing) == 1 && frames_of (p) > f + 48000,
           "the new stream stalls too: no other one for now (%d opened in 2 s), the player goes on into nothing "
           "(%" G_GINT64_FORMAT " frames) and says so once (%d)", g_atomic_int_get (&opened) - o3,
           frames_of (p) - f, g_atomic_int_get (&said_nothing));
    g_atomic_int_set (&stall_write, 1);
    const gint lines0 = g_atomic_int_get (&said_lines);
    WAIT (g_atomic_int_get (&closed) == closed0 + 4, 6000);
    g_usleep (100000);
    CHECK (g_atomic_int_get (&closed) == closed0 + 4 && g_atomic_int_get (&opened) == o3 + 1 &&
           g_atomic_int_get (&said_lines) == lines0,
           "... the stream asked for five seconds later stalls as well: given up (%d closed, wanted 4), and "
           "nothing more is said (%d line(s))", g_atomic_int_get (&closed) - closed0,
           g_atomic_int_get (&said_lines) - lines0);
    WAIT (g_atomic_int_get (&opened) == o3 + 2 && frames_of (p) > 0, 7000);
    f = frames_of (p);
    g_usleep (300000);
    line = walpe_sound_status ();
    CHECK (g_atomic_int_get (&opened) == o3 + 2 && line && strstr (line, "1 stream(s) through") &&
           frames_of (p) > f, "... and the one after that is taken: heard again (%s)", line ? line : "(null)");
    g_free (line);
    walpe_sound_group_free (g);
    CHECK (threads_gone (), "the thread ends: %d left", walpe_sound_threads ());
    CHECK (g_atomic_int_get (&closed) - closed0 == g_atomic_int_get (&opened) - opened0,
           "every stream was closed: %d of %d", g_atomic_int_get (&closed) - closed0,
           g_atomic_int_get (&opened) - opened0);
}

int main (void)
{
    g_unsetenv ("WALPE_SOUND_NULL");       /* the output is the stand-in server */
    g_unsetenv ("WALPE_SOUND_SINK");
    walpe_sound_set_server_calls (&srv);   /* first: nothing here reaches a real server */
    char *dir = g_dir_make_tmp ("walpe-sound-server-XXXXXX", NULL);
    char *wav = dir ? write_wav (dir) : NULL;
    if (!wav) {
        printf ("FAIL: no temp file\n");
        return 1;
    }
    walpe_sound_allow (TRUE);
    walpe_sound_set_master (1.0);

    g_log_set_default_handler (log_count, NULL);
    test_one_at_a_time (wav);
    test_refused_asks_again (wav);
    test_slow_server (wav);
    test_stalled_stream (wav);
    test_server_does_not_answer (wav);   /* (last: it leaves its turn taken for a moment) */

    g_remove (wav);
    g_rmdir (dir);
    g_free (wav);
    g_free (dir);
    if (fails) {
        printf ("scenesound-server: %d check(s) failed\n", fails);
        return 1;
    }
    printf ("scenesound-server: ok\n");
    return 0;
}
