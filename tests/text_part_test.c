/* text_part_test - of a text layer whose string changes, only the part
 * where the old or the new string has ink is recoloured and uploaded
 * (walpe_text_render_part, scenetext.h). The texture that results must be
 * the one a full upload gives, byte for byte.
 *
 * No GL: the "texture" is a buffer the test keeps the way the scene keeps
 * the GL texture - the first string whole, of every later one the part
 * the renderer names, copied to its place - and it is compared with a
 * full render of the same string after every step. Runs over alignments,
 * box sizes, paddings, wrapping, font sizes and strings with tall, deep
 * and overhanging glyphs, with whatever fonts this machine resolves the
 * default family to. Also checks that the part is small when the box is
 * large (the point of it) and that WALPE_TEXT_PART=0 is not needed for
 * exactness. */
#include <glib.h>
#include <cairo.h>
#include <stdio.h>
#include <string.h>
#include "engine/scenetext.h"

static int fails, steps, partial_steps;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf ("FAIL %s:%d: ", __FILE__, __LINE__); \
                                         printf (__VA_ARGS__); printf ("\n"); } } while (0)

typedef struct {
    guint8 *px;          /* w * h * 4, rows top down, no padding */
    WalpeTextInk ink;    /* ink.w = 0: nothing yet */
} Tex;

/* what make_text_texture does with the GL texture */
static void tex_update (Tex *t, const WalpeTextStyle *st, const char *text, gsize *part_px)
{
    float q[4];
    WalpeTextInk now;
    int part[4];
    cairo_surface_t *sf = walpe_text_render_part (st, text, q, t->ink.w > 0 ? &t->ink : NULL, &now,
                                                  part);
    if (!sf) {
        CHECK (FALSE, "no surface for \"%s\"", text);
        return;
    }
    int sw = cairo_image_surface_get_width (sf), sh = cairo_image_surface_get_height (sf);
    int pw = part[2], ph = part[3];
    int stride = cairo_image_surface_get_stride (sf);
    const guint8 *src = cairo_image_surface_get_data (sf);
    CHECK (sw == now.w && sh == now.h, "surface %dx%d, texture %dx%d", sw, sh, now.w, now.h);
    gboolean whole = pw == now.w && ph == now.h;
    if (whole || t->ink.w != now.w || t->ink.h != now.h) {
        CHECK (whole, "\"%s\": a part (%dx%d of %dx%d) for a texture of another size", text, pw, ph,
               now.w, now.h);
        g_free (t->px);
        t->px = g_malloc0 ((gsize) now.w * now.h * 4);
        CHECK (part[0] == 0 && part[1] == 0, "whole texture not at 0 0");
    }
    CHECK (part[0] >= 0 && part[1] >= 0 && part[0] + pw <= now.w && part[1] + ph <= now.h,
           "\"%s\": part %d %d %dx%d outside the %dx%d texture", text, part[0], part[1], pw, ph,
           now.w, now.h);
    for (int y = 0; y < ph; y++)
        memcpy (t->px + ((gsize) (part[1] + y) * now.w + part[0]) * 4,
                src + (gsize) (part[1] + y) * stride + (gsize) part[0] * 4, (gsize) pw * 4);
    cairo_surface_destroy (sf);
    t->ink = now;
    if (part_px)
        *part_px = (gsize) pw * ph;
    if (!whole)
        partial_steps++;
}

static gboolean tex_matches_full (const Tex *t, const WalpeTextStyle *st, const char *text,
                                  const char *what)
{
    float q[4];
    cairo_surface_t *sf = walpe_text_render (st, text, q);
    if (!sf) {
        CHECK (FALSE, "%s: no full render of \"%s\"", what, text);
        return FALSE;
    }
    int w = cairo_image_surface_get_width (sf), h = cairo_image_surface_get_height (sf);
    int stride = cairo_image_surface_get_stride (sf);
    const guint8 *src = cairo_image_surface_get_data (sf);
    gboolean ok = w == t->ink.w && h == t->ink.h;
    CHECK (ok, "%s: \"%s\": texture %dx%d, full render %dx%d", what, text, t->ink.w, t->ink.h, w, h);
    int bad = 0, bx = -1, by = -1;
    for (int y = 0; ok && y < h; y++)
        if (memcmp (t->px + (gsize) y * w * 4, src + (gsize) y * stride, (gsize) w * 4)) {
            for (int x = 0; x < w; x++)
                if (memcmp (t->px + ((gsize) y * w + x) * 4, src + (gsize) y * stride + x * 4, 4)) {
                    if (!bad) {
                        bx = x;
                        by = y;
                    }
                    bad++;
                }
        }
    CHECK (bad == 0, "%s: \"%s\": %d pixels differ from the full render (first at %d %d; ink "
           "%d %d - %d %d of %dx%d)", what, text, bad, bx, by, t->ink.x0, t->ink.y0, t->ink.x1,
           t->ink.y1, w, h);
    /* and the ink rectangle really holds all the ink */
    int out = 0;
    for (int y = 0; ok && y < h; y++)
        for (int x = 0; x < w; x++) {
            gboolean in = x >= t->ink.x0 && x < t->ink.x1 && y >= t->ink.y0 && y < t->ink.y1;
            if (!in && src[(gsize) y * stride + x * 4 + 3])
                out++;
        }
    CHECK (out == 0, "%s: \"%s\": %d inked pixels outside the ink rectangle", what, text, out);
    cairo_surface_destroy (sf);
    steps++;
    return ok && bad == 0 && out == 0;
}

static const char *const STRINGS[] = {
    "12:34:56", "12:34:57", "12:35:00", "9", "Wednesday, 10 November", "fjgpqy|/\\_W",
    "\xc3\x85\xc3\x89\xc3\x9c \xc3\xa7\xc3\xb1 \xe2\x80\x94 \xe2\x82\xac",   /* accents, dash, euro */
    " ", "a", "  x  ", "line one\nline two", "line one\nand a much longer second line\n3",
    "The quick brown fox jumps over the lazy dog", "0", "00:00:00.000", "j", "ffi fl Tj AV",
    "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e",   /* CJK (a fallback font or boxes) */
    "12:34:56",
};

int main (void)
{
    walpe_text_hold ();
    /* styles: box (0 = grows with the text), padding, alignment, wrapping, size */
    static const struct { float bw, bh, pad, pt, maxw; int ha, va; gboolean limw; int rows; gboolean ell; } S[] = {
        { 0, 0, 0, 12, 0, 1, 1, FALSE, 0, FALSE },
        { 900, 300, 8, 16, 0, 0, 0, FALSE, 0, FALSE },
        { 900, 300, 8, 16, 0, 2, 2, FALSE, 0, FALSE },
        { 1600, 900, 32, 24, 0, 1, 1, FALSE, 0, FALSE },
        { 640, 200, 4, 7, 0, 1, 0, FALSE, 0, FALSE },
        { 700, 500, 10, 14, 300, 0, 1, TRUE, 0, FALSE },
        { 700, 500, 10, 14, 300, 2, 1, TRUE, 2, FALSE },
        { 700, 160, 0, 14, 260, 1, 2, TRUE, 1, TRUE },
        { 300, 60, 0, 20, 0, 0, 1, FALSE, 0, FALSE },     /* text larger than its box */
    };
    gsize big_whole = 0, big_part = 0;
    for (guint si = 0; si < G_N_ELEMENTS (S); si++) {
        WalpeTextStyle st = { 0 };
        st.pointsize = S[si].pt;
        st.box_w = S[si].bw; st.box_h = S[si].bh;
        st.padding = S[si].pad;
        st.halign = S[si].ha; st.valign = S[si].va;
        st.maxwidth = S[si].maxw; st.limitwidth = S[si].limw;
        st.maxrows = S[si].rows; st.limitrows = S[si].rows > 0; st.ellipsis = S[si].ell;
        Tex t = { 0 };
        for (guint k = 0; k < G_N_ELEMENTS (STRINGS); k++) {
            char what[64];
            g_snprintf (what, sizeof what, "style %u step %u", si, k);
            gsize ppx = 0;
            tex_update (&t, &st, STRINGS[k], &ppx);
            if (!tex_matches_full (&t, &st, STRINGS[k], what))
                break;
            if (si == 3 && k == 1) {
                /* the seconds digit of a clock in a 1600 x 900 box */
                big_whole = (gsize) t.ink.w * t.ink.h;
                big_part = ppx;
            }
        }
        /* a style change with the same texture size: the old ink goes too */
        if (t.ink.w > 0 && S[si].bw > 0) {
            st.pointsize *= 0.5f;
            tex_update (&t, &st, "resized 12:34", NULL);
            tex_matches_full (&t, &st, "resized 12:34", "half size");
            st.halign = (st.halign + 1) % 3;
            tex_update (&t, &st, "moved 12:34", NULL);
            tex_matches_full (&t, &st, "moved 12:34", "other alignment");
        }
        g_free (t.px);
    }
    CHECK (steps > 150, "only %d steps compared", steps);
    CHECK (partial_steps > 100, "only %d of %d updates took the partial path", partial_steps, steps);
    CHECK (big_whole >= (gsize) 1600 * 900, "big box: texture of %" G_GSIZE_FORMAT " pixels", big_whole);
    CHECK (big_part > 0 && big_part * 8 < big_whole,
           "clock in a 1600x900 box: %" G_GSIZE_FORMAT " of %" G_GSIZE_FORMAT " pixels uploaded "
           "(expected under an eighth)", big_part, big_whole);
    walpe_text_release ();
    printf ("text-part: %d steps (%d partial) compared with full renders, clock digit in a 1600x900 "
            "box: %" G_GSIZE_FORMAT " of %" G_GSIZE_FORMAT " pixels, %d failures\n", steps,
            partial_steps, big_part, big_whole, fails);
    return fails ? 1 : 0;
}
