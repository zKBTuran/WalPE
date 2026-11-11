/* camshake.c - see camshake.h */
#include "engine/camshake.h"
#include <math.h>

/* changes of direction a second at speed 1, and how much faster the rough
 * part runs */
#define SHAKE_RATE        0.6
#define SHAKE_ROUGH_RATE  4.3
/* amplitude 1 = this share of the scene's size */
#define SHAKE_UNIT        0.01f

/* lattice value in [-1, 1] for point i of the noise `seed`. The lattice
 * index wraps at 2^32 points (decades of running time at any speed). */
static float shake_lattice (gint64 i, guint32 seed)
{
    guint32 x = (guint32) ((guint64) i & 0xffffffffu) * 374761393u + seed * 668265263u;
    x = (x ^ (x >> 13)) * 1274126177u;
    x ^= x >> 16;
    return (float) (x & 0xffffffu) * (2.0f / 16777215.0f) - 1.0f;
}

/* smooth value noise: the lattice values joined by a curve that is flat at
 * each of them (no corners in the motion) */
static float shake_noise (double phase, guint32 seed)
{
    double cell = floor (phase);
    float f = (float) (phase - cell);
    float u = f * f * (3.0f - 2.0f * f);
    float a = shake_lattice ((gint64) cell, seed), b = shake_lattice ((gint64) cell + 1, seed);
    return a + (b - a) * u;
}

float walpe_camera_shake_reach (float amplitude)
{
    if (!isfinite (amplitude))
        return 0.0f;
    return fabsf (CLAMP (amplitude, -WALPE_CAMSHAKE_MAX_AMPLITUDE, WALPE_CAMSHAKE_MAX_AMPLITUDE)) *
           SHAKE_UNIT;
}

void walpe_camera_shake (double t, float amplitude, float roughness, float speed, float scene_w,
                         float scene_h, float out[2])
{
    out[0] = out[1] = 0.0f;
    if (!(t >= 0) || !isfinite (t) || !isfinite (amplitude) || !isfinite (roughness) ||
        !isfinite (speed) || !isfinite (scene_w) || !isfinite (scene_h) || amplitude == 0 ||
        !(speed > 0))
        return;
    amplitude = CLAMP (amplitude, -WALPE_CAMSHAKE_MAX_AMPLITUDE, WALPE_CAMSHAKE_MAX_AMPLITUDE);
    const float rough = CLAMP (roughness, 0.0f, WALPE_CAMSHAKE_MAX_ROUGHNESS) * 0.5f;
    const double phase = t * SHAKE_RATE * (double) MIN (speed, WALPE_CAMSHAKE_MAX_SPEED);
    /* the lattice index is taken as a 64-bit integer (shake_noise): a
     * clock beyond that - no running scene gets there, a made-up instant
     * (WALPE_FX_TIME) can - is no motion instead of an undefined cast */
    if (!(phase * SHAKE_ROUGH_RATE < 4.0e18))
        return;
    const float size[2] = { scene_w, scene_h };
    for (guint axis = 0; axis < 2; axis++) {
        /* two noises per axis with seeds of their own: x and y are not
         * the same curve, the rough part is not the slow one again */
        const guint32 seed = 11u + axis * 46u;
        float v = shake_noise (phase, seed);
        if (rough > 0)
            v = (v + rough * shake_noise (phase * SHAKE_ROUGH_RATE, seed + 12u)) / (1.0f + rough);
        out[axis] = amplitude * SHAKE_UNIT * size[axis] * CLAMP (v, -1.0f, 1.0f);
    }
}
