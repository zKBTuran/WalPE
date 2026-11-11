/* webdamage_test.c - the frame comparison behind partial uploads of web
 * wallpaper frames (src/engine/webdamage.h).
 *
 * What must hold: uploading only the returned rectangle of the new frame
 * into a texture that holds the old frame gives exactly the new frame
 * (nothing that changed is left out), identical frames are recognised,
 * and the rectangle is tight where the header promises it.
 * No display, no GL. `meson test -C build webdamage` */
#include "engine/webdamage.h"
#include <stdio.h>
#include <stdlib.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf ("FAIL %s:%d: ", __FILE__, __LINE__); \
                                       printf (__VA_ARGS__); printf ("\n"); } } while (0)

static guint32 rng = 12345;
static guint32 rnd (void)
{
    rng = rng * 1664525u + 1013904223u;
    return rng >> 8;
}

/* change pixels in [x0,x1) x [y0,y1) of img; corners always, the rest at random */
static void scribble (guint8 *img, int w, int x0, int y0, int x1, int y1)
{
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            gboolean corner = (x == x0 || x == x1 - 1) && (y == y0 || y == y1 - 1);
            if (corner || rnd () % 3 == 0)
                img[((gsize) y * w + x) * 4 + rnd () % 4] ^= (guint8) (1 + rnd () % 255);
        }
}

static void apply (guint8 *tex, const guint8 *n, int w, const WalpeWebDamage *d)
{
    for (int y = d->y0; y < d->y1; y++)
        memcpy (tex + ((gsize) y * w + d->x0) * 4, n + ((gsize) y * w + d->x0) * 4,
                (gsize) (d->x1 - d->x0) * 4);
}

static void one (int w, int h, int x0, int y0, int x1, int y1, const char *what)
{
    gsize sz = (gsize) w * h * 4;
    guint8 *o = g_malloc (sz), *n = g_malloc (sz), *tex = g_malloc (sz);
    for (gsize i = 0; i < sz; i++)
        o[i] = (guint8) rnd ();
    memcpy (n, o, sz);
    if (x1 > x0 && y1 > y0)
        scribble (n, w, x0, y0, x1, y1);
    WalpeWebDamage d = { -1, -1, -1, -1 };
    gboolean changed = walpe_web_frame_damage (n, o, w, h, &d);
    if (x1 <= x0 || y1 <= y0) {
        CHECK (!changed, "%s: identical frames reported as changed", what);
    } else {
        CHECK (changed, "%s: change not seen", what);
        CHECK (d.x0 >= 0 && d.y0 >= 0 && d.x1 <= w && d.y1 <= h && d.x0 < d.x1 && d.y0 < d.y1,
               "%s: rectangle %d,%d-%d,%d outside %dx%d", what, d.x0, d.y0, d.x1, d.y1, w, h);
        CHECK (d.y0 == y0 && d.y1 == y1, "%s: rows %d-%d, want %d-%d", what, d.y0, d.y1, y0, y1);
        if (y1 - y0 <= h / 2)
            CHECK (d.x0 == x0 && d.x1 == x1, "%s: columns %d-%d, want %d-%d", what, d.x0, d.x1, x0, x1);
        else
            CHECK (d.x0 <= x0 && d.x1 >= x1, "%s: columns %d-%d do not hold %d-%d", what, d.x0, d.x1, x0, x1);
        memcpy (tex, o, sz);
        apply (tex, n, w, &d);
        CHECK (!memcmp (tex, n, sz), "%s: old frame + damage rectangle is not the new frame", what);
    }
    g_free (o);
    g_free (n);
    g_free (tex);
}

int main (void)
{
    one (64, 48, 0, 0, 0, 0, "no change");
    one (64, 48, 0, 0, 64, 48, "everything");
    one (64, 48, 0, 0, 1, 1, "first pixel");
    one (64, 48, 63, 47, 64, 48, "last pixel");
    one (64, 48, 0, 47, 1, 48, "bottom-left pixel");
    one (64, 48, 63, 0, 64, 1, "top-right pixel");
    one (64, 48, 10, 5, 11, 40, "tall thin column (band over half the height)");
    one (64, 48, 0, 20, 64, 21, "one full row");
    one (1, 1, 0, 0, 1, 1, "1x1 changed");
    one (1, 1, 0, 0, 0, 0, "1x1 same");
    one (3, 5, 1, 1, 2, 4, "tiny frame (rows shorter than a machine word)");
    one (1920, 1080, 100, 100, 164, 164, "64x64 in 1080p");
    for (int i = 0; i < 400; i++) {
        int w = 1 + (int) (rnd () % 97), h = 1 + (int) (rnd () % 71);
        int x0 = (int) (rnd () % (guint) w), y0 = (int) (rnd () % (guint) h);
        int x1 = x0 + 1 + (int) (rnd () % (guint) (w - x0)), y1 = y0 + 1 + (int) (rnd () % (guint) (h - y0));
        char what[96];
        g_snprintf (what, sizeof what, "random %d: %dx%d rect %d,%d-%d,%d", i, w, h, x0, y0, x1, y1);
        one (w, h, x0, y0, x1, y1, what);
    }
    /* two separate changes: the rectangle spans both */
    {
        int w = 80, h = 60;
        gsize sz = (gsize) w * h * 4;
        guint8 *o = g_malloc0 (sz), *n = g_malloc0 (sz), *tex = g_malloc0 (sz);
        n[(5 * w + 70) * 4] = 9;
        n[(20 * w + 3) * 4 + 2] = 7;
        WalpeWebDamage d;
        CHECK (walpe_web_frame_damage (n, o, w, h, &d), "two spots: not seen");
        CHECK (d.x0 == 3 && d.x1 == 71 && d.y0 == 5 && d.y1 == 21, "two spots: %d,%d-%d,%d", d.x0, d.y0,
               d.x1, d.y1);
        apply (tex, n, w, &d);
        CHECK (!memcmp (tex, n, sz), "two spots: not the new frame");
        g_free (o);
        g_free (n);
        g_free (tex);
    }
    printf (fails ? "webdamage: %d FAILED\n" : "webdamage: all passed\n", fails);
    return fails ? 1 : 0;
}
