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
#include <glib/gstdio.h>
#include <cairo.h>
#include <fontconfig/fontconfig.h>
#include <malloc.h>
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

/* bytes the C library has handed out (0 where it cannot say: under
 * AddressSanitizer) */
static gsize heap_in_use (void)
{
#if defined (__GLIBC__) && defined (__GLIBC_PREREQ) && __GLIBC_PREREQ (2, 33)
    struct mallinfo2 mi = mallinfo2 ();
    return mi.uordblks + mi.hblkhd;
#else
    return 0;
#endif
}

static const char *const STRINGS[] = {
    "12:34:56", "12:34:57", "12:35:00", "9", "Wednesday, 10 November", "fjgpqy|/\\_W",
    "\xc3\x85\xc3\x89\xc3\x9c \xc3\xa7\xc3\xb1 \xe2\x80\x94 \xe2\x82\xac",   /* accents, dash, euro */
    " ", "a", "  x  ", "line one\nline two", "line one\nand a much longer second line\n3",
    "The quick brown fox jumps over the lazy dog", "0", "00:00:00.000", "j", "ffi fl Tj AV",
    "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e",   /* CJK (a fallback font or boxes) */
    "12:34:56",
};

/* `from` written as `to`; with fam_from / fam_to (of one length) the copy
 * calls itself fam_to: the strings of the name table are replaced where
 * they stand, as bytes and as UTF-16 (no reader checks the table's sum) */
static gboolean font_copy (const char *from, const char *to, const char *fam_from,
                           const char *fam_to)
{
    char *d = NULL;
    gsize n = 0;
    if (!g_file_get_contents (from, &d, &n, NULL))
        return FALSE;
    const gsize l = fam_from ? strlen (fam_from) : 0;
    for (gsize i = 0; l && i + 2 * l <= n; i++) {
        if (!memcmp (d + i, fam_from, l)) {
            memcpy (d + i, fam_to, l);
            continue;
        }
        gsize k = 0;
        while (k < l && d[i + 2 * k] == 0 && d[i + 2 * k + 1] == fam_from[k])
            k++;
        for (gsize j = 0; k == l && j < l; j++)
            d[i + 2 * j + 1] = fam_to[j];
    }
    gboolean ok = g_file_set_contents (to, d, (gssize) n, NULL);
    g_free (d);
    return ok;
}

static gboolean tex_same (const Tex *a, const Tex *b)
{
    return a->px && b->px && a->ink.w == b->ink.w && a->ink.h == b->ink.h &&
           !memcmp (a->px, b->px, (gsize) a->ink.w * a->ink.h * 4);
}

/* The font FILES of a wallpaper go with the wallpaper, also while another
 * wallpaper with text stays up: they are not kept registered (a pattern
 * per face of every file, for as long as any text was shown), a later
 * wallpaper whose font has the family and style of one that left is drawn
 * with its own file, and the text of the wallpaper that stays is drawn as
 * before, byte for byte. Runs with this machine's TrueType files as the
 * wallpapers' fonts and no system fonts (a system font of the same name
 * would be taken instead of any wallpaper's file). */
static void font_files_leave (void)
{
    enum { MAXF = 16 };
    char *file[MAXF], *fam[MAXF];
    int nf = 0, a = -1, b = -1, c = -1;
    FcInit ();
    FcPattern *any = FcPatternCreate ();
    FcObjectSet *os = FcObjectSetBuild (FC_FILE, FC_FAMILY, FC_STYLE, FC_INDEX, NULL);
    FcFontSet *all = any && os ? FcFontList (NULL, any, os) : NULL;
    for (int i = 0; all && i < all->nfont && nf < MAXF; i++) {
        FcChar8 *f = NULL, *fa = NULL, *sy = NULL;
        int idx = 0;
        GStatBuf sb;
        if (FcPatternGetString (all->fonts[i], FC_FILE, 0, &f) != FcResultMatch ||
            FcPatternGetString (all->fonts[i], FC_FAMILY, 0, &fa) != FcResultMatch ||
            FcPatternGetString (all->fonts[i], FC_STYLE, 0, &sy) != FcResultMatch ||
            strcmp ((const char *) sy, "Regular") ||
            (FcPatternGetInteger (all->fonts[i], FC_INDEX, 0, &idx) == FcResultMatch && idx) ||
            !g_str_has_suffix ((const char *) f, ".ttf") ||
            g_stat ((const char *) f, &sb) || sb.st_size > 2 * 1024 * 1024)
            continue;
        gboolean have = FALSE;
        for (int k = 0; k < nf; k++)
            have |= !strcmp (fam[k], (const char *) fa);
        if (have)
            continue;
        file[nf] = g_strdup ((const char *) f);
        fam[nf++] = g_strdup ((const char *) fa);
    }
    if (all)
        FcFontSetDestroy (all);
    if (os)
        FcObjectSetDestroy (os);
    if (any)
        FcPatternDestroy (any);
    /* two families with names of one length (a and b), and a third font */
    for (int i = 0; i < nf && a < 0; i++)
        for (int k = i + 1; k < nf && a < 0; k++)
            if (strlen (fam[i]) == strlen (fam[k])) {
                a = i;
                b = k;
            }
    for (int i = 0; i < nf && c < 0; i++)
        if (i != a && i != b)
            c = i;
    char *dir = g_dir_make_tmp ("walpe-text-part-XXXXXX", NULL);
    if (!dir || c < 0) {
        printf ("text-part: font files of a wallpaper that left: not checked (%d usable TrueType "
                "files here)\n", nf);
        return;
    }
    char *conf = g_build_filename (dir, "fonts.conf", NULL);
    char *stay = g_build_filename (dir, "stay.ttf", NULL);
    char *one = g_build_filename (dir, "one.ttf", NULL);
    char *two = g_build_filename (dir, "two.ttf", NULL);
    char *left = g_build_filename (dir, "left.ttf", NULL);
    char *old = g_strdup (g_getenv ("FONTCONFIG_FILE"));
    g_file_set_contents (conf, "<?xml version=\"1.0\"?>\n<fontconfig></fontconfig>\n", -1, NULL);
    g_setenv ("FONTCONFIG_FILE", conf, TRUE);
    WalpeTextStyle kst = { 0 }, st1 = { 0 }, st2 = { 0 }, stl = { 0 };
    kst.pointsize = 20;
    kst.box_w = 900; kst.box_h = 300;
    kst.halign = kst.valign = 1;
    st1 = st2 = stl = kst;
    st1.pointsize = st2.pointsize = stl.pointsize = 31;
    kst.font_path = stay;
    st1.font_path = one;
    st2.font_path = two;
    stl.font_path = left;
    CHECK (font_copy (file[c], stay, NULL, NULL), "no copy of %s", file[c]);
    static const char *const TXT = "Wallpaper 0123456789 agQ";
    Tex kt = { 0 }, r1 = { 0 }, r2 = { 0 };
    gboolean pair = a >= 0;
    if (pair) {
        /* what each of the two files draws when it is the only font there is */
        CHECK (font_copy (file[a], one, NULL, NULL) && font_copy (file[b], two, fam[b], fam[a]),
               "no copies of %s and %s", file[a], file[b]);
        walpe_text_hold ();
        tex_update (&r1, &st1, TXT, NULL);
        walpe_text_release ();
        walpe_text_hold ();
        tex_update (&r2, &st2, TXT, NULL);
        walpe_text_release ();
        pair = !tex_same (&r1, &r2);
    }
    walpe_text_hold ();
    tex_update (&kt, &kst, "12:34:56", NULL);
    if (pair) {
        walpe_text_hold ();
        tex_matches_full (&r1, &st1, TXT, "the first wallpaper's font");
        walpe_text_release ();
        tex_matches_full (&kt, &kst, "12:34:56", "text of the wallpaper that stays");
        walpe_text_hold ();
        tex_matches_full (&r2, &st2, TXT, "a later wallpaper's font with the family and style of "
                          "one that left");
        walpe_text_release ();
        printf ("text-part: a font named %s like the one of a wallpaper that left (%s) is drawn "
                "from its own file\n", fam[a], fam[b]);
    } else
        printf ("text-part: a later font with the family of one that left: not checked (no two "
                "families with names of one length here)\n");
    gsize after2 = 0;
    for (int cyc = 0; cyc < 26; cyc++) {
        Tex t = { 0 };
        font_copy (file[cyc % nf], left, NULL, NULL);
        stl.pointsize = 24 + (float) cyc;
        walpe_text_hold ();
        tex_update (&t, &stl, TXT, NULL);
        g_free (t.px);
        walpe_text_release ();
        g_unlink (left);
        g_free (left);
        left = g_strdup_printf ("%s/left-%d.ttf", dir, cyc);
        stl.font_path = left;
        tex_matches_full (&kt, &kst, "12:34:56", "text of the wallpaper that stays");
        if (cyc == 1)
            after2 = heap_in_use ();
    }
    gsize now = heap_in_use ();
    gsize grown = now > after2 ? now - after2 : 0;
    CHECK (grown <= 96 * 1024, "24 more wallpapers with a font file of their own, next to one with "
           "text: %" G_GSIZE_FORMAT " bytes more on the heap", grown);
    printf ("text-part: 24 wallpapers with a font file of their own, next to one with text, leave %"
            G_GSIZE_FORMAT " bytes more on the heap\n", grown);
    walpe_text_release ();
    g_free (kt.px);
    g_free (r1.px);
    g_free (r2.px);
    if (old)
        g_setenv ("FONTCONFIG_FILE", old, TRUE);
    else
        g_unsetenv ("FONTCONFIG_FILE");
    g_unlink (conf);
    g_unlink (stay);
    g_unlink (one);
    g_unlink (two);
    g_unlink (left);
    g_rmdir (dir);
    for (int i = 0; i < nf; i++) {
        g_free (file[i]);
        g_free (fam[i]);
    }
    g_free (conf); g_free (stay); g_free (one); g_free (two); g_free (left); g_free (old); g_free (dir);
}

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
    /* The text code's own font configuration and font map go when the last
     * holder lets go of them (walpe_text_release): setting text up and
     * releasing it again does not make the heap grow. (LeakSanitizer cannot
     * tell: tests/lsan.supp has to list fontconfig as a whole.) */
    {
        gsize after2 = 0, grown = 0;
        for (int cyc = 0; cyc < 10; cyc++) {
            WalpeTextStyle st = { 0 };
            st.pointsize = 14;
            st.halign = st.valign = 1;
            Tex t = { 0 };
            walpe_text_hold ();
            tex_update (&t, &st, "12:34:56", NULL);
            g_free (t.px);
            walpe_text_release ();
            if (cyc == 1)
                after2 = heap_in_use ();
        }
        gsize now = heap_in_use ();
        grown = now > after2 ? now - after2 : 0;
        CHECK (grown <= 8 * 32 * 1024,
               "8 more rounds of text set up and released: %" G_GSIZE_FORMAT " bytes more on the heap", grown);
        printf ("text-part: 8 rounds of set-up and release leave %" G_GSIZE_FORMAT " bytes more on the heap\n", grown);
    }
    /* The glyphs of a wallpaper's fonts go with the wallpaper: 24 scenes
     * with a text size of their own each, one after the other, do not leave
     * their glyph bitmaps behind (cairo kept the last 256 fonts nobody used
     * any more, with every glyph: 7 to 10 MB here). Alone, and next to a
     * scene whose text stays - which keeps the font map, and is drawn as
     * before when its text changes after another scene has left. */
    for (int other = 0; other < 2; other++) {
        WalpeTextStyle kst = { 0 };
        kst.pointsize = 20;
        kst.box_w = 900; kst.box_h = 300;
        kst.halign = kst.valign = 1;
        Tex kt = { 0 };
        if (other) {
            walpe_text_hold ();
            tex_update (&kt, &kst, "12:34:56", NULL);
        }
        gsize after2 = 0;
        for (int cyc = 0; cyc < 26; cyc++) {
            WalpeTextStyle st = { 0 };
            st.pointsize = 24 + (float) cyc + 0.5f * (float) other;   /* (sizes no round before had) */
            st.halign = st.valign = 1;
            Tex t = { 0 };
            walpe_text_hold ();
            tex_update (&t, &st, "-12:34:56- Wednesday", NULL);
            g_free (t.px);
            walpe_text_release ();
            if (other) {
                const char *now = cyc & 1 ? "12:34:57" : "12:34:58";
                tex_update (&kt, &kst, now, NULL);
                tex_matches_full (&kt, &kst, now, "text of a scene that stays");
            }
            if (cyc == 1)
                after2 = heap_in_use ();
        }
        gsize now = heap_in_use ();
        gsize grown = now > after2 ? now - after2 : 0;
        CHECK (grown <= 1024 * 1024,
               "24 more scenes with a text size of their own%s: %" G_GSIZE_FORMAT " bytes more on "
               "the heap", other ? ", next to a scene with text" : "", grown);
        printf ("text-part: 24 scenes with a text size of their own%s leave %" G_GSIZE_FORMAT
                " bytes more on the heap\n", other ? ", next to a scene with text" : "", grown);
        if (other) {
            g_free (kt.px);
            walpe_text_release ();
        }
    }
    font_files_leave ();
    printf ("text-part: %d steps (%d partial) compared with full renders, clock digit in a 1600x900 "
            "box: %" G_GSIZE_FORMAT " of %" G_GSIZE_FORMAT " pixels, %d failures\n", steps,
            partial_steps, big_part, big_whole, fails);
    return fails ? 1 : 0;
}
