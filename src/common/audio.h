/* audio.h - desktop audio capture -> spectrum for audio-reactive wallpapers.
 *
 * Records the default output's monitor source - what the computer plays,
 * never a microphone (audio.c says how that is kept) - through libpulse
 * (works on PulseAudio and on PipeWire via pipewire-pulse), runs a windowed FFT and
 * publishes WE-style spectra: 64/32/16 bands
 * per channel (g_AudioSpectrum{16,32,64}{Left,Right}; web wallpapers get
 * the 128-value array of wallpaperRegisterAudioListener: 64 L then 64 R).
 *
 * Capture runs ONLY while at least one consumer holds a reference: scenes
 * whose effects/particles read audio and web pages that registered a
 * listener take one while they are visible and unpaused. With no reference
 * the capture stream is closed (the idle thread just waits). Built without
 * libpulse-simple, everything reports silence. Thread-safe. */
#pragma once
#include <glib.h>
#include "common/api.h"

#define WALPE_AUDIO_BANDS 64

typedef struct {
    float left[WALPE_AUDIO_BANDS], right[WALPE_AUDIO_BANDS];  /* 64 bands, 0..~1 */
    float left32[32], right32[32];
    float left16[16], right16[16];
    gint64 stamp_us;   /* monotonic time of the last update (0 = never) */
} WalpeAudioSpectrum;

WALPE_API void walpe_audio_ref (void);
WALPE_API void walpe_audio_unref (void);
/* copy the newest spectrum; FALSE (and zeros) when nothing is captured */
WALPE_API gboolean walpe_audio_get (WalpeAudioSpectrum *out);
/* "off" | "capturing <source>" (the source's name as the server gives it:
 * "<output>.monitor"; with " (WALPE_AUDIO_SOURCE)" after it when the user
 * named it) | "error: ..." | "disabled (built without libpulse)" */
char *walpe_audio_status (void);
/* bass level (mean of the lowest bands, both channels), 0..~1 */
float walpe_audio_level (const WalpeAudioSpectrum *s);

/* What the capture may read (pure; the capture thread asks it for the
 * source its stream is on, and again when the server moves the stream).
 * is_monitor: the source is the monitor of an output - what the computer
 * plays. chosen: the source the user named in WALPE_AUDIO_SOURCE (NULL or
 * "" = none). Anything else - a microphone the server put the stream on -
 * is not read. */
gboolean walpe_audio_source_ok (gboolean is_monitor, const char *chosen);

/* pure analysis, exposed for tests: the last n samples of each channel
 * (n = FFT size, power of two) -> smoothed bands updated in place */
void walpe_audio_analyze (const float *left, const float *right, int n, int rate,
                          WalpeAudioSpectrum *inout, float dt);
/* the same step for a window of silence, without the FFT: the bands fall
 * with their release; TRUE once all of them are 0 */
gboolean walpe_audio_decay (WalpeAudioSpectrum *inout, float dt);
