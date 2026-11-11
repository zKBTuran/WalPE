/* region.h - sets of integer rectangles: which part of a monitor other
 * windows leave visible (daemon), and what the renderer is asked to draw
 * (engine/clip.h). Pure arithmetic: no X, no GL, no allocation in the hot
 * paths beyond the region's own array. Tests: tests/region_test.c. */
#pragma once
#include <glib.h>

/* half-open: [x0, x1) x [y0, y1); empty when x1 <= x0 or y1 <= y0 */
typedef struct { int x0, y0, x1, y1; } WalpeBox;

/* A region is kept in ONE canonical form, so two regions cover the same
 * pixels exactly when their arrays are equal: horizontal bands from top to
 * bottom, the boxes of a band from left to right, no two boxes of a band
 * touching, and two bands that follow each other never carrying the same
 * spans (they are one band then). */
typedef struct {
    WalpeBox *b;
    int n, cap;
} WalpeRegion;

static inline gboolean walpe_box_empty (WalpeBox a)
{
    return a.x1 <= a.x0 || a.y1 <= a.y0;
}

static inline gint64 walpe_box_area (WalpeBox a)
{
    return walpe_box_empty (a) ? 0 : (gint64) (a.x1 - a.x0) * (a.y1 - a.y0);
}

static inline WalpeBox walpe_box_intersect (WalpeBox a, WalpeBox c)
{
    WalpeBox r = { MAX (a.x0, c.x0), MAX (a.y0, c.y0), MIN (a.x1, c.x1), MIN (a.y1, c.y1) };
    if (walpe_box_empty (r))
        r = (WalpeBox) { 0, 0, 0, 0 };
    return r;
}

static inline gboolean walpe_box_overlaps (WalpeBox a, WalpeBox c)
{
    return a.x0 < c.x1 && c.x0 < a.x1 && a.y0 < c.y1 && c.y0 < a.y1;
}

/* smallest box holding both (an empty one does not count) */
static inline WalpeBox walpe_box_hull (WalpeBox a, WalpeBox c)
{
    if (walpe_box_empty (a))
        return c;
    if (walpe_box_empty (c))
        return a;
    return (WalpeBox) { MIN (a.x0, c.x0), MIN (a.y0, c.y0), MAX (a.x1, c.x1), MAX (a.y1, c.y1) };
}

void walpe_region_init (WalpeRegion *r);                 /* empty */
void walpe_region_clear (WalpeRegion *r);                /* frees the array */
void walpe_region_copy (WalpeRegion *dst, const WalpeRegion *src);
/* the union of n boxes (they may overlap; empty ones are ignored) */
void walpe_region_set_boxes (WalpeRegion *r, const WalpeBox *b, int n);
/* r = r minus the union of n boxes */
void walpe_region_subtract_boxes (WalpeRegion *r, const WalpeBox *b, int n);
/* r = r united with the union of n boxes */
void walpe_region_add_boxes (WalpeRegion *r, const WalpeBox *b, int n);
/* r = r cut to `clip` */
void walpe_region_intersect_box (WalpeRegion *r, WalpeBox clip);
/* every box grown by `margin` pixels on each side (margin >= 0), the
 * result cut to `bounds` */
void walpe_region_grow (WalpeRegion *r, int margin, WalpeBox bounds);
gint64 walpe_region_area (const WalpeRegion *r);
gboolean walpe_region_equal (const WalpeRegion *a, const WalpeRegion *c);
/* every pixel of `inner` is in `outer` */
gboolean walpe_region_contains (const WalpeRegion *outer, const WalpeRegion *inner);
WalpeBox walpe_region_extents (const WalpeRegion *r);    /* {0,0,0,0} when empty */

/* At most `max` boxes that do not overlap each other and together hold
 * every pixel of r (and as few others as the limit allows): what a
 * renderer loops over with one scissor rectangle each. Two boxes are also
 * joined, below the limit, when that adds no more than `slack` pixels -
 * a draw call per box costs more than shading a few pixels twice would.
 * Returns the number written (0 = r is empty). */
int walpe_region_simplify (const WalpeRegion *r, WalpeBox *out, int max, gint64 slack);
