/* scenesound_pulse_test.c - the calls scene sound makes into the sound
 * server come back (src/engine/scenesound_pulse.c).
 *
 * Every call has a time limit; none is left waiting for a server that has
 * stopped answering. Nothing is heard, and the user's server is not
 * contacted.
 *
 *  1. A "server" that takes the connection and never says a word (a socket
 *     of the test's own that listens and is not read): a new stream fails
 *     within the limit, with "Timeout".
 *  2. A PulseAudio of the test's own with a null sink (skipped where
 *     `pulseaudio` or `pactl` is not installed or the server does not
 *     start): a stream takes data, a drain is answered and a write after
 *     it finds room; a drain of a stream that has two seconds left to play
 *     (a buffer of three) is waited for and not given up after the one
 *     second a drain has otherwise; a sink that is suspended takes nothing:
 *     the write fails within its limit - the short one for a stream that
 *     was drained and is not played again (what pipewire-pulse 1.0.5 does
 *     to a stream now and then), the long one for a stream that was not.
 *     Then the server is stopped (SIGSTOP):
 *     a write fails within its limit instead of waiting for ever, a drain
 *     and a flush come back, the stream is freed at once, and a new stream
 *     fails within its limit. The server goes on (SIGCONT): a new stream
 *     plays.
 * Each call runs in a thread of its own here, so that a call that does
 * not come back is a failed check and not a test that hangs. */
#include <glib.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <pulse/def.h>
#include <pulse/sample.h>
#include "engine/scenesound_server.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf ("FAIL %s:%d: ", __FILE__, __LINE__); printf (__VA_ARGS__); printf ("\n"); } } while (0)

static const WalpeSoundServerCalls *S;

/* ---- one call, in a thread of its own ---- */
typedef struct {
    int what;           /* 'n' new ('N': with a buffer of 3 s), 'w' write (seconds of silence), 'd' drain,
                         * 'f' flush, 'x' free */
    pa_simple *st;
    double seconds;
    /* the answer */
    gint done;
    pa_simple *made;
    int res, err;
    double took;
} Call;

static gpointer call_thread (gpointer data)
{
    Call *c = data;
    const gint64 t0 = g_get_monotonic_time ();
    static const pa_sample_spec ss = { .format = PA_SAMPLE_S16NE, .rate = 48000, .channels = 2 };
    /* (the buffer scene sound asks for: 200 ms) */
    static const pa_buffer_attr ba = { .maxlength = (uint32_t) -1, .tlength = 48000 * 2 * 2 / 5,
                                       .prebuf = (uint32_t) -1, .minreq = (uint32_t) -1, .fragsize = (uint32_t) -1 };
    c->err = 0;
    static const pa_buffer_attr ba2 = { .maxlength = (uint32_t) -1, .tlength = 48000 * 2 * 2 * 3,
                                        .prebuf = (uint32_t) -1, .minreq = (uint32_t) -1, .fragsize = (uint32_t) -1 };
    if (c->what == 'n' || c->what == 'N') {
        c->made = S->new_stream (NULL, "walpe-test", PA_STREAM_PLAYBACK, NULL, "test", &ss, NULL,
                                 c->what == 'N' ? &ba2 : &ba, &c->err);
        c->res = c->made ? 0 : -1;
    } else if (c->what == 'w') {
        static gint16 block[4096];      /* 2048 frames of silence, as a player writes them */
        c->res = 0;
        for (double left = c->seconds; left > 0 && c->res == 0; left -= 2048.0 / 48000.0)
            c->res = S->write (c->st, block, sizeof block, &c->err);
    } else if (c->what == 'd') {
        c->res = S->drain (c->st, &c->err);
    } else if (c->what == 'f') {
        c->res = S->flush (c->st, &c->err);
    } else {
        S->free_stream (c->st);
        c->res = 0;
    }
    c->took = (g_get_monotonic_time () - t0) / 1e6;
    g_atomic_int_set (&c->done, 1);
    return NULL;
}

/* the call, waited for at most `limit` seconds; FALSE: it did not come back
 * (its thread and its Call are left behind) */
static gboolean call (Call **out, int what, pa_simple *st, double seconds, double limit)
{
    Call *c = g_new0 (Call, 1);
    c->what = what;
    c->st = st;
    c->seconds = seconds;
    GThread *t = g_thread_new ("call", call_thread, c);
    const gint64 until = g_get_monotonic_time () + (gint64) (limit * 1e6);
    while (!g_atomic_int_get (&c->done) && g_get_monotonic_time () < until)
        g_usleep (2000);
    *out = c;
    if (!g_atomic_int_get (&c->done)) {
        g_thread_unref (t);
        return FALSE;
    }
    g_thread_join (t);
    return TRUE;
}

/* ---- 1 ---- */
static void test_silent_server (const char *dir)
{
    char *path = g_build_filename (dir, "silent", NULL), *srv = g_strdup_printf ("unix:%s", path);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    const int fd = socket (AF_UNIX, SOCK_STREAM, 0);
    g_strlcpy (a.sun_path, path, sizeof a.sun_path);
    if (fd < 0 || bind (fd, (struct sockaddr *) &a, sizeof a) != 0 || listen (fd, 8) != 0) {
        CHECK (FALSE, "no listening socket at %s", path);
    } else {
        char *old = g_strdup (g_getenv ("PULSE_SERVER"));
        g_setenv ("PULSE_SERVER", srv, TRUE);
        Call *c;
        const gboolean back = call (&c, 'n', NULL, 0, 14.0);
        CHECK (back && !c->made && c->err == PA_ERR_TIMEOUT && c->took > 1.0,
               "a server that takes the connection and says nothing: the new stream %s (stream %s, error %d, "
               "wanted %d = Timeout)", back ? "came back" : "did not come back within 14 s",
               back && c->made ? "made" : "none", back ? c->err : -1, PA_ERR_TIMEOUT);
        if (back) {
            printf ("scenesound-pulse: a silent server: the new stream gave up after %.1f s\n", c->took);
            g_free (c);
        }
        if (old)
            g_setenv ("PULSE_SERVER", old, TRUE);
        else
            g_unsetenv ("PULSE_SERVER");
        g_free (old);
    }
    if (fd >= 0)
        close (fd);
    g_unlink (path);
    g_free (path);
    g_free (srv);
}

/* ---- 2 ---- */
static GPid server_pid;

static void with_the_test (gpointer user)
{
    (void) user;
#ifdef __linux__
    prctl (PR_SET_PDEATHSIG, SIGKILL);
#endif
}

static gboolean pactl_ok (const char *a1, const char *a2, const char *a3)
{
    const char *argv[] = { "pactl", a1, a2, a3, NULL };
    int status = -1;
    return g_spawn_sync (NULL, (char **) argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
                         G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL, &status, NULL) && status == 0;
}

/* TRUE: it ran (FALSE: skipped) */
static gboolean test_real_server (const char *dir)
{
    char *pa = g_find_program_in_path ("pulseaudio"), *ctl = g_find_program_in_path ("pactl");
    char *sock = g_build_filename (dir, "native", NULL), *srv = g_strdup_printf ("unix:%s", sock);
    char *proto = g_strdup_printf ("module-native-protocol-unix auth-anonymous=1 socket=%s", sock);
    char *nobus = g_strdup_printf ("unix:path=%s/no-bus", dir);
    gboolean ran = FALSE;
    Call *c = NULL;
    if (!pa || !ctl) {
        printf ("scenesound-pulse: the part with a real server is skipped: %s is not installed\n",
                pa ? "pactl" : "pulseaudio");
        goto out;
    }
    g_setenv ("XDG_RUNTIME_DIR", dir, TRUE);
    g_setenv ("XDG_CONFIG_HOME", dir, TRUE);
    g_setenv ("HOME", dir, TRUE);
    g_setenv ("PULSE_SERVER", srv, TRUE);
    g_setenv ("DBUS_SESSION_BUS_ADDRESS", nobus, TRUE);     /* (audio_server_test.c says why) */
    g_unsetenv ("PULSE_SINK");
    g_unsetenv ("PULSE_COOKIE");
    {
        char *argv[] = { pa, (char *) "-n", (char *) "--daemonize=no", (char *) "--exit-idle-time=-1",
                         (char *) "--disable-shm=yes", (char *) "--use-pid-file=no", (char *) "-L", proto, NULL };
        gboolean up = g_spawn_async (NULL, argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDOUT_TO_DEV_NULL |
                                     G_SPAWN_STDERR_TO_DEV_NULL, with_the_test, NULL, &server_pid, NULL);
        for (int i = 0; up && i < 100 && !pactl_ok ("info", NULL, NULL); i++)
            g_usleep (100000);
        if (!up || !pactl_ok ("load-module", "module-null-sink", "sink_name=out1") ||
            !pactl_ok ("set-default-sink", "out1", NULL)) {
            printf ("scenesound-pulse: the part with a real server is skipped: pulseaudio does not start\n");
            goto out;
        }
    }
    ran = TRUE;
    /* a server that is well: nothing here is near a limit */
    gboolean back = call (&c, 'n', NULL, 0, 12.0);
    pa_simple *st = back ? c->made : NULL;
    CHECK (back && st && c->took < 3.0, "a stream on a server that is well (%s, error %d, %.2f s)",
           back ? (st ? "made" : "none") : "the call did not come back", back ? c->err : -1, back ? c->took : -1.0);
    if (!st)
        goto out;
    g_free (c);
    back = call (&c, 'w', st, 1.0, 6.0);
    /* (one second of sound into a buffer of 200 ms: the call waits for the server to play it) */
    CHECK (back && c->res == 0 && c->took > 0.5 && c->took < 3.0, "one second written at the pace it plays (%s, %.2f s)",
           back ? (c->res ? "failed" : "ok") : "not back", back ? c->took : -1.0);
    g_free (c);
    back = call (&c, 'd', st, 0, 4.0);
    CHECK (back && c->res == 0 && c->took < 0.9, "the drain is answered (%s, error %d, %.2f s)",
           back ? (c->res ? "failed" : "ok") : "not back", back ? c->err : -1, back ? c->took : -1.0);
    g_free (c);
    back = call (&c, 'w', st, 0.5, 6.0);
    CHECK (back && c->res == 0 && c->took < 2.0, "a write after the drain finds room (%s, error %d, %.2f s)",
           back ? (c->res ? "failed" : "ok") : "not back", back ? c->err : -1, back ? c->took : -1.0);
    g_free (c);
    /* a stream that has a long time left to play when it is drained */
    back = call (&c, 'N', NULL, 0, 12.0);
    pa_simple *st2 = back ? c->made : NULL;
    CHECK (back && st2, "a stream with a buffer of three seconds");
    if (back)
        g_free (c);
    if (st2) {
        back = call (&c, 'w', st2, 2.6, 8.0);
        const double wrote = back ? c->took : -1.0;
        CHECK (back && c->res == 0, "... takes 2.6 s of sound");
        if (back)
            g_free (c);
        back = call (&c, 'd', st2, 0, 8.0);
        CHECK (back && c->res == 0 && wrote + c->took > 2.0,
               "... and its drain is waited for until that is played (%s, error %d; written in %.2f s, drained "
               "after %.2f s more)", back ? (c->res ? "failed" : "ok") : "not back", back ? c->err : -1, wrote,
               back ? c->took : -1.0);
        if (back) {
            printf ("scenesound-pulse: 2.6 s in a long buffer: written in %.2f s, the drain answered after %.2f s\n",
                    wrote, c->took);
            g_free (c);
        }
        S->free_stream (st2);
    }
    /* a sink that is suspended: the server answers, the stream takes what
     * fits and then nothing. First a stream that was drained: it does not
     * begin to play again, and that is known soon */
    back = call (&c, 'd', st, 0, 4.0);
    CHECK (back && c->res == 0, "drained again");
    if (back)
        g_free (c);
    if (pactl_ok ("suspend-sink", "out1", "1")) {
        back = call (&c, 'w', st, 30.0, 12.0);
        CHECK (back && c->res != 0 && c->err == PA_ERR_TIMEOUT && c->took < 1.5,
               "the sink suspended, a drained stream: the write %s (result %d, error %d, wanted Timeout = %d; "
               "%.2f s, wanted less than 1.5)", back ? "came back" : "did not come back within 12 s",
               back ? c->res : 0, back ? c->err : -1, PA_ERR_TIMEOUT, back ? c->took : -1.0);
        if (back) {
            printf ("scenesound-pulse: a suspended sink, a drained stream: the write gave up after %.2f s\n", c->took);
            g_free (c);
        }
        back = call (&c, 'x', st, 0, 6.0);
        CHECK (back && c->took < 0.5, "... the stream is freed %s", back ? "at once" : "- not within 6 s");
        if (back)
            g_free (c);
        /* ... and a stream that was not */
        back = call (&c, 'n', NULL, 0, 12.0);
        st = back ? c->made : NULL;
        CHECK (back && st, "the sink suspended: a new stream is given");
        if (back)
            g_free (c);
        if (!st)
            goto out;
        back = call (&c, 'w', st, 30.0, 12.0);
        CHECK (back && c->res != 0 && c->err == PA_ERR_TIMEOUT && c->took > 2.0,
               "... its write %s (result %d, error %d, wanted Timeout = %d)",
               back ? "came back" : "did not come back within 12 s", back ? c->res : 0, back ? c->err : -1,
               PA_ERR_TIMEOUT);
        if (back) {
            printf ("scenesound-pulse: a suspended sink, a new stream: the write gave up after %.1f s\n", c->took);
            g_free (c);
        }
        back = call (&c, 'x', st, 0, 6.0);
        CHECK (back && c->took < 0.5, "... the stream is freed %s", back ? "at once" : "- not within 6 s");
        if (back)
            g_free (c);
        pactl_ok ("suspend-sink", "out1", "0");
        back = call (&c, 'n', NULL, 0, 12.0);
        st = back ? c->made : NULL;
        CHECK (back && st, "the sink goes on: a new stream");
        if (back)
            g_free (c);
        if (!st)
            goto out;
        back = call (&c, 'w', st, 0.5, 6.0);
        CHECK (back && c->res == 0, "... takes data");
        if (back)
            g_free (c);
    } else {
        CHECK (FALSE, "pactl suspend-sink failed");
    }
    /* the server stops in the middle */
    kill (server_pid, SIGSTOP);
    back = call (&c, 'w', st, 30.0, 12.0);
    CHECK (back && c->res != 0 && c->err == PA_ERR_TIMEOUT && c->took > 2.0,
           "the server stopped: the write %s (result %d, error %d, wanted Timeout = %d)",
           back ? "came back" : "did not come back within 12 s", back ? c->res : 0, back ? c->err : -1,
           PA_ERR_TIMEOUT);
    if (back) {
        printf ("scenesound-pulse: a stopped server: the write gave up after %.1f s\n", c->took);
        g_free (c);
    }
    back = call (&c, 'd', st, 0, 6.0);
    CHECK (back && c->res != 0 && c->took < 2.0, "... a drain %s (%.2f s)",
           back ? "came back with a failure" : "did not come back within 6 s", back ? c->took : -1.0);
    if (back)
        g_free (c);
    back = call (&c, 'f', st, 0, 6.0);
    CHECK (back && c->took < 2.0, "... a flush %s (%.2f s)", back ? "came back" : "did not come back within 6 s",
           back ? c->took : -1.0);
    if (back)
        g_free (c);
    back = call (&c, 'x', st, 0, 6.0);
    CHECK (back && c->took < 0.5, "... the stream is freed %s (%.2f s)", back ? "at once" : "- not within 6 s",
           back ? c->took : -1.0);
    if (back)
        g_free (c);
    back = call (&c, 'n', NULL, 0, 14.0);
    CHECK (back && !c->made && c->took > 1.0, "... a new stream %s",
           back ? (c->made ? "was made by a stopped server?" : "fails within its limit") : "did not come back within 14 s");
    if (back) {
        if (c->made)
            S->free_stream (c->made);
        g_free (c);
    }
    /* and goes on */
    kill (server_pid, SIGCONT);
    back = call (&c, 'n', NULL, 0, 12.0);
    st = back ? c->made : NULL;
    CHECK (back && st, "the server goes on: a new stream (%s)", back ? "none" : "the call did not come back");
    if (back)
        g_free (c);
    if (st) {
        back = call (&c, 'w', st, 0.5, 6.0);
        CHECK (back && c->res == 0, "... takes data");
        if (back)
            g_free (c);
        back = call (&c, 'd', st, 0, 4.0);
        CHECK (back && c->res == 0, "... and is drained");
        if (back)
            g_free (c);
        S->free_stream (st);
    }
out:
    if (server_pid) {
        kill (server_pid, SIGCONT);
        kill (server_pid, SIGTERM);
        for (int i = 0; i < 300 && waitpid (server_pid, NULL, WNOHANG) == 0; i++)
            g_usleep (10000);
        kill (server_pid, SIGKILL);
        waitpid (server_pid, NULL, 0);
        g_spawn_close_pid (server_pid);
    }
    g_free (pa); g_free (ctl); g_free (sock); g_free (srv); g_free (proto); g_free (nobus);
    return ran;
}

static void rm_tree (const char *dir, int depth)
{
    GDir *d = g_dir_open (dir, 0, NULL);
    const char *n;
    while (d && (n = g_dir_read_name (d))) {
        char *p = g_build_filename (dir, n, NULL);
        if (depth < 4 && g_file_test (p, G_FILE_TEST_IS_DIR) && !g_file_test (p, G_FILE_TEST_IS_SYMLINK))
            rm_tree (p, depth + 1);
        else
            g_unlink (p);
        g_free (p);
    }
    if (d)
        g_dir_close (d);
    g_rmdir (dir);
}

int main (void)
{
    setvbuf (stdout, NULL, _IOLBF, 0);      /* (what failed is in the log also when a later call never ends) */
    S = walpe_sound_pulse_calls ();
    /* everything in one directory (a short name: a socket's path is limited) */
    char *dir = g_dir_make_tmp ("walpe-sp-XXXXXX", NULL);
    if (!dir) {
        printf ("FAIL: no temp dir\n");
        return 1;
    }
    signal (SIGPIPE, SIG_IGN);
    test_silent_server (dir);
    const gboolean real = test_real_server (dir);
    rm_tree (dir, 0);
    g_free (dir);
    if (fails) {
        printf ("scenesound-pulse: %d check(s) failed\n", fails);
        return 1;
    }
    printf ("scenesound-pulse: ok%s\n", real ? "" : " (the part with a real server was skipped)");
    return 0;
}
