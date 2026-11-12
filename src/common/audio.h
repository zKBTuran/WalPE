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
/* "off" | "waiting for <source>: ..." (the stream is asked for and has not
 * started: PipeWire starts it when the output plays) | "capturing <source>"
 * (the source's name as the server gives it:
 * "<output>.monitor"; with " (WALPE_AUDIO_SOURCE)" after it when the user
 * named it) | "error: ..." | "disabled (built without libpulse)" */
char *walpe_audio_status (void);
/* bass level (mean of the lowest bands, both channels), 0..~1 */
float walpe_audio_level (const WalpeAudioSpectrum *s);

/* Sound after silence, for a consumer that would otherwise look for it at
 * its frame rate (a scene in which nothing moves but what follows the
 * spectrum draws no frame in silence, and woke 60 times a second to find
 * that out). TRUE: the published spectrum is silence now, and fn (user)
 * will be called once, from the capture thread, when a spectrum that is
 * not silence is published; then it is forgotten. FALSE: there is sound
 * right now (or no room for another waiter): nothing was noted, go on
 * looking. fn must not block and must not call into this file.
 * walpe_audio_wake_cancel takes the note back; when it returns, fn is
 * not running and will not be called for it. */
WALPE_API gboolean walpe_audio_wake_on_sound (void (*fn) (gpointer user), gpointer user);
WALPE_API void walpe_audio_wake_cancel (void (*fn) (gpointer user), gpointer user);

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
