/* scenesound_test.c - sound layers of scenes (src/engine/scenesound.c).
 *
 * Everything here plays into the null device: the test never opens a real
 * output and makes no sound (meson also sets WALPE_SOUND_NULL for it). The
 * sounds are WAV files the test writes itself; compressed files are
 * sounddec_test's.
 *
 *  1. Names and switches: which file names a scene may use, how a
 *     wallpaper's own switch is read.
 *  2. Nothing without permission: a program that did not allow sound gets
 *     no group and no player.
 *  3. Players: single / loop / random, play / pause / stop, the layer's
 *     volume and the user's, every WAV kind, the fade when the volume
 *     changes; the pace of real output, also after a short pause of
 *     random mode.
 *  4. When something is heard: the setting, the wallpaper's own switch,
 *     volume 0, mute, the scene paused, the layer invisible - and that
 *     silence costs no thread.
 *  5. One instance: two scene instances of one wallpaper sound once, the
 *     second takes over when the first is paused; two wallpapers both.
 *  6. Limits and hostile files: players per scene, sample rate, channels,
 *     a file without sound, a file of a few samples in a loop, a file that
 *     is shorter than its header says, a directory, a FIFO, a file that
 *     never ends (a decoder of the test's own).
 *  7. Freeing a group while its players run returns at once; the threads
 *     end by themselves.
 *  8. The process's streams: of more players than WALPE_SOUND_MAX_STREAMS,
 *     over two wallpapers, that many sound and the others wait; a waiting
 *     one gets its output when another falls silent. */
#include <glib.h>
#include <glib/gstdio.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "walpe-build-config.h"   /* HAVE_PULSE: the wording of the status line */
#include "engine/scenesound.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf ("FAIL %s:%d: ", __FILE__, __LINE__); printf (__VA_ARGS__); printf ("\n"); } } while (0)
/* waits up to ms milliseconds for cond */
#define WAIT(cond, ms) do { for (int w_ = 0; w_ < (ms) && !(cond); w_++) g_usleep (1000); } while (0)

static void put32 (guint8 *p, guint32 v) { p[0] = v & 255; p[1] = (v >> 8) & 255; p[2] = (v >> 16) & 255; p[3] = (guint8) (v >> 24); }

/* a WAV file of `frames` frames: fmt 1 = PCM of `bits`, 3 = float; every
 * sample is +half scale, or a 440 Hz sine of amplitude 16000 (sine, 16 bit).
 * claim: the frames its header says it has (< 0: as many as it has) */
static char *write_wav (const char *dir, const char *name, int rate, int ch, int bits, int fmt, int frames,
                        gboolean sine, int claim)
{
    const int bps = bits / 8;
    const gsize dlen = (gsize) frames * ch * bps;
    const gsize said = claim < 0 ? dlen : (gsize) claim * ch * bps;
    guint8 *b = g_malloc0 (44 + dlen);
    memcpy (b, "RIFF", 4);
    put32 (b + 4, (guint32) (36 + said));
    memcpy (b + 8, "WAVEfmt ", 8);
    put32 (b + 16, 16);
    b[20] = (guint8) fmt;
    b[22] = (guint8) ch;
    put32 (b + 24, (guint32) rate);
    put32 (b + 28, (guint32) (rate * ch * bps));
    b[32] = (guint8) (ch * bps);
    b[34] = (guint8) bits;
    memcpy (b + 36, "data", 4);
    put32 (b + 40, (guint32) said);
    for (int i = 0; i < frames * ch; i++) {
        guint8 *q = b + 44 + (gsize) i * bps;
        if (fmt == 3) {
            float f = 0.5f;
            memcpy (q, &f, 4);
        } else if (bits == 8) {
            q[0] = 192;
        } else if (bits == 16) {
            gint16 v = sine ? (gint16) lrint (16000.0 * sin (2.0 * G_PI * 440.0 * (i / ch) / rate)) : 16384;
            q[0] = (guint8) (v & 255);
            q[1] = (guint8) ((v >> 8) & 255);
        } else if (bits == 24) {
            q[2] = 0x40;
        } else {
            q[3] = 0x40;
        }
    }
    char *path = g_build_filename (dir, name, NULL);
    g_file_set_contents (path, (const char *) b, (gssize) (44 + dlen), NULL);
    g_free (b);
    return path;
}

static WalpeSound *snd_new (WalpeSoundGroup *g, const char *a, const char *b, int mode, float vol)
{
    const char *files[2] = { a, b };
    WalpeSoundSpec spec = { files, b ? 2 : 1, vol, mode, 0.0f, 0.0f, "test" };
    return walpe_sound_new (g, &spec);
}

static WalpeSoundStats stats_of (WalpeSound *s)
{
    WalpeSoundStats st;
    walpe_sound_stats (s, &st);
    return st;
}

/* a group that is shown and switched on by itself, whatever the setting */
static WalpeSoundGroup *group_on (const char *key)
{
    WalpeSoundGroup *g = walpe_sound_group_new (key, 1);
    walpe_sound_group_set_active (g, TRUE);
    return g;
}

static gboolean threads_gone (void)
{
    WAIT (walpe_sound_threads () == 0, 5000);
    return walpe_sound_threads () == 0;
}

/* ---- 1 ---- */
static void test_names (void)
{
    CHECK (walpe_sound_switch_parse ("on") == 1 && walpe_sound_switch_parse ("TRUE") == 1 &&
           walpe_sound_switch_parse ("1") == 1 && walpe_sound_switch_parse ("yes") == 1, "switch on");
    CHECK (walpe_sound_switch_parse ("off") == 0 && walpe_sound_switch_parse ("false") == 0 &&
           walpe_sound_switch_parse ("0") == 0 && walpe_sound_switch_parse ("No") == 0, "switch off");
    CHECK (walpe_sound_switch_parse (NULL) == -1 && walpe_sound_switch_parse ("") == -1 &&
           walpe_sound_switch_parse ("perhaps") == -1, "no switch");
    CHECK (walpe_sound_name_ok ("sounds/rain.mp3") && walpe_sound_name_ok ("a..b.ogg") &&
           walpe_sound_name_ok ("sounds\\rain.wav"), "plain names");
    CHECK (!walpe_sound_name_ok ("/etc/passwd") && !walpe_sound_name_ok ("../x.mp3") &&
           !walpe_sound_name_ok ("a/../../x.mp3") && !walpe_sound_name_ok ("a\\..\\x.mp3") &&
           !walpe_sound_name_ok ("C:\\x.mp3") && !walpe_sound_name_ok ("") && !walpe_sound_name_ok (NULL) &&
           !walpe_sound_name_ok ("\\\\host\\share\\x.mp3"), "paths out of the wallpaper are refused");
    CHECK (!walpe_sound_name_ok ("http://example.org/x.mp3") && !walpe_sound_name_ok ("file:///x.mp3") &&
           !walpe_sound_name_ok ("pipe:0"), "addresses are refused");
    CHECK (!walpe_sound_name_ok ("a\nb.mp3") && !walpe_sound_name_ok ("a\033[2Jb.mp3"), "control characters");
    char *lng = g_strnfill (2000, 'a');
    CHECK (!walpe_sound_name_ok (lng), "a name of 2000 bytes");
    g_free (lng);
}

/* ---- 3 ---- */
static void test_players (const char *dir, const char *sine)
{
    WalpeSoundStats st;
    WalpeSoundGroup *g = group_on ("/w/players");
    CHECK (g != NULL && walpe_sound_group_enabled (g) && walpe_sound_group_speaking (g), "group");
    CHECK (snd_new (g, NULL, NULL, WALPE_SOUND_SINGLE, 1.0f) == NULL, "no files, no player");
    CHECK (snd_new (NULL, sine, NULL, WALPE_SOUND_SINGLE, 1.0f) == NULL, "no group, no player");

    WalpeSound *s = snd_new (g, sine, NULL, WALPE_SOUND_SINGLE, 1.0f);
    CHECK (s != NULL, "player");
    CHECK (!walpe_sound_is_playing (s), "created stopped");
    CHECK (walpe_sound_threads () == 0, "a player that never played has no thread");
    walpe_sound_play (s);
    WAIT (!walpe_sound_is_playing (s), 5000);
    CHECK (!walpe_sound_is_playing (s), "a single sound ends by itself");
    st = stats_of (s);
    CHECK (st.frames == 2000 && st.rate == 8000 && st.channels == 1 && st.plays == 1,
           "single: frames %" G_GINT64_FORMAT " rate %d ch %d plays %d", st.frames, st.rate, st.channels, st.plays);
    CHECK (st.peak >= 15990 && st.peak <= 16000 && st.null_out && !st.error[0],
           "single: peak %d null %d error '%s' (a file's first samples are not faded in)", st.peak, st.null_out,
           st.error);
    walpe_sound_play (s);          /* again from the start */
    WAIT (stats_of (s).plays == 2, 5000);
    st = stats_of (s);
    CHECK (st.frames == 4000 && st.plays == 2, "second play: frames %" G_GINT64_FORMAT " plays %d", st.frames, st.plays);

    /* volume: the layer's and the user's */
    WalpeSound *h = snd_new (g, sine, NULL, WALPE_SOUND_SINGLE, 0.5f);
    CHECK (fabsf (walpe_sound_get_volume (h) - 0.5f) < 1e-6f, "volume as given");
    walpe_sound_play (h);
    WAIT (stats_of (h).plays == 1, 5000);
    st = stats_of (h);
    CHECK (st.peak >= 7990 && st.peak <= 8001, "layer volume 0.5: peak %d", st.peak);
    walpe_sound_set_master (0.25);
    WalpeSound *q = snd_new (g, sine, NULL, WALPE_SOUND_SINGLE, 1.0f);
    walpe_sound_set_volume (q, 2.0f);   /* more than 1 is 1 */
    CHECK (fabsf (walpe_sound_get_volume (q) - 1.0f) < 1e-6f, "volume 2 counts as 1");
    walpe_sound_set_volume (q, NAN);    /* not a number: ignored */
    CHECK (fabsf (walpe_sound_get_volume (q) - 1.0f) < 1e-6f, "NaN is not a volume");
    walpe_sound_play (q);
    WAIT (stats_of (q).plays == 1, 5000);
    st = stats_of (q);
    CHECK (st.peak >= 3990 && st.peak <= 4001, "user volume 0.25: peak %d", st.peak);
    walpe_sound_set_master (1.0);

    /* the WAV kinds: every sample is half scale */
    static const struct { int bits, fmt, ch; } kinds[] = { { 8, 1, 1 }, { 16, 1, 2 }, { 24, 1, 2 }, { 32, 1, 1 }, { 32, 3, 2 } };
    for (guint k = 0; k < G_N_ELEMENTS (kinds); k++) {
        char *nm = g_strdup_printf ("k%u.wav", k);
        char *p = write_wav (dir, nm, 22050, kinds[k].ch, kinds[k].bits, kinds[k].fmt, 500, FALSE, -1);
        WalpeSound *w = snd_new (g, p, NULL, WALPE_SOUND_SINGLE, 1.0f);
        walpe_sound_play (w);
        WAIT (stats_of (w).plays == 1, 5000);
        st = stats_of (w);
        CHECK (st.frames == 500 && st.rate == 22050 && st.channels == kinds[k].ch && st.peak >= 16380 &&
               st.peak <= 16385 && !st.error[0],
               "WAV %d bit fmt %d: frames %" G_GINT64_FORMAT " ch %d peak %d error '%s'", kinds[k].bits,
               kinds[k].fmt, st.frames, st.channels, st.peak, st.error);
        g_remove (p);
        g_free (p);
        g_free (nm);
    }

    /* loop and random go on until stopped */
    WalpeSound *l = snd_new (g, sine, NULL, WALPE_SOUND_LOOP, 1.0f);
    walpe_sound_play (l);
    WAIT (stats_of (l).plays >= 3, 5000);
    CHECK (stats_of (l).plays >= 3 && walpe_sound_is_playing (l), "loop: plays %d", stats_of (l).plays);
    walpe_sound_stop (l);
    CHECK (!walpe_sound_is_playing (l), "stop");
    WalpeSound *r = snd_new (g, sine, sine, WALPE_SOUND_RANDOM, 1.0f);
    walpe_sound_play (r);
    WAIT (stats_of (r).plays >= 3, 5000);
    CHECK (stats_of (r).plays >= 3, "random: plays %d", stats_of (r).plays);
    walpe_sound_group_free (g);    /* while one of them plays */
    CHECK (threads_gone (), "threads end after their group: %d left", walpe_sound_threads ());
}

/* pause, the layer's visibility and the scene's pause, at the pace of real
 * output (still the null device) */
static void test_pace (const char *dir)
{
    WalpeSoundStats st;
    walpe_sound_use_null (FALSE);
    char *lng = write_wav (dir, "long.wav", 48000, 1, 16, 1, 48000, TRUE, -1);   /* a second */
    WalpeSoundGroup *g = group_on ("/w/pace");
    WalpeSound *s = snd_new (g, lng, NULL, WALPE_SOUND_LOOP, 1.0f);
    gint64 t0 = g_get_monotonic_time ();
    walpe_sound_play (s);
    WAIT (stats_of (s).frames > 0, 5000);
    walpe_sound_pause (s);
    g_usleep (250000);             /* the block on its way out and the fade are let through */
    st = stats_of (s);
    gint64 f1 = st.frames;
    double el = (g_get_monotonic_time () - t0) / 1e6;
    g_usleep (150000);
    st = stats_of (s);
    CHECK (f1 > 0 && f1 <= (gint64) (el * 48000.0) + 2 * WALPE_SOUND_BLOCK && st.frames == f1 &&
           !walpe_sound_is_playing (s) && !st.running,
           "pause: %" G_GINT64_FORMAT " then %" G_GINT64_FORMAT " frames in %.2f s (not faster than real output)",
           f1, st.frames, el);
    walpe_sound_play (s);
    WAIT (stats_of (s).frames > f1, 5000);
    CHECK (stats_of (s).frames > f1, "play after pause goes on");
    walpe_sound_hold (s, TRUE);    /* the layer is invisible: silent, still "playing" */
    g_usleep (250000);
    gint64 f2 = stats_of (s).frames;
    g_usleep (150000);
    st = stats_of (s);
    CHECK (f2 > f1 && st.frames == f2 && walpe_sound_is_playing (s) && !st.running,
           "invisible: %" G_GINT64_FORMAT " then %" G_GINT64_FORMAT " frames", f2, st.frames);
    walpe_sound_hold (s, FALSE);
    WAIT (stats_of (s).frames > f2, 5000);
    CHECK (stats_of (s).frames > f2, "visible again: it goes on");
    walpe_sound_group_set_active (g, FALSE);   /* the scene is paused */
    g_usleep (250000);
    gint64 f3 = stats_of (s).frames;
    g_usleep (150000);
    st = stats_of (s);
    CHECK (st.frames == f3 && walpe_sound_is_playing (s) && !st.running && !walpe_sound_group_speaking (g),
           "paused scene: %" G_GINT64_FORMAT " then %" G_GINT64_FORMAT " frames", f3, st.frames);
    walpe_sound_group_set_active (g, TRUE);
    WAIT (stats_of (s).frames > f3, 5000);
    st = stats_of (s);
    CHECK (st.frames > f3 && st.plays == 0, "resumed: it goes on where it was (plays %d)", st.plays);
    /* A pause that is longer than a silent player keeps its output (2 s):
     * the output is closed, the file stays where it is, and play opens an
     * output again. (Every pause above is shorter: those players never
     * closed theirs.) A new file for it: the one above is near its end. */
    char *two = write_wav (dir, "long2.wav", 48000, 1, 16, 1, 5 * 48000, TRUE, -1);
    WalpeSound *p2 = snd_new (g, two, NULL, WALPE_SOUND_LOOP, 1.0f);
    walpe_sound_play (p2);
    WAIT (stats_of (p2).frames > 0, 5000);
    walpe_sound_pause (p2);
    g_usleep (2600000);
    gint64 f4 = stats_of (p2).frames;
    walpe_sound_play (p2);
    WAIT (stats_of (p2).frames > f4 + 2 * WALPE_SOUND_BLOCK, 5000);
    st = stats_of (p2);
    CHECK (f4 > 0 && st.frames > f4 + 2 * WALPE_SOUND_BLOCK && st.plays == 0 && st.opens == 1,
           "play after a pause of 2.6 s: %" G_GINT64_FORMAT " then %" G_GINT64_FORMAT " frames, plays %d, opens %d "
           "(the output is opened again, the file is not)", f4, st.frames, st.plays, st.opens);
    walpe_sound_group_free (g);
    CHECK (threads_gone (), "threads end");
    walpe_sound_use_null (TRUE);
    g_remove (lng);
    g_free (lng);
    g_remove (two);
    g_free (two);
}

/* Random mode with a pause shorter than half a second: the player keeps its
 * stream through the pause, and the file after it is put out at the pace of
 * real output like the one before it. (On the null device it was not: the
 * pace clock stood where the last file had ended, the pause counted as time
 * the thread was late, and the next file was written as fast as it decodes
 * until that time was made up - the pause was swallowed, a quarter-second
 * file with a pause of 0.2 s came round every 0.25 s instead of every 0.45 s.
 * A real output never did that: its writes block.) Only a lower bound is
 * checked: a loaded machine makes the round longer, never shorter. */
static void test_random_pace (const char *dir)
{
    walpe_sound_use_null (FALSE);
    char *f = write_wav (dir, "round.wav", 48000, 1, 16, 1, 12000, TRUE, -1);   /* a quarter of a second */
    WalpeSoundGroup *g = group_on ("/w/random-pace");
    const char *files[1] = { f };
    WalpeSoundSpec spec = { files, 1, 1.0f, WALPE_SOUND_RANDOM, 0.2f, 0.2f, "round" };
    WalpeSound *s = walpe_sound_new (g, &spec);
    walpe_sound_play (s);
    double round[3] = { 0.0, 0.0, 0.0 };
    WAIT (stats_of (s).plays >= 1, 5000);
    gint64 t = g_get_monotonic_time ();
    for (int i = 0; i < 3; i++) {
        WAIT (stats_of (s).plays >= i + 2, 5000);
        const gint64 now = g_get_monotonic_time ();
        round[i] = (now - t) / 1e6;
        t = now;
    }
    WalpeSoundStats st = stats_of (s);
    CHECK (st.plays >= 4 && round[0] >= 0.40 && round[1] >= 0.40 && round[2] >= 0.40,
           "random, pause 0.2 s, file 0.25 s: the rounds took %.3f, %.3f, %.3f s (each: the pause and the file at "
           "the pace of output, 0.45 s), plays %d", round[0], round[1], round[2], st.plays);
    CHECK (st.frames >= 4 * 12000 && st.opens >= 4, "random: %" G_GINT64_FORMAT " frames, %d opens", st.frames, st.opens);
    walpe_sound_group_free (g);
    CHECK (threads_gone (), "threads end");
    walpe_sound_use_null (TRUE);
    g_remove (f);
    g_free (f);
}

/* ---- 4 ---- */
static void test_gates (const char *sine)
{
    /* the setting is off and the wallpaper has no switch: silent, no thread */
    walpe_sound_set_enabled (FALSE);
    WalpeSoundGroup *g = walpe_sound_group_new ("/w/gates", -1);
    walpe_sound_group_set_active (g, TRUE);
    WalpeSound *s = snd_new (g, sine, NULL, WALPE_SOUND_LOOP, 1.0f);
    walpe_sound_play (s);
    g_usleep (50000);
    CHECK (!walpe_sound_group_enabled (g) && !walpe_sound_group_speaking (g) && walpe_sound_is_playing (s) &&
           stats_of (s).frames == 0 && walpe_sound_threads () == 0,
           "sound off: told to play, nothing runs (frames %" G_GINT64_FORMAT ", threads %d)",
           stats_of (s).frames, walpe_sound_threads ());
    /* switched on while the wallpaper is shown: heard at once, no reload */
    walpe_sound_set_enabled (TRUE);
    WAIT (stats_of (s).plays >= 2, 5000);
    CHECK (walpe_sound_group_enabled (g) && walpe_sound_group_speaking (g) && stats_of (s).plays >= 2,
           "setting on: plays %d", stats_of (s).plays);
    /* volume 0 and mute: silence, and nothing runs for it */
    walpe_sound_set_master (0.0);
    WAIT (!stats_of (s).running, 5000);
    gint64 f = stats_of (s).frames;
    g_usleep (50000);
    CHECK (stats_of (s).frames == f && !walpe_sound_group_speaking (g) && walpe_sound_is_playing (s),
           "volume 0: nothing is put out");
    walpe_sound_set_master (0.5);
    WAIT (stats_of (s).frames > f, 5000);
    CHECK (stats_of (s).frames > f, "volume up again: it goes on");
    walpe_sound_set_muted (TRUE);
    WAIT (!stats_of (s).running, 5000);
    f = stats_of (s).frames;
    g_usleep (50000);
    CHECK (stats_of (s).frames == f && !walpe_sound_group_speaking (g), "mute: nothing is put out");
    walpe_sound_set_muted (FALSE);
    WAIT (stats_of (s).frames > f, 5000);
    CHECK (stats_of (s).frames > f, "mute off: it goes on");
    walpe_sound_set_master (1.0);
    /* the wallpaper's own switch wins both ways */
    WalpeSoundGroup *off = walpe_sound_group_new ("/w/own-off", 0);
    walpe_sound_group_set_active (off, TRUE);
    WalpeSound *so = snd_new (off, sine, NULL, WALPE_SOUND_LOOP, 1.0f);
    walpe_sound_play (so);
    walpe_sound_set_enabled (FALSE);
    WAIT (!stats_of (s).running, 5000);
    WalpeSoundGroup *on = group_on ("/w/own-on");
    WalpeSound *sn = snd_new (on, sine, NULL, WALPE_SOUND_LOOP, 1.0f);
    walpe_sound_play (sn);
    WAIT (stats_of (sn).plays >= 2, 5000);
    CHECK (stats_of (sn).plays >= 2 && walpe_sound_group_enabled (on),
           "the wallpaper's switch on, setting off: plays %d", stats_of (sn).plays);
    walpe_sound_set_enabled (TRUE);
    g_usleep (50000);
    CHECK (stats_of (so).frames == 0 && !walpe_sound_group_enabled (off) && !walpe_sound_group_speaking (off),
           "the wallpaper's switch off, setting on: silent");
    char *line = walpe_sound_status ();
    CHECK (line && g_str_has_prefix (line, "on"), "status: %s", line ? line : "(null)");
    g_free (line);
    /* every wallpaper with sound is paused (a covered monitor, a fullscreen
     * window): the status line says that this is why nothing is heard, and
     * stops saying it when one of them is shown again */
    walpe_sound_group_set_active (g, FALSE);
    walpe_sound_group_set_active (on, FALSE);
    WAIT (!stats_of (s).running && !stats_of (sn).running, 5000);
    line = walpe_sound_status ();
#ifdef HAVE_PULSE
    CHECK (line && strstr (line, "2 wallpaper(s) with sound paused"), "status, all paused: %s", line ? line : "(null)");
#else
    CHECK (line && g_str_has_prefix (line, "on"), "status, all paused: %s", line ? line : "(null)");
#endif
    g_free (line);
    f = stats_of (s).frames;
    walpe_sound_group_set_active (g, TRUE);
    WAIT (stats_of (s).frames > f, 5000);
    line = walpe_sound_status ();
    CHECK (stats_of (s).frames > f && line && !strstr (line, "paused"), "status, one shown again: %s",
           line ? line : "(null)");
    g_free (line);
    walpe_sound_group_free (g);
    walpe_sound_group_free (off);
    walpe_sound_group_free (on);
    CHECK (threads_gone (), "threads end");
}

/* ---- 5 ---- */
static void test_one_instance (const char *sine)
{
    WalpeSoundGroup *a = group_on ("/w/same"), *b = group_on ("/w/same"), *c = group_on ("/w/other");
    WalpeSound *sa = snd_new (a, sine, NULL, WALPE_SOUND_LOOP, 1.0f);
    WalpeSound *sb = snd_new (b, sine, NULL, WALPE_SOUND_LOOP, 1.0f);
    WalpeSound *sc = snd_new (c, sine, NULL, WALPE_SOUND_LOOP, 1.0f);
    walpe_sound_play (sa);
    walpe_sound_play (sb);
    walpe_sound_play (sc);
    WAIT (stats_of (sa).plays >= 2 && stats_of (sc).plays >= 2, 5000);
    CHECK (walpe_sound_group_speaking (a) && !walpe_sound_group_speaking (b) && walpe_sound_group_speaking (c),
           "one of two instances of a wallpaper speaks; another wallpaper speaks too");
    CHECK (stats_of (sa).plays >= 2 && stats_of (sb).frames == 0 && stats_of (sc).plays >= 2,
           "frames: first %" G_GINT64_FORMAT ", second %" G_GINT64_FORMAT ", other wallpaper %" G_GINT64_FORMAT,
           stats_of (sa).frames, stats_of (sb).frames, stats_of (sc).frames);
    /* the first is paused (its monitor is covered): the second takes over */
    walpe_sound_group_set_active (a, FALSE);
    WAIT (stats_of (sb).plays >= 2, 5000);
    CHECK (!walpe_sound_group_speaking (a) && walpe_sound_group_speaking (b) && stats_of (sb).plays >= 2,
           "the second instance takes over");
    /* the first comes back: the one that speaks keeps the word */
    walpe_sound_group_set_active (a, TRUE);
    WAIT (!stats_of (sa).running, 5000);
    gint64 fa = stats_of (sa).frames;
    g_usleep (50000);
    CHECK (!walpe_sound_group_speaking (a) && walpe_sound_group_speaking (b) && stats_of (sa).frames == fa,
           "no second voice when the first comes back");
    /* the speaker is freed (its wallpaper was changed): the other one speaks */
    walpe_sound_group_free (b);
    WAIT (stats_of (sa).frames > fa, 5000);
    CHECK (walpe_sound_group_speaking (a) && stats_of (sa).frames > fa, "the remaining instance speaks");
    walpe_sound_group_free (a);
    walpe_sound_group_free (c);
    CHECK (threads_gone (), "threads end");
}

/* ---- 6 ---- */

/* a decoder of the test's own: a "file" that never ends (the length limit),
 * or one whose sample rate is out of range */
static int fake_rate = 8000;
static gpointer fake_open (const char *path, int *rate, int *channels, char **err)
{
    (void) path;
    (void) err;
    *rate = fake_rate;
    *channels = 1;
    return g_new0 (int, 1);
}
static int fake_read (gpointer dec, gint16 *out, int max_frames)
{
    (void) dec;
    for (int i = 0; i < max_frames; i++)
        out[i] = 1000;
    return max_frames;
}
static void fake_close (gpointer dec) { g_free (dec); }
static const WalpeSoundDecoder fake = { fake_open, fake_read, fake_close };

static void test_limits (const char *dir, const char *sine)
{
    WalpeSoundStats st;
    WalpeSoundGroup *g = group_on ("/w/limits");

    /* files that are no sound: an error, no noise, no hang, never tried again */
    char *junk = g_build_filename (dir, "junk.wav", NULL);
    g_file_set_contents (junk, "RIFF\020\0\0\0WAVEnothing here at all", 31, NULL);
    char *gone = g_build_filename (dir, "gone.wav", NULL);
    WalpeSound *s = snd_new (g, junk, gone, WALPE_SOUND_LOOP, 1.0f);
    walpe_sound_play (s);
    WAIT (!walpe_sound_is_playing (s), 5000);
    st = stats_of (s);
    CHECK (st.frames == 0 && st.error[0] && !walpe_sound_is_playing (s),
           "junk and missing files: frames %" G_GINT64_FORMAT " error '%s'", st.frames, st.error);
    walpe_sound_play (s);
    g_usleep (30000);
    CHECK (!walpe_sound_is_playing (s) && stats_of (s).opens == 0, "a player without a playable file stays stopped");
    /* one good file among bad ones is played */
    WalpeSound *mix = snd_new (g, junk, sine, WALPE_SOUND_LOOP, 1.0f);
    walpe_sound_play (mix);
    WAIT (stats_of (mix).plays >= 3, 5000);
    st = stats_of (mix);
    CHECK (st.plays >= 3 && st.opens >= st.plays && st.opens <= st.plays + 1,
           "a bad file is left out, the good one loops: plays %d opens %d", st.plays, st.opens);
    walpe_sound_stop (mix);

    /* sample rates and channel counts */
    static const struct { int rate, ch; gboolean ok; int out_ch; } fm[] = {
        { 4000, 1, FALSE, 0 }, { 400000, 2, FALSE, 0 }, { 8000, 1, TRUE, 1 }, { 192000, 2, TRUE, 2 },
        { 44100, 6, TRUE, 2 }, { 44100, 9, FALSE, 0 }, { 44100, 0, FALSE, 0 },
    };
    for (guint k = 0; k < G_N_ELEMENTS (fm); k++) {
        char *nm = g_strdup_printf ("f%u.wav", k);
        char *p = write_wav (dir, nm, fm[k].rate, fm[k].ch, 16, 1, 300, FALSE, -1);
        WalpeSound *w = snd_new (g, p, NULL, WALPE_SOUND_SINGLE, 1.0f);
        if (!w) {
            /* (the players of this group are used up further down, not here) */
            CHECK (FALSE, "no player for %s", nm);
        } else {
            walpe_sound_play (w);
            WAIT (!walpe_sound_is_playing (w), 5000);
            st = stats_of (w);
            CHECK (fm[k].ok ? st.frames == 300 && st.channels == fm[k].out_ch && !st.error[0]
                            : st.frames == 0 && st.error[0],
                   "WAV %d Hz %d ch: frames %" G_GINT64_FORMAT " ch %d error '%s'", fm[k].rate, fm[k].ch,
                   st.frames, st.channels, st.error);
        }
        g_remove (p);
        g_free (p);
        g_free (nm);
    }

    /* no sound in the file: an error, not an endless reopening */
    char *empty = write_wav (dir, "empty.wav", 44100, 2, 16, 1, 0, FALSE, -1);
    WalpeSound *e = snd_new (g, empty, NULL, WALPE_SOUND_LOOP, 1.0f);
    walpe_sound_play (e);
    WAIT (!walpe_sound_is_playing (e), 5000);
    st = stats_of (e);
    CHECK (!walpe_sound_is_playing (e) && st.opens == 1 && st.error[0], "empty file in a loop: opens %d error '%s'",
           st.opens, st.error);
    /* a few samples in a loop: at most 50 files a second, also on the fast device */
    char *tiny = write_wav (dir, "tiny.wav", 44100, 1, 16, 1, 10, FALSE, -1);
    WalpeSound *t = snd_new (g, tiny, NULL, WALPE_SOUND_LOOP, 1.0f);
    walpe_sound_play (t);
    g_usleep (300000);
    st = stats_of (t);
    CHECK (st.opens >= 2 && st.opens <= 40, "10 samples in a loop: %d opens in 0.3 s", st.opens);
    walpe_sound_stop (t);
    /* shorter than its header says: what is there is played, then it ends */
    char *cut = write_wav (dir, "cut.wav", 44100, 1, 16, 1, 700, FALSE, 1000000);
    WalpeSound *c = snd_new (g, cut, NULL, WALPE_SOUND_SINGLE, 1.0f);
    walpe_sound_play (c);
    WAIT (!walpe_sound_is_playing (c), 5000);
    st = stats_of (c);
    CHECK (st.frames == 700 && st.plays == 1, "truncated WAV: frames %" G_GINT64_FORMAT, st.frames);
    /* "fmt " hidden behind more chunks than are walked */
    {
        GByteArray *b = g_byte_array_new ();
        static const guint8 hd[12] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E' };
        static const guint8 ck[8] = { 'j', 'u', 'n', 'k', 0, 0, 0, 0 };
        g_byte_array_append (b, hd, 12);
        for (int i = 0; i < 100; i++)
            g_byte_array_append (b, ck, 8);
        char *many = g_build_filename (dir, "many.wav", NULL);
        g_file_set_contents (many, (const char *) b->data, (gssize) b->len, NULL);
        g_byte_array_free (b, TRUE);
        WalpeSound *m = snd_new (g, many, NULL, WALPE_SOUND_SINGLE, 1.0f);
        walpe_sound_play (m);
        WAIT (!walpe_sound_is_playing (m), 5000);
        CHECK (stats_of (m).frames == 0 && stats_of (m).error[0], "100 chunks and no format: refused");
        g_remove (many);
        g_free (many);
    }
    /* not a regular file: a directory, a FIFO (opening one for reading
     * would wait for a writer for ever) */
    char *fifo = g_build_filename (dir, "pipe.wav", NULL);
    WalpeSound *d = snd_new (g, dir, NULL, WALPE_SOUND_SINGLE, 1.0f);
    walpe_sound_play (d);
    WAIT (!walpe_sound_is_playing (d), 5000);
    CHECK (!walpe_sound_is_playing (d) && stats_of (d).error[0], "a directory: '%s'", stats_of (d).error);
    if (mkfifo (fifo, 0600) == 0) {
        WalpeSound *f = snd_new (g, fifo, NULL, WALPE_SOUND_SINGLE, 1.0f);
        walpe_sound_play (f);
        WAIT (!walpe_sound_is_playing (f), 5000);
        CHECK (!walpe_sound_is_playing (f) && stats_of (f).error[0], "a FIFO: '%s'", stats_of (f).error);
        g_remove (fifo);
    }
    walpe_sound_group_free (g);
    CHECK (threads_gone (), "threads end");

    /* players of one scene */
    g = group_on ("/w/many");
    int made = 0;
    for (int i = 0; i < WALPE_SOUND_MAX_LAYERS + 5; i++)
        made += snd_new (g, sine, NULL, WALPE_SOUND_SINGLE, 1.0f) != NULL;
    CHECK (made == WALPE_SOUND_MAX_LAYERS, "%d players for %d layers", made, WALPE_SOUND_MAX_LAYERS + 5);
    /* files of one layer */
    {
        const char *files[WALPE_SOUND_MAX_FILES + 30];
        for (guint i = 0; i < G_N_ELEMENTS (files); i++)
            files[i] = junk;
        WalpeSoundGroup *g2 = group_on ("/w/files");
        WalpeSoundSpec spec = { files, (int) G_N_ELEMENTS (files), 1.0f, WALPE_SOUND_LOOP, 0.0f, 0.0f, "many files" };
        WalpeSound *mf = walpe_sound_new (g2, &spec);
        walpe_sound_play (mf);
        WAIT (!walpe_sound_is_playing (mf), 5000);
        CHECK (mf && !walpe_sound_is_playing (mf), "94 unplayable files: stopped after the first %d",
               WALPE_SOUND_MAX_FILES);
        walpe_sound_group_free (g2);
    }
    walpe_sound_group_free (g);
    CHECK (threads_gone (), "threads end");

    /* a decoder's word is checked: a sample rate out of range is refused;
     * a file that never ends is cut at the length limit */
    char *other = g_build_filename (dir, "other.bin", NULL);
    g_file_set_contents (other, "not RIFF: goes to the decoder", -1, NULL);
    walpe_sound_set_decoder (&fake);
    g = group_on ("/w/decoder");
    fake_rate = 1000000;
    WalpeSound *bad = snd_new (g, other, NULL, WALPE_SOUND_SINGLE, 1.0f);
    walpe_sound_play (bad);
    WAIT (!walpe_sound_is_playing (bad), 5000);
    CHECK (stats_of (bad).frames == 0 && stats_of (bad).error[0], "decoder says 1 MHz: '%s'", stats_of (bad).error);
    fake_rate = 8000;
    WalpeSound *endless = snd_new (g, other, NULL, WALPE_SOUND_SINGLE, 1.0f);
    walpe_sound_play (endless);
    WAIT (!walpe_sound_is_playing (endless), 120000);
    st = stats_of (endless);
    const gint64 cap = (gint64) 8000 * WALPE_SOUND_MAX_SECONDS;
    CHECK (!walpe_sound_is_playing (endless) && st.plays == 1 && st.frames <= cap && st.frames > cap - 2 * WALPE_SOUND_BLOCK,
           "a file without an end: %" G_GINT64_FORMAT " frames, limit %" G_GINT64_FORMAT, st.frames, cap);
    walpe_sound_group_free (g);
    walpe_sound_set_decoder (NULL);
    CHECK (threads_gone (), "threads end");

    g_remove (junk);
    g_remove (empty);
    g_remove (tiny);
    g_remove (cut);
    g_remove (other);
    g_free (junk);
    g_free (gone);
    g_free (empty);
    g_free (tiny);
    g_free (cut);
    g_free (fifo);
    g_free (other);
}

/* ---- 7 ---- */
static void test_free_running (const char *dir)
{
    walpe_sound_use_null (FALSE);   /* at the pace of real output: the threads sleep in their writes */
    char *lng = write_wav (dir, "free.wav", 48000, 2, 16, 1, 48000, TRUE, -1);
    WalpeSoundGroup *g = group_on ("/w/free");
    for (int i = 0; i < 8; i++)
        walpe_sound_play (snd_new (g, lng, NULL, WALPE_SOUND_LOOP, 1.0f));
    WAIT (walpe_sound_threads () == 8, 5000);
    CHECK (walpe_sound_threads () == 8, "8 players run: %d threads", walpe_sound_threads ());
    gint64 t0 = g_get_monotonic_time ();
    walpe_sound_group_free (g);
    int left = walpe_sound_threads ();
    double ms = (g_get_monotonic_time () - t0) / 1e3;
    /* (each of them sleeps in a write of 40 ms at this moment) */
    CHECK (ms < 500.0 && left > 0, "freeing a group does not wait for its threads (%.1f ms, %d still there)",
           ms, left);
    CHECK (threads_gone (), "the threads end by themselves: %d left", walpe_sound_threads ());
    walpe_sound_use_null (TRUE);
    g_remove (lng);
    g_free (lng);
}

/* ---- 8 ---- */
static void test_stream_cap (const char *dir)
{
    enum { N = WALPE_SOUND_MAX_STREAMS + 4 };
    walpe_sound_use_null (FALSE);   /* at the pace of real output: every player stays inside its file */
    char *lng = write_wav (dir, "cap.wav", 48000, 1, 16, 1, 48000, TRUE, -1);
    WalpeSoundGroup *a = group_on ("/w/cap-a"), *b = group_on ("/w/cap-b");
    WalpeSound *p[N];
    for (int i = 0; i < N; i++) {
        p[i] = snd_new (i % 2 ? b : a, lng, NULL, WALPE_SOUND_LOOP, 1.0f);
        walpe_sound_play (p[i]);
    }
    /* the limit's worth of them sound, the others are told to play and wait */
    int sounding = 0;
    for (int w = 0; w < 5000 && sounding < WALPE_SOUND_MAX_STREAMS; w++) {
        g_usleep (1000);
        sounding = 0;
        for (int i = 0; i < N; i++)
            sounding += stats_of (p[i]).frames > 0;
    }
    g_usleep (700000);              /* (nearly three turns of the waiting players' asking) */
    sounding = 0;
    int playing = 0;
    for (int i = 0; i < N; i++) {
        sounding += stats_of (p[i]).frames > 0;
        playing += walpe_sound_is_playing (p[i]);
    }
    CHECK (sounding == WALPE_SOUND_MAX_STREAMS && playing == N,
           "%d players over two wallpapers: %d have put out sound (limit %d), %d are told to play", N, sounding,
           WALPE_SOUND_MAX_STREAMS, playing);
    char *line = walpe_sound_status ();
    CHECK (line && strstr (line, "limit"), "status at the limit: %s", line ? line : "(null)");
    g_free (line);
    /* four of the sounding ones are stopped: their outputs are closed at
     * once, and the four that waited get theirs */
    int stopped = 0;
    for (int i = 0; i < N && stopped < 4; i++)
        if (stats_of (p[i]).frames > 0) {
            walpe_sound_stop (p[i]);
            stopped++;
        }
    sounding = 0;
    for (int w = 0; w < 5000 && sounding < N; w++) {
        g_usleep (1000);
        sounding = 0;
        for (int i = 0; i < N; i++)
            sounding += stats_of (p[i]).frames > 0;
    }
    CHECK (stopped == 4 && sounding == N, "after 4 fell silent: %d of %d have put out sound", sounding, N);
    line = walpe_sound_status ();
    CHECK (line && strstr (line, "limit"), "status, all outputs taken again: %s", line ? line : "(null)");
    g_free (line);
    /* a group is freed while some of its players wait: nothing hangs */
    walpe_sound_group_free (a);
    walpe_sound_group_free (b);
    CHECK (threads_gone (), "the threads end, also those that waited: %d left", walpe_sound_threads ());
    line = walpe_sound_status ();
    CHECK (line && !strstr (line, "limit"), "status when all outputs are closed: %s", line ? line : "(null)");
    g_free (line);
    walpe_sound_use_null (TRUE);
    g_remove (lng);
    g_free (lng);
}

int main (void)
{
    test_names ();
    char *dir = g_dir_make_tmp ("walpe-sound-XXXXXX", NULL);
    if (!dir) {
        printf ("FAIL: no temp dir\n");
        return 1;
    }
    char *sine = write_wav (dir, "sine.wav", 8000, 1, 16, 1, 2000, TRUE, -1);
    walpe_sound_use_null (TRUE);   /* as fast as it decodes, into nothing */

    /* 2: no output unless the process allowed it */
    CHECK (!walpe_sound_allowed () && walpe_sound_group_new ("/w/x", 1) == NULL,
           "a process that did not allow sound gets no group");
    {
        char *line = walpe_sound_status ();
        CHECK (line && strstr (line, "not available"), "status before: %s", line ? line : "(null)");
        g_free (line);
    }
    walpe_sound_allow (TRUE);
    CHECK (!walpe_sound_enabled (), "the setting is off until it is switched on");
    {
        char *line = walpe_sound_status ();
        CHECK (line && g_str_has_prefix (line, "off"), "status, default: %s", line ? line : "(null)");
        g_free (line);
    }

    test_players (dir, sine);
    test_pace (dir);
    test_random_pace (dir);
    test_gates (sine);
    test_one_instance (sine);
    test_limits (dir, sine);
    test_free_running (dir);
    test_stream_cap (dir);

    g_remove (sine);
    g_rmdir (dir);
    g_free (sine);
    g_free (dir);
    if (fails) {
        printf ("scenesound: %d check(s) failed\n", fails);
        return 1;
    }
    printf ("scenesound: ok\n");
    return 0;
}
