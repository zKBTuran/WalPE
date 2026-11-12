/* thumbs_test - the GUI's preview decoder (src/ui/thumbs.c) on data it
 * cannot trust, without a display: the GIF block parser that counts frames
 * (cut the file anywhere: no crash, no read past the end), and the frames
 * of an animation at tile size (count, delays, bytes, limits); a GIF with
 * an image outside its picture gives neither frames nor a still. */
#include "../src/ui/thumbs.c"
#include "common/util.h"

static int fails;
#define CHECK(c) do { if (!(c)) { g_printerr ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* is gdk-pixbuf's loader module for this format in the process? (They are
 * loaded when a file is first handed to them and stay. A loader built into
 * gdk-pixbuf itself never shows up here.) */
static gboolean loader_module_mapped (const char *format)
{
    char *maps = NULL, *a = g_strdup_printf ("pixbufloader-%s.", format),
         *b = g_strdup_printf ("pixbufloader_%s.", format);
    gboolean found = g_file_get_contents ("/proc/self/maps", &maps, NULL, NULL) &&
                     (strstr (maps, a) || strstr (maps, b));
    g_free (maps);
    g_free (a);
    g_free (b);
    return found;
}

typedef struct { int calls, w, h, to_w, to_h; gboolean refuse; } SizeSeen;

static gboolean size_seen (const char *kind, int w, int h, int *sw, int *sh, gpointer user)
{
    SizeSeen *ss = user;
    (void) kind;
    ss->calls++;
    ss->w = w;
    ss->h = h;
    *sw = ss->to_w;
    *sh = ss->to_h;
    return !ss->refuse;
}

/* 8 x 6, five frames of 30, 100, 10, 60, 200 ms */
static const guint8 test_gif[] = {
    0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x08, 0x00, 0x06, 0x00, 0x81, 0x00, 0x00, 0xff, 0xff, 0xff,
    0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0xff, 0x0b, 0x4e, 0x45, 0x54, 0x53,
    0x43, 0x41, 0x50, 0x45, 0x32, 0x2e, 0x30, 0x03, 0x01, 0x00, 0x00, 0x00, 0x21, 0xf9, 0x04, 0x04,
    0x03, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x06, 0x00, 0x00, 0x08, 0x0f,
    0x00, 0x03, 0x08, 0x1c, 0x28, 0x10, 0x00, 0xc1, 0x83, 0x08, 0x13, 0x2a, 0x5c, 0x18, 0x10, 0x00,
    0x21, 0xf9, 0x04, 0x05, 0x0a, 0x00, 0x02, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x06,
    0x00, 0x81, 0xff, 0xff, 0xff, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x0f,
    0x00, 0x03, 0x08, 0x1c, 0x38, 0x10, 0x00, 0xc1, 0x83, 0x08, 0x13, 0x2a, 0x54, 0x18, 0x10, 0x00,
    0x21, 0xf9, 0x04, 0x05, 0x01, 0x00, 0x02, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x06,
    0x00, 0x81, 0xff, 0xff, 0xff, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x0f,
    0x00, 0x03, 0x08, 0x1c, 0x48, 0x10, 0x00, 0xc1, 0x83, 0x08, 0x13, 0x2a, 0x4c, 0x18, 0x10, 0x00,
    0x21, 0xf9, 0x04, 0x05, 0x06, 0x00, 0x02, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x06,
    0x00, 0x81, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x10,
    0x00, 0x03, 0x08, 0x1c, 0x48, 0x30, 0x00, 0x80, 0x82, 0x08, 0x13, 0x2a, 0x5c, 0x18, 0x20, 0x20,
    0x00, 0x21, 0xf9, 0x04, 0x05, 0x14, 0x00, 0x02, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00,
    0x06, 0x00, 0x81, 0xff, 0xff, 0xff, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08,
    0x0f, 0x00, 0x03, 0x08, 0x1c, 0x48, 0x50, 0x20, 0x80, 0x82, 0x08, 0x13, 0x2a, 0x5c, 0x18, 0x10,
    0x00, 0x3b,
};

/* A JPEG of w x h written by hand. kind 0: progressive, one AC scan per
 * component in which every block lies inside an end-of-band run (15 zero
 * bits cover 16384 blocks) - a few hundred bytes that a decoder has to
 * hold whole at full size. kind 1: sequential with a scan per component
 * (held whole as well). kind 2: sequential, one scan for all components
 * (decoded as it is read). sub: the first component sampled 2 x 2. */
static GByteArray *hand_jpeg (guint w, guint h, guint ncomp, int kind, gboolean sub)
{
    GByteArray *o = g_byte_array_new ();
    #define SEG(m, body, n) do { guint8 hd_[4] = { 0xFF, (m), (guint8) (((n) + 2) >> 8), (guint8) ((n) + 2) }; \
                                 g_byte_array_append (o, hd_, 4); g_byte_array_append (o, (body), (n)); } while (0)
    g_byte_array_append (o, (const guint8 *) "\xFF\xD8", 2);
    guint8 dqt[65];
    memset (dqt, 1, sizeof dqt);
    dqt[0] = 0;
    SEG (0xDB, dqt, 65);
    guint8 sof[6 + 3 * 4] = { 8, (guint8) (h >> 8), (guint8) h, (guint8) (w >> 8), (guint8) w, (guint8) ncomp };
    for (guint c = 0; c < ncomp; c++) {
        sof[6 + 3 * c] = (guint8) (c + 1);
        sof[7 + 3 * c] = sub && c == 0 ? 0x22 : 0x11;
        sof[8 + 3 * c] = 0;
    }
    SEG (kind == 0 ? 0xC2 : 0xC0, sof, 6 + 3 * ncomp);
    /* AC table 0: one code, '0', for symbol 0xE0 (an end-of-band run of
     * 2^14 blocks plus 14 more bits); DC table 0: one code for "no change" */
    guint8 dht[18] = { 0x10, 1 };
    dht[17] = kind == 0 ? 0xE0 : 0x00;
    SEG (0xC4, dht, 18);
    dht[0] = 0x00;
    dht[17] = 0x00;
    SEG (0xC4, dht, 18);
    if (kind == 2) {
        guint8 sos[1 + 2 * 4 + 3] = { (guint8) ncomp };
        for (guint c = 0; c < ncomp; c++)
            sos[1 + 2 * c] = (guint8) (c + 1);
        sos[1 + 2 * ncomp + 1] = 63;
        SEG (0xDA, sos, 1 + 2 * ncomp + 3);
    } else {
        guint64 blocks = (guint64) ((w + 7) / 8) * ((h + 7) / 8) * (sub ? 4 : 1);
        guint zeros = (guint) (((blocks + 16383) / 16384 * 15 + 7) / 8 + 2);
        guint8 *z = g_malloc0 (zeros);
        for (guint c = 0; c < ncomp; c++) {
            guint8 sos[6] = { 1, (guint8) (c + 1), 0, (guint8) (kind == 0 ? 1 : 0), 63, 0 };
            SEG (0xDA, sos, 6);
            if (kind == 0)
                g_byte_array_append (o, z, zeros);
        }
        g_free (z);
    }
    g_byte_array_append (o, (const guint8 *) "\xFF\xD9", 2);
    #undef SEG
    return o;
}

static Job *anim_job (const char *path, int w, int h, ThumbMode mode)
{
    Job *j = g_new0 (Job, 1);
    j->key = g_strdup ("k");
    j->src = g_strdup (path);
    j->w = w;
    j->h = h;
    j->mode = mode;
    j->anim = TRUE;
    g_atomic_int_set (&j->wanted, 1);
    return j;
}

int main (void)
{
    int n = 0, w = 0, h = 0;
    /* a window that is not on screen plays no preview, wherever the focus
     * is: minimised (what an X window unmapped by its window manager is
     * to GDK), suspended (a compositor's "cannot be seen") */
    CHECK (thumbs_state_hidden (GDK_TOPLEVEL_STATE_MINIMIZED));
    CHECK (thumbs_state_hidden (GDK_TOPLEVEL_STATE_MINIMIZED | GDK_TOPLEVEL_STATE_FOCUSED));
#if GTK_CHECK_VERSION (4, 12, 0)
    CHECK (thumbs_state_hidden (GDK_TOPLEVEL_STATE_SUSPENDED | GDK_TOPLEVEL_STATE_FOCUSED));
#endif
    CHECK (!thumbs_state_hidden (0));
    CHECK (!thumbs_state_hidden (GDK_TOPLEVEL_STATE_FOCUSED | GDK_TOPLEVEL_STATE_MAXIMIZED |
                                 GDK_TOPLEVEL_STATE_FULLSCREEN | GDK_TOPLEVEL_STATE_TILED));
    CHECK (!thumbs_toplevel_hidden (NULL));
    int *d = gif_frame_delays (test_gif, sizeof (test_gif), &n, &w, &h);
    CHECK (d && n == 5 && w == 8 && h == 6);
    /* gdk-pixbuf's rule for the third frame: under 20 ms plays as 20 */
    CHECK (d && d[0] == 30 && d[1] == 100 && d[2] == 20 && d[3] == 60 && d[4] == 200);
    g_free (d);
    /* every truncation, in a buffer of exactly that size (ASan / valgrind
     * see any read past it) */
    for (gsize len = 0; len < sizeof (test_gif); len++) {
        guint8 *cut = g_malloc (len ? len : 1);
        memcpy (cut, test_gif, len);
        int n2 = 0;
        int *d2 = gif_frame_delays (cut, len, &n2, &w, &h);
        CHECK (!d2 || (n2 >= 1 && n2 <= 5));
        g_free (d2);
        g_free (cut);
    }
    /* not a GIF; a sub-block length that points far behind the end */
    CHECK (!gif_frame_delays ((const guint8 *) "PNG and then some bytes", 23, &n, &w, &h));
    {
        guint8 bad[64];
        memcpy (bad, test_gif, sizeof (bad));
        memset (bad + 13 + 24, 0xff, sizeof (bad) - 13 - 24);
        d = gif_frame_delays (bad, sizeof (bad), &n, &w, &h);
        g_free (d);
    }

    char *dir = g_dir_make_tmp ("walpe-thumbs-test-XXXXXX", NULL);
    char *path = g_build_filename (dir, "preview.gif", NULL);
    CHECK (g_file_set_contents (path, (const char *) test_gif, sizeof (test_gif), NULL));

    /* tile: fills 4 x 3 exactly; frames shorter than 50 ms are merged with
     * the next: (30 + 100), (10 -> 20 + 60), 200 ms: 5 source frames -> 3 */
    Job *j = anim_job (path, 4, 3, THUMB_COVER);
    decode_anim (j);
    CHECK (j->px && j->n == 3 && j->pw == 4 && j->ph == 3);
    CHECK (j->px && !j->alpha && j->stride == 12 && g_bytes_get_size (j->px) == (gsize) 3 * 4 * 3 * 3);
    CHECK (j->delay && j->delay[0] == 130 && j->delay[1] == 20 + 60 && j->delay[2] == 200);
    if (j->px) {
        /* last pixel of each kept frame: red (file frame 1), blue (3), cyan (5) */
        const guint8 *p = g_bytes_get_data (j->px, NULL);
        gsize fb = 4 * 3 * 3, last = fb - 3;
        CHECK (p[last] > 200 && p[last + 1] < 60 && p[last + 2] < 60);
        CHECK (p[fb + last] < 60 && p[fb + last + 1] < 60 && p[fb + last + 2] > 200);
        CHECK (p[2 * fb + last] < 60 && p[2 * fb + last + 1] > 200 && p[2 * fb + last + 2] > 200);
    }
    job_free (j);

    /* contain: never scaled up */
    j = anim_job (path, 400, 300, THUMB_CONTAIN);
    decode_anim (j);
    CHECK (j->px && j->pw == 8 && j->ph == 6 && j->n == 3);
    job_free (j);

    /* nobody waits for it any more: stops, and that is not a failure */
    j = anim_job (path, 4, 3, THUMB_COVER);
    g_atomic_int_set (&j->wanted, 0);
    decode_anim (j);
    CHECK (!j->px && j->skipped);
    job_free (j);

    /* a still picture and a file that is no GIF: no animation, no crash */
    char *still = g_build_filename (dir, "one.gif", NULL);
    {
        /* the same file cut after its first image block is a 1-frame GIF */
        int n1 = 0;
        gsize len;
        for (len = 20; len < sizeof (test_gif); len++) {
            int *d1 = gif_frame_delays (test_gif, len, &n1, &w, &h);
            g_free (d1);
            if (d1 && n1 == 1)
                break;
        }
        CHECK (g_file_set_contents (still, (const char *) test_gif, (gssize) len, NULL));
    }
    j = anim_job (still, 4, 3, THUMB_COVER);
    decode_anim (j);
    CHECK (!j->px && !j->skipped);
    job_free (j);
    CHECK (g_file_set_contents (still, "certainly not a picture", -1, NULL));
    j = anim_job (still, 4, 3, THUMB_COVER);
    decode_anim (j);
    CHECK (!j->px);
    job_free (j);

    /* a still thumbnail of the animated file is its first frame */
    j = anim_job (path, 4, 3, THUMB_COVER);
    j->anim = FALSE;
    GdkPixbuf *pb = decode (j);
    CHECK (pb && gdk_pixbuf_get_width (pb) == 4 && gdk_pixbuf_get_height (pb) == 3);
    g_clear_object (&pb);
    job_free (j);
    CHECK (is_gif_name ("a/preview.GIF") && !is_gif_name ("gif") && !is_gif_name ("a.gif.png"));

    /* an image that does not lie inside the picture (the first one, moved
     * one pixel to the right: 1 + 8 > 8): no frames are counted, the file
     * does not go to the animation decoder, and the still is refused as
     * well - the tile keeps its placeholder */
    {
        guint8 bad[sizeof (test_gif)];
        memcpy (bad, test_gif, sizeof (bad));
        CHECK (bad[52] == 0x2c && bad[53] == 0 && bad[57] == 8 && bad[59] == 6);
        bad[53] = 1;
        CHECK (!gif_frame_delays (bad, sizeof (bad), &n, &w, &h));
        char *off = g_build_filename (dir, "outside.gif", NULL);
        CHECK (g_file_set_contents (off, (const char *) bad, sizeof (bad), NULL));
        j = anim_job (off, 4, 3, THUMB_COVER);
        decode_anim (j);
        CHECK (!j->px && !j->skipped);
        j->anim = FALSE;
        pb = decode (j);
        CHECK (pb == NULL);
        g_clear_object (&pb);
        job_free (j);
        g_unlink (off);
        g_free (off);
    }

    /* Decoders: the picture formats the
     * library lists and no others. gdk-pixbuf chooses a loader by the
     * first bytes, not the name: a "preview.jpg" that is SVG, XPM or PNM
     * went to those loaders. Now such a file gives no thumbnail. */
    {
        CHECK (thumbs_format_allowed ("jpeg") && thumbs_format_allowed ("png") &&
               thumbs_format_allowed ("gif") && thumbs_format_allowed ("webp") &&
               thumbs_format_allowed ("bmp"));
        CHECK (!thumbs_format_allowed ("svg") && !thumbs_format_allowed ("xpm") &&
               !thumbs_format_allowed ("tga") && !thumbs_format_allowed ("ico") &&
               !thumbs_format_allowed ("tiff") && !thumbs_format_allowed ("pnm") &&
               !thumbs_format_allowed ("ani") && !thumbs_format_allowed (NULL));
        static const struct { const char *name, *text; } bad[] = {
            { "preview.jpg", "<?xml version=\"1.0\"?>\n<svg xmlns=\"http://www.w3.org/2000/svg\" "
                             "width=\"64\" height=\"36\"><rect width=\"64\" height=\"36\" "
                             "fill=\"red\"/></svg>\n" },
            { "preview.png", "/* XPM */\nstatic char *x[] = {\n\"2 2 1 1\",\n\". c #FF0000\",\n"
                             "\"..\",\n\"..\"\n};\n" },
            { "preview.gif", "P3\n2 2\n255\n255 0 0 255 0 0 255 0 0 255 0 0\n" },
        };
        for (guint i = 0; i < G_N_ELEMENTS (bad); i++) {
            char *f = g_build_filename (dir, bad[i].name, NULL);
            CHECK (g_file_set_contents (f, bad[i].text, -1, NULL));
            for (int mode = 0; mode < 2; mode++) {
                Job *bj = anim_job (f, 160, 90, mode ? THUMB_CONTAIN : THUMB_COVER);
                bj->anim = FALSE;
                GdkPixbuf *bp = decode (bj);
                CHECK (bp == NULL);
                g_clear_object (&bp);
                job_free (bj);
            }
            g_unlink (f);
            g_free (f);
        }
        /* Refused is not enough: asked which format a file is, gdk-pixbuf
         * hands it to the loader that matches its bytes, and that loader
         * has read it by the time the answer is there. None of these files
         * may have reached its loader at all. */
        CHECK (!loader_module_mapped ("svg"));
        CHECK (!loader_module_mapped ("xpm"));
        CHECK (!loader_module_mapped ("pnm"));
        /* ... and a real PNG or JPEG under any name still does */
        GdkPixbuf *src = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, 64, 36);
        gdk_pixbuf_fill (src, 0x208040ff);
        static const char *const kinds[] = { "png", "jpeg", "bmp" };
        for (guint i = 0; i < G_N_ELEMENTS (kinds); i++) {
            char *f = g_build_filename (dir, "preview.dat", NULL);
            CHECK (gdk_pixbuf_save (src, f, kinds[i], NULL, NULL));
            Job *gj = anim_job (f, 32, 18, THUMB_COVER);
            gj->anim = FALSE;
            GdkPixbuf *gp = decode (gj);
            CHECK (gp && gdk_pixbuf_get_width (gp) == 32 && gdk_pixbuf_get_height (gp) == 18);
            g_clear_object (&gp);
            job_free (gj);
            g_unlink (f);
            g_free (f);
        }
        g_object_unref (src);
    }

    /* the decoder is chosen by the signature and by the caller's list */
    {
        static const struct { const char *bytes; gsize n; const char *kind; } sig[] = {
            { "\xff\xd8\xff\xe0", 4, "jpeg" },
            { "\x89PNG\r\n\x1a\n", 8, "png" },
            { "GIF89a", 6, "gif" },
            { "GIF87a", 6, "gif" },
            { "RIFF\0\0\0\0WEBP", 12, "webp" },
            { "\0\0\0\x1c" "ftypavif\0\0\0\0", 16, "avif" },
            { "\0\0\0\x1c" "ftypmif1\0\0\0\0miafavif", 24, "avif" },
            { "\0\0\0\x18" "ftypheic\0\0\0\0mif1heic", 24, NULL },
            { "\0\0\0\x10" "ftypisom\0\0\0\0avif", 20, NULL },   /* past the box's end */
            { "BM\0\0", 4, "bmp" },
            { "RIFF\0\0\0\0ACON", 12, NULL },                      /* a Windows cursor */
            { "<?xml version=\"1.0\"?><svg", 24, NULL },
            { "/* XPM */", 9, NULL },
            { "P6\n1 1\n255\n", 11, NULL },
            { "\0\0\1\0\1\0", 6, NULL },                             /* ICO */
            { "II*\0", 4, NULL },                                     /* TIFF */
            { "GIF89", 5, NULL },
            { "", 0, NULL },
        };
        for (guint i = 0; i < G_N_ELEMENTS (sig); i++) {
            /* a buffer of exactly that size: nothing is read past it */
            guint8 *b = g_memdup2 (sig[i].n ? sig[i].bytes : "x", sig[i].n ? sig[i].n : 1);
            CHECK (!g_strcmp0 (walpe_picture_kind (b, sig[i].n), sig[i].kind));
            g_free (b);
        }
        CHECK (walpe_picture_kind (NULL, 0) == NULL);

        GdkPixbuf *src = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, 64, 36);
        gdk_pixbuf_fill (src, 0x208040ff);
        char *png = NULL;
        gsize plen = 0;
        CHECK (gdk_pixbuf_save_to_buffer (src, &png, &plen, "png", NULL, NULL));
        static const char *const all[] = { "jpeg", "png", "gif", "bmp", "webp", "avif", NULL };
        static const char *const no_png[] = { "jpeg", "gif", NULL };
        GError *e = NULL;
        SizeSeen ss = { 0, 0, 0, 16, 9, FALSE };
        GdkPixbuf *got = walpe_picture_decode ((const guint8 *) png, plen, all, size_seen, &ss, &e);
        CHECK (got && !e && ss.calls == 1 && ss.w == 64 && ss.h == 36);
        CHECK (got && gdk_pixbuf_get_width (got) == 16 && gdk_pixbuf_get_height (got) == 9);
        g_clear_object (&got);
        /* no size callback: as large as it is */
        got = walpe_picture_decode ((const guint8 *) png, plen, all, NULL, NULL, &e);
        CHECK (got && !e && gdk_pixbuf_get_width (got) == 64 && gdk_pixbuf_get_height (got) == 36);
        g_clear_object (&got);
        /* a format the caller does not list: not decoded, the callback not asked */
        ss.calls = 0;
        got = walpe_picture_decode ((const guint8 *) png, plen, no_png, size_seen, &ss, &e);
        CHECK (!got && g_error_matches (e, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED) && ss.calls == 0);
        g_clear_error (&e);
        got = walpe_picture_decode ((const guint8 *) png, plen, NULL, NULL, NULL, &e);
        CHECK (!got && g_error_matches (e, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED));
        g_clear_error (&e);
        /* the callback refuses: no picture */
        ss.refuse = TRUE;
        got = walpe_picture_decode ((const guint8 *) png, plen, all, size_seen, &ss, &e);
        CHECK (!got && g_error_matches (e, G_IO_ERROR, G_IO_ERROR_INVALID_DATA) && ss.calls == 1);
        g_clear_error (&e);
        ss.refuse = FALSE;
        /* cut anywhere (a buffer of exactly that size): a picture or an
         * error, never both, never neither */
        for (gsize len = 0; len < plen; len += len < 64 ? 1 : 13) {
            guint8 *cut = g_memdup2 (len ? png : "x", len ? len : 1);
            got = walpe_picture_decode (cut, len, all, size_seen, &ss, &e);
            CHECK ((got != NULL) != (e != NULL));
            g_clear_object (&got);
            g_clear_error (&e);
            g_free (cut);
        }
        /* the same from a file; a directory or a missing file is an error */
        char *f = g_build_filename (dir, "picture.dat", NULL);
        CHECK (g_file_set_contents (f, png, (gssize) plen, NULL));
        got = walpe_picture_decode_file (f, all, size_seen, &ss, &e);
        CHECK (got && !e && gdk_pixbuf_get_width (got) == 16 && gdk_pixbuf_get_height (got) == 9);
        g_clear_object (&got);
        got = walpe_picture_decode_file (f, no_png, size_seen, &ss, &e);
        CHECK (!got && g_error_matches (e, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED));
        g_clear_error (&e);
        g_unlink (f);
        got = walpe_picture_decode_file (f, all, NULL, NULL, &e);
        CHECK (!got && e);
        g_clear_error (&e);
        got = walpe_picture_decode_file (dir, all, NULL, NULL, &e);
        CHECK (!got && e);
        g_clear_error (&e);
        got = walpe_picture_decode_file (NULL, all, NULL, NULL, &e);
        CHECK (!got && e);
        g_clear_error (&e);
        /* a JPEG that is held whole and too large for it is refused here too */
        GByteArray *big = hand_jpeg (12000, 12000, 1, 0, FALSE);
        got = walpe_picture_decode (big->data, big->len, all, NULL, NULL, &e);
        CHECK (!got && g_error_matches (e, G_IO_ERROR, G_IO_ERROR_INVALID_DATA));
        g_clear_error (&e);
        g_byte_array_unref (big);
        g_free (f);
        g_free (png);
        g_object_unref (src);
    }

    /* JPEG is let through at any declared size up to 16384 a side because
     * its decoder scales while it reads - which holds only for a
     * sequential file with one scan. A progressive one, or one with a scan
     * per component, is held whole first: 128 bytes per 8 x 8 block of
     * every component (walpe_jpeg_store_bytes). 400 bytes of file declaring
     * 12000 x 12000 cost 288 MiB and seconds; such a preview gets the
     * placeholder now, a small progressive one is still shown. */
    {
        GByteArray *a = hand_jpeg (4096, 4096, 1, 0, FALSE);
        CHECK (walpe_jpeg_store_bytes (a->data, a->len) == (guint64) 512 * 512 * 128);
        /* cut anywhere (a buffer of exactly that size): no answer, no read
         * past the end; with the first scan's header in, the answer */
        for (gsize len = 0; len < a->len; len += len < 200 ? 1 : 97) {
            guint8 *cut = g_malloc (len ? len : 1);
            memcpy (cut, a->data, len);
            guint64 r = walpe_jpeg_store_bytes (cut, len);
            CHECK (r == G_MAXUINT64 || r == (guint64) 512 * 512 * 128);
            g_free (cut);
        }
        g_byte_array_unref (a);
        a = hand_jpeg (100, 60, 3, 0, TRUE);    /* 4:2:0: 14 x 8 blocks + 2 * (7 x 4) */
        CHECK (walpe_jpeg_store_bytes (a->data, a->len) == (guint64) (14 * 8 + 2 * 7 * 4) * 128);
        g_byte_array_unref (a);
        a = hand_jpeg (4096, 4096, 3, 1, FALSE);    /* sequential, a scan per component */
        CHECK (walpe_jpeg_store_bytes (a->data, a->len) == (guint64) 3 * 512 * 512 * 128);
        g_byte_array_unref (a);
        a = hand_jpeg (4096, 4096, 3, 2, FALSE);    /* sequential, one scan: streams */
        CHECK (walpe_jpeg_store_bytes (a->data, a->len) == 0);
        g_byte_array_unref (a);
        CHECK (walpe_jpeg_store_bytes ((const guint8 *) "GIF89a..", 8) == G_MAXUINT64);
        CHECK (walpe_jpeg_store_bytes (NULL, 0) == G_MAXUINT64);

        char *f = g_build_filename (dir, "preview.jpg", NULL);
        /* what gdk-pixbuf writes is sequential with one scan: 0, from the file too */
        GdkPixbuf *src = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, 64, 36);
        gdk_pixbuf_fill (src, 0x208040ff);
        CHECK (gdk_pixbuf_save (src, f, "jpeg", NULL, NULL));
        CHECK (walpe_jpeg_store_bytes_file (f) == 0);
        g_object_unref (src);
        CHECK (walpe_jpeg_store_bytes_file ("/nonexistent/preview.jpg") == G_MAXUINT64);

        a = hand_jpeg (64, 48, 1, 0, FALSE);
        CHECK (g_file_set_contents (f, (const char *) a->data, (gssize) a->len, NULL));
        g_byte_array_unref (a);
        Job *pj = anim_job (f, 32, 24, THUMB_CONTAIN);
        pj->anim = FALSE;
        GdkPixbuf *pp = decode (pj);
        CHECK (pp && gdk_pixbuf_get_width (pp) == 32 && gdk_pixbuf_get_height (pp) == 24);
        g_clear_object (&pp);
        job_free (pj);

        a = hand_jpeg (12000, 12000, 1, 0, FALSE);
        CHECK (a->len < 1024);
        CHECK (walpe_jpeg_store_bytes (a->data, a->len) > WALPE_JPEG_STORE_MAX);
        CHECK (g_file_set_contents (f, (const char *) a->data, (gssize) a->len, NULL));
        g_byte_array_unref (a);
        for (int mode = 0; mode < 2; mode++) {
            pj = anim_job (f, 160, 90, mode ? THUMB_CONTAIN : THUMB_COVER);
            pj->anim = FALSE;
            pp = decode (pj);
            CHECK (pp == NULL);
            g_clear_object (&pp);
            job_free (pj);
        }
        g_unlink (f);
        g_free (f);
    }

    g_unlink (path);
    g_unlink (still);
    g_rmdir (dir);
    g_free (still);
    g_free (path);
    g_free (dir);
    if (fails)
        g_printerr ("thumbs-test: %d check(s) failed\n", fails);
    else
        g_print ("thumbs-test: ok\n");
    return fails ? 1 : 0;
}
