/* fuzz_picture - the pictures that are not textures: the preview of a
 * wallpaper folder (src/ui/thumbs.c: whatever its author shipped, decoded
 * as soon as the folder is in the library), the previews of workshop items
 * (src/common/workshop_query.c: from Steam's servers), cover art
 * (src/common/media.c: from a player on the session bus). All three go
 * through walpe_picture_decode / walpe_picture_decode_file of
 * src/common/util.c: the format by its signature, the checks that run
 * before a decoder sees the bytes (BMP header sizes, GIF layout, what a JPEG
 * makes its decoder hold), the caller's word on the declared size, then
 * gdk-pixbuf's decoder of that one format.
 *
 * The first byte of the input is the caller: which formats it accepts and
 * what sizes (every caller passes a size function: without one a picture is
 * decoded as large as it says). The sizes are small ones, so that a decoder that reserves
 * memory by a number of the file before the caller was asked - or after it
 * refused - shows as an allocation far beyond what the accepted picture
 * needs (libFuzzer's -malloc_limit_mb, the replay's allocation limit).
 * The two readers (of a buffer, of a file in pieces) have to agree. */
#include "fuzz_common.h"
#include "common/util.h"
#include <gdk-pixbuf/gdk-pixbuf.h>

typedef struct {
    int max_side;           /* per side, every format */
    gint64 max_pixels;      /* every format but JPEG (which is scaled while it is decoded) */
    int fit;                /* 0 = decoded as declared, else the longest side asked for */
    int calls, w, h, sw, sh;
    gboolean refused;
} Caller;

static gboolean size_cb (const char *kind, int w, int h, int *sw, int *sh, gpointer user)
{
    Caller *c = user;
    FUZZ_CHECK (kind && *kind);
    c->calls++;
    c->w = w;
    c->h = h;
    if (w < 1 || h < 1 || w > c->max_side || h > c->max_side ||
        (strcmp (kind, "jpeg") && (gint64) w * h > c->max_pixels)) {
        c->refused = TRUE;
        return FALSE;
    }
    if (c->fit && MAX (w, h) > c->fit) {
        double k = (double) c->fit / MAX (w, h);
        *sw = c->sw = MAX (1, (int) (w * k));
        *sh = c->sh = MAX (1, (int) (h * k));
    }
    return TRUE;
}

/* what a caller does with its picture: reads every pixel */
static void look (GdkPixbuf *pb, const Caller *c)
{
    int w = gdk_pixbuf_get_width (pb), h = gdk_pixbuf_get_height (pb);
    int n = gdk_pixbuf_get_n_channels (pb), stride = gdk_pixbuf_get_rowstride (pb);
    const guint8 *px = gdk_pixbuf_read_pixels (pb);
    FUZZ_CHECK (!c->refused);
    FUZZ_CHECK (w >= 1 && h >= 1 && (n == 3 || n == 4) && stride >= w * n && px);
    /* no larger than the caller accepted, and than it asked for */
    FUZZ_CHECK (c->calls == 0 || (w <= c->max_side && h <= c->max_side));
    FUZZ_CHECK (!c->sw || (w <= c->sw && h <= c->sh));
    for (int y = 0; y < h; y++)
        fuzz_touch (px + (gsize) y * (gsize) stride, (gsize) w * (gsize) n);
}

int LLVMFuzzerInitialize (int *argc, char ***argv)
{
    (void) argc; (void) argv;
    fuzz_quiet ();
    return 0;
}

int LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
    static const char *const folder[] = { "jpeg", "png", "gif", "bmp", "webp", "avif", NULL };  /* thumbs.c */
    static const char *const shop[] = { "jpeg", "png", "gif", "webp", NULL };          /* workshop_query.c */
    static const char *const art[] = { "jpeg", "png", "gif", "webp", "bmp", NULL };    /* media.c */
    static const char *const *const kinds[] = { folder, shop, art, folder };
    if (size < 1)
        return 0;
    guint8 who = data[0];
    data++, size--;
    Caller proto = { .max_side = who & 4 ? 4096 : 1024, .max_pixels = who & 4 ? 2048 * 2048 : 512 * 512,
                     .fit = who & 8 ? 0 : 160 };

    Caller c1 = proto, c2 = proto;
    GError *e1 = NULL, *e2 = NULL;
    GdkPixbuf *p1 = walpe_picture_decode (data, size, kinds[who & 3], size_cb, &c1, &e1);
    GdkPixbuf *p2 = walpe_picture_decode_file (fuzz_file (0, data, size), kinds[who & 3], size_cb, &c2, &e2);
    FUZZ_CHECK (p1 || e1);
    FUZZ_CHECK (p2 || e2);
    FUZZ_CHECK (!p1 == !p2);
    if (p1 && p2) {
        FUZZ_CHECK (gdk_pixbuf_get_width (p1) == gdk_pixbuf_get_width (p2));
        FUZZ_CHECK (gdk_pixbuf_get_height (p1) == gdk_pixbuf_get_height (p2));
    }
    if (p1)
        look (p1, &c1);
    if (p2)
        look (p2, &c2);
    /* a JPEG is walked for how it is coded before its decoder gets it: the walk on the bytes and the one
     * on the file agree, and an arithmetic-coded one is never decoded */
    gboolean coding = walpe_jpeg_coding_ok (data, size);
    FUZZ_CHECK (coding == walpe_jpeg_file_coding_ok (fuzz_file (0, data, size)));
    FUZZ_CHECK (coding || (!p1 && !p2));
    if (g_getenv ("WALPE_FUZZ_LOG"))    /* one line an input: the walk's answer, and what was decoded */
        g_printerr ("fuzz_picture: coding %s, %s %d x %d sum %08x\n", coding ? "ok" : "refused",
                    p1 ? "decoded" : "not decoded", p1 ? gdk_pixbuf_get_width (p1) : 0,
                    p1 ? gdk_pixbuf_get_height (p1) : 0,
                    p1 ? fuzz_touch (gdk_pixbuf_read_pixels (p1), (size_t) gdk_pixbuf_get_rowstride (p1) *
                                     ((size_t) gdk_pixbuf_get_height (p1) - 1) +
                                     (size_t) gdk_pixbuf_get_width (p1) * (size_t) gdk_pixbuf_get_n_channels (p1))
                       : 0);
    for (size_t cut = size > 64 ? size - 64 : 0; cut < size; cut++)     /* (every shorter header as well) */
        walpe_jpeg_coding_ok (data, cut);
    g_clear_object (&p1);
    g_clear_object (&p2);
    g_clear_error (&e1);
    g_clear_error (&e2);
    /* the same question for a BMP header alone */
    walpe_bmp_header_ok (data, size, size);
    return 0;
}
