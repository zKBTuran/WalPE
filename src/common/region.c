/* region.c - see region.h */
#include "common/region.h"
#include <stdlib.h>
#include <string.h>

void walpe_region_init (WalpeRegion *r)
{
    r->b = NULL;
    r->n = r->cap = 0;
}

void walpe_region_clear (WalpeRegion *r)
{
    g_free (r->b);
    walpe_region_init (r);
}

static void region_push (WalpeRegion *r, WalpeBox b)
{
    if (r->n == r->cap) {
        r->cap = r->cap ? r->cap * 2 : 8;
        r->b = g_renew (WalpeBox, r->b, r->cap);
    }
    r->b[r->n++] = b;
}

void walpe_region_copy (WalpeRegion *dst, const WalpeRegion *src)
{
    if (dst == src)
        return;
    dst->n = 0;
    for (int i = 0; i < src->n; i++)
        region_push (dst, src->b[i]);
}

static int cmp_int (const void *a, const void *c)
{
    int x = *(const int *) a, y = *(const int *) c;
    return (x > y) - (x < y);
}

typedef struct { int a, b; } Span;

static int cmp_span (const void *a, const void *c)
{
    const Span *x = a, *y = c;
    return (x->a > y->a) - (x->a < y->a);
}

/* the x spans of the boxes that cover the band [ya, yb), merged and
 * sorted; returns their number */
static int band_spans (const WalpeBox *b, int n, int ya, int yb, Span *out)
{
    int k = 0;
    for (int i = 0; i < n; i++)
        if (!walpe_box_empty (b[i]) && b[i].y0 <= ya && b[i].y1 >= yb)
            out[k++] = (Span) { b[i].x0, b[i].x1 };
    if (k < 2)
        return k;
    qsort (out, (size_t) k, sizeof (Span), cmp_span);
    int m = 0;
    for (int i = 1; i < k; i++) {
        if (out[i].a <= out[m].b)
            out[m].b = MAX (out[m].b, out[i].b);
        else
            out[++m] = out[i];
    }
    return m + 1;
}

enum { OP_UNION, OP_SUBTRACT };

/* out = A op B on one band: both span lists merged and sorted */
static int span_op (const Span *A, int na, const Span *B, int nb, int op, Span *out)
{
    int k = 0;
    if (op == OP_UNION) {
        int i = 0, j = 0;
        while (i < na || j < nb) {
            Span s = (j >= nb || (i < na && A[i].a <= B[j].a)) ? A[i++] : B[j++];
            if (k && s.a <= out[k - 1].b)
                out[k - 1].b = MAX (out[k - 1].b, s.b);
            else
                out[k++] = s;
        }
        return k;
    }
    int j = 0;
    for (int i = 0; i < na; i++) {
        int a = A[i].a;
        while (j < nb && B[j].b <= a)
            j++;
        for (int t = j; t < nb && B[t].a < A[i].b; t++) {
            if (B[t].a > a)
                out[k++] = (Span) { a, B[t].a };
            a = MAX (a, B[t].b);
        }
        if (a < A[i].b)
            out[k++] = (Span) { a, A[i].b };
    }
    return k;
}

/* r = (union of A) op (union of B), in canonical form. A and B may be
 * any lists of boxes; r may alias neither. */
static void region_op (WalpeRegion *r, const WalpeBox *A, int na, const WalpeBox *B, int nb, int op)
{
    r->n = 0;
    int total = na + nb;
    if (total == 0)
        return;
    int *ys = g_new (int, (gsize) total * 2);
    int ny = 0;
    for (int i = 0; i < na; i++)
        if (!walpe_box_empty (A[i])) {
            ys[ny++] = A[i].y0;
            ys[ny++] = A[i].y1;
        }
    for (int i = 0; i < nb; i++)
        if (!walpe_box_empty (B[i])) {
            ys[ny++] = B[i].y0;
            ys[ny++] = B[i].y1;
        }
    qsort (ys, (size_t) ny, sizeof (int), cmp_int);
    Span *sa = g_new (Span, (gsize) total * 3 + 3);
    Span *sb = sa + na + 1, *so = sb + nb + 1;
    int prev_first = -1, prev_n = 0;    /* the band emitted last */
    int prev_y1 = 0;
    for (int i = 0; i + 1 < ny; i++) {
        int ya = ys[i], yb = ys[i + 1];
        if (ya == yb)
            continue;
        int ka = band_spans (A, na, ya, yb, sa);
        int kb = band_spans (B, nb, ya, yb, sb);
        int ko = span_op (sa, ka, sb, kb, op, so);
        if (ko == 0)
            continue;
        /* same spans as the band right above: one taller band */
        gboolean same = prev_first >= 0 && prev_y1 == ya && prev_n == ko;
        for (int k = 0; same && k < ko; k++)
            same = r->b[prev_first + k].x0 == so[k].a && r->b[prev_first + k].x1 == so[k].b;
        if (same) {
            for (int k = 0; k < ko; k++)
                r->b[prev_first + k].y1 = yb;
        } else {
            prev_first = r->n;
            prev_n = ko;
            for (int k = 0; k < ko; k++)
                region_push (r, (WalpeBox) { so[k].a, ya, so[k].b, yb });
        }
        prev_y1 = yb;
    }
    g_free (sa);
    g_free (ys);
}

void walpe_region_set_boxes (WalpeRegion *r, const WalpeBox *b, int n)
{
    WalpeRegion t;
    walpe_region_init (&t);
    region_op (&t, b, n, NULL, 0, OP_UNION);
    g_free (r->b);
    *r = t;
}

void walpe_region_subtract_boxes (WalpeRegion *r, const WalpeBox *b, int n)
{
    if (r->n == 0 || n == 0)
        return;
    WalpeRegion t;
    walpe_region_init (&t);
    region_op (&t, r->b, r->n, b, n, OP_SUBTRACT);
    g_free (r->b);
    *r = t;
}

void walpe_region_add_boxes (WalpeRegion *r, const WalpeBox *b, int n)
{
    if (n == 0)
        return;
    WalpeRegion t;
    walpe_region_init (&t);
    region_op (&t, r->b, r->n, b, n, OP_UNION);
    g_free (r->b);
    *r = t;
}

void walpe_region_intersect_box (WalpeRegion *r, WalpeBox clip)
{
    WalpeBox *c = g_new (WalpeBox, (gsize) MAX (1, r->n));
    int k = 0;
    for (int i = 0; i < r->n; i++) {
        WalpeBox x = walpe_box_intersect (r->b[i], clip);
        if (!walpe_box_empty (x))
            c[k++] = x;
    }
    walpe_region_set_boxes (r, c, k);
    g_free (c);
}

void walpe_region_grow (WalpeRegion *r, int margin, WalpeBox bounds)
{
    WalpeBox *c = g_new (WalpeBox, (gsize) MAX (1, r->n));
    int k = 0;
    if (margin < 0)
        margin = 0;
    for (int i = 0; i < r->n; i++) {
        WalpeBox x = { r->b[i].x0 - margin, r->b[i].y0 - margin, r->b[i].x1 + margin,
                       r->b[i].y1 + margin };
        x = walpe_box_intersect (x, bounds);
        if (!walpe_box_empty (x))
            c[k++] = x;
    }
    walpe_region_set_boxes (r, c, k);
    g_free (c);
}

gint64 walpe_region_area (const WalpeRegion *r)
{
    gint64 a = 0;
    for (int i = 0; i < r->n; i++)
        a += walpe_box_area (r->b[i]);
    return a;
}

gboolean walpe_region_equal (const WalpeRegion *a, const WalpeRegion *c)
{
    return a->n == c->n && (a->n == 0 || !memcmp (a->b, c->b, (size_t) a->n * sizeof (WalpeBox)));
}

gboolean walpe_region_contains (const WalpeRegion *outer, const WalpeRegion *inner)
{
    if (inner->n == 0)
        return TRUE;
    WalpeRegion t;
    walpe_region_init (&t);
    region_op (&t, inner->b, inner->n, outer->b, outer->n, OP_SUBTRACT);
    gboolean in = t.n == 0;
    walpe_region_clear (&t);
    return in;
}

WalpeBox walpe_region_extents (const WalpeRegion *r)
{
    WalpeBox e = { 0, 0, 0, 0 };
    for (int i = 0; i < r->n; i++)
        e = walpe_box_hull (e, r->b[i]);
    return e;
}

/* join boxes i and j of b[0..n) and, so that the list stays free of
 * overlaps, every box their hull touches; returns the pixels this adds
 * (*hull = the joined box, taken[k] = box k went into it) */
static gint64 join_cost (const WalpeBox *b, int n, int i, int j, WalpeBox *hull, guint8 *taken)
{
    memset (taken, 0, (size_t) n);
    taken[i] = taken[j] = 1;
    WalpeBox h = walpe_box_hull (b[i], b[j]);
    gint64 inside = walpe_box_area (b[i]) + walpe_box_area (b[j]);
    for (gboolean again = TRUE; again;) {
        again = FALSE;
        for (int k = 0; k < n; k++)
            if (!taken[k] && walpe_box_overlaps (h, b[k])) {
                taken[k] = 1;
                inside += walpe_box_area (b[k]);
                h = walpe_box_hull (h, b[k]);
                again = TRUE;
            }
    }
    *hull = h;
    return walpe_box_area (h) - inside;
}

#define SIMPLIFY_WORK 48    /* boxes the pairwise search is run on */

int walpe_region_simplify (const WalpeRegion *r, WalpeBox *out, int max, gint64 slack)
{
    if (r->n == 0 || max < 1)
        return 0;
    if (r->n > SIMPLIFY_WORK || max == 1) {
        /* a window with a ragged shape left hundreds of slivers: the cost
         * of looking for the best pairs is not worth it */
        out[0] = walpe_region_extents (r);
        return 1;
    }
    WalpeBox b[SIMPLIFY_WORK];
    guint8 taken[SIMPLIFY_WORK], best_taken[SIMPLIFY_WORK];
    int n = r->n;
    memcpy (b, r->b, (size_t) n * sizeof (WalpeBox));
    while (n > 1) {
        gint64 best = -1;
        WalpeBox best_h = { 0, 0, 0, 0 };
        for (int i = 0; i < n; i++)
            for (int j = i + 1; j < n; j++) {
                WalpeBox h;
                gint64 c = join_cost (b, n, i, j, &h, taken);
                if (best < 0 || c < best) {
                    best = c;
                    best_h = h;
                    memcpy (best_taken, taken, (size_t) n);
                }
            }
        if (n <= max && best > slack)
            break;
        int k = 0;
        for (int i = 0; i < n; i++)
            if (!best_taken[i])
                b[k++] = b[i];
        b[k++] = best_h;
        n = k;
    }
    memcpy (out, b, (size_t) n * sizeof (WalpeBox));
    return n;
}
