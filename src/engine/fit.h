/* fit.h - how a flat picture (video, GIF, image wallpaper) is placed on a
 * monitor: WalPE's built-in "Alignment" options for those wallpaper types
 * (WE offers the same choices for its video wallpapers).
 *
 * The options are ordinary per-wallpaper property overrides, so
 * `walpe-ctl setprop walpe_fit=contain` and the GUI's option rows reach
 * them like any project.json property (walpe_content_builtin_props lists
 * their definitions):
 *   walpe_fit    "cover" (default: fill the monitor, crop the overflow),
 *                "contain" (whole picture, black bars), "stretch",
 *                "center" (1:1 pixels)
 *   walpe_pos_x  0..100, 50 = centred: which part is kept when cropping /
 *   walpe_pos_y  where the picture sits when it is smaller (0 = left / top)
 *   walpe_flip   mirror horizontally
 *
 * Static inline on purpose: the video module (walpe-video.so) uses it
 * without a new exported core function. */
#pragma once
#include <glib.h>
#include <math.h>
#include <string.h>

typedef enum { WALPE_FIT_COVER = 0, WALPE_FIT_CONTAIN, WALPE_FIT_STRETCH, WALPE_FIT_CENTER } WalpeFitMode;

typedef struct {
    int mode;            /* WalpeFitMode */
    gboolean flip_h;
    float pos_x, pos_y;  /* 0..1 */
} WalpeFit;

#define WALPE_FIT_DEFAULT { WALPE_FIT_COVER, FALSE, 0.5f, 0.5f }

static inline int walpe_fit_mode_from_string (const char *s)
{
    if (!s)
        return WALPE_FIT_COVER;
    if (!g_ascii_strcasecmp (s, "contain") || !g_ascii_strcasecmp (s, "fit"))
        return WALPE_FIT_CONTAIN;
    if (!g_ascii_strcasecmp (s, "stretch") || !g_ascii_strcasecmp (s, "fill"))
        return WALPE_FIT_STRETCH;
    if (!g_ascii_strcasecmp (s, "center") || !g_ascii_strcasecmp (s, "centre"))
        return WALPE_FIT_CENTER;
    return WALPE_FIT_COVER;
}

/* quad matrix (column-major 3x3, the +-1 quad of glutil) for an iw x ih
 * picture on a vw x vh view; *k = on-screen pixels per picture pixel (the
 * larger axis), for decode-size hints. Returns TRUE when the picture does
 * not cover the whole view (the caller clears first). */
static inline gboolean walpe_fit_matrix (const WalpeFit *f, float iw, float ih, float vw, float vh,
                                         float m[9], float *k)
{
    memset (m, 0, sizeof (float) * 9);
    m[0] = m[4] = m[8] = 1.0f;
    if (k)
        *k = 1.0f;
    if (iw < 1 || ih < 1 || vw < 1 || vh < 1)
        return FALSE;
    float kx = vw / iw, ky = vh / ih, sx, sy;
    switch (f ? f->mode : WALPE_FIT_COVER) {
    case WALPE_FIT_CONTAIN: kx = ky = fminf (kx, ky); break;
    case WALPE_FIT_STRETCH: break;
    case WALPE_FIT_CENTER:  kx = ky = 1.0f; break;
    default:                kx = ky = fmaxf (kx, ky); break;
    }
    sx = iw * kx / vw;
    sy = ih * ky / vh;
    float px = f ? CLAMP (f->pos_x, 0.0f, 1.0f) : 0.5f, py = f ? CLAMP (f->pos_y, 0.0f, 1.0f) : 0.5f;
    /* both factors positive keeps the picture upright (see walpe_mat3_cover) */
    m[0] = f && f->flip_h ? -sx : sx;
    m[4] = sy;
    m[6] = (1.0f - 2.0f * px) * (sx - 1.0f);
    m[7] = (2.0f * py - 1.0f) * (sy - 1.0f);
    if (k)
        *k = fmaxf (kx, ky);
    return sx < 0.9999f || sy < 0.9999f;
}
