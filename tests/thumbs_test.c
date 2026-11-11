/* thumbs_test - the GUI's preview decoder (src/ui/thumbs.c) on data it
 * cannot trust, without a display: the GIF block parser that counts frames
 * (cut the file anywhere: no crash, no read past the end), and the frames
 * of an animation at tile size (count, delays, bytes, limits). */
#include "../src/ui/thumbs.c"

static int fails;
#define CHECK(c) do { if (!(c)) { g_printerr ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

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
