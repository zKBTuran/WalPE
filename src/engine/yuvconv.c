/* yuvconv.c - YUV420p -> BGRA (BT.601/709/2020, limited or full range).
 * Integer arithmetic in plain C: the same bytes on every CPU and with
 * every compiler (tests/simd_equal_test.c holds fixed results). There is
 * no hand-written kernel for it on any architecture; the compiler
 * vectorises the loops for whatever the build targets. */
#include "yuvconv.h"

#include <stdlib.h>
#include <string.h>

/* fixed-point coefficients (x256): limited range expands Y' 16..235 /
 * CbCr 16..240; full range (yuvj420p, color_range=pc) does not.
 * R = Y + 2(1-Kr) V, B = Y + 2(1-Kb) U, G = Y - 2Kb(1-Kb)/Kg U - 2Kr(1-Kr)/Kg V
 * with Kr/Kb = 0.299/0.114 (BT.601), 0.2126/0.0722 (BT.709),
 * 0.2627/0.0593 (BT.2020); limited chroma gain 255/224 */
typedef struct { int ky, yoff, rv, gu, gv, bu; } YuvK;
static const YuvK K_TAB[3][2] = {
    { { 298, 16, 409, 100, 208, 516 }, { 256, 0, 359, 88, 183, 454 } },   /* BT.601 */
    { { 298, 16, 459, 55, 136, 541 }, { 256, 0, 403, 48, 120, 475 } },    /* BT.709 */
    { { 298, 16, 430, 48, 167, 548 }, { 256, 0, 378, 42, 146, 482 } },    /* BT.2020 NCL */
};

static const YuvK *yuv_k (guint mode)
{
    int m = mode & WALPE_YUV_BT2020 ? 2 : mode & WALPE_YUV_BT709 ? 1 : 0;
    return &K_TAB[m][mode & WALPE_YUV_FULL ? 1 : 0];
}

void walpe_yuv_coeffs (guint mode, int k[6])
{
    const YuvK *c = yuv_k (mode);
    k[0] = c->ky; k[1] = c->yoff; k[2] = c->rv; k[3] = c->gu; k[4] = c->gv; k[5] = c->bu;
}

static inline guint8 clamp8 (int t)
{
    return (guint8) (t < 0 ? 0 : t > 255 ? 255 : t);
}

/* one pixel: luma sample Yv with the chroma pair (U, V) of its 2x2 block */
static inline void yuv_px (const YuvK *k, int Yv, int U, int V, guint8 *d)
{
    int Y = Yv - k->yoff;
    if (Y < 0)
        Y = 0;
    Y *= k->ky;
    d[2] = clamp8 ((Y + k->rv * V + 128) >> 8);
    d[1] = clamp8 ((Y - k->gu * U - k->gv * V + 128) >> 8);
    d[0] = clamp8 ((Y + k->bu * U + 128) >> 8);
    d[3] = 255;
}

/* w x h pixels from w x h luma and ceil(w/2) x ceil(h/2) chroma samples.
 * The 2x2 loop covers the even part of the picture. An odd width or
 * height (H.264 4:2:2 / 4:4:4 crops by single rows: such streams reach
 * here repacked to 4:2:0, video_custom.c cv_normalize) has a last column /
 * last row whose block is only half there: it is converted on its own.
 * The block loop used to run over it as well and wrote a row past the end
 * of dst (h odd: dststride bytes beyond a buffer of dststride * h) and
 * read a luma row / column that does not exist. */
void walpe_yuv420_bgra_ex (const guint8 *y, const guint8 *u, const guint8 *v,
                           gint ystride, gint uvstride,
                           guint8 *dst, gint dststride, gint w, gint h, guint mode)
{
    const YuvK *k = yuv_k (mode);
    if (w < 1 || h < 1)
        return;
    const gint we = w & ~1, he = h & ~1;
    for (gint r = 0; r < he; r += 2) {
        const guint8 *yA = y + (gsize) r * ystride;
        const guint8 *yB = yA + ystride;
        const guint8 *up = u + (gsize) (r / 2) * uvstride;
        const guint8 *vp = v + (gsize) (r / 2) * uvstride;
        guint8 *dA = dst + (gsize) r * dststride;
        guint8 *dB = dA + dststride;
        for (gint c = 0; c < we; c += 2) {
            int U = up[c / 2] - 128;
            int V = vp[c / 2] - 128;
            int uv_r = k->rv * V;
            int uv_g = -k->gu * U - k->gv * V;
            int uv_b = k->bu * U;
            for (int q = 0; q < 2; q++) {
                for (int row = 0; row < 2; row++) {
                    const guint8 *ys = row ? yB : yA;
                    guint8 *d = (row ? dB : dA) + (c + q) * 4;
                    int Y = ys[c + q] - k->yoff;
                    if (Y < 0)
                        Y = 0;
                    Y *= k->ky;
                    d[2] = clamp8 ((Y + uv_r + 128) >> 8);
                    d[1] = clamp8 ((Y + uv_g + 128) >> 8);
                    d[0] = clamp8 ((Y + uv_b + 128) >> 8);
                    d[3] = 255;
                }
            }
        }
    }
    if (w & 1) {    /* the last column, every row */
        const gint c = w - 1;
        for (gint r = 0; r < h; r++) {
            const gsize cr = (gsize) (r / 2) * uvstride + (gsize) (c / 2);
            yuv_px (k, y[(gsize) r * ystride + c], u[cr] - 128, v[cr] - 128,
                    dst + (gsize) r * dststride + (gsize) c * 4);
        }
    }
    if (h & 1) {    /* the last row (its last column is done above) */
        const gint r = h - 1;
        const guint8 *yA = y + (gsize) r * ystride;
        const guint8 *up = u + (gsize) (r / 2) * uvstride;
        const guint8 *vp = v + (gsize) (r / 2) * uvstride;
        guint8 *dA = dst + (gsize) r * dststride;
        for (gint c = 0; c < we; c++)
            yuv_px (k, yA[c], up[c / 2] - 128, vp[c / 2] - 128, dA + (gsize) c * 4);
    }
}

void walpe_yuv420_bgra_scalar (const guint8 *y, const guint8 *u, const guint8 *v,
                               gint ystride, gint uvstride,
                               guint8 *dst, gint dststride, gint w, gint h)
{
    walpe_yuv420_bgra_ex (y, u, v, ystride, uvstride, dst, dststride, w, h, FALSE);
}

/* half size straight from the full-resolution planes: each output pixel
 * is one 2x2 luma block (summed, 10 bits) with exactly the chroma sample
 * 4:2:0 assigns to that block. The old path box-filtered every plane to
 * 8 bits first (rounding away the 2 extra bits of the average, then
 * re-expanding 219 -> 255 levels: ~1/3 fewer distinct colours in smooth
 * gradients) and averaged chroma to quarter resolution. */
void walpe_yuv420_bgra_half (const guint8 *y, const guint8 *u, const guint8 *v,
                             gint ystride, gint uvstride,
                             guint8 *dst, gint dststride, gint ow, gint oh, guint mode)
{
    const YuvK *k = yuv_k (mode);
    for (gint r = 0; r < oh; r++) {
        const guint8 *y0 = y + (gsize) (2 * r) * ystride;
        const guint8 *y1 = y0 + ystride;
        const guint8 *up = u + (gsize) r * uvstride;
        const guint8 *vp = v + (gsize) r * uvstride;
        guint8 *d = dst + (gsize) r * dststride;
        for (gint c = 0; c < ow; c++) {
            int Ys = y0[2 * c] + y0[2 * c + 1] + y1[2 * c] + y1[2 * c + 1] - 4 * k->yoff;
            if (Ys < 0)
                Ys = 0;
            int U = up[c] - 128, V = vp[c] - 128;
            int Y = Ys * k->ky;                     /* x1024 scale */
            d[c * 4 + 2] = clamp8 ((Y + 4 * k->rv * V + 512) >> 10);
            d[c * 4 + 1] = clamp8 ((Y - 4 * k->gu * U - 4 * k->gv * V + 512) >> 10);
            d[c * 4 + 0] = clamp8 ((Y + 4 * k->bu * U + 512) >> 10);
            d[c * 4 + 3] = 255;
        }
    }
}

void walpe_yuv420_bgra (const guint8 *y, const guint8 *u, const guint8 *v,
                        gint ystride, gint uvstride,
                        guint8 *dst, gint dststride, gint w, gint h)
{
    walpe_yuv420_bgra_scalar (y, u, v, ystride, uvstride, dst, dststride, w, h);
}

/* An assembly kernel was dispatched here once; it is gone. The answer was
 * computed with a flag whose "looked, none" value equalled its "have
 * AVX2" value, so it said TRUE on every CPU, x86 or not. */
gboolean walpe_yuv_bgra_is_avx2 (void)
{
    return FALSE;
}
