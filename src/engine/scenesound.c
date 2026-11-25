/* scenesound.c - sound layers of scenes, see scenesound.h.
 *
 * Shared state and who may touch it:
 *  - the settings (allowed, enabled, muted, master volume, null device) are
 *    atomic integers: written by the daemon's main thread, read anywhere;
 *    so is the count of open outputs (n_streams, stream_take): changed by
 *    players' threads, which take no lock for it;
 *  - the registry of groups (which scene instance of a wallpaper is heard)
 *    and every field of a group: reg_lock;
 *  - a player's play state, volume and statistics: the player's own lock.
 *    What a player was created with (files, mode, name) never changes.
 * Lock order: reg_lock, then a player's lock. A player's thread takes its
 * own lock only, for a few instructions at a time: never while it decodes,
 * writes to the sound server or sleeps (and, asked from inside a prepare
 * function whether to stop, holding none: reg_lock and the other players'
 * locks, walpe_sound_prepare_stop). The thread holds a reference to
 * its player and is detached: whoever frees a group only sets `quit`. */
#include "walpe-build-config.h"
#include "scenesound.h"
#include "modules.h"
#include "common/util.h"   /* walpe_log_str */
#include <math.h>
#include <string.h>
#ifdef HAVE_PULSE
#include <pulse/simple.h>      /* (the type's name only: scenesound_pulse.c) */
#include <pulse/error.h>
#include "scenesound_server.h"
#endif

enum { ST_STOPPED, ST_PLAYING, ST_PAUSED };

struct WalpeSound {
    gint refs;           /* atomic: the group's reference and the thread's */
    /* fixed at creation */
    char **files;
    int nfiles, mode;
    float mintime, maxtime;
    char *name;          /* printable, short: log lines and the stream's name */
    /* the rest: lock */
    void (*prepare) (const char *arg);   /* walpe_sound_set_prepare, NULL = none */
    char *prepare_arg;
    GMutex lock;
    GCond cond;
    gboolean has_thread;
    int state;           /* ST_* */
    gboolean hold;       /* the layer is invisible */
    gboolean gate;       /* the group is heard (walpe_sound_group_speaking) */
    gboolean quit;       /* the group was freed: the thread ends */
    gboolean rewind;     /* stop (): let the file go, start again at the next play */
    gboolean restart;    /* walpe_sound_restart_at (): the first file again, from restart_s */
    double restart_s;
    double period;       /* walpe_sound_set_period (), 0 = none */
    gboolean broken;     /* none of its files can be played: never tried again */
    float volume;
    gint wp_q16;         /* atomic: the wallpaper's own volume, 16.16 (the group's) */
    WalpeSoundStats st;
};

struct WalpeSoundGroup {
    char *key;
    int sw;              /* the wallpaper's own switch: -1 none, 0 off, 1 on */
    gboolean active;     /* the scene is shown and not paused */
    gboolean speaking;   /* its players' gates are open */
    gint vol_q16;        /* the wallpaper's own volume, 16.16 (65536 until set) */
    GPtrArray *players;  /* WalpeSound, one reference each */
};

/* ---- settings (atomic) ---- */
static gint allowed;
static gint enabled;
static gint muted;
static gint master_q16 = 65536;   /* the user's volume, 16.16 */
static gint force_null;           /* 0 no, 1 null at real pace, 2 null fast */
static gint n_threads;            /* player threads alive */
static gint n_real;               /* streams open on the sound server */
static gint n_nowhere;            /* streams that play into nothing for want of a server */
static gint n_streams;            /* outputs open, on the server or null (WALPE_SOUND_MAX_STREAMS) */
static gint cap_said;             /* that limit was reached and reported (once) */
static gpointer test_decoder;    /* atomic: a WalpeSoundDecoder of a test */

static GMutex reg_lock;
static GPtrArray *groups;         /* reg_lock: WalpeSoundGroup, in the order they were made */
static GMutex err_lock;           /* (taken alone, by a player's thread and the status line) */
static char *server_error;        /* err_lock: why the last stream that failed has no output */

/* the environment is read once, by the first thread that makes a group */
static struct { gboolean null_out; char *sink; } env;

static gpointer env_read (gpointer unused)
{
    (void) unused;
    const char *n = g_getenv ("WALPE_SOUND_NULL");
    const char *k = g_getenv ("WALPE_SOUND_SINK");
    env.null_out = n && *n && strcmp (n, "0") != 0;
    env.sink = k && *k ? g_strdup (k) : NULL;
    return NULL;
}

static void env_once (void)
{
    static GOnce once = G_ONCE_INIT;
    g_once (&once, env_read, NULL);
}

static void regroup_locked (void);

void walpe_sound_allow (gboolean allow)
{
    g_atomic_int_set (&allowed, allow ? 1 : 0);
}

gboolean walpe_sound_allowed (void)
{
    return g_atomic_int_get (&allowed) != 0;
}

void walpe_sound_set_enabled (gboolean on)
{
    g_mutex_lock (&reg_lock);
    g_atomic_int_set (&enabled, on ? 1 : 0);
    regroup_locked ();
    g_mutex_unlock (&reg_lock);
}

gboolean walpe_sound_enabled (void)
{
    return g_atomic_int_get (&enabled) != 0;
}

void walpe_sound_set_master (double volume)
{
    if (!isfinite (volume))
        return;
    g_mutex_lock (&reg_lock);
    g_atomic_int_set (&master_q16, (gint) (CLAMP (volume, 0.0, 1.0) * 65536.0));
    regroup_locked ();   /* volume 0 is silence: nothing runs for it */
    g_mutex_unlock (&reg_lock);
}

void walpe_sound_set_muted (gboolean m)
{
    g_mutex_lock (&reg_lock);
    g_atomic_int_set (&muted, m ? 1 : 0);
    regroup_locked ();
    g_mutex_unlock (&reg_lock);
}

void walpe_sound_use_null (gboolean fast)
{
    g_atomic_int_set (&force_null, fast ? 2 : 1);
}

int walpe_sound_threads (void)
{
    return g_atomic_int_get (&n_threads);
}

void walpe_sound_set_decoder (const WalpeSoundDecoder *dec)
{
    g_atomic_pointer_set (&test_decoder, (gpointer) dec);
}

int walpe_sound_switch_parse (const char *value)
{
    static const char *const yes[] = { "on", "true", "1", "yes" };
    static const char *const no[] = { "off", "false", "0", "no" };
    for (guint i = 0; value && i < G_N_ELEMENTS (yes); i++) {
        if (!g_ascii_strcasecmp (value, yes[i]))
            return 1;
        if (!g_ascii_strcasecmp (value, no[i]))
            return 0;
    }
    return -1;
}

gboolean walpe_sound_name_ok (const char *file)
{
    if (!file || !*file || strlen (file) > 1024 || g_path_is_absolute (file) || file[0] == '\\' ||
        strchr (file, ':'))
        return FALSE;
    for (const guchar *p = (const guchar *) file; *p; p++)
        if (*p < 0x20 || *p == 0x7f)
            return FALSE;
    char **parts = g_strsplit_set (file, "/\\", -1);
    gboolean ok = TRUE;
    for (int i = 0; parts[i]; i++)
        ok &= strcmp (parts[i], "..") != 0;
    g_strfreev (parts);
    return ok;
}

/* a layer's name as it may appear in a log line and as a stream's name:
 * valid UTF-8, printable, short (the name is the wallpaper's to choose) */
static char *label_of (const char *name)
{
    GString *g = g_string_new (NULL);
    for (const char *p = name ? name : ""; *p && g->len < 48; p = g_utf8_next_char (p)) {
        gunichar c = g_utf8_get_char_validated (p, -1);
        if (c == (gunichar) -1 || c == (gunichar) -2)
            break;
        g_string_append_unichar (g, g_unichar_isprint (c) ? c : '?');
    }
    if (!g->len)
        g_string_assign (g, "sound");
    return g_string_free (g, FALSE);
}

/* ---- WAV ---- */
typedef struct {
    FILE *f;
    int rate, channels, bits, fmt;   /* fmt 1 = PCM, 3 = float */
    guint64 left;                    /* bytes of data left */
} Wav;

static guint32 le32 (const guint8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (guint32) p[3] << 24; }
static guint16 le16 (const guint8 *p) { return (guint16) (p[0] | p[1] << 8); }

/* f is at the start of the file; it belongs to the Wav on success */
static Wav *wav_open (FILE *f, const char **err)
{
    guint8 h[12];
    *err = "WAV: unsupported format or no data";
    if (fread (h, 1, 12, f) != 12 || memcmp (h, "RIFF", 4) || memcmp (h + 8, "WAVE", 4)) {
        *err = "not a WAV file";
        return NULL;
    }
    Wav *w = g_new0 (Wav, 1);
    w->f = f;
    /* chunks: "fmt " has to come before "data"; a file made of thousands
     * of other chunks is not walked to its end */
    for (int guard = 0; guard < 64; guard++) {
        guint8 c[8];
        if (fread (c, 1, 8, f) != 8)
            break;
        guint32 len = le32 (c + 4);
        if (!memcmp (c, "fmt ", 4)) {
            guint8 b[40] = { 0 };
            guint32 n = MIN (len, (guint32) sizeof b);
            if (len < 16 || fread (b, 1, n, f) != n)
                break;
            w->fmt = le16 (b);
            w->channels = le16 (b + 2);
            w->rate = (int) MIN (le32 (b + 4), (guint32) G_MAXINT);
            w->bits = le16 (b + 14);
            if (w->fmt == 0xFFFE && n >= 26)
                w->fmt = le16 (b + 24);   /* WAVE_FORMAT_EXTENSIBLE: the sub-format */
            if (fseeko (f, (off_t) (len - n) + (off_t) (len & 1), SEEK_CUR))
                break;
        } else if (!memcmp (c, "data", 4)) {
            w->left = len;
            gboolean ok = w->channels >= 1 && w->channels <= WALPE_SOUND_MAX_CHANNELS &&
                          w->rate >= WALPE_SOUND_MIN_RATE && w->rate <= WALPE_SOUND_MAX_RATE &&
                          ((w->fmt == 1 && (w->bits == 8 || w->bits == 16 || w->bits == 24 || w->bits == 32)) ||
                           (w->fmt == 3 && w->bits == 32));
            if (ok)
                return w;
            if (w->fmt && (w->rate < WALPE_SOUND_MIN_RATE || w->rate > WALPE_SOUND_MAX_RATE))
                *err = "WAV: sample rate outside 8000..192000 Hz";
            else if (w->fmt && (w->channels < 1 || w->channels > WALPE_SOUND_MAX_CHANNELS))
                *err = "WAV: channel count outside 1..8";
            break;
        } else if (fseeko (f, (off_t) len + (off_t) (len & 1), SEEK_CUR)) {
            break;
        }
    }
    g_free (w);
    return NULL;
}

/* interleaved 16-bit, at most two channels (the first two of the file) */
static int wav_read (Wav *w, gint16 *out, int max_frames)
{
    const int bps = w->bits / 8, fsz = bps * w->channels, oc = MIN (w->channels, 2);
    guint8 buf[4096];
    int done = 0;
    while (done < max_frames) {
        guint64 want = MIN ((guint64) (max_frames - done), MIN (w->left / (guint64) fsz, sizeof buf / (gsize) fsz));
        if (!want)
            break;
        int got = (int) fread (buf, (gsize) fsz, (gsize) want, w->f);
        if (got <= 0) {
            w->left = 0;   /* shorter than its header says */
            break;
        }
        w->left -= (guint64) got * (guint64) fsz;
        for (int i = 0; i < got; i++) {
            for (int c = 0; c < oc; c++) {
                const guint8 *p = buf + i * fsz + c * bps;
                int v;
                if (w->fmt == 3) {
                    float fl;
                    memcpy (&fl, p, 4);
                    v = isfinite (fl) ? (int) lrintf (CLAMP (fl, -1.0f, 1.0f) * 32767.0f) : 0;
                } else if (bps == 1) {
                    v = ((int) p[0] - 128) * 256;
                } else {
                    v = (gint16) le16 (p + bps - 2);   /* the two top bytes */
                }
                out[(done + i) * oc + c] = (gint16) v;
            }
        }
        done += got;
    }
    return done;
}

/* ---- one file being played ---- */
typedef struct {
    Wav *wav;
    const WalpeSoundDecoder *mod;
    gpointer dec;
    int rate, channels;
} Src;

/* walpe-video.so's decoders, when the build has the module */
static const WalpeSoundDecoder *module_decoder (void)
{
    const WalpeSoundDecoder *t = g_atomic_pointer_get (&test_decoder);
    if (t)
        return t;
    const WalpeVideoModule *m = walpe_video_module (TRUE);
    return m && m->sound.open && m->sound.read && m->sound.close ? &m->sound : NULL;
}

/* FALSE: *err says why (g_free) */
static gboolean src_open (Src *src, const char *path, char **err)
{
    memset (src, 0, sizeof *src);
    const char *why = NULL;
    FILE *f = walpe_sound_fopen (path, &why);
    guint8 magic[4] = { 0 };
    if (!f || fread (magic, 1, 4, f) != 4) {
        if (f)
            fclose (f);
        *err = g_strdup (why ? why : "too short for a sound file");
        return FALSE;
    }
    if (!memcmp (magic, "RIFF", 4)) {
        rewind (f);
        src->wav = wav_open (f, &why);
        if (!src->wav) {
            fclose (f);
            *err = g_strdup (why);
            return FALSE;
        }
        src->rate = src->wav->rate;
        src->channels = MIN (src->wav->channels, 2);
        return TRUE;
    }
    fclose (f);
    src->mod = module_decoder ();
    if (!src->mod) {
        *err = g_strdup ("no decoder (walpe-video.so with FFmpeg is needed for MP3, FLAC and Ogg)");
        return FALSE;
    }
    src->dec = src->mod->open (path, &src->rate, &src->channels, err);
    if (!src->dec) {
        if (!*err)
            *err = g_strdup ("the decoder refuses the file");
        return FALSE;
    }
    /* the module checks the same; a decoder is not taken at its word */
    if (src->rate < WALPE_SOUND_MIN_RATE || src->rate > WALPE_SOUND_MAX_RATE || src->channels < 1 ||
        src->channels > 2) {
        src->mod->close (src->dec);
        src->dec = NULL;
        *err = g_strdup ("sample rate outside 8000..192000 Hz or no channels");
        return FALSE;
    }
    return TRUE;
}

static int src_read (Src *src, gint16 *out, int max_frames)
{
    int n = src->wav ? wav_read (src->wav, out, max_frames) : src->mod->read (src->dec, out, max_frames);
    return MIN (n, max_frames);
}

/* The next `frames` frames are passed over; returns how many (fewer: the
 * file ended). A decoder that can go to a place does (sounddec.c); from any
 * other source they are read and dropped, into buf (WALPE_SOUND_BLOCK frames). */
static gint64 src_skip (Src *src, gint16 *buf, gint64 frames)
{
    if (!src->wav && src->mod->skip) {
        const gint64 n = src->mod->skip (src->dec, frames);
        return CLAMP (n, 0, frames);    /* (a decoder is not taken at its word) */
    }
    gint64 done = 0;
    while (done < frames) {
        int n = src_read (src, buf, (int) MIN ((gint64) WALPE_SOUND_BLOCK, frames - done));
        if (n <= 0)
            break;
        done += n;
    }
    return done;
}

static void src_close (Src *src)
{
    if (src->wav) {
        fclose (src->wav->f);
        g_free (src->wav);
    } else if (src->dec) {
        src->mod->close (src->dec);
    }
    memset (src, 0, sizeof *src);
}

/* ---- output ---- */
typedef struct {
#ifdef HAVE_PULSE
    pa_simple *pa;
#endif
    int rate, channels;   /* 0 = closed */
    gboolean null_out;    /* plays into nothing */
    gboolean fast;        /* ... as fast as it decodes */
    gboolean nowhere;     /* ... because no server took the stream (counted in n_nowhere) */
    gboolean said;        /* that was reported */
    gboolean slot;        /* holds one of the process's streams (stream_take): from
                           * before the player opens a file until out_close */
    gboolean drained;     /* the server has played all the stream was given (out_drain) */
    gboolean redrained;   /* the stream was drained (answered) and has taken less than ... */
    gint64 redrained_n;   /* ... AFTER_DRAIN_S of sound since: so many frames */
    gint64 next_us;       /* null device: when the next block is due */
    gint64 retry_us;      /* nowhere: ask the server again then */
    gint64 stall_us;      /* when a stream of this player last stalled (out_write); 0 = none did */
    gboolean stall_said;  /* ... and that its successor stalled too was reported */
} Out;

#define SERVER_RETRY_US (5 * G_USEC_PER_SEC)
/* a stream that stalls so soon after the one it replaced: not the stream */
#define STALL_AGAIN_US (30 * G_USEC_PER_SEC)
/* ... unless it had been drained and stalls with the first sound after
 * that (less than this much taken since the answer): that is the stream */
#define AFTER_DRAIN_S 1

/* ---- the process's streams ----
 * Every player with an output has a connection of its own to the sound
 * server, and a call into the server takes as long as the server lets it:
 * up to the limits of scenesound_pulse.c (seconds; they had none, and a
 * thread whose server did not answer one call stayed in it for good). Two
 * things follow, and one count answers both:
 *  - a server takes a limited number of connections from all programs
 *    together (PulseAudio: 64). Two wallpapers with 32 sounding layers each
 *    left none for anything else;
 *  - a player whose thread is waiting in the server cannot be told to quit.
 *    Its group is freed without it (the thread is detached), and every
 *    load of a wallpaper added up to 32 such threads.
 * A player takes one of WALPE_SOUND_MAX_STREAMS slots before it opens a
 * file and gives it back when its output is closed (out_close, after the
 * last call into the server has returned). A blocked thread keeps its slot,
 * which is the point: a server that takes no data holds at most that many
 * threads, however often wallpapers are loaded; all other players wait
 * here, see `quit` and end. The null device counts like a stream (it stands
 * in for one, and may become one when the server comes back).
 * An atomic: a player's thread takes no lock for it - never the registry's. */
static gboolean stream_take (const char *name)
{
    for (;;) {
        const gint n = g_atomic_int_get (&n_streams);
        if (n >= WALPE_SOUND_MAX_STREAMS) {
            if (g_atomic_int_compare_and_exchange (&cap_said, 0, 1))
                g_message ("sound: %d layers have an output at the same time, which is the limit: layer %s (and "
                           "any further one) waits until one of them falls silent - said once",
                           WALPE_SOUND_MAX_STREAMS, walpe_log_str (name));
            return FALSE;
        }
        if (g_atomic_int_compare_and_exchange (&n_streams, n, n + 1))
            return TRUE;
    }
}

static void stream_give (void)
{
    g_atomic_int_add (&n_streams, -1);
}

#ifdef HAVE_PULSE
/* Why the last stream that has no output has none: the reason of the last
 * failure, for the status line, which shows it while streams play into
 * nothing. It is kept when another player's stream opens: one text serves
 * all players, and a success elsewhere cleared it for those still without
 * an output (status: "no sound server (?)" with 10 of 16 streams waiting);
 * a stream that was lost in a write did not set it at all. */
static void server_error_set (const char *why)
{
    g_mutex_lock (&err_lock);
    g_free (server_error);
    server_error = g_strdup (why);
    g_mutex_unlock (&err_lock);
}

/* The five calls into the sound server: scenesound_pulse.c (libpulse, which
 * the audio capture links anyway; each call with a time limit), or a test's
 * stand-ins (walpe_sound_set_server_calls). */
static struct {
    gsize once;
    gboolean ok;
    pa_simple *(*new_stream) (const char *, const char *, pa_stream_direction_t, const char *, const char *,
                              const pa_sample_spec *, const pa_channel_map *, const pa_buffer_attr *, int *);
    void (*free_stream) (pa_simple *);
    int (*write) (pa_simple *, const void *, size_t, int *);
    int (*drain) (pa_simple *, int *);
    int (*flush) (pa_simple *, int *);
} Pas;

static void server_calls_set (const WalpeSoundServerCalls *calls)
{
    Pas.new_stream = calls->new_stream;
    Pas.free_stream = calls->free_stream;
    Pas.write = calls->write;
    Pas.drain = calls->drain;
    Pas.flush = calls->flush;
    Pas.ok = TRUE;
}

static gboolean pas_load (void)
{
    if (g_once_init_enter (&Pas.once)) {
        server_calls_set (walpe_sound_pulse_calls ());
        g_once_init_leave (&Pas.once, 1);
    }
    return Pas.ok;
}

void walpe_sound_set_server_calls (const WalpeSoundServerCalls *calls)
{
    if (g_once_init_enter (&Pas.once)) {
        server_calls_set (calls);
        g_once_init_leave (&Pas.once, 1);
    }
}

/* One stream is opened at a time, and a connection the server refused is
 * asked for again a few times before the layer is left to play into nothing.
 * Every player has a connection of its own, and the players of a scene ask
 * for theirs in the same instant: when the scene is loaded, and again after
 * every silence of more than LINGER_US (a pause, a covered monitor, mute).
 * A sound server takes new connections from a short queue (the backlog
 * of its listening socket), and a connect () that finds the queue full
 * fails at once - "Connection refused" from a server that is there and
 * well.
 * Measured with a running PulseAudio 16.1, ten starts each: of 16 layers, 4
 * to 10 were refused in seven of the starts; of 12, 2 to 6 in four; of 8, 1
 * or 2 in three. A refused layer played into nothing for SERVER_RETRY_US:
 * five seconds of a scene without some of its sound, after every start.
 * So the players take turns: with one connection on its way at a time the
 * queue never holds more than that one (the layers of a scene get their
 * streams a few milliseconds apart instead of together). Should the server
 * still refuse one (a queue full of other clients), that player asks
 * again, in its turn: up to CONNECT_AGAIN times, 10 ms apart.
 * A player that has no output yet waits while the turns go round. Only
 * when the player whose turn it is has had it for CONNECT_TURN_US - the
 * server has stopped answering it - do those behind it go on as before
 * (into nothing), and they ask for their turn again CONNECT_TURN_US later,
 * not after SERVER_RETRY_US: the server did not refuse them. The limit is
 * on the one turn, not on the time a player spends in the queue: counted
 * from when a player began to wait, 16 layers behind a server that takes
 * 16 ms or more for a stream (a busy machine, a sink that has to wake up
 * first) left the last of them without sound for five seconds, which is
 * what the turns are there to end.
 * A player that plays into nothing and asks again (out_write) does give up
 * after CONNECT_TURN_US in the queue, whoever is served: with its attempts
 * that stays below the half second by which a player on the null device
 * may be late and still keep its place in time. It asks again as soon. */
#define CONNECT_TURN_US (G_USEC_PER_SEC / 4)
#define CONNECT_AGAIN 4
#define CONNECT_AGAIN_US 10000
static GMutex turn_lock;          /* (taken alone, by a player's thread, for a few instructions) */
static GCond turn_cond;
static gboolean turn_taken;       /* turn_lock: a player is opening its stream */
static guint turn_serial;         /* turn_lock: counts the turns */
static gint64 turn_since;         /* turn_lock: when the turn that is taken began */

/* give_up: the time (monotonic) after which the caller does not wait any
 * longer, 0 = as long as the turns go round */
static gboolean connect_turn_take (gint64 give_up)
{
    g_mutex_lock (&turn_lock);
    while (turn_taken) {
        const guint serial = turn_serial;
        gint64 until = turn_since + CONNECT_TURN_US;
        if (give_up && give_up < until)
            until = give_up;
        while (turn_taken && turn_serial == serial && g_cond_wait_until (&turn_cond, &turn_lock, until))
            ;
        if (turn_taken && (turn_serial == serial || (give_up && g_get_monotonic_time () >= give_up)))
            break;      /* the same turn and its time is over, or the caller's is */
    }
    const gboolean mine = !turn_taken;
    if (mine) {
        turn_taken = TRUE;
        turn_serial++;
        turn_since = g_get_monotonic_time ();
    }
    g_mutex_unlock (&turn_lock);
    return mine;
}

static void connect_turn_give (void)
{
    g_mutex_lock (&turn_lock);
    turn_taken = FALSE;
    g_cond_signal (&turn_cond);
    g_mutex_unlock (&turn_lock);
}

/* In the caller's turn. The first attempt, then while the server refuses
 * up to CONNECT_AGAIN more, 10 ms apart. */
static pa_simple *connect_turn_open (const pa_sample_spec *ss, const pa_buffer_attr *ba, const char *name,
                                     int *e)
{
    pa_simple *pa = Pas.new_stream (NULL, "walpe", PA_STREAM_PLAYBACK, env.sink, name, ss, NULL, ba, e);
    for (int i = 0; i < CONNECT_AGAIN && !pa && *e == PA_ERR_CONNECTIONREFUSED; i++) {
        g_usleep (CONNECT_AGAIN_US);
        pa = Pas.new_stream (NULL, "walpe", PA_STREAM_PLAYBACK, env.sink, name, ss, NULL, ba, e);
    }
    return pa;
}

/* TRUE: the stream is open on the sound server. FALSE: o->retry_us says
 * when to ask again. */
static gboolean out_connect (Out *o, const char *name)
{
    /* (the samples are gint16 in memory: the machine's byte order) */
    pa_sample_spec ss = { .format = PA_SAMPLE_S16NE, .rate = (uint32_t) o->rate, .channels = (uint8_t) o->channels };
    /* 200 ms in the server: a change of the volume is heard quickly, and a
     * thread that is late by a frame or two does not underrun */
    pa_buffer_attr ba = { .maxlength = (uint32_t) -1, .tlength = (uint32_t) (o->rate * o->channels * 2 / 5),
                          .prebuf = (uint32_t) -1, .minreq = (uint32_t) -1, .fragsize = (uint32_t) -1 };
    int e = 0;
    const gboolean lib = pas_load ();
    /* (nowhere: the player is on the null device and keeps time there) */
    const gboolean busy = lib && !connect_turn_take (o->nowhere ? g_get_monotonic_time () + CONNECT_TURN_US : 0);
    const char *why = "no calls into a sound server";
    if (busy) {
        why = "the sound server is busy with another stream";
    } else if (lib) {
        o->pa = connect_turn_open (&ss, &ba, name, &e);
        connect_turn_give ();
        why = pa_strerror (e);
    }
    if (o->pa) {
        g_atomic_int_inc (&n_real);
        if (o->said)
            g_message ("sound: %s: the sound server is there, heard from now on", walpe_log_str (name));
        o->said = FALSE;
        /* the first stream this process got: the log of a report about
         * silence shows that the output was opened, and on what (once; a
         * server that refuses has had its lines below from the start) */
        static gint opened_said;
        if (g_atomic_int_compare_and_exchange (&opened_said, 0, 1))
            g_message ("sound: output open: a stream on the sound server (libpulse: PulseAudio or "
                       "PipeWire's pipewire-pulse), %s, %d Hz, %d channel(s); first layer: %s",
                       env.sink ? "the sink of WALPE_SOUND_SINK" : "its default sink", o->rate, o->channels,
                       walpe_log_str (name));
        return TRUE;
    }
    /* (the reason of a layer that only waits behind another one does not
     * replace what the server said to that one) */
    if (!busy || !o->said)
        server_error_set (why);
    if (!o->said && busy)
        g_message ("sound: %s: no audio output yet (%s): playing into nothing until it is this layer's turn",
                   walpe_log_str (name), why);
    else if (!o->said)
        g_message ("sound: %s: no audio output (%s): playing into nothing, asking again every %d s",
                   walpe_log_str (name), why, (int) (SERVER_RETRY_US / G_USEC_PER_SEC));
    o->said = TRUE;
    o->retry_us = g_get_monotonic_time () + (busy ? CONNECT_TURN_US : SERVER_RETRY_US);
    return FALSE;
}
#endif

/* nowhere <-> real, with the count the status line reads */
static void out_set_nowhere (Out *o, gboolean nowhere)
{
    if (o->nowhere == nowhere)
        return;
    o->nowhere = nowhere;
    if (nowhere)
        g_atomic_int_inc (&n_nowhere);
    else
        g_atomic_int_add (&n_nowhere, -1);
}

/* how: OUT_DRAIN = what the server still holds is played to its end first
 * (not asked for again when out_drain has just had the answer); OUT_FLUSH =
 * it is thrown away; OUT_DROP = the server is asked nothing: the stream did
 * not answer the last call in time */
enum { OUT_DROP, OUT_FLUSH, OUT_DRAIN };
static void out_close (Out *o, int how)
{
#ifdef HAVE_PULSE
    if (o->pa) {    /* (set by out_connect only) */
        if (how == OUT_DRAIN && !o->drained)
            Pas.drain (o->pa, NULL);
        else if (how == OUT_FLUSH)
            Pas.flush (o->pa, NULL);
        Pas.free_stream (o->pa);
        o->pa = NULL;
        g_atomic_int_add (&n_real, -1);
    }
#else
    (void) how;
#endif
    out_set_nowhere (o, FALSE);
    o->rate = o->channels = 0;
    o->null_out = FALSE;
    o->drained = FALSE;
    o->redrained = FALSE;
    o->next_us = o->retry_us = 0;
    if (o->slot) {
        o->slot = FALSE;
        stream_give ();   /* (last: nothing of this player is inside the server now) */
    }
}

/* What was written is played now, although nothing follows it for a while
 * and the stream stays open. The server starts a stream when `prebuf` bytes
 * are in its buffer - here the same as the 200 ms of out_connect - and
 * arms that again after every underrun (pulse/def.h). A sound shorter than
 * that, written and then left alone, is not played before more follows it
 * or the stream is drained: a "single" click was heard when its player
 * closed the stream, two seconds later (LINGER_US), or glued to the front
 * of the next sound. Waits until the server has played it (at most the
 * 200 ms plus the sound).
 * A server that does not answer the drain within its time limit
 * (scenesound_pulse.c) has a stream that will not be right again -
 * PipeWire's pipewire-pulse 1.0.5 does that to a stream now and then, and
 * takes no more data into it either: the output is closed without another
 * word to the server, and the player opens a new one when it has
 * something to play. (The call had no limit and its failure was not looked
 * at: the thread stayed inside it, and the wallpaper was silent from that
 * pause, mute or covered monitor on.) */
static void out_drain (Out *o)
{
#ifdef HAVE_PULSE
    if (o->pa && !o->drained) {
        int e = 0;
        if (Pas.drain (o->pa, &e) == 0) {
            o->drained = o->redrained = TRUE;
            o->redrained_n = 0;
        } else {
            out_close (o, OUT_DROP);
        }
    }
#else
    (void) o;
#endif
}

static void out_open (Out *o, int rate, int channels, const char *name)
{
    if (o->rate == rate && o->channels == channels)
        return;   /* the next file of a loop: the same stream, no gap */
    {
        /* (another format: another stream, the same slot) */
        const gboolean slot = o->slot;
        o->slot = FALSE;
        out_close (o, OUT_DRAIN);
        o->slot = slot;
    }
    o->rate = rate;
    o->channels = channels;
    int forced = g_atomic_int_get (&force_null);
    o->fast = forced == 2;
    o->null_out = forced != 0 || env.null_out;
#ifdef HAVE_PULSE
    if (!o->null_out && !out_connect (o, name)) {
        o->null_out = TRUE;
        out_set_nowhere (o, TRUE);
    }
#else
    (void) name;
    o->null_out = TRUE;
#endif
}

static void out_write (Out *o, const gint16 *buf, int frames, const char *name)
{
#ifdef HAVE_PULSE
    if (o->nowhere && g_get_monotonic_time () >= o->retry_us) {
        if (out_connect (o, name)) {
            o->null_out = FALSE;
            out_set_nowhere (o, FALSE);
        }
    }
    if (o->pa) {
        int e = 0;
        if (Pas.write (o->pa, buf, (size_t) frames * (size_t) o->channels * 2, &e) == 0) {
            o->drained = FALSE;
            if (o->redrained && (o->redrained_n += frames) >= (gint64) o->rate * AFTER_DRAIN_S)
                o->redrained = FALSE;
            return;
        }
        /* A stream the server stopped taking data into (the time limit of
         * the write is over) is asked for again with the next block: the
         * server is there, it is this stream that stalled - after a drain
         * on pipewire-pulse 1.0.5, see out_drain. Should the server itself
         * have stopped, the new stream is not given and the usual wait
         * begins. A new stream that stalls as well (within
         * STALL_AGAIN_US): it is not the stream - a sink that is suspended
         * takes nothing from any - and the player goes on into nothing and
         * asks every SERVER_RETRY_US like after any other loss, which is
         * said once for as long as that lasts.
         * Not so a stream that played, was drained at a pause (answered)
         * and stalls with the first sound after it: that is the fault of
         * this stream once more, whenever the one before it stalled. It
         * was taken for "the new stream stalls too": two resumes that met
         * the fault within STALL_AGAIN_US - one resume in about thirty
         * does on pipewire-pulse 1.0.5, and a wallpaper under windows is
         * paused and resumed every few seconds - and the second one was
         * silent for SERVER_RETRY_US (seen 1.9 s after the first, with a
         * server that took the next stream at once). A sink that is
         * suspended is still found out: the stream that follows a drained
         * one there has not been drained when it stalls. */
        const gint64 now = g_get_monotonic_time ();
        const gboolean stalled = e == PA_ERR_TIMEOUT;
        const gboolean again = stalled && !o->redrained && o->stall_us && now - o->stall_us < STALL_AGAIN_US;
        if (!stalled)
            g_message ("sound: %s: output lost (%s): playing into nothing, asking again every %d s",
                       walpe_log_str (name), pa_strerror (e), (int) (SERVER_RETRY_US / G_USEC_PER_SEC));
        else if (!again)
            g_message ("sound: %s: the sound server took no more of the stream (%s): a new stream is asked for",
                       walpe_log_str (name), pa_strerror (e));
        else if (!o->stall_said)
            g_message ("sound: %s: the sound server takes nothing of the new stream either (%s; a suspended sink "
                       "does that): playing into nothing, asking again every %d s",
                       walpe_log_str (name), pa_strerror (e), (int) (SERVER_RETRY_US / G_USEC_PER_SEC));
        if (stalled) {
            o->stall_said = again;
            o->stall_us = now;
        }
        server_error_set (pa_strerror (e));
        Pas.free_stream (o->pa);
        o->pa = NULL;
        g_atomic_int_add (&n_real, -1);
        o->said = !stalled;     /* (a stalled stream: what there is to say was said above) */
        o->null_out = TRUE;
        o->drained = FALSE;
        o->redrained = FALSE;
        out_set_nowhere (o, TRUE);
        o->next_us = 0;
        o->retry_us = now + (stalled && !again ? 0 : SERVER_RETRY_US);
    }
#else
    (void) buf;
    (void) name;
#endif
    if (!o->fast) {
        /* the pace of real output */
        gint64 now = g_get_monotonic_time ();
        if (!o->next_us || o->next_us < now - 500000)
            o->next_us = now;
        o->next_us += (gint64) frames * G_USEC_PER_SEC / MAX (o->rate, 1);
        if (o->next_us > now)
            g_usleep ((gulong) (o->next_us - now));
    }
}

/* ---- volume ---- */

/* buf times the gain, which moves from k0 to k1 (16.16) over the first
 * `ramp` frames: a change of the volume does not step inside a wave.
 * Returns the largest sample. */
static int scale_block (gint16 *buf, int frames, int ch, gint64 k0, gint64 k1, int ramp)
{
    int peak = 0;
    ramp = CLAMP (ramp, 1, MAX (frames, 1));
    for (int i = 0; i < frames; i++) {
        gint64 k = i < ramp ? k0 + (k1 - k0) * (i + 1) / ramp : k1;
        for (int c = 0; c < ch; c++) {
            gint64 v = (gint64) buf[i * ch + c] * k / 65536;
            v = CLAMP (v, -32768, 32767);
            buf[i * ch + c] = (gint16) v;
            peak = MAX (peak, (int) (v < 0 ? -v : v));
        }
    }
    return MIN (peak, 32767);
}

/* ---- the thread of one player ---- */
static void sound_unref (WalpeSound *s)
{
    if (!g_atomic_int_dec_and_test (&s->refs))
        return;
    g_mutex_clear (&s->lock);
    g_cond_clear (&s->cond);
    g_strfreev (s->files);
    g_free (s->name);
    g_free (s->prepare_arg);
    g_free (s);
}

static gboolean may_run_locked (const WalpeSound *s)
{
    return s->state == ST_PLAYING && !s->hold && s->gate && !s->broken;
}

enum { ACT_RUN, ACT_PARK, ACT_REWIND, ACT_QUIT, ACT_RESTART };

/* What the thread does next. It sleeps here while there is nothing to do;
 * with an open output (out_open) it comes back at once with ACT_PARK, so
 * that the caller can end the output before it sleeps. */
static int next_action (WalpeSound *s, gboolean out_is_open, double *restart_s)
{
    int act;
    g_mutex_lock (&s->lock);
    for (;;) {
        /* (a player none of whose files can be played will never have
         * anything to put out: its thread ends with that, it does not wait
         * for the group to be freed - a video wallpaper without a sound
         * track kept one sleeping thread for as long as it was shown) */
        if (s->quit || s->broken)
            act = ACT_QUIT;
        else if (s->rewind)
            act = ACT_REWIND;
        else if (may_run_locked (s) && s->restart) {
            /* (only a player that is heard: until then the place is kept) */
            act = ACT_RESTART;
            s->restart = FALSE;
            *restart_s = s->restart_s;
        } else if (may_run_locked (s))
            act = ACT_RUN;
        else if (out_is_open)
            act = ACT_PARK;
        else {
            s->st.running = FALSE;
            g_cond_wait (&s->cond, &s->lock);
            continue;
        }
        break;
    }
    if (act == ACT_REWIND)
        s->rewind = FALSE;
    /* (`running` begins with the first block that is written - a player
     * that may run can still be waiting for a stream - and with an open
     * output it ends with the fade-out the caller plays, see there) */
    if (act != ACT_RUN && !out_is_open)
        s->st.running = FALSE;
    g_mutex_unlock (&s->lock);
    return act;
}

/* the file at index idx cannot be played (now or ever): said once, and the
 * file is not opened again. TRUE: that was the player's last good file. */
static gboolean file_failed (WalpeSound *s, int idx, const char *msg, guint64 *bad)
{
    char *bn = g_path_get_basename (s->files[idx]);
    g_message ("sound: layer %s: %s is not played: %s", s->name, walpe_log_str (bn), msg ? msg : "?");
    g_free (bn);
    *bad |= G_GUINT64_CONSTANT (1) << idx;
    gboolean all = TRUE;
    for (int i = 0; i < s->nfiles; i++)
        all &= (*bad >> i & 1) != 0;
    g_mutex_lock (&s->lock);
    g_snprintf (s->st.error, sizeof s->st.error, "%s", msg ? msg : "?");
    if (all) {
        s->broken = TRUE;
        s->state = ST_STOPPED;
    }
    g_mutex_unlock (&s->lock);
    return all;
}

/* the next file to play: a random one, or the one after idx; -1 = none is left */
static int pick_file (const WalpeSound *s, GRand *rng, int idx, guint64 bad)
{
    int good = 0;
    for (int i = 0; i < s->nfiles; i++)
        good += !(bad >> i & 1);
    if (!good)
        return -1;
    if (s->mode == WALPE_SOUND_RANDOM) {
        int k = g_rand_int_range (rng, 0, good);
        for (int i = 0; i < s->nfiles; i++)
            if (!(bad >> i & 1) && k-- == 0)
                return i;
    }
    for (int step = 1; step <= s->nfiles; step++) {
        int i = (idx + step) % s->nfiles;
        if (!(bad >> i & 1))
            return i;
    }
    return -1;
}

/* sleeps until `until` (monotonic us) unless the player is told to do
 * something else first */
static void wait_until (WalpeSound *s, gint64 until)
{
    g_mutex_lock (&s->lock);
    while (!s->quit && !s->rewind && s->state == ST_PLAYING && g_get_monotonic_time () < until)
        g_cond_wait_until (&s->cond, &s->lock, until);
    g_mutex_unlock (&s->lock);
}

/* The player fell silent with its stream open. A stream that is needed
 * again within two seconds is kept - a script that blinks a layer, a pause
 * of a moment, the next "single" sound of a music player: closing and
 * opening one is a round trip to the sound server each time. TRUE: the
 * player may run again. */
#define LINGER_US (2 * G_USEC_PER_SEC)
static gboolean linger (WalpeSound *s)
{
    const gint64 until = g_get_monotonic_time () + LINGER_US;
    g_mutex_lock (&s->lock);
    while (!s->quit && !s->rewind && !may_run_locked (s) && g_get_monotonic_time () < until)
        g_cond_wait_until (&s->cond, &s->lock, until);
    gboolean again = !s->quit && !s->rewind && may_run_locked (s);
    g_mutex_unlock (&s->lock);
    return again;
}

/* No stream is free (stream_take). Until one may be - asked again four
 * times a second - or until the player is told something else. */
static void stream_wait (WalpeSound *s)
{
    const gint64 until = g_get_monotonic_time () + G_USEC_PER_SEC / 4;
    g_mutex_lock (&s->lock);
    s->st.running = FALSE;
    while (!s->quit && !s->rewind && may_run_locked (s) && g_get_monotonic_time () < until)
        g_cond_wait_until (&s->cond, &s->lock, until);
    g_mutex_unlock (&s->lock);
}

/* the number of sample frames a FLAC file says it has in its header
 * (STREAMINFO, the first block of every FLAC file), 0 = no FLAC file, or
 * one that does not say */
static gint64 flac_frames (const char *path)
{
    guint8 b[42];
    gint64 n = 0;
    FILE *f = fopen (path, "rb");
    if (f && fread (b, 1, sizeof b, f) == sizeof b && !memcmp (b, "fLaC", 4) && (b[4] & 0x7f) == 0)
        n = (gint64) (b[21] & 0x0f) << 32 | (gint64) b[22] << 24 | b[23] << 16 | b[24] << 8 | b[25];
    if (f)
        fclose (f);
    return n;
}

/* the player whose prepare function the thread is in (walpe_sound_prepare_stop) */
static GPrivate preparing;

gboolean walpe_sound_prepare_stop (void)
{
    WalpeSound *s = g_private_get (&preparing);
    if (!s)
        return FALSE;
    g_mutex_lock (&s->lock);
    gboolean quit = s->quit;
    void (*const prepare) (const char *) = s->prepare;
    char *arg = quit && prepare ? g_strdup (s->prepare_arg) : NULL;
    g_mutex_unlock (&s->lock);
    if (!arg)
        return quit;
    /* Told to end - but what the function makes is also another player's:
     * the same video on a second monitor, whose player has the same
     * function and argument and comes here next (its group speaks once
     * this one is gone, and its call waits for this one's, import.c). Then
     * the work goes on to its end. It was given up here and begun again
     * there: ffmpeg killed and started from nothing, the other monitor's
     * sound later by all that had been done. The groups in the registry
     * are the live ones (walpe_sound_group_free takes a group out before
     * its players are told), so this ends with the last of them. */
    g_mutex_lock (&reg_lock);
    for (guint i = 0; quit && groups && i < groups->len; i++) {
        const WalpeSoundGroup *g = groups->pdata[i];
        for (guint j = 0; quit && j < g->players->len; j++) {
            WalpeSound *o = g->players->pdata[j];
            if (o == s)
                continue;
            g_mutex_lock (&o->lock);
            if (!o->quit && !o->broken && o->prepare == prepare && !g_strcmp0 (o->prepare_arg, arg))
                quit = FALSE;
            g_mutex_unlock (&o->lock);
        }
    }
    g_mutex_unlock (&reg_lock);
    g_free (arg);
    return quit;
}

static gpointer sound_thread (gpointer data)
{
    WalpeSound *s = data;
    Src src;
    Out out = { 0 };
    gboolean have = FALSE;
    gboolean prepared = FALSE;   /* walpe_sound_set_prepare: its function was called by this thread */
    int idx = -1;
    guint64 bad = 0;          /* files that cannot be played (bit = index) */
    gint64 played = 0;        /* frames of the open file handed out so far */
    gint64 k_prev = -1;       /* gain of the block before, 16.16; -1 = none: the
                               * next block starts at its own gain (a file's first
                               * sample sounds as it was made) */
    double skip_s = 0;        /* walpe_sound_restart_at: the file opened next begins there */
    /* walpe_sound_set_period: a pass is pass_n frames of output (0 = the
     * file's own length); pass_out of them are out; the file has ended and
     * the rest is silence (pass_pad); the file is longer than the pass by
     * more than an encoder's padding and is faded where it is cut (pass_fade) */
    gint64 pass_n = 0, pass_out = 0;
    gboolean pass_pad = FALSE, pass_fade = FALSE;
    GRand *rng = g_rand_new ();
    gint16 buf[WALPE_SOUND_BLOCK * 2];
    memset (&src, 0, sizeof src);
    for (;;) {
        int act = next_action (s, out.rate != 0 || out.slot, &skip_s);
        if (act == ACT_RESTART) {
            /* The first file again, from skip_s, in the stream that is
             * open: 20 ms of what was playing, faded out, and no wait for
             * the stream to play what it holds - the new place follows
             * what was written, as the next block of the file would. */
            int n = have && out.rate && k_prev > 0
                        ? src_read (&src, buf, MIN (WALPE_SOUND_BLOCK, MAX (out.rate / 50, 1))) : 0;
            if (n > 0) {
                scale_block (buf, n, src.channels, k_prev, 0, n);
                out_write (&out, buf, n, s->name);
            }
            if (have)
                src_close (&src);
            have = FALSE;
            idx = -1;
            k_prev = out.rate ? 0 : -1;     /* into a stream that was sounding: faded in */
            g_mutex_lock (&s->lock);
            s->st.frames += MAX (n, 0);
            g_mutex_unlock (&s->lock);
            continue;
        }
        if (act != ACT_RUN) {
            if (out.rate) {
                /* Silence from here on. The wave is not cut where it stands
                 * (a click): 20 ms more of it, faded out. The stream plays
                 * what it holds; unless it is needed again soon it is then
                 * closed - an open stream that gets nothing keeps the
                 * output device awake. */
                /* (only those 20 ms are taken out of the file: a whole block
                 * was read and the rest of it thrown away, so the sound
                 * jumped ahead by up to 2048 frames less 20 ms at every
                 * pause, hide, mute or pause of the scene) */
                /* (of a pass with a length - walpe_sound_set_period - no more
                 * than the pass has left: these frames are part of it, and
                 * a pass that had fewer than 20 ms to go was counted past
                 * its end - the block after the resume then had a negative
                 * number of frames to fill) */
                int fade = MIN (WALPE_SOUND_BLOCK, MAX (out.rate / 50, 1));
                if (pass_n > 0)
                    fade = (int) CLAMP (pass_n - pass_out, 0, (gint64) fade);
                int n = have && k_prev > 0 && fade > 0 && !pass_pad ? src_read (&src, buf, fade) : 0;
                if (n > 0) {
                    played += n;
                    pass_out += n;      /* (they are part of the pass they were taken from) */
                    scale_block (buf, n, src.channels, k_prev, 0, n);
                    out_write (&out, buf, n, s->name);
                }
                out_drain (&out);   /* (heard now, not when the stream is closed) */
                /* the pace of the null device begins anew with the next
                 * block: out_write () lets a late thread catch up for up to
                 * half a second, and a silence shorter than that counted as
                 * "late" - the file ran ahead by the length of every short
                 * pause, as fast as it decodes (a real output never did:
                 * its writes block) */
                out.next_us = 0;
                /* `running` ends together with the last frames counted: it
                 * was cleared in next_action () before the fade was read,
                 * written and added, so whoever saw "not running" and then
                 * read `frames` could still see them grow (the tests do
                 * exactly that, with a poll every millisecond) */
                g_mutex_lock (&s->lock);
                s->st.frames += MAX (n, 0);
                s->st.running = FALSE;
                g_mutex_unlock (&s->lock);
                k_prev = have ? 0 : -1;   /* going on inside a file later: faded in */
                if (act == ACT_PARK && linger (s))
                    continue;
            }
            out_close (&out, OUT_DRAIN);   /* (also gives back a slot that never got its output:
                                       * none of the player's files could be opened) */
            if (act == ACT_QUIT)
                break;
            if (act == ACT_REWIND) {
                if (have)
                    src_close (&src);
                have = FALSE;
                idx = -1;
                k_prev = -1;
            }
            continue;
        }
        /* one of the process's streams, before a file is opened for it */
        if (!out.slot) {
            if (!stream_take (s->name)) {
                stream_wait (s);
                continue;
            }
            out.slot = TRUE;
        }
        if (!have) {
            idx = pick_file (s, rng, idx, bad);
            if (idx < 0)
                continue;   /* (broken: next_action parks the thread) */
            if (!prepared) {
                /* the layer is about to be heard for the first time: its
                 * files are put where their names say */
                prepared = TRUE;
                g_mutex_lock (&s->lock);
                void (*prepare) (const char *) = s->prepare;
                char *arg = prepare ? g_strdup (s->prepare_arg) : NULL;
                g_mutex_unlock (&s->lock);
                if (prepare) {
                    g_private_set (&preparing, s);
                    prepare (arg);
                    g_private_set (&preparing, NULL);
                }
                g_free (arg);
                /* told to end while that ran (walpe_sound_prepare_stop): no
                 * file is looked for, and none is said to be missing, for
                 * a wallpaper that is gone */
                g_mutex_lock (&s->lock);
                const gboolean gone = s->quit;
                g_mutex_unlock (&s->lock);
                if (gone)
                    continue;
            }
            char *err = NULL;
            if (!src_open (&src, s->files[idx], &err)) {
                file_failed (s, idx, err, &bad);
                g_free (err);
                continue;
            }
            have = TRUE;
            played = 0;
            g_mutex_lock (&s->lock);
            const double period = s->mode == WALPE_SOUND_LOOP && s->nfiles == 1 ? s->period : 0;
            g_mutex_unlock (&s->lock);
            pass_n = period > 0 ? MAX ((gint64) llround (period * src.rate), 1) : 0;
            pass_out = 0;
            pass_pad = FALSE;
            pass_fade = pass_n > 0 &&
                        flac_frames (s->files[idx]) > pass_n + (gint64) (WALPE_SOUND_PERIOD_PAD_S * src.rate);
            if (pass_n > 0 && skip_s > 0)   /* a place in the pass */
                skip_s = fmod (skip_s, (double) pass_n / src.rate);
            if (skip_s > 0) {
                /* A place inside the file. The sound in front of it is not
                 * decoded where the decoder can do without (src_skip: the
                 * FLAC track of a video): that took a second of one CPU for
                 * every twenty minutes of track in front of the place, in
                 * which nothing was heard and the picture went on - so the
                 * track came in late by that much, was told its place
                 * again (video.c), and from some minutes into a long video
                 * on did nothing else. */
                gint64 want = (gint64) (MIN (skip_s, (double) WALPE_SOUND_MAX_SECONDS) * src.rate);
                const gint64 passed = src_skip (&src, buf, want);
                want -= passed;
                played += passed;
                if (pass_n > 0) {
                    /* (behind the file's end: in the silence that follows it) */
                    pass_out = MIN (played + MAX (want, 0), pass_n - 1);
                    pass_pad = want > 0;
                }
                if (k_prev < 0)
                    k_prev = 0;     /* not the file's first sample: faded in */
            }
            skip_s = 0;
            g_mutex_lock (&s->lock);
            s->st.rate = src.rate;
            s->st.channels = src.channels;
            s->st.opens++;
            s->st.pos = pass_n > 0 ? pass_out : played;
            g_mutex_unlock (&s->lock);
        }
        /* The output for it, whether the file was opened just now or is one
         * the player stopped in: a player that was silent for longer than
         * LINGER_US has closed its output and kept its file. This call
         * stood inside the block above, so such a player went on to write
         * into a closed output: nothing was heard again after any pause,
         * hidden layer or mute of more than two seconds, and out_write ()
         * paced the block at "rate 0" - a sleep of 2048 seconds, in which
         * the thread does not see `quit` either. (Returns at once while
         * the open output fits.) */
        out_open (&out, src.rate, src.channels, s->name);
        int n;
        if (pass_n > 0 && pass_out >= pass_n) {
            /* the pass ended in the frames a pause faded out with: the next
             * one begins, as after a pass's last block */
            g_mutex_lock (&s->lock);
            s->st.plays++;
            g_mutex_unlock (&s->lock);
            src_close (&src);
            have = FALSE;
            continue;
        }
        if (pass_n > 0) {
            /* a pass of exactly pass_n frames: the file, then silence if it
             * is shorter; cut if it is longer */
            const int want = (int) MIN ((gint64) WALPE_SOUND_BLOCK, pass_n - pass_out);
            n = pass_pad ? 0 : src_read (&src, buf, want);
            if (n <= 0 && (played > 0 || pass_pad)) {
                pass_pad = TRUE;
                n = want;
                memset (buf, 0, (gsize) n * src.channels * sizeof (gint16));
            } else if (n > 0 && pass_fade) {
                const gint64 fade = MAX (src.rate / 200, 1);
                for (int i = 0; i < n; i++) {
                    const gint64 left = pass_n - (pass_out + i);
                    if (left <= fade)
                        for (int c = 0; c < src.channels; c++)
                            buf[i * src.channels + c] = (gint16) (buf[i * src.channels + c] * (left - 1) / fade);
                }
            }
        } else {
            n = src_read (&src, buf, WALPE_SOUND_BLOCK);
        }
        if (n > 0 && played + n > (gint64) src.rate * WALPE_SOUND_MAX_SECONDS) {
            g_message ("sound: layer %s: a file is longer than %d hours: it ends there", s->name,
                       WALPE_SOUND_MAX_SECONDS / 3600);
            n = 0;
        }
        if (n <= 0) {
            /* the end of the file (or a decoding failure: the same) */
            const int file_rate = src.rate;
            src_close (&src);
            have = FALSE;
            skip_s = 0;
            if (!played) {
                /* not one frame of sound: such a file would be opened again
                 * and again as fast as the disk allows */
                file_failed (s, idx, "no sound in the file", &bad);
                continue;
            }
            gint64 now = g_get_monotonic_time (), until = 0;
            g_mutex_lock (&s->lock);
            s->st.plays++;
            if (s->mode == WALPE_SOUND_SINGLE) {
                if (s->state == ST_PLAYING)
                    s->state = ST_STOPPED;
                idx = -1;
            } else if (s->mode == WALPE_SOUND_RANDOM && !out.fast) {
                float lo = MAX (s->mintime, 0.0f), hi = MAX (s->maxtime, lo);
                until = now + (gint64) (g_rand_double_range (rng, lo, hi + 1e-6) * 1e6);
            }
            g_mutex_unlock (&s->lock);
            /* a file of a few milliseconds: not more than 50 of them a second */
            gint64 audio_us = played * G_USEC_PER_SEC / MAX (file_rate, 1);
            if (audio_us < 20000)
                until = MAX (until, now + 20000 - audio_us);
            if (until > now + 500000 && out.rate) {
                /* the pause of random mode: without a stream */
                out_close (&out, OUT_DRAIN);
                k_prev = -1;
            } else if (until) {
                /* a shorter pause, with the stream: the file's sound is
                 * played before it, not after it together with the next */
                out_drain (&out);
            }
            if (until) {
                /* The pause is the time between two sounds: it begins when the
                 * sound has been played, not when its last block was handed
                 * over. A real output still holds up to 200 ms at that moment
                 * (out_connect; a short sound in whole), and the two calls
                 * above wait until it has played them: `until` was counted
                 * from before that wait, so every pause was shorter by what
                 * the stream still held - a sound of 0.1 s with a pause of
                 * 0.2 s came every 0.2 s, with a pause of 1 s every 1.0 s
                 * instead of every 0.3 s and 1.1 s. (The null device has
                 * "played" a block when its write returns: no time passes
                 * above, nothing changes there.) */
                until += g_get_monotonic_time () - now;
                wait_until (s, until);
                /* the pace of the null device begins anew after the pause, as
                 * it does after a silence (see there): with the stream kept,
                 * its clock stood at the end of the file before, the pause
                 * counted as time the thread was late, and the next file was
                 * put out as fast as it decodes until that was made up */
                out.next_us = 0;
            }
            continue;
        }
        played += pass_pad ? 0 : n;
        pass_out += n;
        /* volume: the layer's times the user's */
        g_mutex_lock (&s->lock);
        float vol = CLAMP (s->volume, 0.0f, 1.0f);
        g_mutex_unlock (&s->lock);
        /* (... times the wallpaper's own, which is 1 unless the user set it) */
        gint64 k = (gint64) (vol * (float) g_atomic_int_get (&master_q16)) * g_atomic_int_get (&s->wp_q16) / 65536;
        int peak = scale_block (buf, n, src.channels, k_prev < 0 ? k : k_prev, k, src.rate / 100);
        k_prev = k;
        out_write (&out, buf, n, s->name);
        g_mutex_lock (&s->lock);
        s->st.frames += n;
        if (!s->restart)    /* (else the place asked for stands, walpe_sound_restart_at) */
            s->st.pos = pass_n > 0 ? pass_out : played;
        s->st.peak = MAX (s->st.peak, peak);
        s->st.null_out = out.null_out;
        s->st.running = TRUE;
        if (pass_n > 0 && pass_out >= pass_n)
            s->st.plays++;
        g_mutex_unlock (&s->lock);
        if (pass_n > 0 && pass_out >= pass_n) {
            /* the pass is over: the file again from its beginning, in the
             * stream as it is - the next block follows this one (k_prev stays) */
            src_close (&src);
            have = FALSE;
        }
    }
    if (have)
        src_close (&src);
    out_close (&out, OUT_FLUSH);
    g_rand_free (rng);
    g_mutex_lock (&s->lock);
    s->has_thread = FALSE;
    s->st.running = FALSE;
    g_mutex_unlock (&s->lock);
    sound_unref (s);
    g_atomic_int_add (&n_threads, -1);
    return NULL;
}

/* the thread is made when the player has something to put out for the
 * first time, and then stays until the group is freed */
static void kick_locked (WalpeSound *s)
{
    if (!s->has_thread && !s->quit && may_run_locked (s)) {
        GError *e = NULL;
        s->has_thread = TRUE;
        g_atomic_int_inc (&s->refs);
        g_atomic_int_inc (&n_threads);
        GThread *t = g_thread_try_new ("walpe-sound", sound_thread, s, &e);
        if (t) {
            g_thread_unref (t);   /* detached */
        } else {
            g_warning ("sound: layer %s: no thread (%s): silent", s->name, e ? e->message : "?");
            g_clear_error (&e);
            s->has_thread = FALSE;
            s->broken = TRUE;
            s->state = ST_STOPPED;
            g_atomic_int_add (&s->refs, -1);   /* (the caller holds one: never the last) */
            g_atomic_int_add (&n_threads, -1);
        }
    }
    g_cond_broadcast (&s->cond);
}

/* ---- groups ---- */
static gboolean group_on_locked (const WalpeSoundGroup *g)
{
    return g->sw >= 0 ? g->sw != 0 : g_atomic_int_get (&enabled) != 0;
}

static gboolean group_wants_locked (const WalpeSoundGroup *g)
{
    return g->active && group_on_locked (g) && !g_atomic_int_get (&muted) &&
           g_atomic_int_get (&master_q16) > 0 && g->vol_q16 > 0;
}

static void group_speak_locked (WalpeSoundGroup *g, gboolean speak)
{
    if (g->speaking == speak)
        return;
    g->speaking = speak;
    for (guint i = 0; i < g->players->len; i++) {
        WalpeSound *s = g->players->pdata[i];
        g_mutex_lock (&s->lock);
        s->gate = speak;
        kick_locked (s);
        g_mutex_unlock (&s->lock);
    }
}

/* reg_lock: who is heard. A group that speaks keeps the word while it
 * wants it; a wallpaper nobody speaks for is given to the first group in
 * the list that wants it. */
static void regroup_locked (void)
{
    if (!groups)
        return;
    for (guint i = 0; i < groups->len; i++) {
        WalpeSoundGroup *g = groups->pdata[i];
        if (g->speaking && !group_wants_locked (g))
            group_speak_locked (g, FALSE);
    }
    for (guint i = 0; i < groups->len; i++) {
        WalpeSoundGroup *g = groups->pdata[i];
        if (g->speaking || !group_wants_locked (g))
            continue;
        gboolean taken = FALSE;
        for (guint j = 0; j < groups->len && !taken; j++) {
            WalpeSoundGroup *o = groups->pdata[j];
            taken = o != g && o->speaking && !strcmp (o->key, g->key);
        }
        if (!taken)
            group_speak_locked (g, TRUE);
    }
}

WalpeSoundGroup *walpe_sound_group_new (const char *key, int wallpaper_switch)
{
    if (!walpe_sound_allowed ())
        return NULL;
    env_once ();
    WalpeSoundGroup *g = g_new0 (WalpeSoundGroup, 1);
    /* one spelling for one folder ("/lib/1/" and "/lib//1" are the same
     * wallpaper; as text, links are not followed) */
    g->key = key && *key ? g_canonicalize_filename (key, NULL) : g_strdup ("");
    g->sw = wallpaper_switch < 0 ? -1 : wallpaper_switch != 0;
    g->vol_q16 = 65536;
    g->players = g_ptr_array_new ();
    g_mutex_lock (&reg_lock);
    if (!groups)
        groups = g_ptr_array_new ();
    g_ptr_array_add (groups, g);
    g_mutex_unlock (&reg_lock);
    return g;
}

void walpe_sound_group_set_active (WalpeSoundGroup *g, gboolean active)
{
    if (!g)
        return;
    g_mutex_lock (&reg_lock);
    if (g->active != !!active) {
        g->active = !!active;
        regroup_locked ();
    }
    g_mutex_unlock (&reg_lock);
}

/* reg_lock */
static void group_volume_locked (WalpeSoundGroup *g, double volume)
{
    g->vol_q16 = isfinite (volume) ? (gint) (CLAMP (volume, 0.0, 1.0) * 65536.0) : 65536;
    for (guint i = 0; i < g->players->len; i++)
        g_atomic_int_set (&((WalpeSound *) g->players->pdata[i])->wp_q16, g->vol_q16);
}

void walpe_sound_group_set_volume (WalpeSoundGroup *g, double volume)
{
    if (!g)
        return;
    g_mutex_lock (&reg_lock);
    group_volume_locked (g, volume);
    regroup_locked ();   /* volume 0 is silence: nothing runs for it */
    g_mutex_unlock (&reg_lock);
}

guint walpe_sound_wallpaper_set (const char *key, int wallpaper_switch, double volume)
{
    /* (the spelling walpe_sound_group_new gives a key) */
    char *k = key && *key ? g_canonicalize_filename (key, NULL) : g_strdup ("");
    guint n = 0;
    g_mutex_lock (&reg_lock);
    for (guint i = 0; groups && i < groups->len; i++) {
        WalpeSoundGroup *g = groups->pdata[i];
        if (strcmp (g->key, k))
            continue;
        g->sw = wallpaper_switch < 0 ? -1 : wallpaper_switch != 0;
        group_volume_locked (g, volume);
        n++;
    }
    if (n)
        regroup_locked ();
    g_mutex_unlock (&reg_lock);
    g_free (k);
    return n;
}

double walpe_sound_volume_parse (const char *value)
{
    char *end = NULL;
    double v = value && *value ? g_ascii_strtod (value, &end) : 1.0;
    return end && end != value && !*end && isfinite (v) && v >= 0.0 && v <= 1.0 ? v : 1.0;
}

gboolean walpe_sound_group_enabled (WalpeSoundGroup *g)
{
    if (!g)
        return FALSE;
    g_mutex_lock (&reg_lock);
    gboolean on = group_on_locked (g);
    g_mutex_unlock (&reg_lock);
    return on;
}

gboolean walpe_sound_group_speaking (WalpeSoundGroup *g)
{
    if (!g)
        return FALSE;
    g_mutex_lock (&reg_lock);
    gboolean sp = g->speaking;
    g_mutex_unlock (&reg_lock);
    return sp;
}

void walpe_sound_group_free (WalpeSoundGroup *g)
{
    if (!g)
        return;
    g_mutex_lock (&reg_lock);
    g_ptr_array_remove (groups, g);
    for (guint i = 0; i < g->players->len; i++) {
        WalpeSound *s = g->players->pdata[i];
        g_mutex_lock (&s->lock);
        s->quit = TRUE;
        s->gate = FALSE;
        g_cond_broadcast (&s->cond);
        g_mutex_unlock (&s->lock);
        sound_unref (s);   /* its thread, if it has one, holds the other reference */
    }
    regroup_locked ();     /* another instance of the wallpaper may speak now */
    g_mutex_unlock (&reg_lock);
    g_ptr_array_free (g->players, TRUE);
    g_free (g->key);
    g_free (g);
}

/* ---- players ---- */
WalpeSound *walpe_sound_new (WalpeSoundGroup *g, const WalpeSoundSpec *spec)
{
    if (!g || !spec || !spec->files || spec->nfiles < 1)
        return NULL;
    int named = 0;
    for (int i = 0; i < spec->nfiles && named < WALPE_SOUND_MAX_FILES; i++)
        named += spec->files[i] && *spec->files[i];
    if (!named)
        return NULL;
    WalpeSound *s = g_new0 (WalpeSound, 1);
    s->refs = 1;
    s->files = g_new0 (char *, named + 1);
    for (int i = 0; i < spec->nfiles && s->nfiles < named; i++)
        if (spec->files[i] && *spec->files[i])
            s->files[s->nfiles++] = g_strdup (spec->files[i]);
    s->mode = spec->mode == WALPE_SOUND_RANDOM || spec->mode == WALPE_SOUND_SINGLE ? spec->mode : WALPE_SOUND_LOOP;
    s->mintime = isfinite (spec->mintime) ? CLAMP (spec->mintime, 0.0f, 86400.0f) : 0.0f;
    s->maxtime = isfinite (spec->maxtime) ? CLAMP (spec->maxtime, 0.0f, 86400.0f) : 0.0f;
    s->volume = isfinite (spec->volume) ? CLAMP (spec->volume, 0.0f, 1.0f) : 1.0f;
    s->name = label_of (spec->name);
    g_mutex_init (&s->lock);
    g_cond_init (&s->cond);
    s->state = ST_STOPPED;
    g_mutex_lock (&reg_lock);
    gboolean room = g->players->len < WALPE_SOUND_MAX_LAYERS;
    if (room) {
        s->gate = g->speaking;
        s->wp_q16 = g->vol_q16;
        g_ptr_array_add (g->players, s);
    }
    g_mutex_unlock (&reg_lock);
    if (!room) {
        sound_unref (s);
        return NULL;
    }
    return s;
}

void walpe_sound_set_prepare (WalpeSound *s, void (*prepare) (const char *arg), const char *arg)
{
    if (!s)
        return;
    g_mutex_lock (&s->lock);
    g_free (s->prepare_arg);
    s->prepare = prepare;
    s->prepare_arg = prepare ? g_strdup (arg) : NULL;
    g_mutex_unlock (&s->lock);
}

void walpe_sound_play (WalpeSound *s)
{
    if (!s)
        return;
    g_mutex_lock (&s->lock);
    if (!s->broken)
        s->state = ST_PLAYING;
    kick_locked (s);
    g_mutex_unlock (&s->lock);
}

void walpe_sound_restart_at (WalpeSound *s, double seconds)
{
    if (!s || !isfinite (seconds))
        return;
    g_mutex_lock (&s->lock);
    if (!s->broken) {
        s->state = ST_PLAYING;
        s->rewind = FALSE;
        s->restart = TRUE;
        s->restart_s = MAX (seconds, 0.0);
        /* what a reader takes for the player's place from now on, also
         * before the thread got there */
        s->st.pos = s->st.rate > 0 ? (gint64) (s->restart_s * s->st.rate) : 0;
        g_cond_broadcast (&s->cond);
    }
    kick_locked (s);
    g_mutex_unlock (&s->lock);
}

void walpe_sound_set_period (WalpeSound *s, double seconds)
{
    if (!s || !isfinite (seconds))
        return;
    g_mutex_lock (&s->lock);
    s->period = seconds > 0 ? MIN (seconds, (double) WALPE_SOUND_MAX_SECONDS) : 0;
    g_mutex_unlock (&s->lock);
}

void walpe_sound_pause (WalpeSound *s)
{
    if (!s)
        return;
    g_mutex_lock (&s->lock);
    if (s->state == ST_PLAYING)
        s->state = ST_PAUSED;
    g_cond_broadcast (&s->cond);
    g_mutex_unlock (&s->lock);
}

void walpe_sound_stop (WalpeSound *s)
{
    if (!s)
        return;
    g_mutex_lock (&s->lock);
    s->state = ST_STOPPED;
    s->rewind = s->has_thread;
    g_cond_broadcast (&s->cond);
    g_mutex_unlock (&s->lock);
}

gboolean walpe_sound_is_playing (WalpeSound *s)
{
    if (!s)
        return FALSE;
    g_mutex_lock (&s->lock);
    gboolean p = s->state == ST_PLAYING;
    g_mutex_unlock (&s->lock);
    return p;
}

void walpe_sound_set_volume (WalpeSound *s, float volume)
{
    if (!s || !isfinite (volume))
        return;
    g_mutex_lock (&s->lock);
    s->volume = CLAMP (volume, 0.0f, 1.0f);
    g_mutex_unlock (&s->lock);
}

float walpe_sound_get_volume (WalpeSound *s)
{
    if (!s)
        return 0.0f;
    g_mutex_lock (&s->lock);
    float v = s->volume;
    g_mutex_unlock (&s->lock);
    return v;
}

void walpe_sound_hold (WalpeSound *s, gboolean hold)
{
    if (!s)
        return;
    g_mutex_lock (&s->lock);
    s->hold = !!hold;
    kick_locked (s);
    g_mutex_unlock (&s->lock);
}

void walpe_sound_stats (WalpeSound *s, WalpeSoundStats *out)
{
    memset (out, 0, sizeof *out);
    if (!s)
        return;
    g_mutex_lock (&s->lock);
    *out = s->st;
    g_mutex_unlock (&s->lock);
}

void walpe_sound_pages_add (WalpeSoundPages *w, int sw, double own_volume, gboolean paused,
                            gboolean setting_on, gboolean mute, double volume)
{
    if (!w)
        return;
    if (!(sw > 0 || (sw < 0 && setting_on))) {
        w->silent += sw == 0 && setting_on;     /* (off for all: the line says so) */
        return;
    }
    if (mute || !(volume > 0.0))
        return;                                 /* (the line says so of all wallpaper sound) */
    if (!(own_volume > 0.0)) {
        w->silent++;
        return;
    }
    w->own_on += sw > 0;
    if (paused) {
        w->paused++;
        return;
    }
    const int pct = CLAMP ((int) (MIN (volume, 1.0) * MIN (own_volume, 1.0) * 100.0 + 0.5), 1, 100);
    w->percent = w->heard == 0 ? pct : w->percent == pct ? pct : -1;
    w->heard++;
}

char *walpe_sound_status (void)
{
    return walpe_sound_status_with (NULL);
}

char *walpe_sound_status_with (const WalpeSoundPages *web)
{
    if (!walpe_sound_allowed ())
        return g_strdup ("not available in this program");
    static const WalpeSoundPages none;
    if (!web)
        web = &none;
    int own_on = web->own_on, speaking = 0, held = 0;
    char *why = NULL, *broken_err = NULL;
    g_mutex_lock (&reg_lock);
    for (guint i = 0; groups && i < groups->len; i++) {
        WalpeSoundGroup *g = groups->pdata[i];
        own_on += g->sw == 1;
        speaking += g->speaking;
        /* sound is on for it and it has players, but it is paused (or not
         * shown yet): the one reason for silence the line did not name */
        held += !g->active && g->players->len > 0 && group_on_locked (g);
        if (!broken_err) {
            for (guint j = 0; j < g->players->len; j++) {
                WalpeSound *s = g->players->pdata[j];
                g_mutex_lock (&s->lock);
                if (s->broken && s->st.error[0])
                    broken_err = g_strdup (s->st.error);
                g_mutex_unlock (&s->lock);
                if (broken_err)
                    break;
            }
        }
    }
    g_mutex_unlock (&reg_lock);
    g_mutex_lock (&err_lock);
    why = g_strdup (server_error);
    g_mutex_unlock (&err_lock);
    char *r;
    if (!walpe_sound_enabled () && !own_on)
        r = g_strdup ("off (walpe-ctl set scene_sound=on; for one wallpaper: walpe-ctl setprop walpe_sound=on)");
    else if (g_atomic_int_get (&muted))
        r = g_strdup ("on, muted (walpe-ctl set mute=off)");
    else if (g_atomic_int_get (&master_q16) <= 0)
        r = g_strdup ("on, but the volume setting is 0: silent (walpe-ctl set volume=0.5)");
    else {
        int real = g_atomic_int_get (&n_real), nowhere = g_atomic_int_get (&n_nowhere);
        const char *scope = walpe_sound_enabled () ? "on" : "off, on for a wallpaper of its own";
        /* nothing of the player's own to report: no stream, nothing paused */
        gboolean idle = FALSE;
#ifdef HAVE_PULSE
        if (nowhere > 0)
            r = g_strdup_printf ("%s: %d stream(s) play into nothing, no sound server (%s); %d heard", scope,
                                 nowhere, why ? why : "?", real);
        else if (real > 0)
            r = g_strdup_printf ("%s: %d stream(s) through PulseAudio, %d wallpaper(s) with sound shown",
                                 scope, real, speaking);
        else if (held > 0 && !speaking)
            r = g_strdup_printf ("%s: nothing is playing: %d wallpaper(s) with sound paused - sound stops while its "
                                 "wallpaper is paused (windows cover the monitor, a fullscreen window, the display "
                                 "sleeps, walpe-ctl pause)", scope, held);
        else {
            idle = speaking == 0;
            if (speaking > 0 && broken_err)
                r = g_strdup_printf ("%s: nothing is playing (%d wallpaper(s) with sound shown; error: %s)",
                                     scope, speaking, broken_err);
            else
                r = g_strdup_printf ("%s: nothing is playing (%d wallpaper(s) with sound shown)", scope, speaking);
        }
#else
        (void) real;
        (void) nowhere;
        (void) speaking;
        (void) held;
        r = g_strdup_printf ("%s, but built without libpulse: %s", scope,
                             web->heard + web->paused + web->silent > 0 ? "the sound of scenes and videos cannot be heard"
                                                                        : "nothing can be heard");
#endif
        /* the pages of web wallpapers: WebKit plays them, not this player */
        GString *t = g_string_new (NULL);
        if (web->heard > 0) {
            g_string_printf (t, "%d web wallpaper(s) ", web->heard);
            if (web->percent > 0)
                g_string_append_printf (t, "at %d %%", web->percent);
            else
                g_string_append (t, "at levels of their own");
            g_string_append (t, " (what the page plays, through WebKit: the stream \"WebKit\" in the system's mixer)");
        }
        if (web->paused > 0)
            g_string_append_printf (t, "%s%d web wallpaper(s) paused%s", t->len ? "; " : "", web->paused,
                                    idle && !web->heard
                                        ? " - sound stops while its wallpaper is paused (windows cover the monitor, "
                                          "a fullscreen window, the display sleeps, walpe-ctl pause)"
                                        : ": silent until shown again");
        if (web->silent > 0)
            g_string_append_printf (t, "%s%d web wallpaper(s) silent by their own switch or volume (the rows "
                                    "\"Sound\" and \"Volume\" of their options; walpe-ctl setprop walpe_sound=on "
                                    "walpe_volume=1)", t->len ? "; " : "", web->silent);
        if (t->len) {
            char *all;
            if (idle && web->heard > 0)             /* (something is playing: the page) */
                all = g_strdup_printf ("%s: %s", scope, t->str);
            else if (idle && web->paused > 0)
                all = g_strdup_printf ("%s: nothing is playing: %s", scope, t->str);
            else
                all = g_strdup_printf ("%s; %s", r, t->str);
            g_free (r);
            r = all;
        }
        g_string_free (t, TRUE);
        if (g_atomic_int_get (&n_streams) >= WALPE_SOUND_MAX_STREAMS) {
            char *t2 = g_strdup_printf ("%s; %d layers have an output, the limit: further ones wait", r,
                                        WALPE_SOUND_MAX_STREAMS);
            g_free (r);
            r = t2;
        }
    }
    g_free (why);
    g_free (broken_err);
    return r;
}
