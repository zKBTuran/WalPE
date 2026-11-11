/* yuvconv.h - YUV420p -> BGRA conversion (BT.601 / BT.709 / BT.2020,
 * limited or full range), plain C integer arithmetic: byte-identical on
 * every architecture. */
#pragma once
#include <glib.h>
#include "common/api.h"

/* dst: w*4 bytes per row, BGRA byte order, alpha=255.
 * w must be even; y/u/v strides in bytes. */
void walpe_yuv420_bgra_scalar (const guint8 *y, const guint8 *u, const guint8 *v,
                               gint ystride, gint uvstride,
                               guint8 *dst, gint dststride, gint w, gint h);

/* the same as walpe_yuv420_bgra_scalar (kept for its callers' name) */
void walpe_yuv420_bgra (const guint8 *y, const guint8 *u, const guint8 *v,
                        gint ystride, gint uvstride,
                        guint8 *dst, gint dststride, gint w, gint h);

/* always FALSE: no AVX2 kernel exists */
gboolean walpe_yuv_bgra_is_avx2 (void);

/* how the planes are coded (the last argument of the functions below and
 * of walpe_glutil_draw_yuv_range): range and matrix. 0 = BT.601 limited
 * and WALPE_YUV_FULL = 1, so callers passing a plain "full range" boolean
 * keep their meaning. */
enum {
    WALPE_YUV_FULL = 1 << 0,     /* yuvj420p / color_range "pc" */
    WALPE_YUV_BT709 = 1 << 1,    /* HD: what players assume for untagged HD video */
    WALPE_YUV_BT2020 = 1 << 2,   /* BT.2020 non-constant luminance (8 bit, no tone mapping) */
};
/* matrix for a w x h stream whose colorspace tag is `spc` (the H.264 VUI
 * matrix_coefficients / AVColorSpace numbers: 1 = BT.709, 5/6 = BT.601,
 * 9 = BT.2020 NCL, 2 = unspecified). Untagged video is BT.709 from 720p
 * up and BT.601 below, as mpv, VLC, browsers and Media Foundation decide
 * it; treating untagged 4K as BT.601 shifted its greens and reds. */
static inline guint walpe_yuv_matrix_for (int spc, int w, int h)
{
    switch (spc) {
    case 1: return WALPE_YUV_BT709;
    case 4: case 5: case 6: return 0;
    case 9: return WALPE_YUV_BT2020;
    default: return w >= 1280 || h > 576 ? WALPE_YUV_BT709 : 0;
    }
}

/* same with the coding chosen: mode = WALPE_YUV_* bits. Any w and h >= 1:
 * the chroma planes hold ceil(w/2) x ceil(h/2) samples, dst needs
 * dststride * h bytes with dststride >= 4 * w (nothing outside is read or
 * written, also for odd sizes) */
WALPE_API void walpe_yuv420_bgra_ex (const guint8 *y, const guint8 *u, const guint8 *v,
                           gint ystride, gint uvstride,
                           guint8 *dst, gint dststride, gint w, gint h, guint mode);
/* half-size BGRA (ow x oh, source >= 2ow x 2oh) from full-res planes,
 * converting 2x2 luma sums (10 bit) directly */
WALPE_API void walpe_yuv420_bgra_half (const guint8 *y, const guint8 *u, const guint8 *v,
                             gint ystride, gint uvstride,
                             guint8 *dst, gint dststride, gint ow, gint oh, guint mode);
/* the integer coefficients (x256) both the CPU converters and the GPU
 * shader use: k[0] = Y' gain, k[1] = Y' offset, k[2] = R from V,
 * k[3] = G from U, k[4] = G from V, k[5] = B from U */
WALPE_API void walpe_yuv_coeffs (guint mode, int k[6]);
