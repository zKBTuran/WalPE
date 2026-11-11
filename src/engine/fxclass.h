/* fxclass.h - what kind of effect a chain is, and how fast it moves.
 * Pure text functions (no GL): the quality knobs (common/quality.h) use
 * them to decide which effect chains may be redrawn less often, shrunk or
 * left out. Shared with the tests. */
#pragma once
#include <glib.h>

G_BEGIN_DECLS

/* ---- family, by Wallpaper Engine effect name ------------------------
 * The name is the effect's directory: "effects/blurprecise/effect.json",
 * "effects/workshop/2822917890/bloom/effect.json". Matching is on that
 * name only, lower case, punctuation ignored. */
typedef enum {
    WALPE_FXF_NONE = 0,
    WALPE_FXF_BLUR,       /* blur, blurprecise, bokeh blur, radial blur */
    WALPE_FXF_BLOOM,      /* bloom / glow post passes */
    WALPE_FXF_GRAIN,      /* film grain */
    WALPE_FXF_VIGNETTE,
    WALPE_FXF_CHROMA,     /* chromatic aberration */
} WalpeFxFamily;

WalpeFxFamily walpe_fx_family (const char *effect_file);
const char *walpe_fx_family_name (WalpeFxFamily f);
/* purely cosmetic full-screen post passes: post-processing "off" may leave
 * them out (bloom, film grain, vignette, chromatic aberration) */
gboolean walpe_fx_family_cosmetic (WalpeFxFamily f);
/* blur and bloom chains: post-processing "reduced" runs their inner
 * buffers at half size */
gboolean walpe_fx_family_reducible (WalpeFxFamily f);
/* effects whose picture is new in every frame (film grain, VHS, glitch,
 * noise, glitter, fire): never "slow" */
gboolean walpe_fx_name_fast (const char *effect_file);

/* ---- time dependence of a shader ----------------------------------- */
typedef struct {
    gboolean uses_time;   /* a g_Time term was found */
    gboolean derived;     /* every term's factors were known (meaningless
                           * without uses_time) */
    double rate;          /* largest |d(term)/dt|: the product of the factors
                           * g_Time is multiplied with (speed uniforms,
                           * literals); plain g_Time = 1 */
} WalpeFxTime;

/* the value of an identifier a g_Time term is multiplied with: TRUE and
 * its magnitude (largest component), FALSE = unknown */
typedef gboolean (*WalpeFxValueFn) (const char *ident, double *magnitude, gpointer user);

/* Scan GLSL source for g_Time terms and merge what they say into *t
 * (start with a zeroed struct; call once per shader of a pass / chain).
 * A term is g_Time with everything it is multiplied or divided by:
 *     g_Time * g_Speed            sin (g_Speed * g_Time + phase)
 *     (g_Time + g_Offset2) * g_Speed2
 *     g_Time * g_AnimationSpeed * g_AnimationSpeed
 *     frac (g_Speed * g_Time / M_PI_2 * vec4 (1, -0.16, 0.008, -0.0002))
 * Factors: numbers, vecN (...) of numbers, M_PI / M_PI_2 / M_PI_HALF,
 * identifiers known to `value`, rotateVec2 () / normalize () (length 1).
 * Any other factor, or g_Time passed bare to a function that is not a
 * known builtin, makes the scan "not derived". */
void walpe_fx_time_scan (const char *src, WalpeFxValueFn value, gpointer user, WalpeFxTime *t);

/* A sine driven threshold ("bounds" of WE's pulse / shake / swing: the
 * effect is smoothstep (lo, hi, sin (phase) * 0.5 + 0.5)) is crossed in a
 * fraction of the half period: the factor by which the picture changes
 * faster than the phase (1 for the full range 0..1). */
double walpe_fx_bounds_factor (double lo, double hi);

/* Largest divisor <= want (1..3) an animated chain with time dependence t
 * tolerates at `fps` frames per second: redrawn every Nth frame, its
 * phase may advance by at most WALPE_FX_SLOW_STEP per redraw (0.35 rad:
 * at least 18 redraws per oscillation, e.g. speed 10 at 60 fps every 2nd
 * frame, speed 5 at 30 fps). 1 = redraw every frame (not derived, or too
 * fast). sharp = walpe_fx_bounds_factor of the chain (>= 1). */
#define WALPE_FX_SLOW_STEP 0.35
int walpe_fx_slow_div (const WalpeFxTime *t, double sharp, int want, double fps);

G_END_DECLS
