/* webdamage.h - what one captured page frame changed against the one
 * before it (web.c; tests/webdamage_test.c).
 *
 * WebKit's damage does not survive GTK 4 (a widget can only say "redraw
 * me"), so the new frame is compared with the previous one: rows from the
 * top and from the bottom until one differs, then columns inside that
 * band. The cost is that of the part that did NOT change (a frame that
 * changed everywhere is recognised at its first and last row), and that
 * part is then not uploaded - by any monitor. */
#pragma once
#include <glib.h>
#include <string.h>

typedef struct {
    int x0, y0, x1, y1;     /* pixels; x1 / y1 exclusive */
} WalpeWebDamage;

/* first / last byte at which two rows differ, -1 = equal */
static inline int walpe_row_first_diff (const guint8 *a, const guint8 *b, int n)
{
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        guint64 x, y;
        memcpy (&x, a + i, 8);
        memcpy (&y, b + i, 8);
        if (x != y)
            break;
    }
    for (; i < n; i++)
        if (a[i] != b[i])
            return i;
    return -1;
}

static inline int walpe_row_last_diff (const guint8 *a, const guint8 *b, int n)
{
    int i = n;
    for (; i >= 8; i -= 8) {
        guint64 x, y;
        memcpy (&x, a + i - 8, 8);
        memcpy (&y, b + i - 8, 8);
        if (x != y)
            break;
    }
    for (; i > 0; i--)
        if (a[i - 1] != b[i - 1])
            return i - 1;
    return -1;
}

/* two frames of w x h pixels, 4 bytes per pixel, stride w * 4.
 * FALSE = identical. TRUE: *d is a rectangle that holds every pixel that
 * differs (rows exact; columns exact when the band is at most half the
 * frame's height, else the full width). */
static inline gboolean walpe_web_frame_damage (const guint8 *n, const guint8 *o, int w, int h,
                                               WalpeWebDamage *d)
{
    int stride = w * 4;
    int y0 = 0, y1 = h;
    d->x0 = 0;
    d->y0 = 0;
    d->x1 = w;
    d->y1 = h;
    while (y0 < h && !memcmp (n + (gsize) y0 * stride, o + (gsize) y0 * stride, (gsize) stride))
        y0++;
    if (y0 == h)
        return FALSE;
    while (y1 > y0 + 1 &&
           !memcmp (n + (gsize) (y1 - 1) * stride, o + (gsize) (y1 - 1) * stride, (gsize) stride))
        y1--;
    d->y0 = y0;
    d->y1 = y1;
    /* columns: only worth a second pass over a band that is not most of
     * the frame anyway */
    if (y1 - y0 <= h / 2) {
        int x0 = stride, x1 = 0;    /* bytes: first differing, one past the last */
        for (int y = y0; y < y1 && (x0 > 0 || x1 < stride); y++) {
            const guint8 *a = n + (gsize) y * stride, *b = o + (gsize) y * stride;
            int f = walpe_row_first_diff (a, b, x0);                    /* only left of x0 matters */
            if (f >= 0)
                x0 = f;
            int l = walpe_row_last_diff (a + x1, b + x1, stride - x1);  /* only right of x1 */
            if (l >= 0)
                x1 += l + 1;
        }
        if (x0 < x1) {
            d->x0 = x0 / 4;
            d->x1 = (x1 + 3) / 4;
        }
    }
    return TRUE;
}
