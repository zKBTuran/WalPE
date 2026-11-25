/* scenesound_pulse.c - the five calls scenesound.c makes into the sound
 * server, each with a time limit.
 *
 * They were the functions of libpulse-simple (pa_simple_new, _write,
 * _drain, _flush, _free), and those wait for the server without a limit: a
 * player's thread that was inside one when the server did not answer stayed
 * there. That is not only a server that has stopped. PipeWire's
 * pipewire-pulse 1.0.5 now and then does not answer the drain a player
 * asks for when it falls silent (a pause, a covered monitor, mute), or
 * takes no more data into the stream after it: the thread never came back,
 * its wallpaper stayed silent after the resume - for as long as it was
 * shown - and the thread kept one of the 16 streams of the process.
 *
 * The same five calls on libpulse's asynchronous interface (which the
 * program links anyway), with the stream made exactly as pa_simple_new
 * makes it (the same flags, the caller's buffer sizes, context and stream
 * names): the mainloop thread of the connection tells this file's own
 * condition what happened, and the caller waits for that until a time:
 *   a new stream     SRV_CONNECT_US in all (connection and stream)
 *   write            SRV_WRITE_US without the server taking a byte. After a
 *                    drain it answered, SRV_AFTER_DRAIN_US until it asks
 *                    for more for the first time: the stream is empty, has
 *                    room at once and plays as soon as that is filled -
 *                    the stream pipewire-pulse 1.0.5 leaves standing takes
 *                    what fits (200 ms) and then asks for nothing
 *   drain, flush     SRV_DRAIN_US; a drain also the time the stream says
 *                    it still has to play (a sink with a long buffer)
 * A call that runs out of time fails with PA_ERR_TIMEOUT, and the caller
 * gives the stream up (scenesound.c: a new one with the next block or at
 * the next resume). Freeing a stream asks the server nothing.
 * Only in a build with libpulse. */
#include "walpe-build-config.h"
#ifdef HAVE_PULSE
#include <glib.h>
#include <pulse/pulseaudio.h>
#include "scenesound_server.h"

#define SRV_CONNECT_US (8 * G_USEC_PER_SEC)
#define SRV_WRITE_US (4 * G_USEC_PER_SEC)
#define SRV_AFTER_DRAIN_US (300 * 1000)
#define SRV_DRAIN_US (G_USEC_PER_SEC)
#define SRV_PLAY_MOST_US (10 * G_USEC_PER_SEC)

typedef struct {
    pa_threaded_mainloop *ml;
    pa_context *ctx;
    pa_stream *st;
    size_t frame_bytes;
    GMutex m;
    GCond c;
    gboolean event;     /* m: something happened since the caller last looked */
    int op;             /* m: the operation the caller waits for: 0 running, 1 done, -1 refused */
    gboolean drained;   /* (the caller's thread only) the server answered a drain and has not asked for
                         * more data since */
} Srv;

/* ---- the connection's mainloop thread ---- */
static void srv_event (Srv *s)
{
    g_mutex_lock (&s->m);
    s->event = TRUE;
    g_cond_broadcast (&s->c);
    g_mutex_unlock (&s->m);
}

static void ctx_state_cb (pa_context *c, void *user)
{
    (void) c;
    srv_event (user);
}

static void st_state_cb (pa_stream *p, void *user)
{
    (void) p;
    srv_event (user);
}

static void st_room_cb (pa_stream *p, size_t bytes, void *user)
{
    (void) p;
    (void) bytes;
    srv_event (user);
}

static void st_done_cb (pa_stream *p, int success, void *user)
{
    Srv *s = user;
    (void) p;
    g_mutex_lock (&s->m);
    s->op = success ? 1 : -1;
    s->event = TRUE;
    g_cond_broadcast (&s->c);
    g_mutex_unlock (&s->m);
}

/* ---- the caller's thread ---- */

/* until something happened (TRUE) or the time is over; never with the
 * mainloop's lock held */
static gboolean srv_wait (Srv *s, gint64 until)
{
    g_mutex_lock (&s->m);
    while (!s->event && g_cond_wait_until (&s->c, &s->m, until))
        ;
    const gboolean got = s->event;
    s->event = FALSE;
    g_mutex_unlock (&s->m);
    return got;
}

/* (mainloop locked) 0 while connection and stream are well, else the error */
static int srv_bad (Srv *s)
{
    if (PA_CONTEXT_IS_GOOD (pa_context_get_state (s->ctx)) && s->st &&
        pa_stream_get_state (s->st) == PA_STREAM_READY)
        return 0;
    const int e = pa_context_errno (s->ctx);
    return e ? e : PA_ERR_CONNECTIONTERMINATED;
}

static void pulse_free (pa_simple *p)
{
    Srv *s = (Srv *) p;
    if (!s)
        return;
    /* (as pa_simple_free: the thread ends first - it sits in poll (), not
     * in the server - and what is left needs no lock and asks nothing) */
    if (s->ml)
        pa_threaded_mainloop_stop (s->ml);
    if (s->st)
        pa_stream_unref (s->st);
    if (s->ctx) {
        pa_context_disconnect (s->ctx);
        pa_context_unref (s->ctx);
    }
    if (s->ml)
        pa_threaded_mainloop_free (s->ml);
    g_mutex_clear (&s->m);
    g_cond_clear (&s->c);
    g_free (s);
}

static pa_simple *pulse_new (const char *server, const char *app, pa_stream_direction_t dir, const char *device,
                             const char *stream, const pa_sample_spec *spec, const pa_channel_map *map,
                             const pa_buffer_attr *attr, int *error)
{
    const gint64 until = g_get_monotonic_time () + SRV_CONNECT_US;
    int e = PA_ERR_INTERNAL;
    Srv *s = g_new0 (Srv, 1);
    size_t fs = spec ? pa_frame_size (spec) : 0;
    s->frame_bytes = fs > 0 ? fs : 1;
    g_mutex_init (&s->m);
    g_cond_init (&s->c);
    if (dir != PA_STREAM_PLAYBACK) {
        e = PA_ERR_NOTSUPPORTED;
        goto fail;
    }
    s->ml = pa_threaded_mainloop_new ();
    s->ctx = s->ml ? pa_context_new (pa_threaded_mainloop_get_api (s->ml), app) : NULL;
    if (!s->ctx)
        goto fail;
    pa_context_set_state_callback (s->ctx, ctx_state_cb, s);
    if (pa_context_connect (s->ctx, server, PA_CONTEXT_NOFLAGS, NULL) < 0) {
        e = pa_context_errno (s->ctx);
        goto fail;
    }
    if (pa_threaded_mainloop_start (s->ml) < 0)
        goto fail;
    for (;;) {
        gboolean ready = FALSE;
        e = 0;
        pa_threaded_mainloop_lock (s->ml);
        const pa_context_state_t cs = pa_context_get_state (s->ctx);
        if (cs == PA_CONTEXT_READY && !s->st) {
            /* (the flags of pa_simple_new: the server treats the stream as it did) */
            s->st = pa_stream_new (s->ctx, stream, spec, map);
            if (s->st) {
                pa_stream_set_state_callback (s->st, st_state_cb, s);
                pa_stream_set_write_callback (s->st, st_room_cb, s);
            }
            if (!s->st || pa_stream_connect_playback (s->st, device, attr,
                                                      PA_STREAM_INTERPOLATE_TIMING | PA_STREAM_ADJUST_LATENCY |
                                                      PA_STREAM_AUTO_TIMING_UPDATE, NULL, NULL) < 0)
                e = pa_context_errno (s->ctx) ? pa_context_errno (s->ctx) : PA_ERR_INTERNAL;
        }
        if (!e && !PA_CONTEXT_IS_GOOD (cs))
            e = pa_context_errno (s->ctx) ? pa_context_errno (s->ctx) : PA_ERR_CONNECTIONREFUSED;
        if (!e && s->st) {
            const pa_stream_state_t ss = pa_stream_get_state (s->st);
            if (!PA_STREAM_IS_GOOD (ss))
                e = pa_context_errno (s->ctx) ? pa_context_errno (s->ctx) : PA_ERR_INTERNAL;
            ready = ss == PA_STREAM_READY;
        }
        pa_threaded_mainloop_unlock (s->ml);
        if (e)
            goto fail;
        if (ready)
            return (pa_simple *) s;
        if (!srv_wait (s, until)) {
            e = PA_ERR_TIMEOUT;
            goto fail;
        }
    }
fail:
    if (error)
        *error = e ? e : PA_ERR_INTERNAL;
    pulse_free ((pa_simple *) s);
    return NULL;
}

static int pulse_write (pa_simple *p, const void *data, size_t bytes, int *error)
{
    Srv *s = (Srv *) p;
    const guint8 *d = data;
    gint64 until = g_get_monotonic_time () + (s->drained ? SRV_AFTER_DRAIN_US : SRV_WRITE_US);
    gboolean waited = FALSE;    /* there was no room: what comes now, the server asked for */
    while (bytes > 0) {
        size_t n = 0;
        pa_threaded_mainloop_lock (s->ml);
        int e = srv_bad (s);
        if (!e) {
            n = pa_stream_writable_size (s->st);
            if (n == (size_t) -1) {
                n = 0;
                e = pa_context_errno (s->ctx) ? pa_context_errno (s->ctx) : PA_ERR_INTERNAL;
            } else {
                n = MIN (n, bytes);
                n -= n % s->frame_bytes;
                if (n > 0 && pa_stream_write (s->st, d, n, NULL, 0LL, PA_SEEK_RELATIVE) < 0)
                    e = pa_context_errno (s->ctx) ? pa_context_errno (s->ctx) : PA_ERR_INTERNAL;
            }
        }
        pa_threaded_mainloop_unlock (s->ml);
        if (e) {
            if (error)
                *error = e;
            return -1;
        }
        if (n > 0) {
            d += n;
            bytes -= n;
            if (waited)
                s->drained = FALSE;
            waited = FALSE;
            until = g_get_monotonic_time () + (s->drained ? SRV_AFTER_DRAIN_US : SRV_WRITE_US);
        } else {
            waited = TRUE;
            if (!srv_wait (s, until)) {
                /* the server took nothing for that long: it will not */
                if (error)
                    *error = PA_ERR_TIMEOUT;
                return -1;
            }
        }
    }
    return 0;
}

/* drain (what the stream holds is played, then the server answers) or flush
 * (thrown away) */
static int pulse_op (Srv *s, gboolean drain, int *error)
{
    gint64 until = g_get_monotonic_time () + SRV_DRAIN_US;
    pa_operation *o = NULL;
    g_mutex_lock (&s->m);
    s->op = 0;
    g_mutex_unlock (&s->m);
    pa_threaded_mainloop_lock (s->ml);
    int e = srv_bad (s);
    if (!e) {
        /* (what was written and is not played yet: the answer to a drain
         * comes after it) */
        pa_usec_t left = 0;
        int ahead = 0;
        if (drain && pa_stream_get_latency (s->st, &left, &ahead) == 0 && !ahead)
            until += (gint64) MIN (left, (pa_usec_t) SRV_PLAY_MOST_US);
        o = drain ? pa_stream_drain (s->st, st_done_cb, s) : pa_stream_flush (s->st, st_done_cb, s);
        if (!o)
            e = pa_context_errno (s->ctx) ? pa_context_errno (s->ctx) : PA_ERR_INTERNAL;
    }
    pa_threaded_mainloop_unlock (s->ml);
    while (!e) {
        g_mutex_lock (&s->m);
        const int op = s->op;
        g_mutex_unlock (&s->m);
        if (op) {
            e = op > 0 ? 0 : PA_ERR_INTERNAL;
            break;
        }
        pa_threaded_mainloop_lock (s->ml);
        e = srv_bad (s);
        pa_threaded_mainloop_unlock (s->ml);
        if (!e && !srv_wait (s, until))
            e = PA_ERR_TIMEOUT;
    }
    if (o) {
        pa_threaded_mainloop_lock (s->ml);
        if (e)
            pa_operation_cancel (o);    /* (its answer, should it come, is not waited for) */
        pa_operation_unref (o);
        pa_threaded_mainloop_unlock (s->ml);
    }
    s->drained = drain && !e;
    if (e && error)
        *error = e;
    return e ? -1 : 0;
}

static int pulse_drain (pa_simple *p, int *error)
{
    return pulse_op ((Srv *) p, TRUE, error);
}

static int pulse_flush (pa_simple *p, int *error)
{
    return pulse_op ((Srv *) p, FALSE, error);
}

const WalpeSoundServerCalls *walpe_sound_pulse_calls (void)
{
    static const WalpeSoundServerCalls calls = { pulse_new, pulse_free, pulse_write, pulse_drain, pulse_flush };
    return &calls;
}
#endif
