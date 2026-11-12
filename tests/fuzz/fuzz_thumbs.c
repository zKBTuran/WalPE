/* fuzz_thumbs - the preview of a wallpaper folder as the library window's
 * workers make it (src/ui/thumbs.c): the walk over a GIF's blocks that
 * counts its frames and their delays, the frames of an animated preview at
 * tile size (which ones are kept, their one buffer), and the still picture
 * with the tile's size function. A preview is whatever file a downloaded
 * project.json names, and it is decoded as soon as the folder is listed.
 *
 * thumbs.c is part of the GUI and is included here as it is (its workers
 * are called directly: no window, no thread pool). The first byte of the
 * input is the tile: one of the sizes the windows ask for, and how the
 * picture is fitted. */
#include "fuzz_common.h"
#include "../../src/ui/thumbs.c"

int LLVMFuzzerInitialize (int *argc, char ***argv)
{
    (void) argc; (void) argv;
    fuzz_quiet ();
    return 0;
}

int LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
    /* the sizes the windows ask for (main.c: grid tile 160 x 90, selection
     * preview 720 x 405; workshop_page.c: tile 196, detail 320; each times
     * the display's scale). Not smaller ones: the cost of gdk-pixbuf's
     * scaler grows with source side / tile side squared, and a tile of
     * 1 x 1, which no caller has, is a gigabyte of filter table. */
    static const int tiles[8][2] = { { 160, 90 }, { 320, 180 }, { 720, 405 }, { 240, 135 },
                                     { 196, 196 }, { 392, 392 }, { 320, 320 }, { 640, 640 } };
    if (size < 1)
        return 0;
    guint8 tile = data[0];
    data++, size--;
    Job j = { .mode = tile & 1 ? THUMB_COVER : THUMB_CONTAIN, .w = tiles[(tile >> 1) & 7][0],
              .h = tiles[(tile >> 1) & 7][1], .wanted = 1 };
    j.src = (char *) fuzz_file (0, data, size);

    /* the block walk alone */
    int n = 0, iw = 0, ih = 0;
    int *sd = gif_frame_delays (data, size, &n, &iw, &ih);
    if (sd) {
        FUZZ_CHECK (n >= 1 && n <= ANIM_SRC_MAX_FRAMES && iw >= 0 && iw <= 65535 && ih >= 0 && ih <= 65535);
        for (int i = 0; i < n; i++)
            FUZZ_CHECK (sd[i] >= 20 && sd[i] <= 655350);
        g_free (sd);
    }

    /* the frames of an animated preview */
    j.anim = TRUE;
    decode_anim (&j);
    if (j.px) {
        gsize len = 0;
        const guint8 *px = g_bytes_get_data (j.px, &len);
        FUZZ_CHECK (j.n >= 2 && j.n <= ANIM_MAX_FRAMES && j.pw >= 1 && j.ph >= 1 && j.delay);
        FUZZ_CHECK (j.stride == j.pw * (j.alpha ? 4 : 3));
        FUZZ_CHECK (len == (gsize) j.n * (gsize) j.stride * (gsize) j.ph);
        FUZZ_CHECK ((gsize) j.n * (gsize) j.pw * (gsize) j.ph * 4 <= ANIM_MAX_BYTES);
        if (j.mode == THUMB_COVER)
            FUZZ_CHECK (j.pw <= j.w && j.ph <= j.h);
        else
            FUZZ_CHECK ((gint64) j.pw * j.ph <= ANIM_MAX_FRAME_PX + j.pw + j.ph);
        for (int i = 0; i < j.n; i++)
            FUZZ_CHECK (j.delay[i] >= 20);
        fuzz_touch (px, len);
        g_clear_pointer (&j.px, g_bytes_unref);
        g_clear_pointer (&j.delay, g_free);
    } else {
        FUZZ_CHECK (!j.delay);
    }

    /* the still */
    j.anim = FALSE;
    GdkPixbuf *pb = decode (&j);
    if (pb) {
        int w = gdk_pixbuf_get_width (pb), h = gdk_pixbuf_get_height (pb);
        FUZZ_CHECK (w >= 1 && h >= 1 && w <= COVER_MAX_SIDE && h <= COVER_MAX_SIDE);
        if (j.mode == THUMB_COVER)
            FUZZ_CHECK (w <= j.w && h <= j.h);
        const guint8 *px = gdk_pixbuf_read_pixels (pb);
        int stride = gdk_pixbuf_get_rowstride (pb), ch = gdk_pixbuf_get_n_channels (pb);
        for (int y = 0; y < h; y++)
            fuzz_touch (px + (gsize) y * (gsize) stride, (gsize) w * (gsize) ch);
        g_object_unref (pb);
    }
    return 0;
}
