/* gencontent - generates test wallpaper content for WalPE (dev tool) */
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>

static char outdir[512];

static void put_u32 (int fd, guint32 v) { write (fd, &v, 4); }
static void put_str (int fd, const char *s, int n) { write (fd, s, n); }

/* ---------------- .tex writer: TEXV0003 header + raw BGRA ---------------- */
static void write_tex (const char *path, int w, int h, const guint8 *rgba)
{
    int fd = open (path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    g_assert (fd >= 0);
    put_str (fd, "TEXV0003", 8);
    put_u32 (fd, w);
    put_u32 (fd, h);
    put_u32 (fd, 1);   /* mips */
    put_u32 (fd, 0);   /* format 0 = raw BGRA8888 */
    put_u32 (fd, 0);   /* flags */
    guint8 *row = g_malloc (w * 4);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const guint8 *p = rgba + ((gsize) y * w + x) * 4;
            row[x * 4 + 0] = p[2]; /* B */
            row[x * 4 + 1] = p[1]; /* G */
            row[x * 4 + 2] = p[0]; /* R */
            row[x * 4 + 3] = p[3];
        }
        write (fd, row, w * 4);
    }
    g_free (row);
    close (fd);
}

static GdkPixbuf *new_rgb (int w, int h, gboolean alpha)
{
    return gdk_pixbuf_new (GDK_COLORSPACE_RGB, alpha, 8, w, h);
}

static void fill (GdkPixbuf *pb, int x, int y, int r, int g, int b, int a)
{
    guint8 *px = gdk_pixbuf_get_pixels (pb);
    int rs = gdk_pixbuf_get_rowstride (pb);
    int nch = gdk_pixbuf_get_n_channels (pb);
    guint8 *p = px + (gsize) y * rs + x * nch;
    p[0] = r; p[1] = g; p[2] = b; p[3] = a;
}

static void save_png (GdkPixbuf *pb, const char *name)
{
    char *path = g_build_filename (outdir, name, NULL);
    GError *err = NULL;
    if (!gdk_pixbuf_save (pb, path, "png", &err, NULL)) {
        g_printerr ("save %s: %s\n", path, err->message);
        exit (1);
    }
    g_free (path);
}

/* ---------------- GIF89a with clear-code-only LZW (valid, uncompressed-ish) */
typedef struct {
    GByteArray *b;
    guint acc;
    int nbits;
} BitW;

static void bw_init (BitW *w, GByteArray *b) { w->b = b; w->acc = 0; w->nbits = 0; }
static void bw_put (BitW *w, guint code, int bits)
{
    w->acc |= code << w->nbits;
    w->nbits += bits;
    while (w->nbits >= 8) {
        g_byte_array_append (w->b, (guint8[]) { w->acc & 0xFF }, 1);
        w->acc >>= 8;
        w->nbits -= 8;
    }
}
static void bw_flush (BitW *w)
{
    if (w->nbits > 0)
        g_byte_array_append (w->b, (guint8[]) { w->acc & 0xFF }, 1);
    w->acc = 0;
    w->nbits = 0;
}

static GByteArray *gif_data;
static void gb (const void *d, gsize n) { g_byte_array_append (gif_data, d, n); }
static void gs (const char *s) { gb (s, strlen (s)); }

#define CLEAR_CODE 256
#define EOI_CODE 257

/* write one frame's LZW stream (9-bit codes, clear every ~250 literals) */
static void lzw_frame (const guint8 *idx, int npix)
{
    BitW w;
    bw_init (&w, gif_data);
    bw_put (&w, CLEAR_CODE, 9);
    int since_clear = 0;
    for (int i = 0; i < npix; i++) {
        bw_put (&w, idx[i], 9);
        since_clear++;
        if (since_clear >= 250) {
            bw_put (&w, CLEAR_CODE, 9);
            since_clear = 0;
        }
    }
    bw_put (&w, EOI_CODE, 9);
    bw_flush (&w);
    /* emit in <=255 byte sub-blocks */
    GByteArray *d = gif_data;
    /* data already appended; we need sub-block chunking, so wrap instead:
       simplest: the caller must pre-chunk. To stay correct we re-emit below. */
    (void) d;
}

/* whole-gif writer with sub-block chunking */
static void write_gif (const char *path, int w, int h)
{
    /* 3 frames, 256-color palette, moving disc */
    int npix = w * h;
    guint8 *frames[3];
    for (int f = 0; f < 3; f++) {
        frames[f] = g_malloc (npix);
        int cx = w / 2 + (int) (w / 3 * sin (f * 2.0944));
        int cy = h / 2 + (int) (h / 3 * cos (f * 2.0944));
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int dx = x - cx, dy = y - cy;
                int inside = dx * dx + dy * dy < (h / 4) * (h / 4);
                frames[f][y * w + x] = inside ? (guint8) (200 + f) : (guint8) (10 + f);
            }
    }

    gif_data = g_byte_array_new ();
    gs ("GIF89a");
    guint8 lsd[7] = { (guint8) w, (guint8) (w >> 8), (guint8) h, (guint8) (h >> 8),
                      0xF7, 0, 0 }; /* GCT, 256 colors */
    gb (lsd, 7);
    guint8 pal[768];
    for (int i = 0; i < 256; i++) {
        pal[i * 3 + 0] = (guint8) (i * 0.9);
        pal[i * 3 + 1] = (guint8) (i * 0.5 + 60);
        pal[i * 3 + 2] = (guint8) (255 - i * 0.7);
    }
    gb (pal, 768);
    /* NETSCAPE loop forever */
    gs ("\x21\xFF\x0BNETSCAPE2.0\x03\x01\x00\x00\x00");
    for (int f = 0; f < 3; f++) {
        guint8 gce[8] = { 0x21, 0xF9, 0x04, 0x04, 0x14, 0x00, 0xFF, 0x00 }; /* 200ms */
        gb (gce, 8);
        guint8 id[10] = { 0x2C, 0, 0, 0, 0, (guint8) w, (guint8) (w >> 8), (guint8) h,
                          (guint8) (h >> 8), 0x00 };
        gb (id, 10);
        guint8 pcs[3] = { 0x08, 0x00, 0x00 }; /* LZW min code size 8 */
        gb (pcs, 3);

        /* LZW stream in sub-blocks */
        BitW w;
        GByteArray *stream = g_byte_array_new ();
        gif_data = stream;
        bw_init (&w, stream);
        bw_put (&w, CLEAR_CODE, 9);
        int since = 0;
        for (int i = 0; i < npix; i++) {
            bw_put (&w, frames[f][i], 9);
            if (++since >= 250) {
                bw_put (&w, CLEAR_CODE, 9);
                since = 0;
            }
        }
        bw_put (&w, EOI_CODE, 9);
        bw_flush (&w);
        gif_data = g_byte_array_new (); /* switch back to main buffer */

        gsize len = stream->len;
        const guint8 *sd = stream->data;
        gsize off = 0;
        while (off < len) {
            guint8 chunk = (guint8) MIN (255, len - off);
            gb (&chunk, 1);
            gb (sd + off, chunk);
            off += chunk;
        }
        guint8 zero = 0;
        gb (&zero, 1);
        g_byte_array_unref (stream);
    }
    gs (";");
    GError *err = NULL;
    if (!g_file_set_contents (path, (char *) gif_data->data, gif_data->len, &err)) {
        g_printerr ("gif write: %s\n", err->message);
        exit (1);
    }
    g_byte_array_unref (gif_data);
    for (int f = 0; f < 3; f++)
        g_free (frames[f]);
}

int main (int argc, char **argv)
{
    if (argc < 2) {
        g_print ("usage: gencontent <output-dir>\n");
        return 1;
    }
    snprintf (outdir, sizeof (outdir), "%s", argv[1]);
    g_mkdir_with_parents (outdir, 0755);

    /* ---- scene project ---- */
    char *scenedir = g_build_filename (outdir, "test-scene", NULL);
    g_mkdir_with_parents (scenedir, 0755);
    char saved[512];
    snprintf (outdir, sizeof (outdir), "%s", scenedir);

    /* bg: vertical gradient deep blue -> near black */
    GdkPixbuf *bg = new_rgb (1920, 1080, TRUE);
    for (int y = 0; y < 1080; y++)
        for (int x = 0; x < 1920; x++) {
            float t = y / 1080.0f;
            fill (bg, x, y, (int) (18 + 30 * (1 - t)), (int) (24 + 40 * (1 - t)), (int) (48 + 90 * (1 - t)), 255);
        }
    save_png (bg, "bg.png");

    /* additive glow band: cyan gradient with alpha */
    GdkPixbuf *glow = new_rgb (1920, 1080, TRUE);
    for (int y = 0; y < 1080; y++)
        for (int x = 0; x < 1920; x++) {
            float d = fabsf (y - 400.0f) / 300.0f;
            float a = d < 1 ? (1 - d) : 0;
            fill (glow, x, y, (int) (40 * a), (int) (180 * a), (int) (220 * a), (int) (255 * a));
        }
    save_png (glow, "glow.png");

    /* foreground disc, opaque center + soft edge */
    GdkPixbuf *fg = new_rgb (512, 512, TRUE);
    for (int y = 0; y < 512; y++)
        for (int x = 0; x < 512; x++) {
            float dx = x - 256, dy = y - 256;
            float d = sqrtf (dx * dx + dy * dy) / 240.0f;
            int a = d < 1 ? (int) (255 * (1 - d * d)) : 0;
            fill (fg, x, y, 235, 84, 160, a);
        }
    save_png (fg, "fg.png");

    /* particle sprite: small soft white disc */
    GdkPixbuf *pt = new_rgb (64, 64, TRUE);
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            float dx = x - 32, dy = y - 32;
            float d = sqrtf (dx * dx + dy * dy) / 30.0f;
            int a = d < 1 ? (int) (255 * (1 - d) * (1 - d)) : 0;
            fill (pt, x, y, 255, 240, 200, a);
        }
    save_png (pt, "particle.png");

    /* .tex raw BGRA gradient block */
    GdkPixbuf *texsrc = new_rgb (1024, 1024, TRUE);
    for (int y = 0; y < 1024; y++)
        for (int x = 0; x < 1024; x++) {
            float t = (x / 1024.0f + y / 1024.0f) / 2;
            fill (texsrc, x, y, (int) (90 + 120 * t), (int) (60 + 40 * (1 - t)), (int) (140 - 60 * t), 235);
        }
    int rs = gdk_pixbuf_get_rowstride (texsrc);
    guint8 *texdata = g_malloc (1024 * 1024 * 4);
    for (int y = 0; y < 1024; y++)
        memcpy (texdata + (gsize) y * 1024 * 4, gdk_pixbuf_get_pixels (texsrc) + (gsize) y * rs, 1024 * 4);
    char *texp = g_build_filename (outdir, "texbg.tex", NULL);
    write_tex (texp, 1024, 1024, texdata);
    g_free (texp);

    /* preview: reuse bg */
    save_png (bg, "preview.png");

    /* scene.json - WE exports layers top-first; we render bottom-first */
    const char *scene_json =
    "{\n"
    "  \"object\": {\n"
    "    \"width\": 1920,\n"
    "    \"height\": 1080,\n"
    "    \"layers\": [\n"
    "      { \"name\": \"fg\", \"file\": \"fg.png\", \"visible\": true, \"blendmode\": 1,\n"
    "        \"scale\": 1.0, \"opacity\": 255, \"angle\": 0,\n"
    "        \"pivot\": [0.5, 0.5], \"translation\": [960, 760],\n"
    "        \"parallax\": { \"amount\": [1.0, 1.0], \"depth\": 100 } },\n"
    "      { \"name\": \"glow\", \"file\": \"glow.png\", \"visible\": true, \"blendmode\": 2,\n"
    "        \"scale\": 1.0, \"opacity\": 255, \"angle\": 0,\n"
    "        \"pivot\": [0.5, 0.5], \"translation\": [0, 0] },\n"
    "      { \"name\": \"texbg\", \"file\": \"texbg.tex\", \"visible\": true, \"blendmode\": 1,\n"
    "        \"scale\": 1.0, \"opacity\": 255, \"angle\": 0,\n"
    "        \"pivot\": [0.5, 0.5], \"translation\": [960, 540] },\n"
    "      { \"name\": \"bg\", \"file\": \"bg.png\", \"visible\": true, \"blendmode\": 1,\n"
    "        \"scale\": 1.0, \"opacity\": 255, \"angle\": 0,\n"
    "        \"pivot\": [0.5, 0.5], \"translation\": [960, 540] }\n"
    "    ]\n"
    "  },\n"
    "  \"general\": {\n"
    "    \"properties\": {\n"
    "      \"glowintensity\": { \"type\": \"slider\", \"text\": \"Glow strength\",\n"
    "                          \"value\": 1.0, \"min\": 0.0, \"max\": 2.0, \"step\": 0.01 }\n"
    "    }\n"
    "  },\n"
    "  \"particles\": [\n"
    "    { \"name\": \"embers\", \"objectconfig\": {\n"
    "        \"origin\": [960, 950], \"emissionrate\": 40, \"lifetime\": 4.0,\n"
    "        \"size\": 28, \"velocity\": [0, -140], \"gravity\": [0, 25], \"damping\": 0.1,\n"
    "        \"imagepath\": \"particle.png\" } }\n"
    "  ]\n"
    "}\n";
    char *sjp = g_build_filename (scenedir, "scene.json", NULL);
    g_file_set_contents (sjp, scene_json, -1, NULL);

    const char *project_json =
    "{\n"
    "  \"contentrating\": \"Everyone\",\n"
    "  \"description\": \"Built-in test scene: gradients, additive glow, raw .tex, disc parallax, particles\",\n"
    "  \"file\": \"scene.json\",\n"
    "  \"preview\": \"preview.png\",\n"
    "  \"title\": \"WalPE Test Scene\",\n"
    "  \"type\": \"scene\"\n"
    "}\n";
    char *pjp = g_build_filename (scenedir, "project.json", NULL);
    g_file_set_contents (pjp, project_json, -1, NULL);

    /* ---- loose media ---- */
    char *gifdir = g_build_filename (outdir, "..", NULL);
    char *gifpath = g_build_filename (gifdir, "test-animation.gif", NULL);
    write_gif (gifpath, 96, 96);
    g_print ("generated scene project, tex, gif at %s\n", outdir);
    return 0;
}
