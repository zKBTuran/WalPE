/* audio_server_test.c - the capture of src/common/audio.c against a real
 * sound server: which source its stream reads, and that it stays on what
 * the computer plays.
 *
 * The test starts a PulseAudio server of its own (a temporary runtime
 * directory, no configuration file, null sinks: no sound card, nothing is
 * heard, the user's server is not contacted) and is skipped (77) where
 * `pulseaudio` or `pactl` is not installed or the server does not start.
 * Tones are played into its sinks with libpulse-simple and looked for in
 * the spectrum the capture publishes.
 *
 *  1. The capture reads the monitor of the default output and says so by
 *     name; a 1 kHz tone played there is band 35 of 64. A consumer that
 *     waits for sound is called once when the tone starts.
 *  2. A microphone appears - a source that is no monitor - and becomes
 *     the default source, as it does on a machine that had none. The
 *     server moves every recording stream that sat on the default source
 *     to it, and the capture was one of them: it then read the microphone.
 *     What the "microphone" hears (5 kHz) must not show, what the output
 *     plays (1 kHz) still must.
 *  3. Another output becomes the default: the capture follows to its
 *     monitor (it stayed on the old output's: the bars stood still).
 *  4. That output goes away: the capture goes on with the one that is the
 *     default then, without an error.
 *  4b. The output that went away comes back and is the default again (a
 *     headset plugged in a second time): the capture follows once more.
 *  5. WALPE_AUDIO_SOURCE names the microphone: the user's choice is read,
 *     and the status says that it is no output's monitor.
 *  5b. WALPE_AUDIO_SOURCE names a source that does not exist: an error,
 *     nothing is read. (What PipeWire's session manager does with such a
 *     stream - WirePlumber 0.5 links it to the default source, the
 *     microphone - is not in a PulseAudio server's power to show: the
 *     rule that refuses it is in the test audio.) */
#include "common/audio.h"
#include <glib.h>
#include <glib/gstdio.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <pulse/simple.h>
#include <pulse/error.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf ("FAIL %s:%d: ", __FILE__, __LINE__); printf (__VA_ARGS__); printf ("\n"); } } while (0)

static char *tmpdir;

/* pactl ARGS...: its output (NULL: it failed) */
static char *pactl (const char *a1, const char *a2, const char *a3, const char *a4)
{
    const char *argv[] = { "pactl", a1, a2, a3, a4, NULL };
    char *out = NULL;
    int status = 0;
    if (!g_spawn_sync (NULL, (char **) argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL,
                       &out, NULL, &status, NULL) || status != 0) {
        g_free (out);
        return NULL;
    }
    return g_strstrip (out);
}

static gboolean pactl_ok (const char *a1, const char *a2, const char *a3, const char *a4)
{
    char *o = pactl (a1, a2, a3, a4);
    g_free (o);
    return o != NULL;
}

/* the value of a line "Key: value" of `pactl info` */
static char *server_says (const char *key)
{
    char *info = pactl ("info", NULL, NULL, NULL), *r = NULL;
    if (!info)
        return NULL;
    char **lines = g_strsplit (info, "\n", -1);
    for (int i = 0; lines[i]; i++)
        if (g_str_has_prefix (lines[i], key) && lines[i][strlen (key)] == ':')
            r = g_strdup (g_strstrip (lines[i] + strlen (key) + 1));
    g_strfreev (lines);
    g_free (info);
    return r;
}

/* ---- tones ---- */
typedef struct { char *sink; double hz; gint stop; GThread *t; } Tone;

static gpointer tone_thread (gpointer data)
{
    Tone *t = data;
    const pa_sample_spec ss = { .format = PA_SAMPLE_FLOAT32LE, .rate = 48000, .channels = 2 };
    int err = 0;
    pa_simple *s = pa_simple_new (NULL, "walpe-audio-test", PA_STREAM_PLAYBACK, t->sink, "tone", &ss, NULL, NULL, &err);
    if (!s) {
        printf ("  (tone into %s: %s)\n", t->sink, pa_strerror (err));
        return NULL;
    }
    static const int n = 2400;      /* 50 ms */
    float *buf = g_new (float, (gsize) n * 2);
    double ph = 0;
    while (!g_atomic_int_get (&t->stop)) {
        for (int i = 0; i < n; i++) {
            buf[2 * i] = buf[2 * i + 1] = (float) (0.25 * sin (ph));
            ph += 2 * G_PI * t->hz / 48000.0;
        }
        if (pa_simple_write (s, buf, (gsize) n * 2 * sizeof (float), &err) < 0)
            break;
    }
    g_free (buf);
    pa_simple_free (s);
    return NULL;
}

static Tone *tone_start (const char *sink, double hz)
{
    Tone *t = g_new0 (Tone, 1);
    t->sink = g_strdup (sink);
    t->hz = hz;
    t->t = g_thread_new ("tone", tone_thread, t);
    return t;
}

static void tone_stop (Tone *t)
{
    g_atomic_int_set (&t->stop, 1);
    g_thread_join (t->t);
    g_free (t->sink);
    g_free (t);
}

/* ---- what the capture publishes ---- */
static int peak_band (float *value)
{
    WalpeAudioSpectrum s;
    walpe_audio_get (&s);
    int p = 0;
    for (int b = 1; b < WALPE_AUDIO_BANDS; b++)
        if (s.left[b] > s.left[p])
            p = b;
    *value = s.left[p];
    return p;
}

/* TRUE once band is the loudest one, at more than 0.5, within ms */
static gboolean hears (int band, int ms)
{
    for (int w = 0; w < ms; w += 20) {
        float v;
        if (peak_band (&v) == band && v > 0.5f)
            return TRUE;
        g_usleep (20000);
    }
    return FALSE;
}

/* the largest value of band seen during ms */
static float most_of (int band, int ms)
{
    float most = 0;
    for (int w = 0; w < ms; w += 20) {
        WalpeAudioSpectrum s;
        walpe_audio_get (&s);
        most = fmaxf (most, fmaxf (s.left[band], s.right[band]));
        g_usleep (20000);
    }
    return most;
}

static gint woke;

static void on_sound (gpointer user)
{
    g_atomic_int_inc ((gint *) user);
}

static gboolean status_is (const char *want, int ms)
{
    for (int w = 0; w < ms; w += 20) {
        char *st = walpe_audio_status ();
        gboolean ok = !strcmp (st, want);
        g_free (st);
        if (ok)
            return TRUE;
        g_usleep (20000);
    }
    return FALSE;
}

static GPid server_pid;

/* in the server, before it starts: it ends with the test. (It is started
 * with --exit-idle-time=-1; a test that was killed - by hand, or by a
 * runner that ends only the test's own process - left it running for
 * good) */
static void with_the_test (gpointer user)
{
    (void) user;
#ifdef __linux__
    prctl (PR_SET_PDEATHSIG, SIGKILL);
#endif
}

static void cleanup (void)
{
    if (server_pid > 0) {
        kill (server_pid, SIGTERM);
        for (int i = 0; i < 100 && kill (server_pid, 0) == 0; i++) {
            g_usleep (20000);
            waitpid (server_pid, NULL, WNOHANG);
        }
        kill (server_pid, SIGKILL);
        waitpid (server_pid, NULL, 0);
    }
    if (tmpdir) {
        char *argv[] = { (char *) "rm", (char *) "-rf", tmpdir, NULL };
        g_spawn_sync (NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
    }
}

int main (void)
{
    char *pa = g_find_program_in_path ("pulseaudio"), *ctl = g_find_program_in_path ("pactl");
    if (!pa || !ctl) {
        printf ("audio-server: skipped, %s is not installed\n", pa ? "pactl" : "pulseaudio");
        return 77;
    }
    /* everything of the server and of its clients in one directory (a
     * short name: the socket's path is limited) */
    tmpdir = g_dir_make_tmp ("walpe-au-XXXXXX", NULL);
    if (!tmpdir)
        return 77;
    char *sock = g_build_filename (tmpdir, "native", NULL);
    char *srv = g_strdup_printf ("unix:%s", sock);
    g_setenv ("XDG_RUNTIME_DIR", tmpdir, TRUE);
    g_setenv ("XDG_CONFIG_HOME", tmpdir, TRUE);
    g_setenv ("HOME", tmpdir, TRUE);
    g_setenv ("PULSE_SERVER", srv, TRUE);
    /* no session bus for the server: on the user's bus the name
     * org.PulseAudio1 belongs to the sound server of the session, and a
     * second PulseAudio that sees it there ends at once (the test was
     * skipped exactly where a sound server runs). An address nobody
     * listens on: it goes on without the bus */
    char *nobus = g_strdup_printf ("unix:path=%s/no-bus", tmpdir);
    g_setenv ("DBUS_SESSION_BUS_ADDRESS", nobus, TRUE);
    g_free (nobus);
    g_unsetenv ("PULSE_SINK");
    g_unsetenv ("PULSE_SOURCE");
    g_unsetenv ("PULSE_COOKIE");
    g_unsetenv ("WALPE_AUDIO_SOURCE");
    char *proto = g_strdup_printf ("module-native-protocol-unix auth-anonymous=1 socket=%s", sock);
    char *argv[] = { pa, (char *) "-n", (char *) "--daemonize=no", (char *) "--exit-idle-time=-1",
                     (char *) "--disable-shm=yes", (char *) "--use-pid-file=no",
                     (char *) "-L", proto, NULL };
    if (!g_spawn_async (NULL, argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDOUT_TO_DEV_NULL |
                        G_SPAWN_STDERR_TO_DEV_NULL, with_the_test, NULL, &server_pid, NULL)) {
        printf ("audio-server: skipped, pulseaudio does not start\n");
        cleanup ();
        return 77;
    }
    gboolean up = FALSE;
    for (int i = 0; i < 100 && !up; i++) {
        g_usleep (100000);
        up = pactl_ok ("info", NULL, NULL, NULL);
    }
    if (!up || !pactl_ok ("load-module", "module-null-sink", "sink_name=out1", NULL) ||
        !pactl_ok ("set-default-sink", "out1", NULL, NULL)) {
        printf ("audio-server: skipped, the private PulseAudio server did not come up\n");
        cleanup ();
        return 77;
    }

    /* 1. the monitor of the default output, by name */
    walpe_audio_ref ();
    CHECK (status_is ("capturing out1.monitor", 5000), "status: not capturing out1.monitor");
    Tone *t = tone_start ("out1", 1000.0);
    CHECK (hears (35, 5000), "a 1 kHz tone on the output is not band 35");
    tone_stop (t);

    /* 1b. sound after silence (walpe_audio_wake_on_sound): noted while
     * the spectrum is silence, called once with the first sound, not
     * noted while there is sound, not called after it was taken back */
    {
        gboolean noted = FALSE;
        for (int w = 0; w < 5000 && !noted; w += 50) {
            noted = walpe_audio_wake_on_sound (on_sound, &woke);
            if (!noted)
                g_usleep (50000);
        }
        CHECK (noted, "silence, and a waiter for sound was not noted");
        g_usleep (300000);
        CHECK (g_atomic_int_get (&woke) == 0, "woken in silence");
        t = tone_start ("out1", 1000.0);
        for (int w = 0; w < 3000 && !g_atomic_int_get (&woke); w += 10)
            g_usleep (10000);
        CHECK (g_atomic_int_get (&woke) == 1, "sound, and the waiter was called %d times", g_atomic_int_get (&woke));
        CHECK (hears (35, 3000), "the tone that woke the waiter is not band 35");
        CHECK (!walpe_audio_wake_on_sound (on_sound, &woke), "a waiter was noted while there is sound");
        tone_stop (t);
        noted = FALSE;
        for (int w = 0; w < 5000 && !noted; w += 50) {
            noted = walpe_audio_wake_on_sound (on_sound, &woke);
            if (!noted)
                g_usleep (50000);
        }
        CHECK (noted, "silence again, and a waiter was not noted");
        walpe_audio_wake_cancel (on_sound, &woke);
        t = tone_start ("out1", 1000.0);
        CHECK (hears (35, 3000), "the second tone is not heard");
        tone_stop (t);
        CHECK (g_atomic_int_get (&woke) == 1, "a waiter that was taken back was called");
    }

    /* 2. a microphone appears: what the sink "room" is played is what it
     * hears. The server makes it the default source by itself where the
     * default was a monitor; said here as well, so that the step does not
     * depend on that */
    char *before = server_says ("Default Source");
    CHECK (pactl_ok ("load-module", "module-null-sink", "sink_name=room", NULL), "no sink room");
    CHECK (pactl_ok ("set-default-sink", "out1", NULL, NULL), "default sink");
    CHECK (pactl_ok ("load-module", "module-remap-source", "master=room.monitor", "source_name=mic"), "no source mic");
    pactl_ok ("set-default-source", "mic", NULL, NULL);
    char *after = server_says ("Default Source");
    printf ("  default source: %s, with a microphone: %s\n", before ? before : "?", after ? after : "?");
    CHECK (after && !strcmp (after, "mic"), "the microphone is not the default source");
    g_free (before);
    g_free (after);
    g_usleep (300000);
    Tone *speech = tone_start ("room", 5000.0);
    float mic = most_of (52, 2000);
    CHECK (mic < 0.01f, "the capture reads the microphone: band 52 at %.3f", (double) mic);
    CHECK (status_is ("capturing out1.monitor", 100), "status left out1.monitor");
    t = tone_start ("out1", 1000.0);
    CHECK (hears (35, 5000), "the output is not heard any more with a microphone present");
    tone_stop (t);
    tone_stop (speech);

    /* 3. another output becomes the default */
    char *out2 = pactl ("load-module", "module-null-sink", "sink_name=out2", NULL);
    CHECK (out2 != NULL, "no sink out2");
    CHECK (pactl_ok ("set-default-sink", "out2", NULL, NULL), "default sink out2");
    CHECK (status_is ("capturing out2.monitor", 5000), "the capture did not follow to out2.monitor");
    t = tone_start ("out2", 1000.0);
    CHECK (hears (35, 5000), "the new default output is not heard");
    tone_stop (t);

    /* 4. that output goes away */
    CHECK (out2 && pactl_ok ("unload-module", out2, NULL, NULL), "unload out2");
    g_free (out2);
    gboolean moved = FALSE;
    char *st = NULL;
    for (int w = 0; w < 8000 && !moved; w += 50) {
        g_free (st);
        st = walpe_audio_status ();
        moved = g_str_has_prefix (st, "capturing ") && g_str_has_suffix (st, ".monitor") &&
                strcmp (st, "capturing out2.monitor") != 0;
        if (!moved)
            g_usleep (50000);
    }
    CHECK (moved, "after its output went away the capture says: %s", st ? st : "?");
    char *def = server_says ("Default Sink");
    if (moved && def) {
        char *want = g_strdup_printf ("capturing %s.monitor", def);
        CHECK (!strcmp (st, want), "capturing something else than the default output: %s (default %s)", st, def);
        g_free (want);
        t = tone_start (def, 1000.0);
        CHECK (hears (35, 5000), "the output that is left is not heard");
        tone_stop (t);
    }
    g_free (st);
    g_free (def);

    /* 4b. the output that went away is back and the default again: it was
     * the output "followed last" (step 3) and was not followed a second
     * time - the capture stayed on the monitor of the output nothing
     * plays on any more */
    out2 = pactl ("load-module", "module-null-sink", "sink_name=out2", NULL);
    CHECK (out2 != NULL, "no sink out2 the second time");
    CHECK (pactl_ok ("set-default-sink", "out2", NULL, NULL), "default sink out2, the second time");
    CHECK (status_is ("capturing out2.monitor", 5000), "the capture did not follow to an output that came back");
    t = tone_start ("out2", 1000.0);
    CHECK (hears (35, 5000), "the output that came back is not heard");
    tone_stop (t);
    g_free (out2);

    /* 5. the user names the microphone. (The capture thread reads the
     * variable when it makes a stream: it has ended by then, 3 s after
     * the last consumer left) */
    walpe_audio_unref ();
    g_usleep (3600000);
    g_setenv ("WALPE_AUDIO_SOURCE", "mic", TRUE);
    walpe_audio_ref ();
    CHECK (status_is ("capturing mic (WALPE_AUDIO_SOURCE; not the monitor of an output: a microphone?)", 5000),
           "WALPE_AUDIO_SOURCE=mic is not what is captured");
    speech = tone_start ("room", 5000.0);
    CHECK (hears (52, 5000), "the source named in WALPE_AUDIO_SOURCE is not heard");
    tone_stop (speech);
    walpe_audio_unref ();
    CHECK (status_is ("off", 2000), "status after the last consumer left");

    /* 5b. ... and an output's monitor by name is said without the remark;
     * a name the server does not know is an error, nothing is read. (The
     * capture thread has ended each time, as before step 5: a reference
     * taken at once would find the stream it had) */
    g_usleep (3600000);
    g_setenv ("WALPE_AUDIO_SOURCE", "out1.monitor", TRUE);
    walpe_audio_ref ();
    CHECK (status_is ("capturing out1.monitor (WALPE_AUDIO_SOURCE)", 5000), "WALPE_AUDIO_SOURCE=out1.monitor");
    walpe_audio_unref ();
    CHECK (status_is ("off", 2000), "status after the last consumer left (5b)");
    g_usleep (3600000);
    g_setenv ("WALPE_AUDIO_SOURCE", "no-such-source", TRUE);
    walpe_audio_ref ();
    {
        gboolean err = FALSE;
        for (int w = 0; w < 5000 && !err; w += 20) {
            st = walpe_audio_status ();
            err = g_str_has_prefix (st, "error: no-such-source");
            g_free (st);
            if (!err)
                g_usleep (20000);
        }
        CHECK (err, "a source that does not exist is no error");
        speech = tone_start ("room", 5000.0);
        mic = most_of (52, 1000);
        CHECK (mic == 0, "WALPE_AUDIO_SOURCE names no source, and something is read: band 52 at %.3f", (double) mic);
        tone_stop (speech);
    }
    walpe_audio_unref ();

    cleanup ();
    if (!fails)
        printf ("audio-server: the output's monitor by name, not moved to a microphone, follows the default "
                "output, survives its output, WALPE_AUDIO_SOURCE OK\n");
    return fails ? 1 : 0;
}
