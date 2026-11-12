/* media.c - MPRIS media integration (see media.h).
 *
 * Threading: all D-Bus work happens on the "walpe-media" thread, which
 * owns a private GMainContext (pushed as its thread-default, so GDBus
 * delivers signals and async replies there). Consumers on render threads
 * and the GTK main thread only touch `pub` under `lock`. Album art is
 * decoded on a GTask worker and published back through the media thread
 * (results of a superseded load are dropped by generation number). */
#include "walpe-build-config.h"
#include "media.h"
#include "util.h"
#include "gifcheck.h"
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <string.h>
#include <math.h>
#ifdef HAVE_SOUP
#include <libsoup/soup.h>
#endif

#define MPRIS_PREFIX "org.mpris.MediaPlayer2."
#define MPRIS_PATH "/org/mpris/MediaPlayer2"
#define MPRIS_PLAYER "org.mpris.MediaPlayer2.Player"
#define THUMB_MAX 640              /* px, longest side */
#define ART_MAX_BYTES (16 << 20)
#define ART_MAX_SIDE 8192      /* px per side of cover art that is decoded at all */
#define ART_HTTP_SECONDS 10    /* a download gets this long in total */
#define GRACE_MS 3000              /* last unref -> unsubscribe */
#define RESYNC_S 5                 /* Position re-query while playing */

/* ------------------------------------------------ published snapshot */

static GMutex lock;
static int refs;
static GThread *thread;
static GMainContext *mctx;
static gint serial;                /* atomic */
static WalpeNowPlaying pub;         /* strings owned; under lock */
static gint64 pub_pos_stamp;       /* monotonic us of pub.position */
static double pub_rate = 1.0;
static char *status_msg;

static void info_init_strings (WalpeNowPlaying *i)
{
    char **f[] = { &i->title, &i->artist, &i->subtitle, &i->album_title, &i->album_artist,
                   &i->genres, &i->content_type };
    for (guint k = 0; k < G_N_ELEMENTS (f); k++)
        if (!*f[k])
            *f[k] = g_strdup ("");
}

void walpe_media_now_clear (WalpeNowPlaying *i)
{
    g_free (i->title);
    g_free (i->artist);
    g_free (i->subtitle);
    g_free (i->album_title);
    g_free (i->album_artist);
    g_free (i->genres);
    g_free (i->content_type);
    g_free (i->thumb_data_url);
    g_free (i->player);
    if (i->thumb)
        g_bytes_unref (i->thumb);
    if (i->prev_thumb)
        g_bytes_unref (i->prev_thumb);
    memset (i, 0, sizeof (*i));
}

guint64 walpe_media_serial (void)
{
    return (guint64) (guint) g_atomic_int_get (&serial);
}

void walpe_media_get (WalpeNowPlaying *o)
{
    memset (o, 0, sizeof (*o));
    g_mutex_lock (&lock);
    *o = pub;
    o->serial = walpe_media_serial ();
    o->title = g_strdup (pub.title);
    o->artist = g_strdup (pub.artist);
    o->subtitle = g_strdup (pub.subtitle);
    o->album_title = g_strdup (pub.album_title);
    o->album_artist = g_strdup (pub.album_artist);
    o->genres = g_strdup (pub.genres);
    o->content_type = g_strdup (pub.content_type);
    o->thumb_data_url = g_strdup (pub.thumb_data_url);
    o->player = g_strdup (pub.player);
    o->thumb = pub.thumb ? g_bytes_ref (pub.thumb) : NULL;
    o->prev_thumb = pub.prev_thumb ? g_bytes_ref (pub.prev_thumb) : NULL;
    if (pub.state == WALPE_MEDIA_PLAYING && pub_pos_stamp) {
        double dt = (double) (g_get_monotonic_time () - pub_pos_stamp) / 1e6;
        o->position = pub.position + dt * pub_rate;
        if (pub.duration > 0 && o->position > pub.duration)
            o->position = pub.duration;
    }
    g_mutex_unlock (&lock);
    info_init_strings (o);
}

char *walpe_media_status (void)
{
    g_mutex_lock (&lock);
    char *s = g_strdup (status_msg ? status_msg : "off");
    g_mutex_unlock (&lock);
    return s;
}

static void set_status (const char *fmt, ...) G_GNUC_PRINTF (1, 2);
static void set_status (const char *fmt, ...)
{
    va_list ap;
    va_start (ap, fmt);
    char *m = g_strdup_vprintf (fmt, ap);
    va_end (ap);
    g_mutex_lock (&lock);
    gboolean same = !g_strcmp0 (m, status_msg);
    g_free (status_msg);
    status_msg = m;
    g_mutex_unlock (&lock);
    if (!same)
        g_message ("media: %s", m);
}

/* ---------------------------------------------------- thumbnail colours */

static double lin (double c)
{
    return c <= 0.04045 ? c / 12.92 : pow ((c + 0.055) / 1.055, 2.4);
}

static double luminance (const float c[3])
{
    return 0.2126 * lin (c[0]) + 0.7152 * lin (c[1]) + 0.0722 * lin (c[2]);
}

static double contrast (const float a[3], const float b[3])
{
    double la = luminance (a), lb = luminance (b);
    return (MAX (la, lb) + 0.05) / (MIN (la, lb) + 0.05);
}

static float cdist (const float a[3], const float b[3])
{
    float d = 0;
    for (int k = 0; k < 3; k++)
        d += (a[k] - b[k]) * (a[k] - b[k]);
    return sqrtf (d);
}

/* Dominant colours of the art: pixels (alpha >= 128, sampled on a <= 64x64
 * grid) are binned 4 bits per channel; primary = mean of the fullest bin,
 * secondary/tertiary = the fullest bins far enough (RGB distance > 0.3,
 * relaxed to 0.15) from the ones already chosen (else a lightened /
 * darkened primary). highContrast = black or white, whichever contrasts
 * more with primary; text = secondary or tertiary when its WCAG contrast
 * with primary is >= 4.5, else highContrast (MediaThumbnailEvent docs). */
void walpe_media_colors (const guint8 *rgba, int w, int h, float primary[3], float secondary[3],
                         float tertiary[3], float text[3], float high_contrast[3])
{
    enum { NB = 4096 };
    guint *cnt = g_new0 (guint, NB);
    double (*sum)[3] = g_malloc0 (sizeof (double) * 3 * NB);
    int step = MAX (1, MAX (w, h) / 64);
    for (int y = 0; y < h; y += step) {
        for (int x = 0; x < w; x += step) {
            const guint8 *p = rgba + ((gsize) y * w + x) * 4;
            if (p[3] < 128)
                continue;
            int b = (p[0] >> 4) << 8 | (p[1] >> 4) << 4 | (p[2] >> 4);
            cnt[b]++;
            for (int k = 0; k < 3; k++)
                sum[b][k] += p[k] / 255.0;
        }
    }
    float pick[3][3];
    int npick = 0;
    for (int pass = 0; pass < 2 && npick < 3; pass++) {
        float minsep = pass ? 0.15f : 0.3f;
        while (npick < 3) {
            int best = -1;
            for (int b = 0; b < NB; b++) {
                if (!cnt[b] || (best >= 0 && cnt[b] <= cnt[best]))
                    continue;
                float c[3] = { (float) (sum[b][0] / cnt[b]), (float) (sum[b][1] / cnt[b]),
                               (float) (sum[b][2] / cnt[b]) };
                gboolean far = TRUE;
                for (int q = 0; q < npick; q++)
                    far &= cdist (c, pick[q]) > minsep;
                if (far)
                    best = b;
            }
            if (best < 0)
                break;
            for (int k = 0; k < 3; k++)
                pick[npick][k] = (float) (sum[best][k] / cnt[best]);
            cnt[best] = 0;
            npick++;
        }
    }
    g_free (cnt);
    g_free (sum);
    if (npick == 0) {
        pick[0][0] = pick[0][1] = pick[0][2] = 0.0f;
        npick = 1;
    }
    for (; npick < 3; npick++) {
        /* flat art: lighter / darker variants of the primary */
        gboolean dark = luminance (pick[0]) < 0.18;
        float f = npick == 1 ? 0.45f : 0.75f;
        for (int k = 0; k < 3; k++)
            pick[npick][k] = dark ? pick[0][k] + (1.0f - pick[0][k]) * f : pick[0][k] * (1.0f - f);
    }
    static const float white[3] = { 1, 1, 1 }, black[3] = { 0, 0, 0 };
    memcpy (primary, pick[0], sizeof (float) * 3);
    memcpy (secondary, pick[1], sizeof (float) * 3);
    memcpy (tertiary, pick[2], sizeof (float) * 3);
    memcpy (high_contrast, contrast (pick[0], white) >= contrast (pick[0], black) ? white : black,
            sizeof (float) * 3);
    if (contrast (pick[1], pick[0]) >= 4.5)
        memcpy (text, pick[1], sizeof (float) * 3);
    else if (contrast (pick[2], pick[0]) >= 4.5)
        memcpy (text, pick[2], sizeof (float) * 3);
    else
        memcpy (text, high_contrast, sizeof (float) * 3);
}

/* -------------------------------------------------------- media thread */

typedef struct {
    char *name;            /* org.mpris.MediaPlayer2.x */
    char *owner;           /* :1.42 */
    int state;
    gint64 last_active;    /* monotonic us: last status or track change */
    gint64 play_start;     /* monotonic us: last transition to Playing */
    char *trackid;         /* mpris:trackid (track identity, with title/url) */
    char *title, *artist, *album, *album_artist, *genres, *art_url, *url;
    double length;         /* s */
    gint64 pos_us, pos_stamp;
    double rate;
} Player;

static GDBusConnection *conn;          /* media thread only from here on */
static guint sub_noc, sub_props, sub_seek;
static GHashTable *players;            /* name -> Player */
static gboolean active;
static guint grace_id, tick_id;
static int tick_n;
static GCancellable *cancel;
static char *cur_art;                  /* art URL of the published thumbnail */
static guint art_gen;
static char *cur_player;
#ifdef HAVE_SOUP
static SoupSession *soup;
#endif

static void player_free (gpointer p)
{
    Player *pl = p;
    g_free (pl->name);
    g_free (pl->owner);
    g_free (pl->title);
    g_free (pl->artist);
    g_free (pl->album);
    g_free (pl->album_artist);
    g_free (pl->genres);
    g_free (pl->art_url);
    g_free (pl->url);
    g_free (pl->trackid);
    g_free (pl);
}

static void bump (guint *field)
{
    (*field)++;
    g_atomic_int_inc (&serial);
}

/* pub fields <- string, bumping props_serial on change (lock held) */
static gboolean set_str (char **dst, const char *v)
{
    if (!g_strcmp0 (*dst, v ? v : ""))
        return FALSE;
    g_free (*dst);
    *dst = g_strdup (v ? v : "");
    return TRUE;
}

static Player *active_player (void)
{
    Player *best = NULL;
    int best_rank = -1;
    GHashTableIter it;
    gpointer k, v;
    if (!players)
        return NULL;
    g_hash_table_iter_init (&it, players);
    while (g_hash_table_iter_next (&it, &k, &v)) {
        Player *p = v;
        /* Playing > Paused > Stopped; among Playing the one that started
         * most recently, otherwise the last real status/track change (a
         * tab re-sending identical Metadata no longer counts as activity) */
        int rank = p->state == WALPE_MEDIA_PLAYING ? 2 : p->state == WALPE_MEDIA_PAUSED ? 1 : 0;
        gint64 key = rank == 2 ? p->play_start : p->last_active;
        gint64 bkey = best ? (best_rank == 2 ? best->play_start : best->last_active) : 0;
        if (rank > best_rank || (rank == best_rank && best && key > bkey)) {
            best = p;
            best_rank = rank;
        }
    }
    /* a stopped player is no media session (WE shows nothing) */
    Player *r = best && best->state != WALPE_MEDIA_STOPPED ? best : NULL;
    static char *logged;   /* media thread only */
    if (g_strcmp0 (logged, r ? r->name : NULL)) {
        g_free (logged);
        logged = g_strdup (r ? r->name : NULL);
        g_debug ("media: active player: %s (%s)", r ? r->name : "none",
                 !r ? "-" : r->state == WALPE_MEDIA_PLAYING ? "playing" : "paused");
    }
    return r;
}

static void load_art (const char *url);
static void tick_update (void);

static void publish (void)
{
    Player *p = active ? active_player () : NULL;
    g_mutex_lock (&lock);
    gboolean en = active && conn != NULL;
    if (pub.enabled != en) {
        pub.enabled = en;
        bump (&pub.status_serial);
    }
    int st = p ? p->state : WALPE_MEDIA_STOPPED;
    if (pub.state != st) {
        pub.state = st;
        bump (&pub.playback_serial);
    }
    gboolean ch = FALSE;
    ch |= set_str (&pub.title, p ? p->title : "");
    ch |= set_str (&pub.artist, p ? p->artist : "");
    ch |= set_str (&pub.subtitle, "");
    ch |= set_str (&pub.album_title, p ? p->album : "");
    ch |= set_str (&pub.album_artist, p ? p->album_artist : "");
    ch |= set_str (&pub.genres, p ? p->genres : "");
    const char *ctype = "";
    if (p) {
        static const char *const vext[] = { ".mp4", ".mkv", ".webm", ".avi", ".mov", ".m4v", NULL };
        ctype = "audio";
        for (int k = 0; p->url && vext[k]; k++)
            if (g_str_has_suffix (p->url, vext[k]))
                ctype = "video";
    }
    ch |= set_str (&pub.content_type, ctype);
    if (ch)
        bump (&pub.props_serial);
    double pos = p ? (double) p->pos_us / 1e6 : 0.0, dur = p ? p->length : 0.0;
    gint64 stamp = p ? p->pos_stamp : 0;
    if (fabs (pos - pub.position) > 1e-3 || fabs (dur - pub.duration) > 1e-3 ||
        stamp != pub_pos_stamp) {
        pub.position = pos;
        pub.duration = dur;
        pub_pos_stamp = stamp;
        pub_rate = p && p->rate > 0 ? p->rate : 1.0;
        bump (&pub.timeline_serial);
    }
    g_free (pub.player);
    pub.player = g_strdup (p ? p->name : NULL);
    g_mutex_unlock (&lock);

    const char *art = p && p->art_url ? p->art_url : "";
    if (g_strcmp0 (art, cur_art ? cur_art : "")) {
        g_free (cur_art);
        cur_art = g_strdup (art);
        load_art (art);
    }
    if (g_strcmp0 (cur_player, p ? p->name : NULL)) {
        g_free (cur_player);
        cur_player = g_strdup (p ? p->name : NULL);
    }
    if (!active)
        set_status ("off");
    else if (!conn)
        ; /* status already says why */
    else if (!p)
        set_status ("idle (no player)");
    else
        set_status ("%s: %s", p->name + strlen (MPRIS_PREFIX),
                    p->state == WALPE_MEDIA_PLAYING ? "playing" : "paused");
    tick_update ();
}

/* --- album art */

typedef struct {
    guint gen;
    char *url;             /* as the player named it (cache key) */
    char *path;            /* file:// art */
    GBytes *data;          /* downloaded art */
    /* results */
    GBytes *rgba;
    int w, h;
    char *data_url;
    float col[5][3];
    /* http(s) download: its own cancellable (size / time limit), chained
     * to the thread's `cancel` */
    GCancellable *http_cancel, *chain_src;
    gulong chain_id;
    GSource *http_deadline;
    gsize http_got;
    GObject *http_msg;         /* the request, while its body is read */
    GInputStream *http_in;     /* the body as it comes out of the content decoder */
    GByteArray *http_body;     /* what has been read of it */
    gboolean decoding;         /* counted in art_decodes_alive (art_decode) */
} ArtJob;

/* Decode jobs that still exist. A job is the data of its GTask, and the
 * worker thread of GLib's task pool gives its task up only after it has
 * queued the result for this thread: the job - with its own reference to
 * the decoded picture - can outlive art_done () by the time that thread
 * needs to run again. Nothing is lost by that; walpe_media_art_settled ()
 * lets a test wait for it. */
static gint art_decodes_alive;

/* The covers decoded last, by URL (media thread only). The published
 * thumbnail follows the active player: switching between two players, or
 * a player that clears and restores its metadata between tracks, asked
 * for the same cover again and again - each time a download (http) or a
 * read and a decode. Two decoded covers (<= 640 px: at most 1.6 MB each)
 * are kept instead; dropped when media integration is released. A local
 * file is the same cover only while its size and time stamp are.
 *
 * What a D-Bus peer can make this hold: ART_CACHE_N results of decodes
 * that passed every limit of art_decode_thread (regular file, byte and
 * pixel limits), each the THUMB_MAX thumbnail (RGBA + its data: URL, the
 * same two things that are published) - never the input, and never under
 * a key longer than ART_KEY_MAX (longer URLs are simply not cached). */
#define ART_CACHE_N 2
#define ART_KEY_MAX 4096
typedef struct {
    char *key;
    ArtJob *job;           /* results only */
} ArtCacheEnt;
static ArtCacheEnt art_cache[ART_CACHE_N];
static guint art_cache_next;

static char *art_cache_key (const char *url)
{
    char *path = NULL;
    if (g_str_has_prefix (url, "file://"))
        path = g_filename_from_uri (url, NULL, NULL);
    else if (url[0] == '/')
        path = g_strdup (url);
    else if (g_str_has_prefix (url, "data:"))
        return NULL;       /* the payload is the URL: nothing to save by a cache */
    if (strlen (url) > ART_KEY_MAX) {
        g_free (path);
        return NULL;       /* the key is a copy of the URL: bounded */
    }
    if (!path)
        return g_strdup (url);
    GStatBuf st;
    char *key = g_stat (path, &st) == 0
        ? g_strdup_printf ("%s|%" G_GINT64_FORMAT "|%" G_GINT64_FORMAT, url, (gint64) st.st_size,
                           (gint64) st.st_mtime)
        : NULL;
    g_free (path);
    return key;
}

/* the request and its body stream belong to the thread that made them:
 * let go of both before a job moves on to the decode thread */
static void art_http_release (ArtJob *j)
{
    if (j->http_msg) {
        g_signal_handlers_disconnect_by_data (j->http_msg, j);
        g_clear_object (&j->http_msg);
    }
    g_clear_object (&j->http_in);      /* closes it: an unfinished download ends here */
}

static void art_job_free (ArtJob *j)
{
    if (j->http_deadline) {
        g_source_destroy (j->http_deadline);
        g_source_unref (j->http_deadline);
    }
    if (j->chain_src) {
        if (j->chain_id)
            g_cancellable_disconnect (j->chain_src, j->chain_id);
        g_object_unref (j->chain_src);
    }
    art_http_release (j);
    if (j->http_body)
        g_byte_array_unref (j->http_body);
    g_clear_object (&j->http_cancel);
    g_free (j->url);
    g_free (j->path);
    if (j->data)
        g_bytes_unref (j->data);
    if (j->rgba)
        g_bytes_unref (j->rgba);
    g_free (j->data_url);
    gboolean decoding = j->decoding;
    g_free (j);
    if (decoding)
        g_atomic_int_add (&art_decodes_alive, -1);
}

/* Cover art goes to the decoder of the format its own first bytes name,
 * and only JPEG, PNG, GIF, WebP or BMP (util.h, walpe_picture_decode): the
 * bytes are a file a D-Bus peer names or whatever a server sent for the
 * URL it names, and gdk-pixbuf left to choose runs any loader the system
 * has on them - SVG (librsvg), ANI / ICO, XPM, PNM, TGA, QTIF, ICNS ...
 *
 * Decoded at most ~THUMB_MAX: a small compressed file (a player's art
 * URL, e.g. a web page's MediaSession artwork) can declare 30000x30000 px
 * and would otherwise be decoded at full size (GBs) before the scale-down.
 * The size asked for is honoured by the JPEG loader only: PNG (and others)
 * decode at full size first - a 31 KB PNG of 16000x16000 cost 750 MB.
 * Cover art beyond ART_MAX_SIDE is refused before any pixel exists. */
static const char *const art_kinds[] = { "jpeg", "png", "gif", "webp", "bmp", NULL };

typedef struct { gboolean too_big; int w, h; } ArtSize;

static gboolean art_size (const char *kind, int w, int h, int *sw, int *sh, gpointer user)
{
    (void) kind;
    ArtSize *as = user;
    int lim = THUMB_MAX * 2;   /* headroom: the precise scale happens below */
    if (w > ART_MAX_SIDE || h > ART_MAX_SIDE) {
        as->too_big = TRUE;
        as->w = w;
        as->h = h;
        return FALSE;
    }
    if (w > lim || h > lim) {
        double k = (double) lim / MAX (w, h);
        *sw = MAX (1, (int) (w * k));
        *sh = MAX (1, (int) (h * k));
    }
    return TRUE;
}

static void art_decode_thread (GTask *t, gpointer src, gpointer data, GCancellable *c)
{
    (void) src; (void) c;
    ArtJob *j = data;
    GdkPixbuf *pb = NULL;
    GError *err = NULL;
    if (j->path && !j->data) {
        /* the path comes from a D-Bus peer: a regular file only (opening
         * a FIFO would park this worker thread for good), and read, not
         * mapped. The file is the player's: it writes the next cover over
         * the same name when the track changes (or a peer cuts it short on
         * purpose), and a decoder reading a mapping whose file has just
         * been truncated ends the whole daemon with SIGBUS. */
        j->data = walpe_read_regular (j->path, 4 * ART_MAX_BYTES, &err);
    }
    if (j->data) {
        gsize dlen = 0;
        const guint8 *d = g_bytes_get_data (j->data, &dlen);
        ArtSize as = { FALSE, 0, 0 };
        pb = walpe_picture_decode (d, dlen, art_kinds, art_size, &as, &err);
        if (as.too_big) {
            g_clear_error (&err);
            g_set_error (&err, G_IO_ERROR, G_IO_ERROR_FAILED,
                         "%dx%d px is more than the %d px per side accepted for cover art",
                         as.w, as.h, ART_MAX_SIDE);
        }
    }
    if (!pb) {
        g_message ("media: album art not decodable: %s", err ? err->message : "?");
        g_clear_error (&err);
        g_task_return_boolean (t, FALSE);
        return;
    }
    GdkPixbuf *o = gdk_pixbuf_apply_embedded_orientation (pb);
    g_object_unref (pb);
    pb = o;
    int w = gdk_pixbuf_get_width (pb), h = gdk_pixbuf_get_height (pb);
    if (MAX (w, h) > THUMB_MAX) {
        double k = (double) THUMB_MAX / MAX (w, h);
        int nw = MAX (1, (int) lround (w * k)), nh = MAX (1, (int) lround (h * k));
        GdkPixbuf *s = gdk_pixbuf_scale_simple (pb, nw, nh, GDK_INTERP_BILINEAR);
        g_object_unref (pb);
        pb = s;
        w = nw;
        h = nh;
    }
    gboolean alpha = gdk_pixbuf_get_has_alpha (pb);
    GdkPixbuf *ra = alpha ? g_object_ref (pb) : gdk_pixbuf_add_alpha (pb, FALSE, 0, 0, 0);
    int rs = gdk_pixbuf_get_rowstride (ra);
    const guint8 *px = gdk_pixbuf_read_pixels (ra);
    guint8 *tight = g_malloc ((gsize) w * h * 4);
    for (int y = 0; y < h; y++)
        memcpy (tight + (gsize) y * w * 4, px + (gsize) y * rs, (gsize) w * 4);
    g_object_unref (ra);
    walpe_media_colors (tight, w, h, j->col[0], j->col[1], j->col[2], j->col[3], j->col[4]);
    j->rgba = g_bytes_new_take (tight, (gsize) w * h * 4);
    j->w = w;
    j->h = h;
    gchar *buf = NULL;
    gsize len = 0;
    const char *fmt = alpha ? "png" : "jpeg";
    gboolean ok = alpha ? gdk_pixbuf_save_to_buffer (pb, &buf, &len, fmt, NULL, NULL)
                        : gdk_pixbuf_save_to_buffer (pb, &buf, &len, fmt, NULL, "quality", "90", NULL);
    if (ok) {
        char *b64 = g_base64_encode ((const guchar *) buf, len);
        j->data_url = g_strconcat ("data:image/", fmt, ";base64,", b64, NULL);
        g_free (b64);
    }
    g_free (buf);
    g_object_unref (pb);
    g_task_return_boolean (t, TRUE);
}

static void set_thumb (ArtJob *j)
{
    g_mutex_lock (&lock);
    if (pub.thumb && (!j || pub.thumb != j->rgba)) {
        /* the replaced art becomes $mediaPreviousThumbnail */
        if (pub.prev_thumb)
            g_bytes_unref (pub.prev_thumb);
        pub.prev_thumb = pub.thumb;
        pub.prev_w = pub.thumb_w;
        pub.prev_h = pub.thumb_h;
        pub.thumb = NULL;
    }
    g_clear_pointer (&pub.thumb_data_url, g_free);
    pub.has_thumbnail = j != NULL;
    if (j) {
        /* the picture that is published already (the cover from the cache
         * once more, while another load was under way): the reference it
         * holds was written over, one per such request, and the picture
         * was never freed */
        GBytes *shown = pub.thumb;
        pub.thumb = g_bytes_ref (j->rgba);
        if (shown)
            g_bytes_unref (shown);
        pub.thumb_w = j->w;
        pub.thumb_h = j->h;
        pub.thumb_data_url = g_strdup (j->data_url);
        memcpy (pub.primary, j->col[0], sizeof (pub.primary));
        memcpy (pub.secondary, j->col[1], sizeof (pub.secondary));
        memcpy (pub.tertiary, j->col[2], sizeof (pub.tertiary));
        memcpy (pub.text, j->col[3], sizeof (pub.text));
        memcpy (pub.high_contrast, j->col[4], sizeof (pub.high_contrast));
    } else {
        pub.thumb_w = pub.thumb_h = 0;
        memset (pub.primary, 0, sizeof (pub.primary));
        memset (pub.secondary, 0, sizeof (pub.secondary));
        memset (pub.tertiary, 0, sizeof (pub.tertiary));
        memset (pub.text, 0, sizeof (pub.text));
        memset (pub.high_contrast, 0, sizeof (pub.high_contrast));
    }
    bump (&pub.thumb_serial);
    g_mutex_unlock (&lock);
}

static void art_done (GObject *src, GAsyncResult *res, gpointer user)
{
    (void) src; (void) user;
    GTask *t = G_TASK (res);
    ArtJob *j = g_task_get_task_data (t);
    gboolean ok = g_task_propagate_boolean (t, NULL);
    if (j->gen != art_gen || !active)
        return;                       /* superseded */
    if (ok) {
        g_message ("media: album art %dx%d", j->w, j->h);
        set_thumb (j);
        char *key = j->url ? art_cache_key (j->url) : NULL;
        if (key) {
            /* keep the decoded result (not the compressed input) */
            ArtCacheEnt *e = &art_cache[art_cache_next++ % ART_CACHE_N];
            g_free (e->key);
            if (e->job)
                art_job_free (e->job);
            e->key = key;
            e->job = g_new0 (ArtJob, 1);
            e->job->rgba = g_bytes_ref (j->rgba);
            e->job->w = j->w;
            e->job->h = j->h;
            e->job->data_url = g_strdup (j->data_url);
            memcpy (e->job->col, j->col, sizeof (j->col));
        }
    } else {
        set_thumb (NULL);
    }
}

static void art_cache_clear (void)
{
    for (guint i = 0; i < ART_CACHE_N; i++) {
        g_clear_pointer (&art_cache[i].key, g_free);
        if (art_cache[i].job)
            art_job_free (art_cache[i].job);
        art_cache[i].job = NULL;
    }
}

/* tests: what a player's art URL does (limits of the decode, the cache),
 * without a session bus. See media.h. */
gboolean walpe_media_art_probe (const char *url, int *w, int *h, guint *cached, gsize *cached_bytes)
{
    gboolean was = active, has = FALSE;
    active = TRUE;      /* results are dropped while media integration is off */
    g_mutex_lock (&lock);
    guint ser = pub.thumb_serial;
    g_mutex_unlock (&lock);
    if (url)
        load_art (url);
    else
        art_cache_clear ();
    for (;;) {
        g_mutex_lock (&lock);
        gboolean done = !url || pub.thumb_serial != ser;
        has = pub.has_thumbnail;
        if (w)
            *w = pub.thumb_w;
        if (h)
            *h = pub.thumb_h;
        g_mutex_unlock (&lock);
        if (done)
            break;
        g_main_context_iteration (g_main_context_get_thread_default (), TRUE);
    }
    active = was;
    guint n = 0;
    gsize bytes = 0;
    for (guint i = 0; i < ART_CACHE_N; i++) {
        if (!art_cache[i].job)
            continue;
        n++;
        bytes += strlen (art_cache[i].key);
        if (art_cache[i].job->rgba)
            bytes += g_bytes_get_size (art_cache[i].job->rgba);
        if (art_cache[i].job->data_url)
            bytes += strlen (art_cache[i].job->data_url);
    }
    if (cached)
        *cached = n;
    if (cached_bytes)
        *cached_bytes = bytes;
    return url && has;
}

/* tests: TRUE once no decode job exists any more (art_decodes_alive),
 * FALSE when that took longer than max_ms. See media.h. */
gboolean walpe_media_art_settled (guint max_ms)
{
    gint64 end = g_get_monotonic_time () + (gint64) max_ms * 1000;
    while (g_atomic_int_get (&art_decodes_alive) > 0) {
        if (g_get_monotonic_time () >= end)
            return FALSE;
        g_usleep (200);
    }
    return TRUE;
}

static void art_decode (ArtJob *j)
{
    GTask *t = g_task_new (NULL, NULL, art_done, NULL);
    j->decoding = TRUE;
    g_atomic_int_inc (&art_decodes_alive);
    g_task_set_task_data (t, j, (GDestroyNotify) art_job_free);
    g_task_run_in_thread (t, art_decode_thread);
    g_object_unref (t);
}

#ifdef HAVE_SOUP
/* The body is read in pieces and given up beyond ART_MAX_BYTES, counted
 * as it comes out of libsoup's content decoder. Any URL a D-Bus peer names
 * is fetched, so the server is anybody's: one that never stops, or a few
 * hundred KB of "Content-Encoding: gzip" that unpack to gigabytes, grew
 * the daemon until the OOM killer when the whole body was collected first
 * and measured afterwards (the bytes counted on the wire are the packed
 * ones). A Content-Length beyond the limit ends the request at once, and
 * ART_HTTP_SECONDS bound the whole transfer. */
#define ART_HTTP_PIECE 65536

static void art_http_failed (ArtJob *j, GError *err, const char *why)
{
    if (j->gen == art_gen && active) {
        g_message ("media: album art download failed (%s)", err ? err->message : why);
        set_thumb (NULL);
    }
    g_clear_error (&err);
    art_job_free (j);
}

static void art_http_piece (GObject *src, GAsyncResult *res, gpointer user)
{
    ArtJob *j = user;
    GError *err = NULL;
    GBytes *piece = g_input_stream_read_bytes_finish (G_INPUT_STREAM (src), res, &err);
    gsize n = piece ? g_bytes_get_size (piece) : 0;
    if (!piece || j->http_body->len + n > ART_MAX_BYTES) {
        if (piece)
            g_bytes_unref (piece);
        art_http_failed (j, err, "too large");
        return;
    }
    if (n > 0) {
        g_byte_array_append (j->http_body, g_bytes_get_data (piece, NULL), (guint) n);
        g_bytes_unref (piece);
        g_input_stream_read_bytes_async (j->http_in, ART_HTTP_PIECE, G_PRIORITY_LOW, j->http_cancel,
                                         art_http_piece, j);
        return;
    }
    g_bytes_unref (piece);             /* the end of the body */
    j->data = g_byte_array_free_to_bytes (j->http_body);
    j->http_body = NULL;
    art_http_release (j);
    art_decode (j);
}

static void art_http_done (GObject *src, GAsyncResult *res, gpointer user)
{
    ArtJob *j = user;
    GError *err = NULL;
    j->http_in = soup_session_send_finish (SOUP_SESSION (src), res, &err);
    SoupMessage *m = soup_session_get_async_result_message (SOUP_SESSION (src), res);
    guint code = m ? soup_message_get_status (m) : 0;
    if (!j->http_in || code < 200 || code >= 300) {
        art_http_failed (j, err, "HTTP error");
        return;
    }
    j->http_body = g_byte_array_new ();
    g_input_stream_read_bytes_async (j->http_in, ART_HTTP_PIECE, G_PRIORITY_LOW, j->http_cancel,
                                     art_http_piece, j);
}

static void art_http_headers (SoupMessage *m, gpointer user)
{
    ArtJob *j = user;
    if (soup_message_headers_get_content_length (soup_message_get_response_headers (m)) >
        (goffset) ART_MAX_BYTES)
        g_cancellable_cancel (j->http_cancel);
}

static void art_http_chunk (SoupMessage *m, guint len, gpointer user)
{
    (void) m;
    ArtJob *j = user;
    j->http_got += len;
    if (j->http_got > ART_MAX_BYTES)
        g_cancellable_cancel (j->http_cancel);
}

static gboolean art_http_timeout (gpointer user)
{
    g_cancellable_cancel (((ArtJob *) user)->http_cancel);
    return G_SOURCE_REMOVE;
}

static void art_chain_cancel (GCancellable *c, gpointer rc)
{
    (void) c;
    g_cancellable_cancel (rc);
}
#endif

static void load_art (const char *url)
{
    art_gen++;
    if (!url || !*url) {
        set_thumb (NULL);
        return;
    }
    char *key = art_cache_key (url);
    for (guint i = 0; key && i < ART_CACHE_N; i++)
        if (art_cache[i].key && !strcmp (art_cache[i].key, key)) {
            g_debug ("media: album art from the cache");
            set_thumb (art_cache[i].job);
            g_free (key);
            return;
        }
    g_free (key);
    ArtJob *j = g_new0 (ArtJob, 1);
    j->gen = art_gen;
    j->url = g_strdup (url);
    if (g_str_has_prefix (url, "file://")) {
        j->path = g_filename_from_uri (url, NULL, NULL);
        if (!j->path) {
            art_job_free (j);
            set_thumb (NULL);
            return;
        }
        art_decode (j);
        return;
    }
    if (url[0] == '/') {           /* a bare path (some players) */
        j->path = g_strdup (url);
        art_decode (j);
        return;
    }
    if (g_str_has_prefix (url, "data:")) {
        /* data:[mime][;base64],payload - mpv-mpris sends embedded cover
         * art this way; WE's SMTC thumbnail stream has no URL at all */
        const char *comma = strchr (url, ',');
        if (comma && comma - url < 128) {
            gboolean b64 = g_strstr_len (url, comma - url, ";base64") != NULL;
            gsize n = 0;
            guchar *raw = NULL;
            if (b64) {
                raw = g_base64_decode (comma + 1, &n);
            } else {
                char *u = g_uri_unescape_string (comma + 1, NULL);
                n = u ? strlen (u) : 0;
                raw = (guchar *) u;
            }
            if (raw && n > 0 && n <= ART_MAX_BYTES) {
                j->data = g_bytes_new_take (raw, n);
                art_decode (j);
                return;
            }
            g_free (raw);
        }
        g_message ("media: undecodable data: album art URL");
        art_job_free (j);
        set_thumb (NULL);
        return;
    }
    if (g_str_has_prefix (url, "http://") || g_str_has_prefix (url, "https://")) {
#ifdef HAVE_SOUP
        if (!soup)
            soup = soup_session_new_with_options ("timeout", 15, "user-agent", "walpe",
                                                  "max-conns", 2, NULL);
        SoupMessage *m = soup_message_new ("GET", url);
        if (m) {
            j->http_cancel = g_cancellable_new ();
            if (cancel) {
                j->chain_src = g_object_ref (cancel);
                j->chain_id = g_cancellable_connect (cancel, G_CALLBACK (art_chain_cancel),
                                                     g_object_ref (j->http_cancel), g_object_unref);
            }
            j->http_deadline = g_timeout_source_new_seconds (ART_HTTP_SECONDS);
            g_source_set_callback (j->http_deadline, art_http_timeout, j, NULL);
            g_source_attach (j->http_deadline, g_main_context_get_thread_default ());
            g_signal_connect (m, "got-headers", G_CALLBACK (art_http_headers), j);
            g_signal_connect (m, "got-body-data", G_CALLBACK (art_http_chunk), j);
            j->http_msg = G_OBJECT (m);
            soup_session_send_async (soup, m, G_PRIORITY_LOW, j->http_cancel, art_http_done, j);
            return;
        }
#else
        g_message ("media: album art %s needs libsoup (built without it)", url);
#endif
    } else {
        g_message ("media: unsupported album art URL %.80s", url);
    }
    art_job_free (j);
    set_thumb (NULL);
}

/* --- player state */

static char *join_strv (GVariant *v)
{
    if (g_variant_is_of_type (v, G_VARIANT_TYPE_STRING))
        return g_variant_dup_string (v, NULL);
    if (!g_variant_is_of_type (v, G_VARIANT_TYPE_STRING_ARRAY))
        return g_strdup ("");
    const char **a = g_variant_get_strv (v, NULL);
    char *s = g_strjoinv (", ", (char **) a);
    g_free (a);
    return s;
}

static gint64 var_int (GVariant *v)
{
    if (g_variant_is_of_type (v, G_VARIANT_TYPE_INT64))
        return g_variant_get_int64 (v);
    if (g_variant_is_of_type (v, G_VARIANT_TYPE_UINT64))
        return (gint64) g_variant_get_uint64 (v);
    if (g_variant_is_of_type (v, G_VARIANT_TYPE_INT32))
        return g_variant_get_int32 (v);
    if (g_variant_is_of_type (v, G_VARIANT_TYPE_UINT32))
        return g_variant_get_uint32 (v);
    if (g_variant_is_of_type (v, G_VARIANT_TYPE_DOUBLE))
        return (gint64) g_variant_get_double (v);
    return 0;
}

/* A text of a player's metadata into *dst, at most max bytes of it (cut
 * between two characters). The dictionary is whatever a program on the
 * session bus sends: the same key twice (each value replaced the one
 * before without freeing it: megabytes a message, for as many messages as
 * the sender liked), or a "title" of a hundred megabytes, which every
 * wallpaper with media integration then copied and handed to its page or
 * its scripts. No real title, artist, album or genre list is near
 * MD_TEXT_MAX; an art address can be a whole picture (data:) and has the
 * size limit of one. */
#define MD_TEXT_MAX 4096
#define MD_ART_URL_MAX ((gsize) ART_MAX_BYTES / 3 * 4 + 4096)

static void md_take (char **dst, char *s, gsize max)
{
    g_free (*dst);
    *dst = s;
    if (s && strlen (s) > max) {
        if (max == MD_ART_URL_MAX) {
            *s = 0;         /* a cut address is no address: none */
        } else {
            char *e = g_utf8_find_prev_char (s, s + max + 1);
            *(e ? e : s) = 0;
        }
    }
}

/* returns TRUE when the track changed (trackid, title or url) */
static gboolean apply_metadata (Player *p, GVariant *md)
{
    char *o_title = g_steal_pointer (&p->title), *o_url = g_steal_pointer (&p->url);
    char *o_id = g_steal_pointer (&p->trackid);
    g_clear_pointer (&p->title, g_free);
    g_clear_pointer (&p->artist, g_free);
    g_clear_pointer (&p->album, g_free);
    g_clear_pointer (&p->album_artist, g_free);
    g_clear_pointer (&p->genres, g_free);
    g_clear_pointer (&p->art_url, g_free);
    g_clear_pointer (&p->url, g_free);
    p->length = 0;
    GVariantIter it;
    const char *k;
    GVariant *v;
    g_variant_iter_init (&it, md);
    while (g_variant_iter_next (&it, "{&sv}", &k, &v)) {
        if (!strcmp (k, "xesam:title"))
            md_take (&p->title, join_strv (v), MD_TEXT_MAX);
        else if (!strcmp (k, "xesam:artist"))
            md_take (&p->artist, join_strv (v), MD_TEXT_MAX);
        else if (!strcmp (k, "xesam:album"))
            md_take (&p->album, join_strv (v), MD_TEXT_MAX);
        else if (!strcmp (k, "xesam:albumArtist"))
            md_take (&p->album_artist, join_strv (v), MD_TEXT_MAX);
        else if (!strcmp (k, "xesam:genre"))
            md_take (&p->genres, join_strv (v), MD_TEXT_MAX);
        else if (!strcmp (k, "mpris:artUrl"))
            md_take (&p->art_url, join_strv (v), MD_ART_URL_MAX);
        else if (!strcmp (k, "xesam:url"))
            md_take (&p->url, join_strv (v), MD_TEXT_MAX);
        else if (!strcmp (k, "mpris:length"))
            p->length = (double) var_int (v) / 1e6;
        else if (!strcmp (k, "mpris:trackid"))
            md_take (&p->trackid,
                     g_variant_is_of_type (v, G_VARIANT_TYPE_OBJECT_PATH) ||
                     g_variant_is_of_type (v, G_VARIANT_TYPE_STRING)
                         ? g_variant_dup_string (v, NULL) : NULL, MD_TEXT_MAX);
        g_variant_unref (v);
    }
    gboolean changed = g_strcmp0 (o_title, p->title) || g_strcmp0 (o_url, p->url) ||
                       g_strcmp0 (o_id, p->trackid);
    g_free (o_title);
    g_free (o_url);
    g_free (o_id);
    return changed;
}

static void position_query (Player *p);

static void apply_props (Player *p, GVariant *dict)
{
    GVariantIter it;
    const char *k;
    GVariant *v;
    gint64 now = g_get_monotonic_time ();
    gboolean new_track = FALSE, got_pos = FALSE;
    g_variant_iter_init (&it, dict);
    while (g_variant_iter_next (&it, "{&sv}", &k, &v)) {
        if (!strcmp (k, "PlaybackStatus") && g_variant_is_of_type (v, G_VARIANT_TYPE_STRING)) {
            const char *s = g_variant_get_string (v, NULL);
            int st = !strcmp (s, "Playing") ? WALPE_MEDIA_PLAYING
                   : !strcmp (s, "Paused") ? WALPE_MEDIA_PAUSED : WALPE_MEDIA_STOPPED;
            if (st != p->state) {
                /* freeze/restart the position clock at the transition */
                if (p->state == WALPE_MEDIA_PLAYING && p->pos_stamp)
                    p->pos_us += (gint64) ((now - p->pos_stamp) * (p->rate > 0 ? p->rate : 1.0));
                p->pos_stamp = now;
                if (st == WALPE_MEDIA_PLAYING)
                    p->play_start = now;
                p->state = st;
                p->last_active = now;
            }
        } else if (!strcmp (k, "Metadata") && g_variant_is_of_type (v, G_VARIANT_TYPE_VARDICT)) {
            if (apply_metadata (p, v)) {
                p->last_active = now;
                new_track = TRUE;
            }
        } else if (!strcmp (k, "Position")) {
            p->pos_us = var_int (v);
            p->pos_stamp = now;
            got_pos = TRUE;
        } else if (!strcmp (k, "Rate") && g_variant_is_of_type (v, G_VARIANT_TYPE_DOUBLE)) {
            p->rate = g_variant_get_double (v);
        } else if (!strcmp (k, "PlaybackStatus") || !strcmp (k, "Metadata")
                   || !strcmp (k, "Rate")) {
            /* a malformed player is otherwise ignored without a trace */
            g_debug ("media: %s: ignoring %s of unexpected type %s", p->name, k,
                     g_variant_get_type_string (v));
        }
        g_variant_unref (v);
    }
    if (new_track && !got_pos) {
        /* players do not signal Position, and most send no Seeked when the
         * next track starts: the clock kept running from the previous
         * track's position (clamped to the new length) until the 5 s
         * resync. A new track starts at 0; ask at once for the real value. */
        p->pos_us = 0;
        p->pos_stamp = now;
        position_query (p);
    }
}

static Player *player_by_owner (const char *owner)
{
    GHashTableIter it;
    gpointer k, v;
    if (!players || !owner)
        return NULL;
    g_hash_table_iter_init (&it, players);
    while (g_hash_table_iter_next (&it, &k, &v))
        if (!g_strcmp0 (((Player *) v)->owner, owner))
            return v;
    return NULL;
}

static void getall_done (GObject *src, GAsyncResult *res, gpointer user)
{
    char *name = user;
    GError *err = NULL;
    GVariant *r = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, &err);
    Player *p = r && active && players ? g_hash_table_lookup (players, name) : NULL;
    if (p) {
        GVariant *d = g_variant_get_child_value (r, 0);
        apply_props (p, d);
        g_variant_unref (d);
        publish ();
    } else if (err && !g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_message ("media: %s: GetAll failed: %s", name, err->message);
    }
    g_clear_error (&err);
    if (r)
        g_variant_unref (r);
    g_free (name);
}

static void player_query (const char *name)
{
    g_dbus_connection_call (conn, name, MPRIS_PATH, "org.freedesktop.DBus.Properties", "GetAll",
                            g_variant_new ("(s)", MPRIS_PLAYER), G_VARIANT_TYPE ("(a{sv})"),
                            G_DBUS_CALL_FLAGS_NO_AUTO_START, 3000, cancel, getall_done,
                            g_strdup (name));
}

static void player_add (const char *name, const char *owner)
{
    Player *p = g_hash_table_lookup (players, name);
    if (!p) {
        p = g_new0 (Player, 1);
        p->name = g_strdup (name);
        p->rate = 1.0;
        g_hash_table_insert (players, p->name, p);
        g_message ("media: player %s appeared", name);
    }
    g_free (p->owner);
    p->owner = g_strdup (owner);
    player_query (name);
}

static void owner_done (GObject *src, GAsyncResult *res, gpointer user)
{
    char *name = user;
    GError *err = NULL;
    GVariant *r = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, &err);
    if (r && active && players) {
        const char *owner = NULL;
        g_variant_get (r, "(&s)", &owner);
        player_add (name, owner);
    } else if (err && !g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_debug ("media: %s: GetNameOwner failed: %s", name, err->message);
    }
    g_clear_error (&err);
    if (r)
        g_variant_unref (r);
    g_free (name);
}

static void list_done (GObject *src, GAsyncResult *res, gpointer user)
{
    (void) user;
    GError *err = NULL;
    GVariant *r = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, &err);
    if (r && active && players) {
        GVariantIter *it = NULL;
        const char *n;
        g_variant_get (r, "(as)", &it);
        while (g_variant_iter_next (it, "&s", &n))
            if (g_str_has_prefix (n, MPRIS_PREFIX))
                g_dbus_connection_call (conn, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                        "org.freedesktop.DBus", "GetNameOwner",
                                        g_variant_new ("(s)", n), G_VARIANT_TYPE ("(s)"),
                                        G_DBUS_CALL_FLAGS_NONE, 3000, cancel, owner_done,
                                        g_strdup (n));
        g_variant_iter_free (it);
        publish ();
    } else if (err && !g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_message ("media: ListNames failed: %s", err->message);
    }
    g_clear_error (&err);
    if (r)
        g_variant_unref (r);
}

static void on_name_owner (GDBusConnection *c, const char *sender, const char *path,
                           const char *iface, const char *sig, GVariant *params, gpointer u)
{
    (void) c; (void) sender; (void) path; (void) iface; (void) sig; (void) u;
    const char *name, *old, *neu;
    if (!g_variant_is_of_type (params, G_VARIANT_TYPE ("(sss)")))
        return;
    g_variant_get (params, "(&s&s&s)", &name, &old, &neu);
    if (!active || !players || !g_str_has_prefix (name, MPRIS_PREFIX))
        return;
    if (!*neu) {
        if (g_hash_table_remove (players, name)) {
            g_message ("media: player %s left", name);
            publish ();
        }
    } else {
        player_add (name, neu);
    }
}

static void on_props (GDBusConnection *c, const char *sender, const char *path,
                      const char *iface, const char *sig, GVariant *params, gpointer u)
{
    (void) c; (void) path; (void) iface; (void) sig; (void) u;
    Player *p = active ? player_by_owner (sender) : NULL;
    if (!p)
        return;
    const char *ifn = NULL;
    GVariant *changed = NULL, *inval = NULL;
    /* the bus does not type-check signal bodies: any owner of an MPRIS
     * name can send PropertiesChanged with another signature, and
     * g_variant_get then leaves all three pointers unset (strcmp + two
     * unrefs of stack leftovers = the daemon's death) */
    if (!g_variant_is_of_type (params, G_VARIANT_TYPE ("(sa{sv}as)")))
        return;
    g_variant_get (params, "(&s@a{sv}@as)", &ifn, &changed, &inval);
    if (!strcmp (ifn, MPRIS_PLAYER)) {
        apply_props (p, changed);
        if (g_variant_n_children (inval) > 0)
            player_query (p->name);   /* invalidated: re-read everything */
        publish ();
    }
    g_variant_unref (changed);
    g_variant_unref (inval);
}

static void on_seeked (GDBusConnection *c, const char *sender, const char *path,
                       const char *iface, const char *sig, GVariant *params, gpointer u)
{
    (void) c; (void) path; (void) iface; (void) sig; (void) u;
    Player *p = active ? player_by_owner (sender) : NULL;
    if (!p || !g_variant_is_of_type (params, G_VARIANT_TYPE ("(x)")))
        return;
    g_variant_get (params, "(x)", &p->pos_us);
    p->pos_stamp = g_get_monotonic_time ();
    publish ();
}

static void pos_done (GObject *src, GAsyncResult *res, gpointer user)
{
    char *name = user;
    GVariant *r = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, NULL);
    Player *p = r && active && players ? g_hash_table_lookup (players, name) : NULL;
    if (p) {
        GVariant *v = NULL;
        g_variant_get (r, "(v)", &v);
        gint64 us = var_int (v);
        g_variant_unref (v);
        gint64 now = g_get_monotonic_time ();
        /* only a real jump re-publishes (players drift by a few ms) */
        double pred = (double) p->pos_us + (now - p->pos_stamp) * (p->rate > 0 ? p->rate : 1.0);
        if (fabs (pred - (double) us) > 250000) {
            p->pos_us = us;
            p->pos_stamp = now;
            publish ();
        }
    }
    if (r)
        g_variant_unref (r);
    g_free (name);
}

static void position_query (Player *p)
{
    if (!conn)
        return;
    g_dbus_connection_call (conn, p->name, MPRIS_PATH, "org.freedesktop.DBus.Properties", "Get",
                            g_variant_new ("(ss)", MPRIS_PLAYER, "Position"),
                            G_VARIANT_TYPE ("(v)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 2000,
                            cancel, pos_done, g_strdup (p->name));
}

/* media thread: the context its timers belong to - its own, which it
 * runs as its thread-default (media_thread). The global `mctx` is for the
 * other threads, under `lock`: it is cleared when this thread is retired
 * and replaced when a successor is started, so reading it from here
 * without the lock was only right as long as nothing of this thread ran
 * after its retirement. */
static GMainContext *media_ctx (void)
{
    return g_main_context_get_thread_default ();
}

/* while something plays: timeline event once a second (WE: "sent
 * frequently while media is playing"), Position resync every 5 s */
static gboolean tick_cb (gpointer u)
{
    (void) u;
    Player *p = active ? active_player () : NULL;
    if (!p || p->state != WALPE_MEDIA_PLAYING) {
        tick_id = 0;
        return G_SOURCE_REMOVE;
    }
    g_mutex_lock (&lock);
    bump (&pub.timeline_serial);
    g_mutex_unlock (&lock);
    if (++tick_n % RESYNC_S == 0)
        position_query (p);
    return G_SOURCE_CONTINUE;
}

static void tick_update (void)
{
    Player *p = active ? active_player () : NULL;
    gboolean want = p && p->state == WALPE_MEDIA_PLAYING;
    if (want && !tick_id) {
        GSource *s = g_timeout_source_new_seconds (1);
        g_source_set_callback (s, tick_cb, NULL, NULL);
        tick_id = g_source_attach (s, media_ctx ());
        g_source_unref (s);
    } else if (!want && tick_id) {
        GSource *s = g_main_context_find_source_by_id (media_ctx (), tick_id);
        if (s)
            g_source_destroy (s);
        tick_id = 0;
    }
}

static gboolean session_bus_present (void)
{
    if (g_getenv ("DBUS_SESSION_BUS_ADDRESS"))
        return TRUE;
    const char *rd = g_getenv ("XDG_RUNTIME_DIR");
    char *sock = rd ? g_build_filename (rd, "bus", NULL) : NULL;
    gboolean ok = sock && g_file_test (sock, G_FILE_TEST_EXISTS);
    g_free (sock);
    return ok;
}

static void activate (void)
{
    if (grace_id) {
        GSource *s = g_main_context_find_source_by_id (media_ctx (), grace_id);
        if (s)
            g_source_destroy (s);
        grace_id = 0;
    }
    if (active)
        return;
    active = TRUE;
    cancel = g_cancellable_new ();
    players = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, player_free);
    const char *off = g_getenv ("WALPE_NO_MEDIA");
    if (off && *off && *off != '0') {
        set_status ("disabled (WALPE_NO_MEDIA)");
    } else if (!session_bus_present ()) {
        set_status ("disabled (no D-Bus session bus)");
    } else {
        GError *err = NULL;
        conn = g_bus_get_sync (G_BUS_TYPE_SESSION, cancel, &err);
        if (!conn) {
            set_status ("disabled (session bus: %s)", err ? err->message : "?");
            g_clear_error (&err);
        } else {
            sub_noc = g_dbus_connection_signal_subscribe (
                conn, "org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged",
                "/org/freedesktop/DBus", "org.mpris.MediaPlayer2",
                G_DBUS_SIGNAL_FLAGS_MATCH_ARG0_NAMESPACE, on_name_owner, NULL, NULL);
            sub_props = g_dbus_connection_signal_subscribe (
                conn, NULL, "org.freedesktop.DBus.Properties", "PropertiesChanged", MPRIS_PATH,
                MPRIS_PLAYER, G_DBUS_SIGNAL_FLAGS_NONE, on_props, NULL, NULL);
            sub_seek = g_dbus_connection_signal_subscribe (
                conn, NULL, MPRIS_PLAYER, "Seeked", MPRIS_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
                on_seeked, NULL, NULL);
            g_dbus_connection_call (conn, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                    "org.freedesktop.DBus", "ListNames", NULL,
                                    G_VARIANT_TYPE ("(as)"), G_DBUS_CALL_FLAGS_NONE, 3000,
                                    cancel, list_done, NULL);
            set_status ("idle (no player)");
        }
    }
    publish ();
}

static gboolean deactivate (gpointer u)
{
    (void) u;
    grace_id = 0;
    g_mutex_lock (&lock);
    int r = refs;
    g_mutex_unlock (&lock);
    if (r > 0 || !active)
        return G_SOURCE_REMOVE;
    g_cancellable_cancel (cancel);
    g_clear_object (&cancel);
    if (conn) {
        g_dbus_connection_signal_unsubscribe (conn, sub_noc);
        g_dbus_connection_signal_unsubscribe (conn, sub_props);
        g_dbus_connection_signal_unsubscribe (conn, sub_seek);
        sub_noc = sub_props = sub_seek = 0;
        g_clear_object (&conn);
    }
    g_clear_pointer (&players, g_hash_table_unref);
    active = FALSE;
    art_gen++;
    g_clear_pointer (&cur_art, g_free);
    g_clear_pointer (&cur_player, g_free);
    art_cache_clear ();
    set_thumb (NULL);
    g_mutex_lock (&lock);
    if (pub.prev_thumb)
        g_clear_pointer (&pub.prev_thumb, g_bytes_unref);
    g_mutex_unlock (&lock);
    publish ();          /* enabled=false, stopped, empty */
#ifdef HAVE_SOUP
    g_clear_object (&soup);
#endif
    /* no consumer left: the media thread and its context end too (the
     * next walpe_media_ref starts fresh ones) */
    GMainContext *retire = NULL;
    g_mutex_lock (&lock);
    if (refs == 0) {
        retire = mctx;   /* media_thread sees its context retired and returns */
        mctx = NULL;
    }
    g_mutex_unlock (&lock);
    if (retire)
        g_main_context_unref (retire);   /* the thread holds its own ref */
    g_message ("media: unsubscribed (no consumer)");
    return G_SOURCE_REMOVE;
}

static gboolean activate_cb (gpointer u)
{
    (void) u;
    activate ();
    return G_SOURCE_REMOVE;
}

static gboolean release_cb (gpointer u)
{
    (void) u;
    g_mutex_lock (&lock);
    int r = refs;
    g_mutex_unlock (&lock);
    if (r == 0 && active && !grace_id) {
        GSource *s = g_timeout_source_new (GRACE_MS);
        g_source_set_callback (s, deactivate, NULL, NULL);
        grace_id = g_source_attach (s, media_ctx ());
        g_source_unref (s);
    }
    return G_SOURCE_REMOVE;
}

/* always queue on the media context: g_main_context_invoke() would run the
 * callback on the CALLING thread while the context is not yet acquired */
static void post (GSourceFunc fn)
{
    g_mutex_lock (&lock);
    GMainContext *ctx = mctx ? g_main_context_ref (mctx) : NULL;
    g_mutex_unlock (&lock);
    if (!ctx)
        return;
    GSource *s = g_idle_source_new ();
    g_source_set_callback (s, fn, NULL, NULL);
    g_source_attach (s, ctx);
    g_source_unref (s);
    g_main_context_unref (ctx);
}

/* runs until its context is retired (deactivate with no consumer left) */
static gpointer media_thread (gpointer u)
{
    GMainContext *ctx = u;
    g_main_context_push_thread_default (ctx);
    for (;;) {
        g_main_context_iteration (ctx, TRUE);
        g_mutex_lock (&lock);
        gboolean retired = mctx != ctx;
        g_mutex_unlock (&lock);
        if (retired)
            break;
    }
    g_main_context_pop_thread_default (ctx);
    g_main_context_unref (ctx);
    g_message ("media: thread ended");
    return NULL;
}

void walpe_media_ref (void)
{
    g_mutex_lock (&lock);
    refs++;
    GThread *old = NULL;
    if (!mctx) {
        old = thread;     /* a retired thread is returning: reap it */
        mctx = g_main_context_new ();
        info_init_strings (&pub);
        thread = g_thread_new ("walpe-media", media_thread, g_main_context_ref (mctx));
    }
    g_mutex_unlock (&lock);
    if (old)
        g_thread_join (old);
    post (activate_cb);
}

void walpe_media_unref (void)
{
    g_mutex_lock (&lock);
    if (refs > 0)
        refs--;
    int r = refs;
    g_mutex_unlock (&lock);
    if (r == 0)
        post (release_cb);
}
