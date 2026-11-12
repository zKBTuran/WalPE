/* audio_test.c - the spectrum analysis of src/common/audio.c: one FFT for
 * both channels must keep them apart, band values and the 32/16-band
 * folding are as documented, and a silent window decays exactly like an
 * analysed one without being analysed. */
#include "common/audio.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define N 4096
#define RATE 44100

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; fprintf (stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static int band_of (double hz)
{
    return (int) floor (log (hz / 30.0) / log (16000.0 / 30.0) * WALPE_AUDIO_BANDS);
}

static void tone (float *x, double hz, double amp)
{
    for (int i = 0; i < N; i++)
        x[i] = (float) (amp * sin (2 * G_PI * hz * i / RATE));
}

static int peak (const float *b, int n)
{
    int p = 0;
    for (int i = 1; i < n; i++)
        if (b[i] > b[p])
            p = i;
    return p;
}

int main (void)
{
    static float l[N], r[N], zero[N];
    WalpeAudioSpectrum s;

    /* a full-scale 1 kHz sine on the left only */
    memset (&s, 0, sizeof s);
    tone (l, 1000.0, 1.0);
    walpe_audio_analyze (l, zero, N, RATE, &s, 1.0f / 30);
    int b1k = band_of (1000.0);
    CHECK (peak (s.left, WALPE_AUDIO_BANDS) == b1k);
    CHECK (s.left[b1k] > log10f (1.0f + 36.0f * 0.84f) && s.left[b1k] < log10f (37.0f) + 0.01f);
    for (int b = 0; b < WALPE_AUDIO_BANDS; b++)
        CHECK (s.right[b] < 1e-3f);                           /* nothing leaks to the right */
    CHECK (s.left32[b1k / 2] == fmaxf (s.left[b1k & ~1], s.left[b1k | 1]));
    CHECK (s.left16[b1k / 4] >= s.left[b1k]);
    CHECK (s.stamp_us > 0);

    /* different tones left and right, quarter amplitude: log10 (1 + 9) = 1 */
    memset (&s, 0, sizeof s);
    tone (l, 440.0, 0.25);
    tone (r, 5000.0, 0.25);
    walpe_audio_analyze (l, r, N, RATE, &s, 1.0f / 30);
    CHECK (peak (s.left, WALPE_AUDIO_BANDS) == band_of (440.0));
    CHECK (peak (s.right, WALPE_AUDIO_BANDS) == band_of (5000.0));
    /* (a tone between two bins reads up to 1.4 dB low with a Hann window:
     * 0.92 .. 1.0) */
    CHECK (s.left[band_of (440.0)] > 0.92f && s.left[band_of (440.0)] < 1.01f);
    CHECK (s.right[band_of (5000.0)] > 0.92f && s.right[band_of (5000.0)] < 1.01f);
    CHECK (s.left[band_of (5000.0)] < 1e-3f);
    CHECK (s.right[band_of (440.0)] < 1e-3f);

    /* the same signal on both channels gives the same bands */
    memset (&s, 0, sizeof s);
    walpe_audio_analyze (l, l, N, RATE, &s, 1.0f / 30);
    for (int b = 0; b < WALPE_AUDIO_BANDS; b++)
        CHECK (fabsf (s.left[b] - s.right[b]) < 1e-4f);

    /* silence after sound: the release without an FFT is the release
     * with one, and it ends at exactly 0 */
    WalpeAudioSpectrum a = s, d = s;
    walpe_audio_analyze (zero, zero, N, RATE, &a, 1.0f / 30);
    gboolean done = walpe_audio_decay (&d, 1.0f / 30);
    CHECK (!done);
    for (int b = 0; b < WALPE_AUDIO_BANDS; b++) {
        CHECK (fabsf (a.left[b] - d.left[b]) < 1e-6f);
        CHECK (fabsf (a.right[b] - d.right[b]) < 1e-6f);
    }
    int steps = 1;
    while (!walpe_audio_decay (&d, 1.0f / 30) && steps < 1000)
        steps++;
    CHECK (steps < 60);                                       /* within two seconds */
    for (int b = 0; b < WALPE_AUDIO_BANDS; b++)
        CHECK (d.left[b] == 0 && d.right[b] == 0);
    for (int b = 0; b < 16; b++)
        CHECK (d.left16[b] == 0 && d.right16[b] == 0);

    /* the bands as numbers (what a wallpaper sees; measured the same end
     * to end, through a sound server, in a web page's listener, a scene's
     * shader uniforms and SceneScript's buffers): 64 bands from 30 Hz to
     * 16 kHz, each a 64th of the nine octaves between them. A 440 Hz tone
     * is band 27 of 64, 13 of 32, 6 of 16. (Below about 110 Hz a band is
     * narrower than one bin of the transform and neighbours read alike:
     * no tone from there in this table) */
    static const struct { double hz; int b64, b32, b16; } tones[] = {
        { 200, 19, 9, 4 }, { 440, 27, 13, 6 }, { 1000, 35, 17, 8 },
        { 5000, 52, 26, 13 }, { 10000, 59, 29, 14 }, { 15000, 63, 31, 15 },
    };
    for (guint i = 0; i < G_N_ELEMENTS (tones); i++) {
        memset (&s, 0, sizeof s);
        tone (l, tones[i].hz, 0.25);
        walpe_audio_analyze (l, l, N, RATE, &s, 1.0f / 30);
        CHECK (peak (s.left, 64) == tones[i].b64);
        CHECK (peak (s.right, 64) == tones[i].b64);
        CHECK (peak (s.left32, 32) == tones[i].b32);
        CHECK (peak (s.left16, 16) == tones[i].b16);
        CHECK (peak (s.right16, 16) == tones[i].b16);
        if (fails)
            fprintf (stderr, "  (tone %g Hz: band %d / %d / %d)\n", tones[i].hz, peak (s.left, 64),
                     peak (s.left32, 32), peak (s.left16, 16));
        /* a quarter of full scale (-12 dBFS) reads 1.0: louder reads more
         * (full scale 1.57), as Wallpaper Engine's values "may exceed 1" */
        CHECK (s.left[tones[i].b64] > 0.90f && s.left[tones[i].b64] < 1.01f);
        for (int b = 0; b < 64; b++)
            CHECK (s.left[b] >= 0 && s.left[b] < 1.01f);
    }

    /* which source the capture reads: the monitor of an output, or the
     * one source the user named - never one the server chose that is no
     * monitor (PulseAudio moves a recording stream to the microphone that
     * was just plugged in when the stream sat on the default source) */
    CHECK (walpe_audio_source_ok (TRUE, NULL));
    CHECK (walpe_audio_source_ok (TRUE, ""));
    CHECK (!walpe_audio_source_ok (FALSE, NULL));
    CHECK (!walpe_audio_source_ok (FALSE, ""));
    CHECK (walpe_audio_source_ok (FALSE, "alsa_input.usb-headset"));
    CHECK (walpe_audio_source_ok (TRUE, "some.monitor"));

    /* WALPE_NO_AUDIO: a reference starts nothing and status says why
     * (set before the first reference of this process: no sound server
     * is contacted by this test) */
    g_setenv ("WALPE_NO_AUDIO", "1", TRUE);
    walpe_audio_ref ();
    {
        char *st = walpe_audio_status ();
        CHECK (g_str_has_prefix (st, "disabled"));
        g_free (st);
        WalpeAudioSpectrum off;
        CHECK (!walpe_audio_get (&off));
        CHECK (off.left[27] == 0 && off.stamp_us == 0);
    }
    walpe_audio_unref ();

    /* silence in: silence out */
    memset (&s, 0, sizeof s);
    walpe_audio_analyze (zero, zero, N, RATE, &s, 1.0f / 30);
    for (int b = 0; b < WALPE_AUDIO_BANDS; b++)
        CHECK (s.left[b] == 0 && s.right[b] == 0);

    if (!fails)
        printf ("audio: channel separation, band values, bands of six tones, folding, silence decay, "
                "source rule, WALPE_NO_AUDIO OK\n");
    return fails ? 1 : 0;
}
