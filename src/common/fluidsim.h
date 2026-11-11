/* fluidsim.h - the parts of a simulation effect that need no GPU
 *
 * Wallpaper Engine's Fluid Simulation is an effect whose passes read what
 * they wrote in the frame before: velocity, pressure and dye live in render
 * targets from frame to frame, and a "swap" command at the end of the pass
 * list makes this frame's result the next frame's input. The executor
 * (engine/fx_sim.h) runs such a chain; this file holds what can be said and
 * tested without a graphics context:
 *
 *  - how many simulation steps a frame runs (a fixed step with an
 *    accumulator, so the fluid moves at the same speed at 30, 60 and 144
 *    frames per second);
 *  - the size of a target declared with fbos[].fit;
 *  - the pointer as the simulation sees it: one position per step along
 *    the path the pointer took since the step before, and no path at all
 *    across a gap (the pointer was not on this monitor);
 *  - the matrix that takes the pointer from the frame to the layer;
 *  - the quality levels and their names.
 *
 * Plain data and pure functions (header only): the engine, the daemon and
 * the tests share them. */
#pragma once
#include <glib.h>
#include <math.h>
#include <string.h>

G_BEGIN_DECLS

/* ---- quality ----------------------------------------------------------
 * config.json / IPC key "fluid_quality". What a level may change is chosen
 * so that the fluid keeps its speed and its shape on screen:
 *  - the step rate (how often the fields are advanced per second of scene
 *    time; the step length follows, so a second of smoke stays a second);
 *  - the size of the picture-sized fields (the dye: what is seen), as a
 *    divisor of the size the effect declares;
 *  - how many steps one frame may run to catch up.
 * The grid of the flow itself (fbos[].fit) is never reduced: the shaders
 * measure speed in grid texels, a coarser grid would be a faster fluid. */
typedef enum {
    WALPE_FLUID_FULL = 0,   /* as the effect declares it, 60 steps a second */
    WALPE_FLUID_REDUCED,    /* dye at half its declared width and height */
    WALPE_FLUID_LOW,        /* ... and 30 steps a second */
    WALPE_FLUID_N
} WalpeFluidQuality;

typedef struct {
    double step_hz;     /* simulation steps per second of scene time */
    int max_steps;      /* the most steps one frame runs; time beyond that is
                         * dropped (the fluid slows down instead of the frame) */
    int field_div;      /* picture-sized state targets at 1 / field_div */
} WalpeFluidParams;

static inline const WalpeFluidParams *walpe_fluid_params (WalpeFluidQuality q)
{
    static const WalpeFluidParams p[WALPE_FLUID_N] = {
        [WALPE_FLUID_FULL]    = { 60.0, 4, 1 },
        [WALPE_FLUID_REDUCED] = { 60.0, 3, 2 },
        [WALPE_FLUID_LOW]     = { 30.0, 2, 2 },
    };
    return &p[q >= WALPE_FLUID_FULL && q < WALPE_FLUID_N ? q : WALPE_FLUID_FULL];
}

static inline const char *walpe_fluid_quality_name (WalpeFluidQuality q)
{
    static const char *const n[WALPE_FLUID_N] = { "full", "reduced", "low" };
    return n[q >= WALPE_FLUID_FULL && q < WALPE_FLUID_N ? q : WALPE_FLUID_FULL];
}

/* FALSE: not a level's name (*q untouched) */
static inline gboolean walpe_fluid_quality_parse (const char *s, WalpeFluidQuality *q)
{
    for (int i = 0; s && i < WALPE_FLUID_N; i++)
        if (!g_ascii_strcasecmp (s, walpe_fluid_quality_name ((WalpeFluidQuality) i))) {
            if (q)
                *q = (WalpeFluidQuality) i;
            return TRUE;
        }
    return FALSE;
}

/* ---- the fixed step ------------------------------------------------------
 * *acc holds the scene time not simulated yet. A frame adds the time since
 * the frame before and runs as many whole steps of length h as fit.
 *
 * A display that runs at the step rate delivers frames a little early and a
 * little late in turn; counted strictly, such a frame would run no step and
 * the next one two - the smoke would stutter although nothing is slow. So a
 * step is taken up to WALPE_FLUID_SLACK of its length early (the remainder
 * may go that far below zero); the long-term rate is unchanged.
 *
 * More than max_steps are never run in one frame: what is left over is
 * dropped. A frame rate below step rate / max_steps therefore slows the
 * fluid down instead of making slow frames slower still. */
#define WALPE_FLUID_SLACK 0.2

static inline int walpe_fluid_steps (double *acc, double elapsed, double h, int max_steps)
{
    if (!(h > 0.0) || max_steps < 1)
        return 0;
    if (!(elapsed > 0.0) || !isfinite (elapsed))
        elapsed = 0.0;
    double a = *acc + elapsed;
    if (!isfinite (a))
        a = 0.0;
    double f = floor (a / h + WALPE_FLUID_SLACK);
    if (f < 0.0)
        f = 0.0;
    if (f > (double) max_steps) {
        *acc = 0.0;
        return max_steps;
    }
    *acc = a - f * h;
    return (int) f;
}

/* the steps a frame `elapsed` seconds after the last one would run, without
 * running them */
static inline int walpe_fluid_steps_peek (double acc, double elapsed, double h, int max_steps)
{
    return walpe_fluid_steps (&acc, elapsed, h, max_steps);
}

/* ---- fbos[].fit ------------------------------------------------------------
 * A target whose longer side has `fit` texels, in the proportions of the
 * w x h layer: the simulation grid has the same number of cells whatever
 * the layer's or the monitor's size. (Wallpaper Engine's files say no more
 * than the number; the stock shaders correct for the grid's aspect ratio,
 * so the grid is not square.) */
/* The most cells a grid gets on its longer side. An effect file comes from
 * the Steam Workshop: what it declares is not to be trusted with the video
 * memory. The stock effect asks for 256; 1024 is 16 times its cells (a
 * float target of at most 4 MiB, 64 MiB for the 16 targets a chain may
 * have). A file that asks for more gets this many (the executor says so
 * once per effect). */
#define WALPE_FLUID_FIT_MAX 1024

/* fbos[].fit as an effect file declares it -> the cells the grid gets:
 * 0 = no fit (nothing declared, not a number, less than one cell: the
 * target then has the size its "scale" gives), else the whole number of
 * cells, at most WALPE_FLUID_FIT_MAX. *asked (may be NULL) = what the file
 * declared, as an int (anything beyond a thousand million counts as that),
 * so that the caller can tell a number that was reduced. */
static inline int walpe_fluid_fit_cells (double declared, int *asked)
{
    /* (a NaN compares false and is no fit; an infinity is a huge number) */
    const int a = declared >= 1.0 ? (declared > 1e9 ? 1000000000 : (int) declared) : 0;
    if (asked)
        *asked = a;
    return MIN (a, WALPE_FLUID_FIT_MAX);
}

/* A target of *w x *h of which no side may be longer than max_side (the
 * largest texture a graphics driver makes): both sides shrink by the same
 * factor, none below 1. TRUE = it was reduced. max_side < 1: the limit is
 * not known, nothing changes. */
static inline gboolean walpe_fluid_limit_size (int *w, int *h, int max_side)
{
    const int longer = MAX (*w, *h);
    if (max_side < 1 || longer <= max_side)
        return FALSE;
    const double f = (double) max_side / (double) longer;
    const int nw = (int) floor ((double) *w * f + 0.5), nh = (int) floor ((double) *h * f + 0.5);
    *w = CLAMP (nw, 1, max_side);
    *h = CLAMP (nh, 1, max_side);
    return TRUE;
}

static inline void walpe_fluid_fit_size (int w, int h, int fit, int *tw, int *th)
{
    const double lw = MAX (w, 1), lh = MAX (h, 1);
    const double longer = MAX (lw, lh);
    fit = CLAMP (fit, 1, WALPE_FLUID_FIT_MAX);
    int x = (int) floor (lw * (double) fit / longer + 0.5);
    int y = (int) floor (lh * (double) fit / longer + 0.5);
    *tw = CLAMP (x, 1, WALPE_FLUID_FIT_MAX);
    *th = CLAMP (y, 1, WALPE_FLUID_FIT_MAX);
}

/* ---- the pointer ------------------------------------------------------------
 * Positions are 0..1 over the frame, y from the top (the convention of
 * g_PointerPosition that the stock shaders' arithmetic requires).
 *
 * The simulation turns pointer movement into force along the line from the
 * position at the step before (g_PointerPositionLast) to the position now
 * (g_PointerPosition). Three things follow:
 *  - a frame that runs several steps hands each step its share of the
 *    path, so the force is spread along the whole movement;
 *  - a frame that runs no step leaves the start of the path where the last
 *    step ended: the movement is not lost, the next step covers it;
 *  - when the pointer was not there (it is on another monitor; on Wayland
 *    it is over a window) and comes back, no line leads from where it was
 *    last seen to where it is now. The same holds for a jump no hand makes
 *    (WALPE_FLUID_TELEPORT: a shared scene follows the pointer from one
 *    monitor to the other).
 * While there is no pointer both positions are WALPE_FLUID_AWAY, far
 * outside the picture: a shader that draws something at the pointer draws
 * nothing. */
#define WALPE_FLUID_AWAY (-16.0f)
/* frame widths per second beyond which a movement counts as a jump */
#define WALPE_FLUID_TELEPORT 20.0f

typedef struct {
    gboolean have;      /* x, y: where the last step (or sample) left the pointer */
    float x, y;
    double since;       /* ... and the scene time that has passed since */
    gboolean fhave;     /* fx, fy: this frame's sample */
    float fx, fy;
    gboolean jump;      /* this frame's sample is not connected to x, y */
} WalpeFluidPointer;

/* once per frame, before its steps: the pointer now (present = FALSE: there
 * is none); elapsed = seconds of scene time since the frame before */
static inline void walpe_fluid_pointer_frame (WalpeFluidPointer *p, gboolean present, float x,
                                              float y, double elapsed)
{
    present = present && isfinite (x) && isfinite (y);
    p->fhave = present;
    p->jump = FALSE;
    if (p->have && elapsed > 0.0 && isfinite (elapsed))
        p->since += elapsed;
    if (!present)
        return;
    p->fx = x;
    p->fy = y;
    if (p->have) {
        /* the speed over the whole way since x, y: frames without a step
         * in between must not make a quick stroke look like a jump */
        const float dx = x - p->x, dy = y - p->y;
        const double t = p->since > 1.0 / 240.0 ? p->since : 1.0 / 240.0;
        p->jump = sqrt ((double) dx * dx + (double) dy * dy) > WALPE_FLUID_TELEPORT * t;
    }
}

/* step j (1..n) of the n steps of this frame: the two positions it gets */
static inline void walpe_fluid_pointer_step (const WalpeFluidPointer *p, int j, int n, float cur[2],
                                             float last[2])
{
    if (!p->fhave) {
        cur[0] = cur[1] = last[0] = last[1] = WALPE_FLUID_AWAY;
        return;
    }
    if (!p->have || p->jump || n < 1) {
        cur[0] = last[0] = p->fx;
        cur[1] = last[1] = p->fy;
        return;
    }
    j = CLAMP (j, 1, n);
    const float a = (float) (j - 1) / (float) n, b = (float) j / (float) n;
    last[0] = p->x + (p->fx - p->x) * a;
    last[1] = p->y + (p->fy - p->y) * a;
    /* the last step ends exactly on the sample */
    cur[0] = j == n ? p->fx : p->x + (p->fx - p->x) * b;
    cur[1] = j == n ? p->fy : p->y + (p->fy - p->y) * b;
}

/* once per frame, after its n steps (0 = none ran) */
static inline void walpe_fluid_pointer_end (WalpeFluidPointer *p, int n)
{
    if (!p->fhave) {
        p->have = FALSE;
        p->since = 0.0;
        return;
    }
    if (n > 0 || !p->have || p->jump) {
        p->x = p->fx;
        p->y = p->fy;
        p->since = 0.0;
    }
    p->have = TRUE;
    p->jump = FALSE;
}

/* ---- from the frame to the layer -------------------------------------------
 * The stock shaders take the pointer as a clip-space point of the frame
 * (x right, y up, -1..1) through g_EffectTextureProjectionMatrixInverse
 * and expect the layer's own clip space back (the layer's picture spans
 * -1..1): for a layer that fills the frame the matrix is the identity.
 *
 * m3 = column-major 3x3, the transform the layer's +-1 quad is drawn with
 * into the frame; out16 = column-major 4x4 for the uniform (x, y and w
 * take part, z passes through). FALSE and the identity when m3 cannot be
 * inverted (a layer scaled to nothing). */
static inline gboolean walpe_fluid_clip_to_layer (const float *m3, float out16[16])
{
    static const float ident[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1 };
    memcpy (out16, ident, sizeof (ident));
    if (!m3)
        return FALSE;
    /* a[row][col] */
    const double a00 = m3[0], a10 = m3[1], a20 = m3[2];
    const double a01 = m3[3], a11 = m3[4], a21 = m3[5];
    const double a02 = m3[6], a12 = m3[7], a22 = m3[8];
    const double c00 = a11 * a22 - a12 * a21, c01 = a12 * a20 - a10 * a22, c02 = a10 * a21 - a11 * a20;
    const double det = a00 * c00 + a01 * c01 + a02 * c02;
    if (!isfinite (det) || fabs (det) < 1e-12)
        return FALSE;
    const double id = 1.0 / det;
    /* inverse = adjugate / det; i[row][col] */
    const double i00 = c00 * id, i01 = (a02 * a21 - a01 * a22) * id, i02 = (a01 * a12 - a02 * a11) * id;
    const double i10 = c01 * id, i11 = (a00 * a22 - a02 * a20) * id, i12 = (a02 * a10 - a00 * a12) * id;
    const double i20 = c02 * id, i21 = (a01 * a20 - a00 * a21) * id, i22 = (a00 * a11 - a01 * a10) * id;
    /* rows / columns 0, 1 stay, the third (the homogeneous one) becomes w */
    out16[0] = (float) i00;  out16[1] = (float) i10;  out16[3] = (float) i20;
    out16[4] = (float) i01;  out16[5] = (float) i11;  out16[7] = (float) i21;
    out16[12] = (float) i02; out16[13] = (float) i12; out16[15] = (float) i22;
    for (int k = 0; k < 16; k++)
        if (!isfinite (out16[k])) {
            memcpy (out16, ident, sizeof (ident));
            return FALSE;
        }
    return TRUE;
}

G_END_DECLS
