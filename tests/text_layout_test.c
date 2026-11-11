/* text_layout_test - a text layer's ink lies inside its texture whatever
 * the alignment, the wrap width and the direction of the script (no GL).
 *
 * walpe_text_render sizes the texture for the laid-out text and places the
 * text block in it by the layer's horizontal alignment. The same string
 * drawn left-aligned, centred and right-aligned is the same glyphs at
 * three places: the three textures must hold the same amount of ink, and
 * none of it may touch the texture's edge when the layer has padding.
 * Pango aligns a right-to-left line (Hebrew, Arabic) at the other end of a
 * wrap width than the layer asked for; the block was then placed as if it
 * had not, and most of such a text fell outside its texture.
 *
 * Runs with whatever fonts this machine has: a string is only checked when
 * the fonts draw it at all (its ink is counted in a layer without width
 * limit first). */
#include <glib.h>
#include <cairo.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine/scenetext.h"

static int fails, checks, skipped;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf ("FAIL %s:%d: ", __FILE__, __LINE__); \
                                                 printf (__VA_ARGS__); printf ("\n"); } } while (0)

typedef struct {
    double sum;              /* summed alpha / 255 */
    int w, h;                /* the texture */
    int x0, y0, x1, y1;      /* bounds of the ink (x1 < x0: none) */
} Ink;

/* digest: every pixel of the texture is folded into it (or NULL) */
static gboolean measure (const WalpeTextStyle *st, const char *text, Ink *ink, guint64 *digest)
{
    float q[4];
    memset (ink, 0, sizeof (*ink));
    ink->x0 = ink->y0 = G_MAXINT;
    ink->x1 = ink->y1 = -1;
    cairo_surface_t *sf = walpe_text_render (st, text, q);
    if (!sf)
        return FALSE;
    ink->w = cairo_image_surface_get_width (sf);
    ink->h = cairo_image_surface_get_height (sf);
    int stride = cairo_image_surface_get_stride (sf);
    const guint8 *px = cairo_image_surface_get_data (sf);
    for (int y = 0; y < ink->h; y++) {
        const guint32 *row = (const guint32 *) (const void *) (px + (gsize) y * stride);
        for (int x = 0; x < ink->w; x++) {
            guint a = row[x] >> 24;
            if (digest)
                *digest = (*digest ^ row[x]) * G_GUINT64_CONSTANT (1099511628211);
            if (!a)
                continue;
            ink->sum += a / 255.0;
            ink->x0 = MIN (ink->x0, x);
            ink->x1 = MAX (ink->x1, x);
            ink->y0 = MIN (ink->y0, y);
            ink->y1 = MAX (ink->y1, y);
        }
    }
    cairo_surface_destroy (sf);
    return TRUE;
}

int main (void)
{
    static const struct { const char *what, *text; gboolean rtl; } texts[] = {
        { "Latin", "The quick brown fox jumps over the lazy dog", FALSE },
        { "Latin, two lines", "first line\nand a longer second line", FALSE },
        { "digits", "23:59:07", FALSE },
        { "Hebrew", "\327\251\327\234\327\225\327\235 \327\242\327\225\327\234\327\235 \327\224\327\251\327\242\327\225\327\237 \327\251\327\234\327\231", TRUE },
        { "Arabic", "\331\205\330\261\330\255\330\250\330\247 \330\250\330\247\331\204\330\271\330\247\331\204\331\205 \330\247\331\204\330\254\331\205\331\212\331\204", TRUE },
        { "Hebrew with Latin", "\327\251\327\234\327\225\327\235 Friday \327\242\327\225\327\234\327\235 12:30", TRUE },
        { "Japanese", "\346\227\245\346\234\254\350\252\236\343\201\256\343\203\206\343\202\255\343\202\271\343\203\210 \346\234\210\346\233\234\346\227\245", FALSE },
        { "Korean", "\354\225\210\353\205\225\355\225\230\354\204\270\354\232\224 \354\233\224\354\232\224\354\235\274", FALSE },
    };
    static const float widths[] = { 0, 4000, 900, 380 };   /* 0 = no limit; wide; wraps; wraps often */
    static const char *const al[] = { "left", "centre", "right" };
    /* the textures of the left-to-right strings, to compare two builds */
    guint64 ltr_digest = G_GUINT64_CONSTANT (14695981039346656037);
    for (guint t = 0; t < G_N_ELEMENTS (texts); t++) {
        WalpeTextStyle st = { 0 };
        st.pointsize = 14;
        st.padding = 24;
        st.halign = 1;
        st.valign = 1;
        st.box_w = 2000;       /* room on both sides of every string below */
        st.box_h = 700;
        Ink free_ink;
        if (!measure (&st, texts[t].text, &free_ink, NULL) || free_ink.sum < 50) {
            printf ("skip: no font here draws the %s string\n", texts[t].what);
            skipped++;
            continue;
        }
        for (guint wi = 0; wi < G_N_ELEMENTS (widths); wi++) {
            Ink ink[3];
            gboolean ok = TRUE;
            for (int a = 0; a < 3 && ok; a++) {
                st.halign = a;
                st.limitwidth = widths[wi] > 0;
                st.maxwidth = widths[wi];
                ok = measure (&st, texts[t].text, &ink[a], texts[t].rtl ? NULL : &ltr_digest);
                CHECK (ok, "%s, width %g, %s: rendered", texts[t].what, (double) widths[wi], al[a]);
            }
            if (!ok)
                continue;
            for (int a = 0; a < 3; a++) {
                /* the same glyphs at another place: the ink differs by what
                 * the fractional position does to the edges of the strokes */
                double ref = ink[1].sum, d = fabs (ink[a].sum - ref);
                CHECK (d <= 0.02 * ref + 4.0,
                       "%s, width %g, %s: %.0f units of ink, centred %.0f (the text is cut off)",
                       texts[t].what, (double) widths[wi], al[a], ink[a].sum, ref);
                CHECK (ink[a].x1 >= ink[a].x0 && ink[a].x0 >= 4 && ink[a].x1 < ink[a].w - 4 &&
                       ink[a].y0 >= 4 && ink[a].y1 < ink[a].h - 4,
                       "%s, width %g, %s: ink from %d,%d to %d,%d in a %dx%d texture with 24 px of padding",
                       texts[t].what, (double) widths[wi], al[a], ink[a].x0, ink[a].y0, ink[a].x1,
                       ink[a].y1, ink[a].w, ink[a].h);
            }
            /* where the block lies: at the padding of the left edge, in the
             * middle, at the padding of the right edge */
            for (int a = 0; a < 3; a++) {
                int left = ink[a].x0, right = ink[a].w - 1 - ink[a].x1;
                gboolean placed = a == 0 ? left <= 24 + 20 : a == 2 ? right <= 24 + 20
                                                           : abs (left - right) <= 16;
                CHECK (placed, "%s, width %g, %s: %d px free on the left, %d on the right",
                       texts[t].what, (double) widths[wi], al[a], left, right);
            }
        }
    }
    printf ("%d checks, %d failed, %d strings without a font; left-to-right textures %016" G_GINT64_MODIFIER "x\n",
            checks, fails, skipped, ltr_digest);
    return fails ? 1 : 0;
}
