/* thumbs.c - asynchronous, exact-size, cached thumbnails (see thumbs.h) */
#include "thumbs.h"
#include "common/config.h"   /* WALPE_PREVIEW_* */
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gstdio.h>
#include <malloc.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define MEM_BUDGET (96u << 20)   /* decoded thumbnails kept in RAM */
#define KEY_DATA "walpe-thumb-key"

/* untrusted sources: what is not decoded at all, and how large the scaled
 * picture may get before the centre crop (a 1 x 30000 preview asked for a
 * 160 x 4 800 000 one) */
#define SRC_MAX_SIDE 16384
#define SRC_MAX_PIXELS (64u << 20)
#define COVER_MAX_SIDE 4096

/* animated previews */
#define ANIM_MAX_FRAMES 48            /* kept per file (longer loops are thinned out) */
#define ANIM_MIN_DELAY_MS 50          /* 20 pictures a second at most */
#define ANIM_TICK_MS 50
#define ANIM_MAX_BYTES (16u << 20)    /* frames of one file at one size */
#define ANIM_MAX_FRAME_PX 110000      /* a large (detail) preview animates at about 330 x 330 */
#define ANIM_SRC_MAX_SIDE 4096
#define ANIM_SRC_MAX_PIXELS (4u << 20)
#define ANIM_SRC_MAX_FILE (64u << 20)
#define ANIM_SRC_MAX_FRAMES 2000
#define ANIM_IDLE_DROP_S 20           /* nothing played for this long: the frames are freed */

typedef struct {
    char *key;
    char *src;
    int w, h;
    ThumbMode mode;
    gboolean anim;      /* decode the animation's frames, not the still */
    guint seq;          /* LIFO: the newest request (visible tile) first */
    gint wanted;        /* atomic: targets still waiting for this key */
    GPtrArray *targets; /* GWeakRef* to GtkPicture (main thread only) */
    /* worker -> main */
    GBytes *px;
    int pw, ph, stride;
    gboolean alpha;
    int n;              /* anim: frames in px */
    int *delay;         /* anim: ms each frame is shown */
    gboolean skipped;   /* nobody wanted it any more: not a failure */
} Job;

typedef struct {
    GdkTexture *tex;
    gsize bytes;
    GList *link;        /* in lru (head = most recent) */
} MemEnt;

static GThreadPool *pool;
static GHashTable *mem;       /* key -> MemEnt */
static GHashTable *failed;    /* key set: undecodable / missing sources */
static GHashTable *inflight;  /* key -> Job */
static GQueue lru = G_QUEUE_INIT;
static gsize mem_bytes;
static guint seq_ctr;
static char *cache_dir;

static void weak_free (gpointer p)
{
    g_weak_ref_clear (p);
    g_free (p);
}

static void job_free (Job *j)
{
    g_free (j->key);
    g_free (j->src);
    g_free (j->delay);
    if (j->targets)
        g_ptr_array_unref (j->targets);
    if (j->px)
        g_bytes_unref (j->px);
    g_free (j);
}

static void mem_free (gpointer p)
{
    MemEnt *e = p;
    g_object_unref (e->tex);
    g_free (e);
}

/* ------------------------------------------------------------- worker */

static char *disk_path (const Job *j, const struct stat *st)
{
    char *id = g_strdup_printf ("%s|%lld|%lld|%dx%d|%d", j->src, (long long) st->st_mtime,
                                (long long) st->st_size, j->w, j->h, (int) j->mode);
    char *sum = g_compute_checksum_for_string (G_CHECKSUM_MD5, id, -1);
    char *name = g_strconcat (sum, ".png", NULL);
    char *p = g_build_filename (cache_dir, name, NULL);
    g_free (name);
    g_free (sum);
    g_free (id);
    return p;
}

gboolean thumbs_format_allowed (const char *gdk_pixbuf_format_name)
{
    static const char *const ok[] = { "jpeg", "png", "gif", "bmp", "webp", "avif", "heif/avif",
                                      NULL };
    for (int i = 0; gdk_pixbuf_format_name && ok[i]; i++)
        if (!strcmp (gdk_pixbuf_format_name, ok[i]))
            return TRUE;
    return FALSE;
}

static GdkPixbuf *decode (const Job *j)
{
    int iw = 0, ih = 0;
    GdkPixbufFormat *fmt = gdk_pixbuf_get_file_info (j->src, &iw, &ih);
    if (!fmt || iw < 1 || ih < 1)
        return NULL;
    /* The preview is whatever the wallpaper's author shipped. Decompression
     * bombs: only JPEG is scaled while it is decoded; every other format is
     * decoded at full size first, and a PNG of a few KB can declare
     * gigabytes of pixels (20000 x 20000 cost 2.4 GB here, three at a
     * time). Such a picture gets the placeholder. Limits: SRC_MAX_SIDE per
     * side for every format, SRC_MAX_PIXELS (8192 x 8192) for all but JPEG. */
    if (iw > SRC_MAX_SIDE || ih > SRC_MAX_SIDE)
        return NULL;
    char *fname = gdk_pixbuf_format_get_name (fmt);
    gboolean scales_on_load = !g_strcmp0 (fname, "jpeg");
    /* Decoders: the picture formats the library lists (store.c) and
     * nothing else. gdk-pixbuf picks a loader by the file's first bytes,
     * whatever its name: a "preview.jpg" that is really SVG went to
     * librsvg, others to the XPM, TGA, ICO, TIFF, PNM, QTIF, ANI ... loaders
     * the system happens to have. Such a preview gets the placeholder. */
    gboolean known = thumbs_format_allowed (fname);
    g_free (fname);
    if (!known)
        return NULL;
    if (!scales_on_load && (guint64) iw * (guint64) ih > SRC_MAX_PIXELS)
        return NULL;
    GError *err = NULL;
    GdkPixbuf *out = NULL;
    if (j->mode == THUMB_COVER) {
        double s = MAX ((double) j->w / iw, (double) j->h / ih);
        int lw = (int) ceil (iw * s), lh = (int) ceil (ih * s);
        if (lw > COVER_MAX_SIDE || lh > COVER_MAX_SIDE) {
            /* a very long or very tall picture (1 x 30000 asked for a
             * 160 x 4 800 000 intermediate): keep the shape, bound the size;
             * the thumbnail then covers only part of the tile */
            double k = MIN ((double) COVER_MAX_SIDE / lw, (double) COVER_MAX_SIDE / lh);
            lw = MAX (1, (int) (lw * k));
            lh = MAX (1, (int) (lh * k));
        } else {
            lw = MAX (j->w, lw);
            lh = MAX (j->h, lh);
        }
        GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_scale (j->src, lw, lh, FALSE, &err);
        if (pb) {
            lw = gdk_pixbuf_get_width (pb);
            lh = gdk_pixbuf_get_height (pb);
            int cw = MIN (j->w, lw), ch = MIN (j->h, lh);
            GdkPixbuf *sub = gdk_pixbuf_new_subpixbuf (pb, (lw - cw) / 2, (lh - ch) / 2, cw, ch);
            out = gdk_pixbuf_copy (sub);   /* tight rows, parent freed */
            g_object_unref (sub);
            g_object_unref (pb);
        }
    } else {
        double s = MIN (1.0, MIN ((double) j->w / iw, (double) j->h / ih));
        int lw = MAX (1, (int) lround (iw * s)), lh = MAX (1, (int) lround (ih * s));
        out = gdk_pixbuf_new_from_file_at_scale (j->src, lw, lh, FALSE, &err);
    }
    g_clear_error (&err);
    return out;
}

/* Frames of a GIF and the time each is shown (ms, with gdk-pixbuf's own
 * rules: 0 -> 100, under 20 -> 20), read from the block structure alone:
 * no pixel is decoded. NULL: not a GIF, damaged, or too many frames. */
static int *gif_frame_delays (const guint8 *d, gsize len, int *n_out, int *w_out, int *h_out)
{
    if (len < 13 || (memcmp (d, "GIF87a", 6) && memcmp (d, "GIF89a", 6)))
        return NULL;
    *w_out = d[6] | (d[7] << 8);
    *h_out = d[8] | (d[9] << 8);
    gsize i = 13;
    if (d[10] & 0x80)
        i += 3u * (1u << ((d[10] & 7) + 1));
    GArray *out = g_array_new (FALSE, FALSE, sizeof (int));
    int delay_cs = 0;
    gboolean ok = TRUE;
    while (i < len) {
        guint8 b = d[i++];
        if (b == 0x3B)
            break;                              /* trailer */
        if (b == 0x21) {                        /* extension */
            if (i >= len)
                break;
            guint8 label = d[i++];
            if (label == 0xF9 && i + 5 <= len && d[i] == 4)
                delay_cs = d[i + 2] | (d[i + 3] << 8);
            while (i < len && d[i])             /* data sub-blocks */
                i += 1u + d[i];
            i++;
        } else if (b == 0x2C) {                 /* image */
            if (i + 9 > len)
                break;
            guint8 f = d[i + 8];
            i += 9;
            if (f & 0x80)
                i += 3u * (1u << ((f & 7) + 1));
            i++;                                /* LZW minimum code size */
            while (i < len && d[i])
                i += 1u + d[i];
            i++;
            int ms = delay_cs * 10;
            if (ms == 0)
                ms = 100;
            else if (ms < 20)
                ms = 20;
            g_array_append_val (out, ms);
            delay_cs = 0;
            if (out->len > ANIM_SRC_MAX_FRAMES) {
                ok = FALSE;
                break;
            }
        } else {
            break;                              /* damaged: what was found counts */
        }
    }
    if (!ok || out->len < 1) {
        g_array_free (out, TRUE);
        return NULL;
    }
    *n_out = (int) out->len;
    return (int *) g_array_free (out, FALSE);
}

/* the frames of an animated GIF at the size asked for: one buffer, tight
 * rows, RGB (RGBA only when a frame has transparent pixels) */
static void decode_anim (Job *j)
{
    struct stat st;
    if (g_stat (j->src, &st) < 0 || !S_ISREG (st.st_mode) || st.st_size > (goffset) ANIM_SRC_MAX_FILE ||
        st.st_size < 13)
        return;
    char *data = NULL;
    gsize len = 0;
    if (!g_file_get_contents (j->src, &data, &len, NULL))
        return;
    int n = 0, iw = 0, ih = 0;
    int *sd = gif_frame_delays ((const guint8 *) data, len, &n, &iw, &ih);
    if (!sd || n < 2 || iw < 1 || ih < 1 || iw > ANIM_SRC_MAX_SIDE || ih > ANIM_SRC_MAX_SIDE ||
        (guint64) iw * (guint64) ih > ANIM_SRC_MAX_PIXELS) {
        g_free (sd);
        g_free (data);
        return;     /* a still, not a GIF, or over the limits: the first frame stays */
    }
    /* geometry of every frame */
    double s, ox = 0.0, oy = 0.0;
    int pw, ph;
    if (j->mode == THUMB_COVER) {
        s = MAX ((double) j->w / iw, (double) j->h / ih);
        double lw = iw * s, lh = ih * s;
        pw = MIN (j->w, MAX (1, (int) ceil (lw)));
        ph = MIN (j->h, MAX (1, (int) ceil (lh)));
        ox = -(lw - pw) / 2.0;
        oy = -(lh - ph) / 2.0;
    } else {
        s = MIN (1.0, MIN ((double) j->w / iw, (double) j->h / ih));
        /* the picture scales it up again: 48 frames of a 500 x 500 preview
         * would be 36 MB */
        if (iw * s * ih * s > ANIM_MAX_FRAME_PX)
            s = sqrt ((double) ANIM_MAX_FRAME_PX / ((double) iw * ih));
        pw = MAX (1, (int) lround (iw * s));
        ph = MAX (1, (int) lround (ih * s));
    }
    gsize fb = (gsize) pw * ph * 4;
    /* which frames are kept: at least ANIM_MIN_DELAY_MS each, then every
     * step-th so that count and bytes stay inside the limits */
    int *group = g_new0 (int, n);      /* frame -> group (first frame of a group is shown) */
    int ng = 0, acc = ANIM_MIN_DELAY_MS;
    for (int i = 0; i < n; i++) {
        if (acc >= ANIM_MIN_DELAY_MS) {
            ng++;
            acc = 0;
        }
        group[i] = ng - 1;
        acc += sd[i];
    }
    int cap = (int) MIN ((gsize) ANIM_MAX_FRAMES, MAX ((gsize) 1, ANIM_MAX_BYTES / fb));
    int step = (ng + cap - 1) / cap;
    int kept = (ng + step - 1) / step;
    if (kept < 2) {
        g_free (group);
        g_free (sd);
        g_free (data);
        return;
    }
    int *delay = g_new0 (int, kept);
    for (int i = 0; i < n; i++)
        delay[group[i] / step] += sd[i];

    GInputStream *in = g_memory_input_stream_new_from_data (data, (gssize) len, NULL);
    GdkPixbufAnimation *an = gdk_pixbuf_animation_new_from_stream (in, NULL, NULL);
    g_object_unref (in);
    guint8 *buf = NULL;
    int got = 0;
    gboolean transparent = FALSE;
    if (an && !gdk_pixbuf_animation_is_static_image (an)) {
        buf = g_try_malloc ((gsize) kept * fb);
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        GTimeVal t = { 1000000, 0 };    /* a clock of our own: frame by frame */
        GdkPixbufAnimationIter *it = buf ? gdk_pixbuf_animation_get_iter (an, &t) : NULL;
        GdkPixbuf *dst = it ? gdk_pixbuf_new (GDK_COLORSPACE_RGB, TRUE, 8, pw, ph) : NULL;
        for (int i = 0; dst && i < n && got < kept; i++) {
            if (!g_atomic_int_get (&j->wanted)) {
                j->skipped = TRUE;
                break;
            }
            if (group[i] % step == 0 && (i == 0 || group[i] != group[i - 1])) {
                GdkPixbuf *f = gdk_pixbuf_animation_iter_get_pixbuf (it);
                if (!f || gdk_pixbuf_get_width (f) != iw || gdk_pixbuf_get_height (f) != ih)
                    break;
                gdk_pixbuf_scale (f, dst, 0, 0, pw, ph, ox, oy, s, s, GDK_INTERP_BILINEAR);
                const guint8 *sp = gdk_pixbuf_read_pixels (dst);
                int rs = gdk_pixbuf_get_rowstride (dst);
                guint8 *dp = buf + (gsize) got * fb;
                for (int y = 0; y < ph; y++)
                    memcpy (dp + (gsize) y * pw * 4, sp + (gsize) y * rs, (gsize) pw * 4);
                for (gsize p = 3; !transparent && p < fb; p += 4)
                    transparent = dp[p] != 0xff;
                got++;
            }
            int dl = gdk_pixbuf_animation_iter_get_delay_time (it);
            if (dl < 0)
                break;                  /* a GIF that plays once: its last frame */
            glong us = t.tv_usec + (glong) dl * 1000;
            t.tv_sec += us / 1000000;
            t.tv_usec = us % 1000000;
            gdk_pixbuf_animation_iter_advance (it, &t);
        }
        G_GNUC_END_IGNORE_DEPRECATIONS
        g_clear_object (&dst);
        g_clear_object (&it);
    }
    g_clear_object (&an);
    g_free (group);
    g_free (sd);
    g_free (data);
    if (got < 2 || j->skipped) {
        g_free (buf);
        g_free (delay);
        return;
    }
    for (int i = got; i < kept; i++)    /* fewer frames than the file announced */
        delay[got - 1] += delay[i];
    int bpp = 4;
    if (!transparent) {
        /* opaque (nearly every preview): 3 bytes a pixel */
        gsize px = (gsize) got * pw * ph;
        for (gsize p = 0; p < px; p++)
            memmove (buf + p * 3, buf + p * 4, 3);
        guint8 *sm = g_try_realloc (buf, px * 3);
        if (sm)
            buf = sm;
        bpp = 3;
    }
    j->px = g_bytes_new_take (buf, (gsize) got * pw * ph * bpp);
    j->pw = pw;
    j->ph = ph;
    j->stride = pw * bpp;
    j->alpha = transparent;
    j->n = got;
    j->delay = delay;
}

static gboolean deliver (gpointer data);

static void work (gpointer data, gpointer user)
{
    (void) user;
    Job *j = data;
    /* decoding previews is background work: below the UI thread (Linux:
     * nice is per thread), once per pool thread */
    static GPrivate niced;
    if (!g_private_get (&niced)) {
        g_private_set (&niced, GINT_TO_POINTER (1));
        if (setpriority (PRIO_PROCESS, (id_t) syscall (SYS_gettid), 10) != 0) {
            /* not allowed here: stays at the process's priority */
        }
    }
    if (g_atomic_int_get (&j->wanted) <= 0) {
        j->skipped = TRUE;
        goto out;   /* scrolled past before we got to it */
    }
    if (j->anim) {
        decode_anim (j);
        goto out;
    }
    struct stat st;
    if (g_stat (j->src, &st) < 0 || !S_ISREG (st.st_mode))
        goto out;
    char *dp = disk_path (j, &st);
    GdkPixbuf *pb = NULL;
    if (g_file_test (dp, G_FILE_TEST_IS_REGULAR))
        pb = gdk_pixbuf_new_from_file (dp, NULL);
    if (!pb) {
        pb = decode (j);
        if (pb) {
            char *tmp = g_strconcat (dp, ".tmp", NULL);
            if (gdk_pixbuf_save (pb, tmp, "png", NULL, "compression", "3", NULL))
                g_rename (tmp, dp);
            else
                g_unlink (tmp);
            g_free (tmp);
        }
    }
    g_free (dp);
    if (pb) {
        j->pw = gdk_pixbuf_get_width (pb);
        j->ph = gdk_pixbuf_get_height (pb);
        j->stride = gdk_pixbuf_get_rowstride (pb);
        j->alpha = gdk_pixbuf_get_has_alpha (pb) && gdk_pixbuf_get_n_channels (pb) == 4;
        if (gdk_pixbuf_get_n_channels (pb) == (j->alpha ? 4 : 3) &&
            gdk_pixbuf_get_bits_per_sample (pb) == 8)
            j->px = gdk_pixbuf_read_pixel_bytes (pb);
        g_object_unref (pb);
    }
out:
    g_main_context_invoke (NULL, deliver, j);
}

static gint job_order (gconstpointer a, gconstpointer b, gpointer u)
{
    (void) u;
    const Job *x = a, *y = b;
    if (x->anim != y->anim)
        return x->anim ? 1 : -1;                     /* stills first */
    return x->seq > y->seq ? -1 : x->seq < y->seq;   /* newest first */
}

/* ------------------------------------------------- animated previews
 * WalpeThumbAnim: the frames of one file at one size as a GdkPaintable
 * shared by every picture that shows it. It does not run by itself:
 * anim_scan() moves the ones that have a visible picture. */

#define WALPE_TYPE_THUMB_ANIM (walpe_thumb_anim_get_type ())
G_DECLARE_FINAL_TYPE (WalpeThumbAnim, walpe_thumb_anim, WALPE, THUMB_ANIM, GObject)

struct _WalpeThumbAnim {
    GObject parent;
    char *key;
    GdkTexture **tex;
    int *delay;
    int n, cur, w, h;
    guint total_ms, pos_ms;
    gint64 t0_us;
    gsize bytes;
    gboolean want, playing;
    GList *link;            /* in anim_lru (head = shown most recently) */
};

static void anim_snapshot (GdkPaintable *p, GdkSnapshot *snap, double w, double h)
{
    WalpeThumbAnim *a = WALPE_THUMB_ANIM (p);
    gdk_paintable_snapshot (GDK_PAINTABLE (a->tex[a->cur]), snap, w, h);
}

static GdkPaintable *anim_current_image (GdkPaintable *p)
{
    WalpeThumbAnim *a = WALPE_THUMB_ANIM (p);
    return GDK_PAINTABLE (g_object_ref (a->tex[a->cur]));
}

static int anim_width (GdkPaintable *p)
{
    return WALPE_THUMB_ANIM (p)->w;
}

static int anim_height (GdkPaintable *p)
{
    return WALPE_THUMB_ANIM (p)->h;
}

static void anim_paintable_init (GdkPaintableInterface *i)
{
    i->snapshot = anim_snapshot;
    i->get_current_image = anim_current_image;
    i->get_intrinsic_width = anim_width;
    i->get_intrinsic_height = anim_height;
}

G_DEFINE_FINAL_TYPE_WITH_CODE (WalpeThumbAnim, walpe_thumb_anim, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (GDK_TYPE_PAINTABLE, anim_paintable_init))

static void walpe_thumb_anim_finalize (GObject *o)
{
    WalpeThumbAnim *a = WALPE_THUMB_ANIM (o);
    for (int i = 0; i < a->n; i++)
        g_clear_object (&a->tex[i]);
    g_free (a->tex);
    g_free (a->delay);
    g_free (a->key);
    G_OBJECT_CLASS (walpe_thumb_anim_parent_class)->finalize (o);
}

static void walpe_thumb_anim_class_init (WalpeThumbAnimClass *k)
{
    G_OBJECT_CLASS (k)->finalize = walpe_thumb_anim_finalize;
}

static void walpe_thumb_anim_init (WalpeThumbAnim *a)
{
    (void) a;
}

/* a picture that may show an animation */
typedef struct {
    GtkPicture *pic;        /* not owned: the entry goes when the picture does */
    char *akey;             /* animation: "a|<mode>|<w>x<h>|<file>" */
    char *src;
    int w, h;
    ThumbMode mode;
    char *skey;             /* thumbs_set: the still's key; NULL = thumbs_anim_watch */
    GdkPaintable *still;    /* watch: the caller's still while the animation shows */
    gboolean hover;
    gboolean showing;       /* the picture shows the animation now */
} APic;

static int anim_mode = WALPE_PREVIEW_ALWAYS;
static gsize anim_budget = 48u << 20;
static GHashTable *anims;      /* akey -> WalpeThumbAnim (owned) */
static GHashTable *apics;      /* GtkPicture -> APic */
static GHashTable *deferred;   /* akey set: decoded once, no room while everything shown plays */
static GHashTable *hooked;     /* windows / adjustments / pictures whose signals kick the scan */
static GQueue anim_lru = G_QUEUE_INIT;
static gsize anim_bytes;
static guint anim_tick_id, anim_kick_id;

static void init (void);
static void request_still (GtkPicture *pic, const char *key, const char *src, int w, int h,
                           ThumbMode mode);

static void apic_free (gpointer p)
{
    APic *ap = p;
    g_free (ap->akey);
    g_free (ap->src);
    g_free (ap->skey);
    g_clear_object (&ap->still);
    g_free (ap);
}

static void hooked_gone (gpointer data, GObject *where)
{
    (void) data;
    if (hooked)
        g_hash_table_remove (hooked, where);
}

static void on_notify_kick (GObject *o, GParamSpec *ps, gpointer u)
{
    (void) o; (void) ps; (void) u;
    thumbs_anim_kick ();
}

static void on_any_kick (gpointer o, gpointer u)
{
    (void) o; (void) u;
    thumbs_anim_kick ();
}

/* window focus, scrolling and mapping decide what can be seen: follow them
 * (once per window / adjustment / picture) */
static void hook_object (gpointer obj, const char *sig, GCallback cb)
{
    if (!obj || g_hash_table_contains (hooked, obj))
        return;
    g_hash_table_add (hooked, obj);
    g_signal_connect (obj, sig, cb, NULL);
    g_object_weak_ref (obj, hooked_gone, NULL);
}

static void hook_env (APic *ap)
{
    GtkWidget *w = GTK_WIDGET (ap->pic);
    GtkRoot *root = gtk_widget_get_root (w);
    if (root && GTK_IS_WINDOW (root))
        hook_object (root, "notify::is-active", G_CALLBACK (on_notify_kick));
    GtkWidget *sw = gtk_widget_get_ancestor (w, GTK_TYPE_SCROLLED_WINDOW);
    if (sw) {
        hook_object (gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (sw)),
                     "value-changed", G_CALLBACK (on_any_kick));
        hook_object (gtk_scrolled_window_get_hadjustment (GTK_SCROLLED_WINDOW (sw)),
                     "value-changed", G_CALLBACK (on_any_kick));
    }
}

static gboolean pic_can_play (APic *ap)
{
    if (anim_mode == WALPE_PREVIEW_OFF)
        return FALSE;
    GtkWidget *w = GTK_WIDGET (ap->pic);
    if (!gtk_widget_get_mapped (w))
        return FALSE;       /* another tab, a hidden window */
    GtkRoot *root = gtk_widget_get_root (w);
    if (!root || !GTK_IS_WINDOW (root) || !gtk_window_is_active (GTK_WINDOW (root)))
        return FALSE;       /* minimised, or another window has the focus */
    if (anim_mode == WALPE_PREVIEW_HOVER && !ap->hover)
        return FALSE;
    GtkWidget *sw = gtk_widget_get_ancestor (w, GTK_TYPE_SCROLLED_WINDOW);
    if (sw) {
        /* list views keep rows above and below the viewport alive */
        graphene_rect_t r;
        if (!gtk_widget_compute_bounds (w, sw, &r))
            return FALSE;
        if (r.origin.x + r.size.width <= 0 || r.origin.y + r.size.height <= 0 ||
            r.origin.x >= gtk_widget_get_width (sw) || r.origin.y >= gtk_widget_get_height (sw))
            return FALSE;
    }
    return TRUE;
}

/* back to the still picture */
static void apic_show_still (APic *ap)
{
    if (!ap->showing)
        return;
    ap->showing = FALSE;
    if (ap->skey) {
        MemEnt *e = g_hash_table_lookup (mem, ap->skey);
        if (e)
            gtk_picture_set_paintable (ap->pic, GDK_PAINTABLE (e->tex));
        else
            request_still (ap->pic, ap->skey, ap->src, ap->w, ap->h, ap->mode);
    } else {
        gtk_picture_set_paintable (ap->pic, ap->still);
        g_clear_object (&ap->still);
    }
}

static void anim_drop (WalpeThumbAnim *a)
{
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init (&it, apics);
    while (g_hash_table_iter_next (&it, &k, &v)) {
        APic *ap = v;
        if (ap->showing && !strcmp (ap->akey, a->key))
            apic_show_still (ap);
    }
    anim_bytes -= a->bytes;
    g_queue_delete_link (&anim_lru, a->link);
    g_hash_table_remove (anims, a->key);    /* drops the table's reference */
}

/* over the budget: the animations shown longest ago go, never one that
 * plays; when everything plays the newcomer (LRU head, not playing yet)
 * is the one without room */
static void anim_trim (void)
{
    while (anim_bytes > anim_budget) {
        WalpeThumbAnim *victim = NULL;
        for (GList *l = anim_lru.tail; l && !victim; l = l->prev)
            if (!((WalpeThumbAnim *) l->data)->playing)
                victim = l->data;
        if (!victim)
            break;
        if (victim->link == anim_lru.head)
            g_hash_table_add (deferred, g_strdup (victim->key));
        anim_drop (victim);
    }
}

static void anim_request (APic *ap)
{
    Job *j = g_new0 (Job, 1);
    j->key = g_strdup (ap->akey);
    j->src = g_strdup (ap->src);
    j->w = ap->w;
    j->h = ap->h;
    j->mode = ap->mode;
    j->anim = TRUE;
    j->seq = ++seq_ctr;
    g_atomic_int_set (&j->wanted, 1);
    g_hash_table_insert (inflight, j->key, j);
    g_thread_pool_push (pool, j, NULL);
}

/* which animations have a picture that can be seen: take those pictures
 * over, ask for missing frames, move the clocks. TRUE = something plays. */
static gboolean anim_scan (void)
{
    if (!apics)
        return FALSE;
    gint64 now = g_get_monotonic_time ();
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init (&it, apics);
    while (g_hash_table_iter_next (&it, &k, &v)) {
        APic *ap = v;
        hook_env (ap);
        if (!pic_can_play (ap))
            continue;
        WalpeThumbAnim *a = g_hash_table_lookup (anims, ap->akey);
        if (a) {
            a->want = TRUE;
            if (!ap->showing) {
                if (!ap->skey) {
                    GdkPaintable *cur = gtk_picture_get_paintable (ap->pic);
                    g_set_object (&ap->still, cur);
                }
                gtk_picture_set_paintable (ap->pic, GDK_PAINTABLE (a));
                ap->showing = TRUE;
            }
        } else if (!g_hash_table_contains (failed, ap->akey) &&
                   !g_hash_table_contains (deferred, ap->akey)) {
            Job *j = g_hash_table_lookup (inflight, ap->akey);
            if (j)
                g_atomic_int_set (&j->wanted, 1);
            else
                anim_request (ap);
        }
    }
    gboolean any = FALSE;
    g_hash_table_iter_init (&it, anims);
    while (g_hash_table_iter_next (&it, &k, &v)) {
        WalpeThumbAnim *a = v;
        if (!a->want) {
            a->playing = FALSE;
            continue;
        }
        a->want = FALSE;
        any = TRUE;
        if (!a->playing) {
            /* goes on where it stopped */
            a->playing = TRUE;
            a->t0_us = now - (gint64) a->pos_ms * 1000;
            g_queue_unlink (&anim_lru, a->link);
            g_queue_push_head_link (&anim_lru, a->link);
        }
        a->pos_ms = (guint) (((now - a->t0_us) / 1000) % a->total_ms);
        int f = 0;
        guint acc = 0;
        while (f < a->n - 1 && acc + (guint) a->delay[f] <= a->pos_ms)
            acc += (guint) a->delay[f++];
        if (f != a->cur) {
            a->cur = f;
            gdk_paintable_invalidate_contents (GDK_PAINTABLE (a));
        }
    }
    return any;
}

/* The window lost the focus (or was hidden, or the tiles in view are all
 * stills) and stayed that way: the decoded frames - up to the whole
 * budget - are of no use to a GUI sitting in the background. The pictures
 * show their stills; the frames are decoded again when they play next. */
static guint anim_idle_id;

static gboolean anim_idle_drop (gpointer u)
{
    (void) u;
    anim_idle_id = 0;
    if (anim_tick_id)
        return G_SOURCE_REMOVE;     /* playing again */
    while (anim_lru.head)
        anim_drop (anim_lru.head->data);
    malloc_trim (0);
    return G_SOURCE_REMOVE;
}

static void anim_idle_arm (gboolean playing)
{
    if (playing) {
        if (anim_idle_id)
            g_source_remove (anim_idle_id);
        anim_idle_id = 0;
    } else if (!anim_idle_id && anim_lru.head) {
        anim_idle_id = g_timeout_add_seconds (ANIM_IDLE_DROP_S, anim_idle_drop, NULL);
    }
}

static gboolean anim_tick (gpointer u)
{
    (void) u;
    if (anim_scan ())
        return G_SOURCE_CONTINUE;
    anim_tick_id = 0;       /* nothing visible plays: no timer */
    anim_idle_arm (FALSE);
    return G_SOURCE_REMOVE;
}

static gboolean anim_kick_idle (gpointer u)
{
    (void) u;
    anim_kick_id = 0;
    gboolean any = anim_scan ();
    if (any && !anim_tick_id) {
        anim_tick_id = g_timeout_add (ANIM_TICK_MS, anim_tick, NULL);
    } else if (!any && anim_tick_id) {
        g_source_remove (anim_tick_id);
        anim_tick_id = 0;
    }
    anim_idle_arm (any);
    return G_SOURCE_REMOVE;
}

void thumbs_anim_kick (void)
{
    if (apics && g_hash_table_size (apics) && !anim_kick_id)
        anim_kick_id = g_idle_add_full (G_PRIORITY_DEFAULT_IDLE, anim_kick_idle, NULL, NULL);
}

static void pic_gone (gpointer data, GObject *where)
{
    (void) data;
    if (apics)
        g_hash_table_remove (apics, where);
}

static void on_hover (GtkEventControllerMotion *c, double x, double y, gpointer pic)
{
    (void) c; (void) x; (void) y;
    APic *ap = apics ? g_hash_table_lookup (apics, pic) : NULL;
    g_object_set_data (G_OBJECT (pic), "walpe-thumb-hover", GINT_TO_POINTER (1));
    if (ap) {
        ap->hover = TRUE;
        thumbs_anim_kick ();
    }
}

static void on_unhover (GtkEventControllerMotion *c, gpointer pic)
{
    (void) c;
    APic *ap = apics ? g_hash_table_lookup (apics, pic) : NULL;
    g_object_set_data (G_OBJECT (pic), "walpe-thumb-hover", NULL);
    if (ap) {
        ap->hover = FALSE;
        thumbs_anim_kick ();
    }
}

/* pic no longer shows this file */
static void anim_forget (GtkPicture *pic)
{
    APic *ap = apics ? g_hash_table_lookup (apics, pic) : NULL;
    if (!ap)
        return;
    /* frames still being decoded for nobody: the worker stops */
    Job *j = g_hash_table_lookup (inflight, ap->akey);
    if (j && j->anim) {
        gboolean other = FALSE;
        GHashTableIter it;
        gpointer k, v;
        g_hash_table_iter_init (&it, apics);
        while (!other && g_hash_table_iter_next (&it, &k, &v))
            other = v != ap && !strcmp (((APic *) v)->akey, ap->akey);
        if (!other)
            g_atomic_int_set (&j->wanted, 0);
    }
    g_hash_table_remove_all (deferred);     /* room may have been made */
    g_object_weak_unref (G_OBJECT (pic), pic_gone, NULL);
    g_hash_table_remove (apics, pic);
    thumbs_anim_kick ();
}

static void anim_register (GtkPicture *pic, const char *src, int w, int h, ThumbMode mode,
                           const char *skey)
{
    char *akey = g_strdup_printf ("a|%d|%dx%d|%s", (int) mode, w, h, src);
    APic *ap = g_hash_table_lookup (apics, pic);
    if (ap && !strcmp (ap->akey, akey)) {
        g_free (akey);
        thumbs_anim_kick ();
        return;
    }
    if (ap)
        anim_forget (pic);
    ap = g_new0 (APic, 1);
    ap->pic = pic;
    ap->akey = akey;
    ap->src = g_strdup (src);
    ap->w = w;
    ap->h = h;
    ap->mode = mode;
    ap->skey = g_strdup (skey);
    ap->hover = g_object_get_data (G_OBJECT (pic), "walpe-thumb-hover") != NULL;
    g_hash_table_insert (apics, pic, ap);
    g_object_weak_ref (G_OBJECT (pic), pic_gone, NULL);
    if (!g_hash_table_contains (hooked, pic)) {
        /* once per picture: mapping (tab switches) and the pointer */
        g_hash_table_add (hooked, pic);
        g_object_weak_ref (G_OBJECT (pic), hooked_gone, NULL);
        g_signal_connect (pic, "map", G_CALLBACK (on_any_kick), NULL);
        g_signal_connect (pic, "unmap", G_CALLBACK (on_any_kick), NULL);
        GtkEventController *mc = gtk_event_controller_motion_new ();
        g_signal_connect (mc, "enter", G_CALLBACK (on_hover), pic);
        g_signal_connect (mc, "leave", G_CALLBACK (on_unhover), pic);
        gtk_widget_add_controller (GTK_WIDGET (pic), mc);
    }
    thumbs_anim_kick ();
}

static gboolean is_gif_name (const char *src)
{
    gsize l = strlen (src);
    return l > 4 && !g_ascii_strcasecmp (src + l - 4, ".gif");
}

/* main thread: the frames arrived (or did not) */
static void deliver_anim (Job *j)
{
    if (j->px && j->n >= 2) {
        WalpeThumbAnim *a = g_object_new (WALPE_TYPE_THUMB_ANIM, NULL);
        a->key = g_strdup (j->key);
        a->n = j->n;
        a->w = j->pw;
        a->h = j->ph;
        a->tex = g_new0 (GdkTexture *, j->n);
        a->delay = j->delay;
        j->delay = NULL;
        gsize fb = (gsize) j->stride * j->ph;
        for (int i = 0; i < j->n; i++) {
            GBytes *b = g_bytes_new_from_bytes (j->px, (gsize) i * fb, fb);
            a->tex[i] = gdk_memory_texture_new (j->pw, j->ph,
                                                j->alpha ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8,
                                                b, (gsize) j->stride);
            g_bytes_unref (b);
            a->total_ms += (guint) MAX (1, a->delay[i]);
        }
        a->bytes = g_bytes_get_size (j->px);
        g_queue_push_head (&anim_lru, a);
        a->link = anim_lru.head;
        g_hash_table_insert (anims, a->key, a);
        anim_bytes += a->bytes;
        anim_trim ();
    } else if (!j->skipped) {
        g_hash_table_add (failed, g_strdup (j->key));   /* a still, or not decodable */
    }
    thumbs_anim_kick ();
}

/* -------------------------------------------------------- main thread */

static void mem_trim (void)
{
    while (mem_bytes > MEM_BUDGET && lru.tail) {
        char *k = lru.tail->data;
        MemEnt *e = g_hash_table_lookup (mem, k);
        g_queue_delete_link (&lru, lru.tail);
        if (e) {
            mem_bytes -= e->bytes;
            g_hash_table_remove (mem, k);   /* frees k (the hash key) and e */
        }
    }
}

static void apply_to (GtkPicture *pic, const char *key, GdkTexture *tex)
{
    if (g_strcmp0 (g_object_get_data (G_OBJECT (pic), KEY_DATA), key))
        return;   /* recycled for another wallpaper meanwhile */
    APic *ap = g_hash_table_lookup (apics, pic);
    if (ap && ap->showing)
        return;   /* its animation is on already */
    gtk_picture_set_paintable (pic, tex ? GDK_PAINTABLE (tex) : NULL);
}

static gboolean deliver (gpointer data)
{
    Job *j = data;
    g_hash_table_steal (inflight, j->key);
    if (j->anim) {
        deliver_anim (j);
        job_free (j);
        return G_SOURCE_REMOVE;
    }
    GdkTexture *tex = NULL;
    if (j->px) {
        tex = gdk_memory_texture_new (j->pw, j->ph,
                                      j->alpha ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8,
                                      j->px, (gsize) j->stride);
        MemEnt *e = g_new0 (MemEnt, 1);
        e->tex = tex;
        e->bytes = g_bytes_get_size (j->px);
        char *k = g_strdup (j->key);
        g_queue_push_head (&lru, k);
        e->link = lru.head;
        g_hash_table_replace (mem, k, e);
        mem_bytes += e->bytes;
    } else if (g_atomic_int_get (&j->wanted) > 0) {
        g_hash_table_add (failed, g_strdup (j->key));
    }
    for (guint i = 0; i < j->targets->len; i++) {
        GtkPicture *pic = g_weak_ref_get (g_ptr_array_index (j->targets, i));
        if (pic) {
            apply_to (pic, j->key, tex);
            g_object_unref (pic);
        }
    }
    job_free (j);
    mem_trim ();
    return G_SOURCE_REMOVE;
}

static void init (void)
{
    if (pool)
        return;
    mem = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, mem_free);
    failed = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    inflight = g_hash_table_new (g_str_hash, g_str_equal);
    anims = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, g_object_unref);
    apics = g_hash_table_new_full (NULL, NULL, NULL, apic_free);
    deferred = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    hooked = g_hash_table_new (NULL, NULL);
    const char *mb = g_getenv ("WALPE_UI_ANIM_MB");     /* tests: frame budget */
    if (mb && *mb)
        anim_budget = (gsize) CLAMP (g_ascii_strtoull (mb, NULL, 10), 1, 4096) << 20;
    cache_dir = g_build_filename (g_get_user_cache_dir (), "walpe", "thumbs", NULL);
    /* private: the thumbnails show what is in the user's library (the
     * Workshop cache next to it always was 0700) */
    g_mkdir_with_parents (cache_dir, 0700);
    g_chmod (cache_dir, 0700);
    pool = g_thread_pool_new (work, NULL, 3, FALSE, NULL);
    g_thread_pool_set_sort_function (pool, job_order, NULL);
}

/* the job pic was waiting for no longer needs it */
static void release_old (GtkPicture *pic)
{
    const char *old = g_object_get_data (G_OBJECT (pic), KEY_DATA);
    Job *oj = old ? g_hash_table_lookup (inflight, old) : NULL;
    if (oj)
        g_atomic_int_add (&oj->wanted, -1);
}

void thumbs_clear (GtkPicture *pic)
{
    if (!pool)
        init ();
    anim_forget (pic);
    release_old (pic);
    g_object_set_data (G_OBJECT (pic), KEY_DATA, NULL);
    gtk_picture_set_paintable (pic, NULL);
}

/* the still for key on pic: from memory, else from the worker pool
 * (pic's KEY_DATA is key already) */
static void request_still (GtkPicture *pic, const char *key, const char *src, int w, int h,
                           ThumbMode mode)
{
    MemEnt *e = g_hash_table_lookup (mem, key);
    if (e) {
        /* LRU touch */
        g_queue_unlink (&lru, e->link);
        g_queue_push_head_link (&lru, e->link);
        gtk_picture_set_paintable (pic, GDK_PAINTABLE (e->tex));
        return;
    }
    gtk_picture_set_paintable (pic, NULL);
    if (g_hash_table_contains (failed, key))
        return;
    Job *j = g_hash_table_lookup (inflight, key);
    if (!j) {
        j = g_new0 (Job, 1);
        j->key = g_strdup (key);
        j->src = g_strdup (src);
        j->w = w;
        j->h = h;
        j->mode = mode;
        j->targets = g_ptr_array_new_with_free_func (weak_free);
        g_hash_table_insert (inflight, j->key, j);
        j->seq = ++seq_ctr;
        g_atomic_int_set (&j->wanted, 1);
        g_thread_pool_push (pool, j, NULL);
    } else {
        g_atomic_int_add (&j->wanted, 1);
    }
    GWeakRef *wr = g_new0 (GWeakRef, 1);
    g_weak_ref_init (wr, pic);
    g_ptr_array_add (j->targets, wr);
}

void thumbs_set (GtkPicture *pic, const char *src, int w, int h, ThumbMode mode)
{
    init ();
    if (!src || !*src || w < 1 || h < 1) {
        thumbs_clear (pic);
        return;
    }
    /* the key names the file as it is now (time, size): a preview that was
     * replaced or was still being copied gets a new picture, and a failed
     * decode is tried again once the file has changed */
    struct stat kst;
    long long kt = 0, ks = 0;
    if (g_stat (src, &kst) == 0) {
        kt = (long long) kst.st_mtime;
        ks = (long long) kst.st_size;
    }
    char *key = g_strdup_printf ("%d|%dx%d|%lld|%lld|%s", (int) mode, w, h, kt, ks, src);
    const char *cur = g_object_get_data (G_OBJECT (pic), KEY_DATA);
    if (!g_strcmp0 (cur, key) && gtk_picture_get_paintable (pic)) {
        g_free (key);
        return;   /* already showing it */
    }
    anim_forget (pic);
    release_old (pic);
    g_object_set_data_full (G_OBJECT (pic), KEY_DATA, g_strdup (key), g_free);
    request_still (pic, key, src, w, h, mode);
    /* an animated preview plays on top of its first frame once it can be
     * seen (registered in every mode: switching the setting works live) */
    if (is_gif_name (src))
        anim_register (pic, src, w, h, mode, key);
    g_free (key);
}

void thumbs_anim_watch (GtkPicture *pic, const char *file, int w, int h, ThumbMode mode)
{
    init ();
    if (!file || !*file || w < 1 || h < 1) {
        thumbs_anim_unwatch (pic);
        return;
    }
    anim_register (pic, file, w, h, mode, NULL);
}

void thumbs_anim_unwatch (GtkPicture *pic)
{
    if (!pool)
        return;
    APic *ap = g_hash_table_lookup (apics, pic);
    if (ap && ap->showing && !ap->skey)
        g_clear_object (&ap->still);    /* the caller sets its own picture next */
    anim_forget (pic);
}

void thumbs_set_anim_mode (int mode)
{
    init ();
    mode = CLAMP (mode, WALPE_PREVIEW_ALWAYS, WALPE_PREVIEW_OFF);
    if (mode == anim_mode)
        return;
    anim_mode = mode;
    g_hash_table_remove_all (deferred);
    if (mode == WALPE_PREVIEW_OFF) {
        /* stills everywhere, and the frames are given back */
        while (anim_lru.head)
            anim_drop (anim_lru.head->data);
    }
    thumbs_anim_kick ();
}

int thumbs_get_anim_mode (void)
{
    return anim_mode;
}

void thumbs_anim_stats (guint *loaded, guint *playing, gsize *bytes)
{
    guint p = 0;
    if (anims) {
        GHashTableIter it;
        gpointer k, v;
        g_hash_table_iter_init (&it, anims);
        while (g_hash_table_iter_next (&it, &k, &v))
            p += ((WalpeThumbAnim *) v)->playing;
    }
    if (loaded)
        *loaded = anims ? g_hash_table_size (anims) : 0;
    if (playing)
        *playing = p;
    if (bytes)
        *bytes = anim_bytes;
}
