/* region_test.c - rectangle-set arithmetic behind "Skip hidden areas"
 * (src/common/region.c): every operation is checked against a plain bitmap
 * of the same boxes, on fixed layouts (tiled, monocle, floating, two
 * monitors) and on random ones. No display, no GL. */
#include <glib.h>
#include <string.h>
#include "common/region.h"

#define W 96
#define H 64

typedef struct { guint8 px[H][W]; } Bitmap;

static void bm_fill (Bitmap *m, WalpeBox b, guint8 v)
{
    for (int y = MAX (0, b.y0); y < MIN (H, b.y1); y++)
        for (int x = MAX (0, b.x0); x < MIN (W, b.x1); x++)
            m->px[y][x] = v;
}

static void bm_from_region (Bitmap *m, const WalpeRegion *r)
{
    memset (m, 0, sizeof (*m));
    for (int i = 0; i < r->n; i++)
        for (int y = MAX (0, r->b[i].y0); y < MIN (H, r->b[i].y1); y++)
            for (int x = MAX (0, r->b[i].x0); x < MIN (W, r->b[i].x1); x++)
                m->px[y][x]++;     /* 2 = two boxes of the region overlap */
}

static gint64 bm_count (const Bitmap *m)
{
    gint64 n = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            n += m->px[y][x] != 0;
    return n;
}

/* the region is in canonical form and covers exactly the bitmap */
static void check_region (const WalpeRegion *r, const Bitmap *want)
{
    Bitmap got;
    bm_from_region (&got, r);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            g_assert_cmpint (got.px[y][x], <=, 1);          /* no overlap */
            g_assert_cmpint (got.px[y][x], ==, want->px[y][x] ? 1 : 0);
        }
    for (int i = 0; i < r->n; i++) {
        g_assert_false (walpe_box_empty (r->b[i]));
        if (i == 0)
            continue;
        const WalpeBox *p = &r->b[i - 1], *c = &r->b[i];
        gboolean same_band = p->y0 == c->y0 && p->y1 == c->y1;
        if (same_band)
            g_assert_cmpint (p->x1, <, c->x0);              /* sorted, not touching */
        else
            g_assert_cmpint (p->y1, <=, c->y0);             /* bands top to bottom */
    }
    g_assert_cmpint (walpe_region_area (r), ==, bm_count (want));
}

static WalpeBox rnd_box (GRand *g)
{
    int x = g_rand_int_range (g, -8, W), y = g_rand_int_range (g, -8, H);
    return (WalpeBox) { x, y, x + g_rand_int_range (g, 0, 50), y + g_rand_int_range (g, 0, 40) };
}

static void test_random_ops (void)
{
    GRand *g = g_rand_new_with_seed (12);
    const WalpeBox mon = { 0, 0, W, H };
    for (int round = 0; round < 400; round++) {
        WalpeBox a[6], s[6];
        int na = g_rand_int_range (g, 0, 7), ns = g_rand_int_range (g, 0, 7);
        Bitmap bm;
        memset (&bm, 0, sizeof (bm));
        for (int i = 0; i < na; i++) {
            a[i] = rnd_box (g);
            bm_fill (&bm, a[i], 1);
        }
        WalpeRegion r, r2;
        walpe_region_init (&r);
        walpe_region_init (&r2);
        walpe_region_set_boxes (&r, a, na);
        walpe_region_intersect_box (&r, mon);
        check_region (&r, &bm);

        /* the same boxes in another order give the same array */
        for (int i = na - 1; i >= 0; i--)
            walpe_region_add_boxes (&r2, &a[i], 1);
        walpe_region_intersect_box (&r2, mon);
        g_assert_true (walpe_region_equal (&r, &r2));

        for (int i = 0; i < ns; i++) {
            s[i] = rnd_box (g);
            bm_fill (&bm, s[i], 0);
        }
        walpe_region_copy (&r2, &r);
        walpe_region_subtract_boxes (&r, s, ns);
        check_region (&r, &bm);
        g_assert_true (walpe_region_contains (&r2, &r));
        g_assert_cmpint (walpe_region_contains (&r, &r2), ==, walpe_region_equal (&r, &r2));

        /* grow: every pixel within `m` (Chebyshev) of the region, in bounds */
        int m = g_rand_int_range (g, 0, 5);
        Bitmap grown;
        memset (&grown, 0, sizeof (grown));
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                if (bm.px[y][x])
                    bm_fill (&grown, (WalpeBox) { x - m, y - m, x + m + 1, y + m + 1 }, 1);
        walpe_region_copy (&r2, &r);
        walpe_region_grow (&r2, m, mon);
        check_region (&r2, &grown);
        g_assert_true (walpe_region_contains (&r2, &r));

        /* simplify: no overlap, nothing lost, the limit kept */
        for (int max = 1; max <= 6; max += 1 + round % 3) {
            WalpeBox out[8];
            int n = walpe_region_simplify (&r, out, max, round % 2 ? 0 : 40);
            g_assert_cmpint (n, <=, max);
            g_assert_cmpint (n == 0, ==, r.n == 0);
            Bitmap cover;
            memset (&cover, 0, sizeof (cover));
            for (int i = 0; i < n; i++)
                for (int y = MAX (0, out[i].y0); y < MIN (H, out[i].y1); y++)
                    for (int x = MAX (0, out[i].x0); x < MIN (W, out[i].x1); x++)
                        cover.px[y][x]++;
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++) {
                    g_assert_cmpint (cover.px[y][x], <=, 1);
                    if (bm.px[y][x])
                        g_assert_cmpint (cover.px[y][x], ==, 1);
                }
            /* with room for every box and no slack nothing is added */
            if (max >= r.n && round % 2) {
                gint64 a2 = 0;
                for (int i = 0; i < n; i++)
                    a2 += walpe_box_area (out[i]);
                g_assert_cmpint (a2, ==, walpe_region_area (&r));
            }
        }
        walpe_region_clear (&r);
        walpe_region_clear (&r2);
    }
    g_rand_free (g);
}

/* visible part of a 1920x1080 monitor under the window layouts of a
 * tiling window manager */
static void visible (WalpeRegion *r, const WalpeBox *wins, int n)
{
    const WalpeBox mon = { 0, 0, 1920, 1080 };
    walpe_region_set_boxes (r, &mon, 1);
    walpe_region_subtract_boxes (r, wins, n);
}

static void test_layouts (void)
{
    WalpeRegion r;
    WalpeBox out[8];
    walpe_region_init (&r);

    /* nothing open but a 22 px bar: one box below it */
    const WalpeBox bar = { 0, 0, 1920, 22 };
    visible (&r, &bar, 1);
    g_assert_cmpint (r.n, ==, 1);
    g_assert_cmpint (walpe_region_area (&r), ==, 1920 * (1080 - 22));

    /* tiled: master left (opaque), a see-through terminal right is not in
     * the list: the right half stays visible */
    const WalpeBox tiled[] = { bar, { 0, 22, 1056, 1080 } };
    visible (&r, tiled, 2);
    g_assert_cmpint (r.n, ==, 1);
    g_assert_cmpint (r.b[0].x0, ==, 1056);
    g_assert_cmpint (r.b[0].y0, ==, 22);
    g_assert_cmpint (walpe_region_simplify (&r, out, 4, 0), ==, 1);

    /* monocle: everything hidden */
    const WalpeBox mono[] = { bar, { 0, 22, 1920, 1080 } };
    visible (&r, mono, 2);
    g_assert_cmpint (r.n, ==, 0);
    g_assert_cmpint (walpe_region_simplify (&r, out, 4, 0), ==, 0);
    g_assert_true (walpe_box_empty (walpe_region_extents (&r)));

    /* floating window in the middle: a frame of four boxes, kept as four
     * (joining any two would add the window's own pixels) */
    const WalpeBox fl = { 600, 300, 1400, 800 };
    visible (&r, &fl, 1);
    g_assert_cmpint (r.n, ==, 4);
    g_assert_cmpint (walpe_region_area (&r), ==, 1920 * 1080 - 800 * 500);
    g_assert_cmpint (walpe_region_simplify (&r, out, 4, 4096), ==, 4);
    /* limit 2: still no overlap, all of it covered */
    int n = walpe_region_simplify (&r, out, 2, 0);
    g_assert_cmpint (n, <=, 2);
    gint64 a = 0;
    for (int i = 0; i < n; i++)
        a += walpe_box_area (out[i]);
    g_assert_cmpint (a, >=, walpe_region_area (&r));

    /* two windows side by side with a 2 px gap between them and the
     * screen edge: slivers are joined into the box next to them when
     * that costs less than the slack */
    const WalpeBox gap[] = { { 2, 2, 958, 1078 }, { 962, 2, 1918, 1078 } };
    visible (&r, gap, 2);
    g_assert_cmpint (walpe_region_area (&r), ==, 1920 * 1080 - 2 * 956 * 1076);
    n = walpe_region_simplify (&r, out, 6, 0);
    g_assert_cmpint (n, <=, 6);

    /* a window on another monitor (x >= 1920) hides nothing here */
    const WalpeBox other = { 1920, 0, 3840, 1080 };
    visible (&r, &other, 1);
    g_assert_cmpint (walpe_region_area (&r), ==, 1920 * 1080);

    /* a window that moved 10 px: the old region no longer holds the new
     * one (something became visible) */
    WalpeRegion r2;
    walpe_region_init (&r2);
    const WalpeBox fl2 = { 610, 300, 1410, 800 };
    visible (&r, &fl, 1);
    visible (&r2, &fl2, 1);
    g_assert_false (walpe_region_contains (&r, &r2));
    g_assert_false (walpe_region_equal (&r, &r2));
    /* a window that only grew: nothing new to draw */
    const WalpeBox fl3 = { 590, 290, 1410, 810 };
    visible (&r2, &fl3, 1);
    g_assert_true (walpe_region_contains (&r, &r2));

    walpe_region_clear (&r);
    walpe_region_clear (&r2);
}

static void test_many_boxes (void)
{
    /* a comb of 200 slivers (a shaped window): simplify answers with the
     * bounding box instead of searching pairs */
    WalpeBox comb[200];
    for (int i = 0; i < 200; i++)
        comb[i] = (WalpeBox) { i * 4, 0, i * 4 + 2, 50 + i % 7 };
    WalpeRegion r;
    walpe_region_init (&r);
    walpe_region_set_boxes (&r, comb, 200);
    g_assert_cmpint (r.n, >, 48);
    WalpeBox out[4];
    g_assert_cmpint (walpe_region_simplify (&r, out, 4, 0), ==, 1);
    g_assert_cmpint (out[0].x0, ==, 0);
    g_assert_cmpint (out[0].x1, ==, 199 * 4 + 2);
    walpe_region_clear (&r);
}

int main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/region/random-ops", test_random_ops);
    g_test_add_func ("/region/layouts", test_layouts);
    g_test_add_func ("/region/many-boxes", test_many_boxes);
    return g_test_run ();
}
