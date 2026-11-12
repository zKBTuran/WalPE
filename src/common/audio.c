/* audio.c - desktop audio capture + spectrum (see audio.h).
 *
 * Capture: one thread ("walpe-audio") runs a PulseAudio main loop of its
 * own and a record stream on the default output's monitor source; it
 * works unchanged on PulseAudio and on PipeWire (pipewire-pulse is the
 * default sound server setup on Gentoo's PipeWire profiles;
 * media-libs/libpulse is present on practically every desktop install).
 * Until version 0.12 this was the *simple* API: its blocking read loop needs
 * a second thread inside libpulse, both woke for every fragment (193-245
 * wake-ups a second), it asks the server for timing updates nothing here
 * uses, and it cannot set stream flags. The stream now carries
 * PA_STREAM_DONT_INHIBIT_AUTO_SUSPEND: while nothing plays, the server
 * may suspend the output device and its monitor delivers nothing, so the
 * thread sleeps; before, the capture itself kept the device running. (On
 * PipeWire it still did: there the stream's property "node.passive" is
 * what lets the output suspend, see capture_thread.) A
 * native libpipewire-0.3 stream was implemented and measured earlier:
 * it was no better, so libpulse stays.
 *
 * Which source: what the computer plays, never what a microphone hears.
 * "@DEFAULT_MONITOR@" names the monitor of the default output when the
 * stream is made; where it is afterwards is the server's business unless
 * the stream says otherwise. PulseAudio moves every recording stream that
 * sits on the default source when another source becomes the default, and
 * on a machine without a microphone the default source IS the monitor of
 * the output: plug in a headset or a camera and the server moved this
 * stream onto its microphone (measured with PulseAudio 16.1; the status
 * went on saying "capturing @DEFAULT_MONITOR@" and a web wallpaper was
 * handed the spectrum of the room). So:
 *  - the stream carries PA_STREAM_DONT_MOVE: the server leaves it where
 *    it is (and ends it when its source goes away);
 *  - nothing is analysed before the server has said which source the
 *    stream reads, and a source that is no output's monitor is refused
 *    (walpe_audio_source_ok). Asked again whenever the stream is moved
 *    all the same;
 *  - following the user to another output is done here: when the default
 *    output changes (one server event), or the stream ends because its
 *    output is gone, the stream is made again on the new default's
 *    monitor. PulseAudio left it on the old output's monitor: the bars
 *    stood still after a switch from loudspeakers to headphones.
 * WALPE_AUDIO_SOURCE=<source name> is the user's own choice of a source,
 * a microphone included: taken as it is, never moved, not followed.
 *
 * Analysis (best-effort match of WE's behaviour; WE does not document its
 * normalisation): 44.1 kHz stereo float, one 4096-point Hann-windowed FFT
 * for both channels (left as the real part, right as the imaginary part:
 * the two spectra separate again from bins k and n-k) 30 times a second
 * (WALPE_AUDIO_HZ=10..200; WE hands out its spectrum at about 30 Hz; was
 * 60 with one FFT per channel), 64 log-spaced bands 30 Hz..16 kHz, band
 * value = peak bin amplitude (1.0 = full-scale sine) on a soft log curve
 * log10(1 + 9 * 4a) so typical music sits in 0.2..1 and silence is 0;
 * fast attack, ~150 ms release. 32/16 bands are the peak of their 2/4
 * sub-bands (same "peak bin of the range" rule as the 64). Twiddle
 * factors, window and band edges are tables (they were recomputed for
 * every analysis: 128 pow() calls each). Silence is not analysed: while
 * the whole window is below -100 dBFS the bands fall with their release
 * and then stay 0 without an FFT. */
#include "walpe-build-config.h"
#include "audio.h"
#include <math.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#ifdef HAVE_PULSE
#include <pulse/pulseaudio.h>
#endif

#define FFT_N 4096
#define RATE 44100
#ifndef AUDIO_HZ
#define AUDIO_HZ 30     /* spectra per second (WALPE_AUDIO_HZ overrides) */
#endif
#define SILENCE 1e-5f   /* |sample| below this (-100 dBFS) is silence */
/* no spectrum for this long: nothing is captured any more (the output
 * device was suspended): consumers read silence */
#define STALE_US 300000

static GMutex lock;
static G_GNUC_UNUSED GCond cond;   /* (unused without libpulse) */
static G_GNUC_UNUSED int refs;
static G_GNUC_UNUSED gboolean thread_running; /* capture thread alive (lock) */
#define AUDIO_IDLE_EXIT_S 3
static WalpeAudioSpectrum cur;
static gboolean have_data;
static gboolean cur_zero = TRUE;   /* cur holds silence */
static G_GNUC_UNUSED char *status_msg;

/* ------------------------------------------------------------- analysis */

/* tables for one FFT size and sample rate (per thread: the capture thread
 * and tests) */
static __thread struct {
    int n, rate;
    float *cs, *sn;         /* cos / -sin of 2 pi k / n, k < n/2 */
    float *win;             /* Hann window */
    float *re, *im;         /* work buffers */
    int k0[WALPE_AUDIO_BANDS], k1[WALPE_AUDIO_BANDS];   /* bins of each band */
} ft;

/* the capture thread exits when unused: its FFT tables go with it */
static G_GNUC_UNUSED void fft_buffers_free (void)
{
    g_clear_pointer (&ft.cs, g_free);
    g_clear_pointer (&ft.sn, g_free);
    g_clear_pointer (&ft.win, g_free);
    g_clear_pointer (&ft.re, g_free);
    g_clear_pointer (&ft.im, g_free);
    ft.n = ft.rate = 0;
}

static void fft_tables (int n, int rate)
{
    if (ft.n == n && ft.rate == rate)
        return;
    if (ft.n != n) {
        g_free (ft.cs); g_free (ft.sn); g_free (ft.win); g_free (ft.re); g_free (ft.im);
        ft.cs = g_new (float, n / 2);
        ft.sn = g_new (float, n / 2);
        ft.win = g_new (float, n);
        ft.re = g_new (float, n);
        ft.im = g_new (float, n);
        for (int k = 0; k < n / 2; k++) {
            double a = -2 * G_PI * k / n;
            ft.cs[k] = (float) cos (a);
            ft.sn[k] = (float) sin (a);
        }
        for (int i = 0; i < n; i++)
            ft.win[i] = 0.5f - 0.5f * cosf (2.0f * (float) G_PI * i / (n - 1));
    }
    const double f0 = 30.0, f1 = 16000.0;
    double binhz = (double) rate / n;
    for (int b = 0; b < WALPE_AUDIO_BANDS; b++) {
        double lo = f0 * pow (f1 / f0, (double) b / WALPE_AUDIO_BANDS);
        double hi = f0 * pow (f1 / f0, (double) (b + 1) / WALPE_AUDIO_BANDS);
        int k0 = (int) floor (lo / binhz), k1 = (int) ceil (hi / binhz);
        ft.k0[b] = CLAMP (k0, 1, n / 2 - 1);
        ft.k1[b] = CLAMP (k1, ft.k0[b] + 1, n / 2);
    }
    ft.n = n;
    ft.rate = rate;
}

/* in-place radix-2 FFT of ft.re / ft.im (n a power of two) */
static void fft (int n)
{
    float *re = ft.re, *im = ft.im;
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        int half = len / 2, step = n / len;
        for (int i = 0; i < n; i += len) {
            for (int k = 0; k < half; k++) {
                float cr = ft.cs[k * step], ci = ft.sn[k * step];
                int a = i + k, b = a + half;
                float xr = re[b] * cr - im[b] * ci;
                float xi = re[b] * ci + im[b] * cr;
                re[b] = re[a] - xr;
                im[b] = im[a] - xi;
                re[a] += xr;
                im[a] += xi;
            }
        }
    }
}

/* band values of both channels from ONE complex FFT: z = left + i right;
 * with Z = FFT (z), the channels' spectra are L[k] = (Z[k] + conj Z[n-k])
 * / 2 and R[k] = (Z[k] - conj Z[n-k]) / 2i */
static void bands_of (const float *l, const float *r, int n, int rate, float *l64, float *r64)
{
    fft_tables (n, rate);
    for (int i = 0; i < n; i++) {
        ft.re[i] = l[i] * ft.win[i];
        ft.im[i] = r[i] * ft.win[i];
    }
    fft (n);
    const float *re = ft.re, *im = ft.im;
    for (int b = 0; b < WALPE_AUDIO_BANDS; b++) {
        float pl = 0, pr = 0;   /* peak squared magnitudes (x4) */
        for (int k = ft.k0[b]; k < ft.k1[b]; k++) {
            float ar = re[k], ai = im[k], br = re[n - k], bi = im[n - k];
            float lr = ar + br, li = ai - bi, rr = ai + bi, ri = br - ar;
            float ml = lr * lr + li * li, mr = rr * rr + ri * ri;
            if (ml > pl)
                pl = ml;
            if (mr > pr)
                pr = mr;
        }
        /* Hann coherent gain 0.5: a full-scale sine peaks at n/4 */
        float al = 0.5f * sqrtf (pl) * 4.0f / n, ar_ = 0.5f * sqrtf (pr) * 4.0f / n;
        l64[b] = log10f (1.0f + 36.0f * al);
        r64[b] = log10f (1.0f + 36.0f * ar_);
    }
}

/* 32/16 bands cover 2/4 of the 64 bands' frequency ranges; a band is
 * the PEAK bin of its range like the 64 (the old pair average quartered
 * a pure tone in the 16-band array: a -12 dBFS 1 kHz sine read 1.0 in
 * 64 bands but 0.25 in 16, below WE's default pulse/shake audio bounds
 * of 0.5..1.0 - CreateAudioResponse in assets/effects/pulse) */
static void bands_fold (WalpeAudioSpectrum *s)
{
    for (int b = 0; b < 32; b++) {
        s->left32[b] = fmaxf (s->left[2 * b], s->left[2 * b + 1]);
        s->right32[b] = fmaxf (s->right[2 * b], s->right[2 * b + 1]);
    }
    for (int b = 0; b < 16; b++) {
        s->left16[b] = fmaxf (s->left32[2 * b], s->left32[2 * b + 1]);
        s->right16[b] = fmaxf (s->right32[2 * b], s->right32[2 * b + 1]);
    }
    s->stamp_us = g_get_monotonic_time ();
}

void walpe_audio_analyze (const float *left, const float *right, int n, int rate,
                          WalpeAudioSpectrum *s, float dt)
{
    float l[WALPE_AUDIO_BANDS], r[WALPE_AUDIO_BANDS];
    bands_of (left, right, n, rate, l, r);
    /* fast attack, ~150 ms release (WE bars fall smoothly) */
    float rel = expf (-dt / 0.15f);
    for (int b = 0; b < WALPE_AUDIO_BANDS; b++) {
        s->left[b] = l[b] >= s->left[b] ? l[b] : s->left[b] * rel + l[b] * (1 - rel);
        s->right[b] = r[b] >= s->right[b] ? r[b] : s->right[b] * rel + r[b] * (1 - rel);
    }
    bands_fold (s);
}

gboolean walpe_audio_decay (WalpeAudioSpectrum *s, float dt)
{
    /* what walpe_audio_analyze does with an all-zero window, without the
     * FFT: every band falls with the release; below 0.001 it is 0 */
    float rel = expf (-dt / 0.15f), top = 0;
    for (int b = 0; b < WALPE_AUDIO_BANDS; b++) {
        s->left[b] *= rel;
        s->right[b] *= rel;
        top = fmaxf (top, fmaxf (s->left[b], s->right[b]));
    }
    if (top < 0.001f) {
        memset (s->left, 0, sizeof (s->left));
        memset (s->right, 0, sizeof (s->right));
    }
    bands_fold (s);
    return top < 0.001f;
}

float walpe_audio_level (const WalpeAudioSpectrum *s)
{
    float sum = 0;
    for (int b = 0; b < 8; b++)
        sum += s->left[b] + s->right[b];
    return sum / 16.0f;
}

gboolean walpe_audio_source_ok (gboolean is_monitor, const char *chosen)
{
    return is_monitor || (chosen && *chosen);
}

#ifdef HAVE_PULSE
#define HAVE_CAPTURE 1
static pa_mainloop *cap_ml;        /* lock: the capture thread's loop while it runs */

static void set_status (const char *fmt, ...) G_GNUC_PRINTF (1, 2);
static void set_status (const char *fmt, ...)
{
    va_list ap;
    va_start (ap, fmt);
    char *m = g_strdup_vprintf (fmt, ap);
    va_end (ap);
    g_mutex_lock (&lock);
    g_free (status_msg);
    status_msg = m;
    g_mutex_unlock (&lock);
}

static gboolean wanted (void)
{
    g_mutex_lock (&lock);
    gboolean w = refs > 0;
    g_mutex_unlock (&lock);
    return w;
}

/* wait (no busy loop) until refs drop or 5 s pass: retry pacing */
static void retry_wait (void)
{
    g_mutex_lock (&lock);
    gint64 until = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;
    while (refs > 0 && g_cond_wait_until (&cond, &lock, until))
        ;
    g_mutex_unlock (&lock);
}

/* analysis rate: spectra per second (hop = rate / hz frames) */
static int analysis_hz (void)
{
    const char *e = g_getenv ("WALPE_AUDIO_HZ");
    int hz = e ? atoi (e) : AUDIO_HZ;
    return CLAMP (hz, 10, 200);
}

/* sample ring (capture thread only) */
typedef struct {
    float ringl[FFT_N], ringr[FFT_N], lin[FFT_N], rin[FFT_N];
    int pos, since, hop, rate;
    float hop_peak;         /* largest |sample| of the hop being filled */
    int quiet;              /* frames in a row below SILENCE (capped) */
    gboolean zero;          /* the published spectrum is all 0 */
    WalpeAudioSpectrum spec;
} Ring;

static void ring_reset (Ring *r, int rate)
{
    memset (r, 0, sizeof (*r));
    r->rate = rate;
    r->hop = MAX (64, rate / analysis_hz ());
    r->zero = TRUE;
}

static void publish (const WalpeAudioSpectrum *s, gboolean zero)
{
    g_mutex_lock (&lock);
    cur = *s;
    cur_zero = zero;
    have_data = TRUE;
    g_mutex_unlock (&lock);
}

/* one hop of samples is in: the next spectrum. A window that is silent
 * from end to end is not analysed: the bands fall with their release and
 * then stay 0, with nothing computed and nothing published. */
static void ring_hop (Ring *r)
{
    float dt = (float) r->hop / r->rate;
    r->quiet = r->hop_peak < SILENCE ? MIN (r->quiet + r->hop, 2 * FFT_N) : 0;
    r->hop_peak = 0;
    if (r->quiet >= FFT_N) {
        if (r->zero)
            return;
        r->zero = walpe_audio_decay (&r->spec, dt);
        publish (&r->spec, r->zero);
        return;
    }
    int head = FFT_N - r->pos;
    memcpy (r->lin, r->ringl + r->pos, head * sizeof (float));
    memcpy (r->lin + head, r->ringl, r->pos * sizeof (float));
    memcpy (r->rin, r->ringr + r->pos, head * sizeof (float));
    memcpy (r->rin + head, r->ringr, r->pos * sizeof (float));
    walpe_audio_analyze (r->lin, r->rin, FFT_N, r->rate, &r->spec, dt);
    r->zero = FALSE;
    publish (&r->spec, FALSE);
}

/* append interleaved stereo (or mono) float frames (x NULL = a gap in the
 * stream: silence); a spectrum every hop */
static void ring_push (Ring *r, const float *x, int frames, int ch)
{
    for (int i = 0; i < frames; i++) {
        float l = x ? x[i * ch] : 0, rr = x ? x[i * ch + (ch > 1)] : 0;
        r->ringl[r->pos] = l;
        r->ringr[r->pos] = rr;
        r->pos = (r->pos + 1) % FFT_N;
        float a = fmaxf (fabsf (l), fabsf (rr));
        if (a > r->hop_peak)
            r->hop_peak = a;
        if (++r->since < r->hop)
            continue;
        r->since = 0;
        ring_hop (r);
    }
}

static void capture_ended (void)
{
    g_mutex_lock (&lock);
    have_data = FALSE;
    cur_zero = TRUE;
    memset (&cur, 0, sizeof (cur));
    g_mutex_unlock (&lock);
    g_message ("audio: capture stopped");
    set_status ("off");
}

/* ------------------------------------------- PulseAudio stream
 * (PulseAudio servers, or PipeWire through pipewire-pulse), driven by a
 * pa_mainloop that this thread iterates itself: no helper thread */
typedef struct {
    Ring *ring;
    gboolean ctx_ready, st_ready, failed;
    char *why;
    const char *chosen;     /* WALPE_AUDIO_SOURCE: the user named the source */
    gboolean checked;       /* the server said which source the stream reads,
                             * and it may be read: samples are analysed */
    gboolean restart;       /* make the stream again (another default output,
                             * or its output is gone): no error */
    char *source;           /* the source the stream reads */
    char *sink;             /* the output it is the monitor of */
    char *def_sink;         /* the default output, as far as known */
    char **followed;        /* the default output the stream was last made
                             * again for (kept from stream to stream) */
    pa_stream *st;
} Cap;

static void cap_fail (Cap *c, const char *what, pa_context *ctx)
{
    if (!c->failed) {
        g_free (c->why);
        c->why = g_strdup_printf ("%s: %s", what, ctx ? pa_strerror (pa_context_errno (ctx)) : "?");
    }
    c->failed = TRUE;
}

static void cap_ctx_state (pa_context *ctx, void *user)
{
    Cap *c = user;
    switch (pa_context_get_state (ctx)) {
    case PA_CONTEXT_READY: c->ctx_ready = TRUE; break;
    case PA_CONTEXT_FAILED: cap_fail (c, "connection", ctx); break;
    case PA_CONTEXT_TERMINATED: c->failed = TRUE; break;
    default: break;
    }
}

/* the stream is made again when the default output is another one than
 * the one it listens to (both names known) */
static void cap_follow (Cap *c)
{
    if (c->chosen || c->restart || !c->checked || !c->sink || !c->def_sink ||
        !strcmp (c->sink, c->def_sink))
        return;
    /* once per default output: a server that names one output as the
     * default and hands out the monitor of another when asked for the
     * default's would have the stream made again for ever */
    if (*c->followed && !strcmp (*c->followed, c->def_sink))
        return;
    g_message ("audio: the default output is %s now (was %s): capturing its monitor", c->def_sink,
               c->sink);
    g_free (*c->followed);
    *c->followed = g_strdup (c->def_sink);
    c->restart = TRUE;
}

static void cap_source_info (pa_context *ctx, const pa_source_info *i, int eol, void *user)
{
    Cap *c = user;
    if (eol < 0) {
        /* (nothing is read from a source the server tells nothing about) */
        cap_fail (c, "the stream's source", ctx);
        return;
    }
    if (!i)
        return;
    const gboolean is_monitor = i->monitor_of_sink != PA_INVALID_INDEX;
    g_free (c->source);
    c->source = g_strdup (i->name ? i->name : "?");
    g_free (c->sink);
    c->sink = is_monitor && i->monitor_of_sink_name ? g_strdup (i->monitor_of_sink_name) : NULL;
    if (!walpe_audio_source_ok (is_monitor, c->chosen)) {
        if (!c->failed) {
            g_free (c->why);
            c->why = g_strdup_printf ("its source %s is not the monitor of an output (a microphone?): "
                                      "not read. WALPE_AUDIO_SOURCE=<source> names a source to read", c->source);
        }
        c->failed = TRUE;
        return;
    }
    if (!c->checked) {
        if (c->chosen)
            g_message ("audio: capturing %s (WALPE_AUDIO_SOURCE)", c->source);
        else
            g_message ("audio: capturing %s (what the output %s plays)", c->source, c->sink ? c->sink : "?");
        set_status ("capturing %s%s", c->source, c->chosen ? " (WALPE_AUDIO_SOURCE)" : "");
    }
    c->checked = TRUE;
    cap_follow (c);
}

/* which source does the stream read? Until the answer is in, its samples
 * are dropped (cap_read) */
static void cap_ask_source (Cap *c)
{
    c->checked = FALSE;
    pa_context *ctx = pa_stream_get_context (c->st);
    pa_operation *o = pa_context_get_source_info_by_index (ctx, pa_stream_get_device_index (c->st),
                                                           cap_source_info, c);
    if (o)
        pa_operation_unref (o);
    else
        cap_fail (c, "the stream's source", ctx);
}

static void cap_server_info (pa_context *ctx, const pa_server_info *i, void *user)
{
    (void) ctx;
    Cap *c = user;
    /* (a name, not a placeholder: pipewire-pulse says "@DEFAULT_SINK@"
     * for a moment after the default output was removed) */
    if (!i || !i->default_sink_name || !*i->default_sink_name || *i->default_sink_name == '@')
        return;
    char *was = c->def_sink;
    c->def_sink = g_strdup (i->default_sink_name);
    if (!c->chosen && !c->checked && !c->restart && was && strcmp (was, c->def_sink)) {
        /* the stream has not started yet (PipeWire starts it when its
         * output plays something) and was asked for on the output that
         * was the default then: it would wait there */
        g_message ("audio: the default output is %s now (was %s): capturing its monitor", c->def_sink, was);
        c->restart = TRUE;
    }
    g_free (was);
    cap_follow (c);
}

/* a server event: the default output may be another one now */
static void cap_event (pa_context *ctx, pa_subscription_event_type_t t, uint32_t idx, void *user)
{
    (void) idx;
    if ((t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) != PA_SUBSCRIPTION_EVENT_SERVER)
        return;
    pa_operation *o = pa_context_get_server_info (ctx, cap_server_info, user);
    if (o)
        pa_operation_unref (o);
}

static void cap_stream_state (pa_stream *st, void *user)
{
    Cap *c = user;
    switch (pa_stream_get_state (st)) {
    case PA_STREAM_READY:
        c->st_ready = TRUE;
        cap_ask_source (c);
        break;
    case PA_STREAM_FAILED:
        /* a stream that was running and is ended by the server while the
         * connection stands: its output is gone (unplugged, switched off).
         * Not an error: the next default output's monitor is taken */
        if (c->checked && !c->chosen &&
            pa_context_get_state (pa_stream_get_context (st)) == PA_CONTEXT_READY) {
            g_message ("audio: the stream on %s was ended by the server (%s): connecting again",
                       c->source ? c->source : "?", pa_strerror (pa_context_errno (pa_stream_get_context (st))));
            c->checked = FALSE;
            c->restart = TRUE;
        } else {
            cap_fail (c, "stream", pa_stream_get_context (st));
        }
        break;
    case PA_STREAM_TERMINATED: c->failed = TRUE; break;
    default: break;
    }
}

/* the server moved the stream (it is asked not to): it is on another
 * source now, which is looked at before anything of it is analysed */
static void cap_moved (pa_stream *st, void *user)
{
    (void) st;
    cap_ask_source (user);
}

static void cap_read (pa_stream *st, size_t nbytes, void *user)
{
    (void) nbytes;
    Cap *c = user;
    for (;;) {
        const void *data = NULL;
        size_t n = 0;
        if (pa_stream_peek (st, &data, &n) < 0) {
            cap_fail (c, "read", pa_stream_get_context (st));
            return;
        }
        if (!n)
            return;
        if (c->checked)
            ring_push (c->ring, data, (int) (n / (2 * sizeof (float))), 2);
        pa_stream_drop (st);
    }
}

/* a stream that is made again and again (a server that ends it each time)
 * is an error like any other: so many times in a row without a wait */
#define CAP_RESTARTS 3

static gpointer capture_thread (gpointer data)
{
    (void) data;
    Ring *ring = g_new0 (Ring, 1);
    gboolean warned = FALSE;
    int restarts = 0;
    char *followed = NULL;
    for (;;) {
        /* unused for AUDIO_IDLE_EXIT_S (a wallpaper switch unrefs and refs
         * again within it): the thread ends, nothing stays resident */
        g_mutex_lock (&lock);
        gint64 until = g_get_monotonic_time () + AUDIO_IDLE_EXIT_S * G_TIME_SPAN_SECOND;
        while (refs <= 0 && g_cond_wait_until (&cond, &lock, until))
            ;
        if (refs <= 0) {
            thread_running = FALSE;
            g_mutex_unlock (&lock);
            g_free (ring);
            g_free (followed);
            fft_buffers_free ();
            g_message ("audio: capture thread ended (no audio-reactive wallpaper)");
            return NULL;
        }
        g_mutex_unlock (&lock);

        const char *chosen = g_getenv ("WALPE_AUDIO_SOURCE");
        if (chosen && !*chosen)
            chosen = NULL;
        const char *dev = chosen ? chosen : "@DEFAULT_MONITOR@";
        ring_reset (ring, RATE);
        Cap cap = { .ring = ring, .chosen = chosen, .followed = &followed };
        pa_mainloop *ml = pa_mainloop_new ();
        pa_context *ctx = ml ? pa_context_new (pa_mainloop_get_api (ml), "walpe") : NULL;
        pa_stream *st = NULL;
        if (!ctx) {
            cap.failed = TRUE;
            cap.why = g_strdup ("no PulseAudio context");
        } else {
            g_mutex_lock (&lock);
            cap_ml = ml;        /* walpe_audio_unref wakes it */
            g_mutex_unlock (&lock);
            pa_context_set_state_callback (ctx, cap_ctx_state, &cap);
            if (pa_context_connect (ctx, NULL, PA_CONTEXT_NOFLAGS, NULL) < 0)
                cap_fail (&cap, "connection", ctx);
        }
        while (!cap.failed && !cap.ctx_ready && wanted ())
            if (pa_mainloop_iterate (ml, 1, NULL) < 0)
                cap.failed = TRUE;
        if (!cap.failed && cap.ctx_ready) {
            if (!chosen) {
                /* the default output, now and whenever it changes (one
                 * event per change of the server's settings: nothing
                 * while they stay as they are) */
                pa_operation *o;
                pa_context_set_subscribe_callback (ctx, cap_event, &cap);
                if ((o = pa_context_subscribe (ctx, PA_SUBSCRIPTION_MASK_SERVER, NULL, NULL)))
                    pa_operation_unref (o);
                if ((o = pa_context_get_server_info (ctx, cap_server_info, &cap)))
                    pa_operation_unref (o);
            }
            pa_sample_spec ss = { .format = PA_SAMPLE_FLOAT32LE, .rate = RATE, .channels = 2 };
            /* one fragment per spectrum; the latency follows it (as the
             * simple API asked for) */
            pa_buffer_attr ba = { .maxlength = (guint32) -1, .tlength = (guint32) -1,
                                  .prebuf = (guint32) -1, .minreq = (guint32) -1,
                                  .fragsize = (guint32) (ring->hop * 2 * sizeof (float)) };
            /* PipeWire: a stream linked to an output's monitor keeps that
             * output running - the device never suspends and the monitor
             * delivers silence for ever - unless its node is passive.
             * pipewire-pulse does not take DONT_INHIBIT_AUTO_SUSPEND for
             * that (1.0.5); it does hand the stream's properties on.
             * PulseAudio keeps the property and makes nothing of it. */
            pa_proplist *pl = pa_proplist_new ();
            if (pl)
                pa_proplist_sets (pl, "node.passive", "true");
            st = pl ? pa_stream_new_with_proplist (ctx, "audio-reactive wallpaper", &ss, NULL, pl)
                    : pa_stream_new (ctx, "audio-reactive wallpaper", &ss, NULL);
            if (pl)
                pa_proplist_free (pl);
            if (!st) {
                cap_fail (&cap, "stream", ctx);
            } else {
                cap.st = st;
                pa_stream_set_state_callback (st, cap_stream_state, &cap);
                pa_stream_set_read_callback (st, cap_read, &cap);
                pa_stream_set_moved_callback (st, cap_moved, &cap);
                /* DONT_INHIBIT_AUTO_SUSPEND: watching the output must not
                 * keep an idle output device running. DONT_MOVE: the
                 * server must not put the stream on another source (the
                 * head of this file) */
                if (pa_stream_connect_record (st, dev, &ba, PA_STREAM_ADJUST_LATENCY |
                                              PA_STREAM_DONT_INHIBIT_AUTO_SUSPEND |
                                              PA_STREAM_DONT_MOVE) < 0)
                    cap_fail (&cap, "stream", ctx);
                else
                    /* PulseAudio: for a moment. PipeWire starts a stream
                     * on the monitor of an output that is suspended when
                     * the output plays again */
                    set_status ("waiting for %s: the stream starts when the output plays", dev);
            }
            /* (until the server has said which source it is) */
            while (!cap.failed && !cap.restart && !(cap.st_ready && cap.checked) && wanted ())
                if (pa_mainloop_iterate (ml, 1, NULL) < 0)
                    cap.failed = TRUE;
        }
        if (cap.failed || cap.restart || !cap.st_ready) {
            if (cap.failed) {
                if (!warned)
                    g_warning ("audio: cannot capture %s: %s (audio-reactive wallpapers stay "
                               "silent; retrying every 5 s)", dev, cap.why ? cap.why : "?");
                warned = TRUE;
                set_status ("error: %s: %s", dev, cap.why ? cap.why : "?");
            }
        } else {
            warned = FALSE;
            gint64 since = g_get_monotonic_time ();
            /* sleeps in poll() until a fragment arrives (nothing arrives
             * while the server has the device suspended), the server
             * says that its settings changed, or walpe_audio_unref wakes
             * the loop */
            while (wanted () && !cap.failed && !cap.restart)
                if (pa_mainloop_iterate (ml, 1, NULL) < 0)
                    cap.failed = TRUE;
            if (cap.failed) {
                g_warning ("audio: capture lost: %s", cap.why ? cap.why : "?");
                set_status ("error: %s", cap.why ? cap.why : "?");
            }
            if (g_get_monotonic_time () - since > 10 * G_TIME_SPAN_SECOND)
                restarts = 0;
        }
        g_mutex_lock (&lock);
        cap_ml = NULL;
        g_mutex_unlock (&lock);
        gboolean failed = cap.failed, restart = cap.restart && !cap.failed;
        if (st) {
            pa_stream_set_read_callback (st, NULL, NULL);
            pa_stream_set_state_callback (st, NULL, NULL);
            pa_stream_set_moved_callback (st, NULL, NULL);
            pa_stream_disconnect (st);
            pa_stream_unref (st);
        }
        if (ctx) {
            pa_context_set_subscribe_callback (ctx, NULL, NULL);
            pa_context_set_state_callback (ctx, NULL, NULL);
            pa_context_disconnect (ctx);
            pa_context_unref (ctx);
        }
        if (ml)
            pa_mainloop_free (ml);
        g_free (cap.why);
        g_free (cap.source);
        g_free (cap.sink);
        g_free (cap.def_sink);
        if (restart && ++restarts > CAP_RESTARTS) {
            g_warning ("audio: the capture stream was made %d times in a row and ended each time: "
                       "waiting 5 s", restarts);
            set_status ("error: %s: the server ends the stream again and again", dev);
            restarts = 0;
            restart = FALSE;
            failed = TRUE;
        }
        if (failed || restart) {
            /* consumers read silence until the next stream delivers */
            g_mutex_lock (&lock);
            have_data = FALSE;
            cur_zero = TRUE;
            memset (&cur, 0, sizeof (cur));
            g_mutex_unlock (&lock);
            if (failed)
                retry_wait ();
        } else {
            capture_ended ();
        }
    }
    return NULL;
}
#endif

void walpe_audio_ref (void)
{
#ifdef HAVE_CAPTURE
    g_mutex_lock (&lock);
    refs++;
    if (!thread_running) {
        thread_running = TRUE;
        g_thread_unref (g_thread_new ("walpe-audio", capture_thread, NULL)); /* detached */
    }
    g_cond_broadcast (&cond);
    g_mutex_unlock (&lock);
#endif
}

void walpe_audio_unref (void)
{
#ifdef HAVE_CAPTURE
    g_mutex_lock (&lock);
    if (refs > 0)
        refs--;
    g_cond_broadcast (&cond);
    if (refs == 0 && cap_ml)
        pa_mainloop_wakeup (cap_ml);    /* the capture loop sees it at once */
    g_mutex_unlock (&lock);
#endif
}

gboolean walpe_audio_get (WalpeAudioSpectrum *out)
{
    g_mutex_lock (&lock);
    gboolean ok = have_data;
    /* a spectrum that is not silence and has not been renewed: the stream
     * delivers nothing any more (output device suspended) */
    if (ok && !cur_zero && g_get_monotonic_time () - cur.stamp_us > STALE_US) {
        gint64 stamp = cur.stamp_us;
        memset (&cur, 0, sizeof (cur));
        cur.stamp_us = stamp;
        cur_zero = TRUE;
    }
    if (ok)
        *out = cur;
    else
        memset (out, 0, sizeof (*out));
    g_mutex_unlock (&lock);
    return ok;
}

char *walpe_audio_status (void)
{
#ifdef HAVE_CAPTURE
    g_mutex_lock (&lock);
    char *s = g_strdup (refs > 0 && status_msg ? status_msg : "off");
    g_mutex_unlock (&lock);
    return s;
#else
    return g_strdup ("disabled (built without libpulse-simple)");
#endif
}
