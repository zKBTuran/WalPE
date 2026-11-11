/* scenesound_server_test.c - scene sound and the sound server
 * (src/engine/scenesound.c, out_connect).
 *
 * No sound server is contacted and nothing is played: the five functions
 * of libpulse-simple are replaced by the stand-ins below
 * (engine/scenesound_server.h) before the first stream is opened.
 *
 *  1. The players of a scene start together, each with a connection of its
 *     own. A server takes connections from a short queue, and what does
 *     not fit is refused: streams are opened one at a time.
 *  2. A server that does not answer the stream being opened holds that one
 *     player. The others do not wait behind it without a limit: they play
 *     into nothing, as after a refusal, and their group can be freed. */
#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
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
    if (g_atomic_int_compare_and_exchange (&hang, 1, 2)) {
        g_atomic_int_set (&hanging, 1);
        while (g_atomic_int_get (&hang))
            g_usleep (1000);
        g_atomic_int_set (&hanging, 0);
    } else {
        g_usleep (3000);     /* the round trips of opening a stream */
    }
    Stream *st = g_new0 (Stream, 1);
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
    (void) data; (void) error;
    g_usleep ((gulong) ((guint64) bytes * G_USEC_PER_SEC / ((Stream *) s)->bytes_per_s));
    return 0;
}

static int srv_drain (pa_simple *s, int *error) { (void) s; (void) error; return 0; }
static int srv_flush (pa_simple *s, int *error) { (void) s; (void) error; return 0; }

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

    test_one_at_a_time (wav);
    test_server_does_not_answer (wav);

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
