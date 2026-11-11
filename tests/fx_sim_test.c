/* Simulation effects (src/engine/fx_sim.h, src/common/fluidsim.h): effect
 * chains with a "swap" command, the kind Wallpaper Engine's Fluid
 * Simulation is.
 *
 *   fx-sim-test
 *
 * Without a graphics context: the fixed step (how many steps a frame runs
 * at 30, 60 and 144 frames per second, with jitter, after a stall), the
 * size of an fbos[].fit target (fit 0, negative, 256, 1024, 4096, huge;
 * extreme proportions; the driver's largest texture), the pointer's path
 * from step to step (several steps in a frame, no step in a frame, a
 * pointer that is not there, a jump), the matrix from the frame to the
 * layer, the quality levels and their preset row, "conditions".
 *
 * With an EGL device or Mesa's surfaceless platform (skipped without), a
 * small simulation effect written for this test - a velocity field with an
 * emitter and a pointer push, a dye field carried by it, a pass that draws
 * the dye over the layer, two swaps - through the executor:
 *
 *  1. what the chain is: step passes, the presenting pass, a pass and a
 *     target left out by "conditions", targets of the fit size in float
 *     format, cleared before the first step; a pass whose uniforms are
 *     classified again after the build (fx_specialize) stays a simulation
 *     pass;
 *  2. determinism: the same frames twice give the same fields and the
 *     same pictures, bit for bit;
 *  3. frame-rate independence: after the same number of steps the fields
 *     are the same bits at 30, 60 and 144 frames per second, and two
 *     seconds are 120 steps at each;
 *  4. never static, never held, never kept, never drawn in part: the
 *     static cache, the effect update rate, a caller's region and the
 *     "Skip hidden areas" clip leave the fields as they are without them;
 *     a second run in the same frame runs no second step;
 *  5. frames without a step show the picture of the last step (the swaps
 *     are taken back for the presenting pass), and reuse it without a
 *     pass when the input stands still;
 *  6. reset rules: fx_sim_reset, the switch (off: targets freed, the set
 *     static, the input returned), a function of the effect file; NOT a
 *     constant a script sets, NOT a change of the quality level or of the
 *     working size (the fields are carried over);
 *  7. the pointer: no push without movement or without a pointer, a push
 *     under the pointer (top of the frame = top of the texture), through
 *     the layer's place in the frame, none across a gap or a jump;
 *  8. what an effect file asks for is bounded: a "fit" of 4096 gives a grid
 *     of WALPE_FLUID_FIT_MAX cells, no target is larger than the driver's
 *     largest texture, the reduced targets are the ones counted, the chain
 *     says so once;
 *  9. memory accounting and the fallback when float targets cannot be
 *     rendered to;
 * 10. effects without a swap get none of this (fit ignored, both pointer
 *     uniforms the frame's position).
 *
 * The shaders below are this test's own. The engine file is included so
 * the static functions are reachable. */
#include "../src/engine/scenenode_effects.c"
#include <epoxy/egl.h>
#include <glib/gstdio.h>
#include <stdio.h>

static int fails, checks;

static void result (gboolean ok, const char *what, const char *detail)
{
    checks++;
    printf ("%s %s%s%s%s\n", ok ? "ok  " : "FAIL", what, detail ? "  [" : "", detail ? detail : "",
            detail ? "]" : "");
    if (!ok)
        fails++;
}

#define CHECK(c, ...) do { char *d_ = g_strdup_printf (__VA_ARGS__); result ((c), d_, NULL); g_free (d_); } while (0)

/* ------------------------------------------------------------ no GL needed */

static void step_checks (void)
{
    const double h = 1.0 / 60.0;
    /* 60 frames per second, exactly: one step per frame */
    double acc = 0;
    int total = 0, worst_lo = 9, worst_hi = 0;
    for (int f = 0; f < 600; f++) {
        int n = walpe_fluid_steps (&acc, h, h, 4);
        total += n;
        worst_lo = MIN (worst_lo, n);
        worst_hi = MAX (worst_hi, n);
    }
    CHECK (total == 600 && worst_lo == 1 && worst_hi == 1, "60 fps: one step in every frame (%d steps, %d..%d per frame)",
           total, worst_lo, worst_hi);
    /* frames a tenth early and a tenth late in turn: still one each */
    acc = 0, total = 0, worst_lo = 9, worst_hi = 0;
    for (int f = 0; f < 600; f++) {
        int n = walpe_fluid_steps (&acc, h * ((f & 1) ? 1.1 : 0.9), h, 4);
        total += n;
        worst_lo = MIN (worst_lo, n);
        worst_hi = MAX (worst_hi, n);
    }
    CHECK (total == 600 && worst_lo == 1 && worst_hi == 1, "60 fps with 10 %% jitter: one step in every frame "
           "(%d steps, %d..%d per frame)", total, worst_lo, worst_hi);
    /* 30: two per frame; 144: none or one, 60 a second */
    acc = 0, total = 0, worst_lo = 9, worst_hi = 0;
    for (int f = 0; f < 300; f++) {
        int n = walpe_fluid_steps (&acc, 1.0 / 30.0, h, 4);
        total += n;
        worst_lo = MIN (worst_lo, n);
        worst_hi = MAX (worst_hi, n);
    }
    CHECK (total == 600 && worst_lo == 2 && worst_hi == 2, "30 fps: two steps in every frame (%d, %d..%d)", total,
           worst_lo, worst_hi);
    acc = 0, total = 0, worst_hi = 0;
    for (int f = 0; f < 1440; f++) {
        int n = walpe_fluid_steps (&acc, 1.0 / 144.0, h, 4);
        total += n;
        worst_hi = MAX (worst_hi, n);
    }
    CHECK (abs (total - 600) <= 1 && worst_hi == 1, "144 fps: 60 steps a second, never two in a frame (%d in 10 s, "
           "at most %d)", total, worst_hi);
    /* a stall: the most steps, and nothing left over to catch up with */
    acc = 0;
    int n = walpe_fluid_steps (&acc, 3.0, h, 4);
    CHECK (n == 4 && acc == 0.0, "a 3 s stall runs the most steps a frame may (4) and drops the rest (%d, %.4f left)",
           n, acc);
    n = walpe_fluid_steps (&acc, h, h, 4);
    CHECK (n == 1, "... the frame after it runs one step again (%d)", n);
    acc = 0;
    CHECK (walpe_fluid_steps (&acc, -1.0, h, 4) == 0 && walpe_fluid_steps (&acc, NAN, h, 4) == 0 &&
           walpe_fluid_steps (&acc, 0.0, h, 4) == 0 && acc == 0.0, "no time, negative time, not a number: no step");
    CHECK (walpe_fluid_steps_peek (0.0, h, h, 4) == 1 && walpe_fluid_steps_peek (0.0, h * 0.5, h, 4) == 0,
           "asking leaves the accumulator alone");
    /* the levels */
    WalpeFluidQuality q = WALPE_FLUID_FULL;
    CHECK (walpe_fluid_quality_parse ("low", &q) && q == WALPE_FLUID_LOW &&
           walpe_fluid_quality_parse ("Reduced", &q) && q == WALPE_FLUID_REDUCED &&
           walpe_fluid_quality_parse ("full", &q) && q == WALPE_FLUID_FULL &&
           !walpe_fluid_quality_parse ("best", &q) && !walpe_fluid_quality_parse (NULL, &q) && q == WALPE_FLUID_FULL,
           "the level names are read, others refused");
    CHECK (walpe_fluid_params (WALPE_FLUID_FULL)->step_hz == 60.0 && walpe_fluid_params (WALPE_FLUID_FULL)->field_div == 1 &&
           walpe_fluid_params (WALPE_FLUID_REDUCED)->step_hz == 60.0 && walpe_fluid_params (WALPE_FLUID_REDUCED)->field_div == 2 &&
           walpe_fluid_params (WALPE_FLUID_LOW)->step_hz == 30.0 && walpe_fluid_params (WALPE_FLUID_LOW)->field_div == 2,
           "full: 60 steps, fields whole; reduced: 60, halved; low: 30, halved");
    {
        guint ne = 0;
        const WalpePresetExtra *x = walpe_preset_extras (&ne);
        gboolean found = FALSE;
        for (guint i = 0; i < ne; i++)
            if (!strcmp (x[i].key, "fluid_quality"))
                found = !strcmp (x[i].value[WALPE_PRESET_MAX], "full") && !strcmp (x[i].value[WALPE_PRESET_HIGH], "full") &&
                        !strcmp (x[i].value[WALPE_PRESET_BALANCED], "reduced") &&
                        !strcmp (x[i].value[WALPE_PRESET_LOW], "low") && !x[i].cost[WALPE_PRESET_HIGH] &&
                        x[i].cost[WALPE_PRESET_BALANCED] && x[i].cost[WALPE_PRESET_LOW];
        CHECK (found, "the presets set fluid_quality: max and high full, balanced reduced, low low");
    }
}

static void fit_checks (void)
{
    int w = 0, h = 0;
    walpe_fluid_fit_size (1920, 1080, 256, &w, &h);
    CHECK (w == 256 && h == 144, "fit 256 of a 1920x1080 layer: %dx%d", w, h);
    walpe_fluid_fit_size (3840, 2160, 256, &w, &h);
    CHECK (w == 256 && h == 144, "... of a 3840x2160 layer the same grid: %dx%d", w, h);
    walpe_fluid_fit_size (1080, 1920, 256, &w, &h);
    CHECK (w == 144 && h == 256, "... upright: %dx%d", w, h);
    walpe_fluid_fit_size (2560, 1080, 256, &w, &h);
    CHECK (w == 256 && h == 108, "... 21:9: %dx%d", w, h);
    walpe_fluid_fit_size (100, 100, 256, &w, &h);
    CHECK (w == 256 && h == 256, "... a small square layer: %dx%d", w, h);
    walpe_fluid_fit_size (0, 0, 256, &w, &h);
    CHECK (w >= 1 && h >= 1, "no size: still a target (%dx%d)", w, h);
    walpe_fluid_fit_size (10000, 1, 1 << 20, &w, &h);
    CHECK (w == WALPE_FLUID_FIT_MAX && h == 1, "a silly fit is bounded (%dx%d)", w, h);
    /* ---- what an effect file may declare (it is not trusted): nothing,
     * nonsense, the stock effect's 256, the limit, more than the limit */
    int asked = -1;
    CHECK (walpe_fluid_fit_cells (0.0, &asked) == 0 && asked == 0 && walpe_fluid_fit_cells (-256.0, &asked) == 0 &&
           asked == 0 && walpe_fluid_fit_cells (0.5, NULL) == 0 && walpe_fluid_fit_cells (NAN, &asked) == 0 && asked == 0,
           "fit 0, negative, less than one cell, not a number: no fit");
    CHECK (WALPE_FLUID_FIT_MAX == 1024 && walpe_fluid_fit_cells (256.0, &asked) == 256 && asked == 256 &&
           walpe_fluid_fit_cells (256.7, &asked) == 256 && walpe_fluid_fit_cells (1024.0, &asked) == 1024 && asked == 1024,
           "fit 256 and fit 1024 are taken as they are (the limit is %d)", WALPE_FLUID_FIT_MAX);
    int a1 = 0, a2 = 0, a3 = 0, a4 = 0;
    CHECK (walpe_fluid_fit_cells (4096.0, &a1) == WALPE_FLUID_FIT_MAX && a1 == 4096 &&
           walpe_fluid_fit_cells (1025.0, &a2) == WALPE_FLUID_FIT_MAX && a2 == 1025 &&
           walpe_fluid_fit_cells (1e30, &a3) == WALPE_FLUID_FIT_MAX && a3 == 1000000000 &&
           walpe_fluid_fit_cells (INFINITY, &a4) == WALPE_FLUID_FIT_MAX && a4 == 1000000000,
           "fit 4096, 1025, 1e30, infinity: the limit, and the caller can tell that it was reduced (%d, %d, %d, %d)",
           a1, a2, a3, a4);
    walpe_fluid_fit_size (1920, 1080, 1024, &w, &h);
    CHECK (w == 1024 && h == 576, "fit 1024 of a 1920x1080 layer: %dx%d", w, h);
    walpe_fluid_fit_size (1920, 1080, 4096, &w, &h);
    CHECK (w == 1024 && h == 576, "... and a fit beyond the limit gives the limit's grid: %dx%d", w, h);
    int w2 = 0, h2 = 0;
    walpe_fluid_fit_size (1920, 1080, 0, &w, &h);
    walpe_fluid_fit_size (1920, 1080, -7, &w2, &h2);
    CHECK (w == 1 && h == 1 && w2 == 1 && h2 == 1, "asked for a grid of no or of minus seven cells: one cell, never "
           "an empty target (%dx%d, %dx%d)", w, h, w2, h2);
    /* extreme proportions: the short side never falls below one cell, the
     * long side never exceeds the fit */
    walpe_fluid_fit_size (100000, 3, 256, &w, &h);
    walpe_fluid_fit_size (3, 100000, 256, &w2, &h2);
    CHECK (w == 256 && h == 1 && w2 == 1 && h2 == 256, "a layer 100000 x 3 and one 3 x 100000: %dx%d, %dx%d", w, h, w2,
           h2);
    walpe_fluid_fit_size (G_MAXINT, 1, 1024, &w, &h);
    walpe_fluid_fit_size (1, G_MAXINT, 1 << 30, &w2, &h2);
    CHECK (w == 1024 && h == 1 && w2 == 1 && h2 == 1024, "the largest sizes an int holds: %dx%d, %dx%d", w, h, w2, h2);
    walpe_fluid_fit_size (-5, -9, 256, &w, &h);
    CHECK (w == 256 && h == 256, "negative layer sizes count as one unit: %dx%d", w, h);
    /* ---- the driver's largest texture */
    int lw = 1024, lh = 576;
    gboolean cut = walpe_fluid_limit_size (&lw, &lh, 2048);
    CHECK (!cut && lw == 1024 && lh == 576, "a target within the driver's largest texture stays as it is");
    lw = 4096, lh = 2304;
    cut = walpe_fluid_limit_size (&lw, &lh, 2048);
    CHECK (cut && lw == 2048 && lh == 1152, "a larger one shrinks in proportion (%dx%d)", lw, lh);
    lw = 10000, lh = 1;
    int lw2 = 7, lh2 = 30000;
    cut = walpe_fluid_limit_size (&lw, &lh, 2048);
    gboolean cut2 = walpe_fluid_limit_size (&lw2, &lh2, 512);
    CHECK (cut && lw == 2048 && lh == 1 && cut2 && lw2 == 1 && lh2 == 512, "... and keeps one texel on its short "
           "side (%dx%d, %dx%d)", lw, lh, lw2, lh2);
    lw = 300, lh = 200;
    CHECK (!walpe_fluid_limit_size (&lw, &lh, 0) && !walpe_fluid_limit_size (&lw, &lh, -1) && lw == 300 && lh == 200,
           "a limit that is not known changes nothing");
}

static gboolean near2 (const float a[2], float x, float y)
{
    return fabsf (a[0] - x) < 1e-5f && fabsf (a[1] - y) < 1e-5f;
}

static void pointer_checks (void)
{
    WalpeFluidPointer p = { 0 };
    float c[2], l[2];
    const double dt = 1.0 / 60.0;
    /* the first sample: a place, no path */
    walpe_fluid_pointer_frame (&p, TRUE, 0.2f, 0.3f, dt);
    walpe_fluid_pointer_step (&p, 1, 1, c, l);
    CHECK (near2 (c, 0.2f, 0.3f) && near2 (l, 0.2f, 0.3f), "the first position seen has no path leading to it");
    walpe_fluid_pointer_end (&p, 1);
    /* one frame, two steps: each gets half of the way */
    walpe_fluid_pointer_frame (&p, TRUE, 0.3f, 0.5f, dt * 2);
    walpe_fluid_pointer_step (&p, 1, 2, c, l);
    gboolean ok = near2 (l, 0.2f, 0.3f) && near2 (c, 0.25f, 0.4f);
    walpe_fluid_pointer_step (&p, 2, 2, c, l);
    ok = ok && near2 (l, 0.25f, 0.4f) && near2 (c, 0.3f, 0.5f);
    CHECK (ok, "two steps in a frame share the pointer's way");
    walpe_fluid_pointer_end (&p, 2);
    /* a frame without a step: the way is kept for the next step */
    walpe_fluid_pointer_frame (&p, TRUE, 0.32f, 0.5f, dt * 0.4);
    walpe_fluid_pointer_end (&p, 0);
    walpe_fluid_pointer_frame (&p, TRUE, 0.34f, 0.5f, dt * 0.4);
    walpe_fluid_pointer_step (&p, 1, 1, c, l);
    CHECK (near2 (l, 0.3f, 0.5f) && near2 (c, 0.34f, 0.5f), "movement in a frame without a step goes into the next step");
    walpe_fluid_pointer_end (&p, 1);
    /* no pointer: far away, and no line when it is back */
    walpe_fluid_pointer_frame (&p, FALSE, 0.9f, 0.9f, dt);
    walpe_fluid_pointer_step (&p, 1, 1, c, l);
    CHECK (c[0] == WALPE_FLUID_AWAY && c[1] == WALPE_FLUID_AWAY && l[0] == WALPE_FLUID_AWAY && l[1] == WALPE_FLUID_AWAY,
           "no pointer: both positions far outside the picture");
    walpe_fluid_pointer_end (&p, 1);
    walpe_fluid_pointer_frame (&p, TRUE, 0.36f, 0.5f, dt);
    walpe_fluid_pointer_step (&p, 1, 1, c, l);
    CHECK (near2 (c, 0.36f, 0.5f) && near2 (l, 0.36f, 0.5f), "the pointer is back: no line from where it left");
    walpe_fluid_pointer_end (&p, 1);
    /* a jump no hand makes (a shared scene: the pointer changed monitors) */
    walpe_fluid_pointer_frame (&p, TRUE, 0.95f, 0.5f, dt);
    walpe_fluid_pointer_step (&p, 1, 1, c, l);
    CHECK (near2 (c, 0.95f, 0.5f) && near2 (l, 0.95f, 0.5f), "a jump across the picture in one frame draws no line");
    walpe_fluid_pointer_end (&p, 1);
    /* ... while a fast stroke does */
    walpe_fluid_pointer_frame (&p, TRUE, 0.80f, 0.5f, dt);
    walpe_fluid_pointer_step (&p, 1, 1, c, l);
    CHECK (near2 (c, 0.80f, 0.5f) && near2 (l, 0.95f, 0.5f), "a fast stroke (9 picture widths a second) is a line");
}

/* the stock cursor shaders' arithmetic for one point: the pointer 0..1 with
 * y from the top, through the matrix, to the layer's texture coordinate */
static void through (const float m[16], float px, float py, float *u, float *v)
{
    const float cx = px * 2.0f - 1.0f, cy = (1.0f - py) * 2.0f - 1.0f;
    const float x = m[0] * cx + m[4] * cy + m[12], y = m[1] * cx + m[5] * cy + m[13];
    const float w = m[3] * cx + m[7] * cy + m[15];
    *u = x * 0.5f / w + 0.5f;
    *v = 1.0f - (y * 0.5f / w + 0.5f);
}

static void matrix_checks (void)
{
    float m[16], u, v;
    const float ident[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    gboolean ok = walpe_fluid_clip_to_layer (ident, m);
    through (m, 0.25f, 0.1f, &u, &v);
    CHECK (ok && fabsf (u - 0.25f) < 1e-5f && fabsf (v - 0.1f) < 1e-5f,
           "a layer that fills the frame: the pointer's place is its texture coordinate (%.3f, %.3f)", u, v);
    /* the layer is the right half of the frame, full height */
    const float right[9] = { 0.5f, 0, 0, 0, 1, 0, 0.5f, 0, 1 };
    ok = walpe_fluid_clip_to_layer (right, m);
    through (m, 0.75f, 0.2f, &u, &v);
    gboolean a = fabsf (u - 0.5f) < 1e-5f && fabsf (v - 0.2f) < 1e-5f;
    through (m, 0.5f, 1.0f, &u, &v);
    CHECK (ok && a && fabsf (u) < 1e-5f && fabsf (v - 1.0f) < 1e-5f,
           "a layer on the right half: the frame's middle is its left edge");
    /* a quarter-size layer in the top left corner, upside down */
    const float corner[9] = { 0.5f, 0, 0, 0, -0.5f, 0, -0.5f, 0.5f, 1 };
    ok = walpe_fluid_clip_to_layer (corner, m);
    through (m, 0.25f, 0.25f, &u, &v);
    a = fabsf (u - 0.5f) < 1e-5f && fabsf (v - 0.5f) < 1e-5f;
    through (m, 0.0f, 0.0f, &u, &v);
    CHECK (ok && a && fabsf (u) < 1e-5f && fabsf (v - 1.0f) < 1e-5f,
           "a flipped layer in a corner: its centre and its flipped edge are found");
    const float none[9] = { 0, 0, 0, 0, 0, 0, 0.3f, 0.3f, 1 };
    ok = walpe_fluid_clip_to_layer (none, m);
    through (m, 0.4f, 0.6f, &u, &v);
    CHECK (!ok && fabsf (u - 0.4f) < 1e-5f && fabsf (v - 0.6f) < 1e-5f && !walpe_fluid_clip_to_layer (NULL, m),
           "a layer scaled to nothing, or no matrix: the frame stands in for the layer");
}

static void condition_checks (void)
{
    JsonNode *root = wpj_parse_data (
        "{\"inst\":{\"passes\":[{\"combos\":{\"A\":1}},{},{\"combos\":{\"B\":3}}]},"
        "\"lit\":{\"conditions\":[{\"A\":1}]},\"both\":{\"conditions\":[{\"A\":1},{\"B\":3}]},"
        "\"no\":{\"conditions\":[{\"A\":1},{\"B\":2}]},\"unset\":{\"conditions\":[{\"C\":1}]},"
        "\"zero\":{\"conditions\":[{\"C\":0}]},\"ge\":{\"conditions\":{\"B\":{\"op\":\"ge\",\"value\":2}}},"
        "\"lt\":{\"conditions\":{\"B\":{\"op\":\"lt\",\"value\":2}}},\"plain\":{}}", -1);
    if (!root) {
        result (FALSE, "conditions: the test's JSON is read", NULL);
        return;
    }
    JsonObject *o = json_node_get_object (root);
    JsonArray *ip = wpj_arr (wpj_obj (o, "inst"), "passes");
    CHECK (fxsim_condition (wpj_obj (o, "lit"), ip) && fxsim_condition (wpj_obj (o, "both"), ip) &&
           !fxsim_condition (wpj_obj (o, "no"), ip) && !fxsim_condition (wpj_obj (o, "unset"), ip) &&
           fxsim_condition (wpj_obj (o, "zero"), ip) && fxsim_condition (wpj_obj (o, "ge"), ip) &&
           !fxsim_condition (wpj_obj (o, "lt"), ip) && fxsim_condition (wpj_obj (o, "plain"), ip) &&
           fxsim_condition (NULL, ip) && !fxsim_condition (wpj_obj (o, "lit"), NULL),
           "conditions: every combo named must have its value in the instance; none set counts as 0");
    json_node_unref (root);
}

/* ------------------------------------------------------------------ with GL */

static char *tmpdir;

static void put (const char *rel, const char *text)
{
    char *p = g_build_filename (tmpdir, rel, NULL);
    char *d = g_path_get_dirname (p);
    g_mkdir_with_parents (d, 0700);
    g_file_set_contents (p, text, -1, NULL);
    g_free (d);
    g_free (p);
}

static void put_material (const char *name)
{
    char *m = g_strdup_printf ("materials/effects/%s.json", name);
    char *mj = g_strdup_printf ("{\"passes\":[{\"shader\":\"effects/%s\"}]}", name);
    put (m, mj);
    g_free (m);
    g_free (mj);
}

static void put_shader (const char *name, const char *vert, const char *frag)
{
    char *v = g_strdup_printf ("shaders/effects/%s.vert", name), *f = g_strdup_printf ("shaders/effects/%s.frag", name);
    put (v, vert);
    put (f, frag);
    g_free (v);
    g_free (f);
    put_material (name);
}

static const char *quad_vert =
    "attribute vec3 a_Position;\nattribute vec2 a_TexCoord;\nvarying vec2 v_TexCoord;\n"
    "void main() {\n    gl_Position = vec4(a_Position, 1.0);\n    v_TexCoord = a_TexCoord;\n}\n";
static const char *mvp_vert =
    "uniform mat4 g_ModelViewProjectionMatrix;\nattribute vec3 a_Position;\nattribute vec2 a_TexCoord;\n"
    "varying vec2 v_TexCoord;\nvoid main() {\n"
    "    gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);\n    v_TexCoord = a_TexCoord;\n}\n";
/* velocity: the field of the step before, damped; an emitter whose
 * direction turns with time; a push along the pointer's way */
static const char *force_vert =
    "uniform mat4 g_EffectTextureProjectionMatrixInverse;\n"
    "uniform vec2 g_PointerPosition;\nuniform vec2 g_PointerPositionLast;\n"
    "attribute vec3 a_Position;\nattribute vec2 a_TexCoord;\n"
    "varying vec2 v_TexCoord;\nvarying vec2 v_Ptr;\nvarying vec2 v_PtrLast;\n"
    "vec2 onLayer(vec2 p) {\n"
    "    vec4 c = mul(vec4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0), g_EffectTextureProjectionMatrixInverse);\n"
    "    return vec2(c.x / c.w * 0.5 + 0.5, 0.5 - c.y / c.w * 0.5);\n}\n"
    "void main() {\n    gl_Position = vec4(a_Position, 1.0);\n    v_TexCoord = a_TexCoord;\n"
    "    v_Ptr = onLayer(g_PointerPosition);\n    v_PtrLast = onLayer(g_PointerPositionLast);\n}\n";
static const char *force_frag =
    "uniform float g_Frametime;\nuniform float g_Time;\n"
    "uniform sampler2D g_Texture0; // {\"hidden\":true}\n"
    "uniform float u_Push; // {\"material\":\"push\",\"default\":1.0}\n"
    "uniform float u_Emit; // {\"material\":\"emit\",\"default\":40.0}\n"
    "uniform vec4 g_PointerState;\n"
    "varying vec2 v_TexCoord;\nvarying vec2 v_Ptr;\nvarying vec2 v_PtrLast;\n"
    "void main() {\n"
    "    vec2 vel = texSample2D(g_Texture0, v_TexCoord).xy * (1.0 - 0.5 * g_Frametime);\n"
    "    float e = step(length(v_TexCoord - vec2(0.3, 0.5)), 0.12);\n"
    "    vel += e * g_Frametime * u_Emit * vec2(cos(g_Time * 3.0), sin(g_Time * 3.0));\n"
    "    vec2 seg = v_Ptr - v_PtrLast;\n"
    "    float len = length(seg);\n"
    "    vec2 dir = seg / max(len, 0.00001);\n"
    "    float along = clamp(dot(v_TexCoord - v_PtrLast, dir), 0.0, len);\n"
    "    float d = length(v_TexCoord - (v_PtrLast + dir * along));\n"
    "    vel += u_Push * (1.0 + g_PointerState.z) * step(d, 0.08) * step(0.00001, len) * dir * (200.0 * len);\n"
    "    gl_FragColor = vec4(vel, 0.0, 1.0);\n}\n";
/* dye: carried along the velocity, fading; the emitter adds the colour of
 * the layer under it */
static const char *dye_frag =
    "uniform float g_Frametime;\n"
    "uniform sampler2D g_Texture0; // {\"hidden\":true}\n"
    "uniform sampler2D g_Texture1; // {\"hidden\":true}\n"
    "uniform sampler2D g_Texture2; // {\"hidden\":true}\n"
    "uniform vec4 g_Texture0Resolution;\n"
    "varying vec2 v_TexCoord;\n"
    "void main() {\n"
    "    vec2 texel = vec2(1.0) / g_Texture0Resolution.xy;\n"
    "    vec2 from = v_TexCoord - g_Frametime * texSample2D(g_Texture0, v_TexCoord).xy * texel;\n"
    "    vec4 dye = texSample2D(g_Texture1, from) * (1.0 - 0.3 * g_Frametime);\n"
    "    float e = smoothstep(0.12, 0.0, length(v_TexCoord - vec2(0.3, 0.5)));\n"
    "    vec4 under = texSample2D(g_Texture2, v_TexCoord);\n"
    "    gl_FragColor = min(dye + vec4(e * under.rgb, e) * (4.0 * g_Frametime), vec4(1.0));\n}\n";
static const char *lit_frag =
    "uniform sampler2D g_Texture0; // {\"hidden\":true}\nvarying vec2 v_TexCoord;\n"
    "void main() {\n    gl_FragColor = vec4(vec3(texSample2D(g_Texture0, v_TexCoord).a), 1.0);\n}\n";
static const char *show_frag =
    "uniform sampler2D g_Texture0; // {\"hidden\":true}\n"
    "uniform sampler2D g_Texture1; // {\"hidden\":true}\n"
    "varying vec2 v_TexCoord;\n"
    "void main() {\n"
    "    vec4 dye = texSample2D(g_Texture0, v_TexCoord);\n"
    "    vec4 under = texSample2D(g_Texture1, v_TexCoord);\n"
    "    gl_FragColor = vec4(mix(under.rgb, dye.rgb, dye.a), under.a);\n}\n";
/* (a comma before a closing bracket, as Wallpaper Engine's own files have) */
static const char *tsim_effect =
    "{\"passes\":[\n"
    " {\"material\":\"materials/effects/tsim_force.json\",\"target\":\"_rt_TsVelB\","
    "\"bind\":[{\"name\":\"_rt_TsVelA\",\"index\":0}]},\n"
    " {\"material\":\"materials/effects/tsim_dye.json\",\"target\":\"_rt_TsDyeB\","
    "\"bind\":[{\"name\":\"_rt_TsVelB\",\"index\":0},{\"name\":\"_rt_TsDyeA\",\"index\":1},"
    "{\"name\":\"previous\",\"index\":2}]},\n"
    " {\"material\":\"materials/effects/tsim_lit.json\",\"target\":\"_rt_TsLit\","
    "\"bind\":[{\"name\":\"_rt_TsDyeB\",\"index\":0}],\"conditions\":[{\"LIT\":1}]},\n"
    " {\"material\":\"materials/effects/tsim_show.json\","
    "\"bind\":[{\"name\":\"_rt_TsDyeB\",\"index\":0},{\"name\":\"previous\",\"index\":1}]},\n"
    " {\"command\":\"swap\",\"source\":\"_rt_TsVelA\",\"target\":\"_rt_TsVelB\"},\n"
    " {\"command\":\"swap\",\"source\":\"_rt_TsDyeA\",\"target\":\"_rt_TsDyeB\"}],\n"
    "\"fbos\":[\n"
    " {\"name\":\"_rt_TsVelA\",\"fit\":32,\"format\":\"rg1616f\",\"clear\":\"0 0 0 0\"},\n"
    " {\"name\":\"_rt_TsVelB\",\"fit\":32,\"format\":\"rg1616f\",\"clear\":\"0 0 0 0\"},\n"
    " {\"name\":\"_rt_TsDyeA\",\"scale\":2,\"format\":\"rgba8888\",\"clear\":\"0 0 0 0\"},\n"
    " {\"name\":\"_rt_TsDyeB\",\"scale\":2,\"format\":\"rgba8888\",\"clear\":\"0 0 0 0\"},\n"
    " {\"name\":\"_rt_TsLit\",\"scale\":2,\"format\":\"rgba8888\",\"conditions\":[{\"LIT\":1}]},\n"
    "],\n"
    "\"functions\":{\"clearDye\":{\"action\":\"clear\",\"fbos\":[\"_rt_TsDyeA\",\"_rt_TsDyeB\"]},\n"
    " \"clearVelocity\":{\"action\":\"clear\",\"fbos\":[\"_rt_TsVelA\",\"_rt_TsVelB\"]}}}\n";
/* an effect without a swap that uses the same words: two passes, a target
 * with a "fit", both pointer uniforms */
static const char *tplain_effect =
    "{\"passes\":[{\"material\":\"materials/effects/tplain_a.json\",\"target\":\"_rt_TpBuf\"},"
    "{\"material\":\"materials/effects/tplain_b.json\",\"bind\":[{\"name\":\"_rt_TpBuf\",\"index\":0}]}],"
    "\"fbos\":[{\"name\":\"_rt_TpBuf\",\"fit\":8,\"format\":\"rgba8888\",\"clear\":\"1 1 1 1\"}]}";
static const char *tplain_a_frag =
    "uniform sampler2D g_Texture0; // {\"hidden\":true}\n"
    "uniform vec2 g_PointerPosition;\nuniform vec2 g_PointerPositionLast;\nvarying vec2 v_TexCoord;\n"
    "void main() {\n    vec4 c = texSample2D(g_Texture0, v_TexCoord);\n"
    "    c.rg += 0.25 * (g_PointerPosition - g_PointerPositionLast) + 0.01 * g_PointerPosition;\n"
    "    gl_FragColor = c;\n}\n";
static const char *tplain_b_frag =
    "uniform sampler2D g_Texture0; // {\"hidden\":true}\nvarying vec2 v_TexCoord;\n"
    "void main() {\n    gl_FragColor = texSample2D(g_Texture0, v_TexCoord).bgra;\n}\n";

#define W 64
#define H 48
#define NPX (W * H * 4)
#define GW 32
#define GH 24

#define TSIM(consts, lit) "[{\"file\":\"effects/tsim/effect.json\",\"passes\":[{\"constantshadervalues\":{" consts \
                          "}},{},{\"combos\":{\"LIT\":" lit "}},{}]}]"

static GLuint smooth_tex (void)
{
    static guint8 px[NPX];
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            guint8 *p = &px[(y * W + x) * 4];
            p[0] = (guint8) (127.0 + 120.0 * sin (x * 0.21) * cos (y * 0.17));
            p[1] = (guint8) (127.0 + 120.0 * sin (x * 0.11 + y * 0.13));
            p[2] = (guint8) (40 + 3 * x);
            p[3] = 255;
        }
    GLuint t = 0;
    glGenTextures (1, &t);
    glBindTexture (GL_TEXTURE_2D, t);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    return t;
}

static struct FxSet *build_set (const char *effects_json)
{
    char *wrapped = g_strdup_printf ("{\"effects\":%s}", effects_json);
    JsonNode *root = wpj_parse_data (wrapped, -1);
    g_free (wrapped);
    if (!root)
        return NULL;
    struct FxSet *set = fx_build_for_object (tmpdir, wpj_arr (json_node_get_object (root), "effects"), "test", NULL);
    json_node_unref (root);
    return set;
}

static guint64 fnv (guint64 h, const void *data, gsize n)
{
    const guint8 *b = data;
    for (gsize i = 0; i < n; i++) {
        h ^= b[i];
        h *= 1099511628211ull;
    }
    return h;
}

/* the texels of a texture as RGBA floats (g_free); row 0 = v = 0 */
static float *read_float (GLuint tex, int *w, int *h)
{
    *w = *h = 0;
    if (!tex)
        return NULL;
    glBindTexture (GL_TEXTURE_2D, tex);
    glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, w);
    glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, h);
    if (*w < 1 || *h < 1)
        return NULL;
    float *px = g_malloc0 ((gsize) *w * (gsize) *h * 4 * sizeof (float));
    /* Everything drawn so far is in the texture before it is read. No GL
     * rule asks for this, but Mesa's llvmpipe (25.2) with rasteriser threads
     * now and then hands out a target's old content when it is read right
     * after a glClear of its framebuffer and a change of framebuffer (the
     * clearDye case: one run in four with two threads, never with none; a
     * second read is right). */
    glFinish ();
    glGetTexImage (GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, px);
    return px;
}

static GLuint target_tex (struct FxSet *set, const char *name)
{
    EffTarget *t = set && set->chains[0] ? fx_find_target (set->chains[0], name) : NULL;
    return t ? t->tex : 0;
}

/* the fields as the frame left them: after the swaps the A targets hold
 * what the last step wrote */
static guint64 state_hash (struct FxSet *set)
{
    guint64 hsh = 1469598103934665603ull;
    static const char *const names[] = { "_rt_TsVelA", "_rt_TsDyeA" };
    for (guint i = 0; i < G_N_ELEMENTS (names); i++) {
        int w, h;
        float *px = read_float (target_tex (set, names[i]), &w, &h);
        hsh = fnv (hsh, &w, sizeof (w));
        hsh = fnv (hsh, &h, sizeof (h));
        if (px)
            hsh = fnv (hsh, px, (gsize) w * h * 4 * sizeof (float));
        g_free (px);
    }
    return hsh;
}

/* sum of |values| of the first two channels (velocity) or of all four */
static double field_sum (struct FxSet *set, const char *name, int channels)
{
    int w, h;
    float *px = read_float (target_tex (set, name), &w, &h);
    double s = 0;
    for (int i = 0; px && i < w * h; i++)
        for (int c = 0; c < channels; c++)
            s += fabs ((double) px[i * 4 + c]);
    g_free (px);
    return s;
}

/* where two sets' velocity fields differ: the sum of the differences and
 * their centre, in texture coordinates (v = 0 = row 0) */
static double vel_diff (struct FxSet *a, struct FxSet *b, double *cu, double *cv)
{
    int aw, ah, bw, bh;
    float *pa = read_float (target_tex (a, "_rt_TsVelA"), &aw, &ah);
    float *pb = read_float (target_tex (b, "_rt_TsVelA"), &bw, &bh);
    double s = 0, sx = 0, sy = 0;
    if (pa && pb && aw == bw && ah == bh)
        for (int y = 0; y < ah; y++)
            for (int x = 0; x < aw; x++) {
                const int i = (y * aw + x) * 4;
                const double d = fabs ((double) pa[i] - pb[i]) + fabs ((double) pa[i + 1] - pb[i + 1]);
                s += d;
                sx += d * (x + 0.5) / aw;
                sy += d * (y + 0.5) / ah;
            }
    else
        s = -1;
    g_free (pa);
    g_free (pb);
    if (cu)
        *cu = s > 0 ? sx / s : -1;
    if (cv)
        *cv = s > 0 ? sy / s : -1;
    return s;
}

static void read_tex (GLuint tex, guint8 *out)
{
    glBindTexture (GL_TEXTURE_2D, tex);
    glGetTexImage (GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, out);
}

/* ---- frames. One frame of "a scene": the frame's values once, then the
 * runs of its layers. The pointer is given as a scene gets it from the
 * daemon and hands it on: x 0..1, y with 1 at the TOP. */
static double scene_t;
static guint scene_frame;
static int q_div = 1;
static gboolean q_all;

static void frame_begin (double dt, gboolean present, float x, float y_from_top)
{
    fx_set_frame_info (W, H, x, 1.0f - y_from_top, (float) dt);
    fx_set_pointer_state (present, FALSE);
    fx_set_quality (q_div, q_all, WALPE_POST_FULL, 60.0, scene_frame++);
    scene_t += dt;
}

static GLuint run (struct FxSet *set, GLuint in, gboolean in_static)
{
    return fx_run_for_object (set, in, W, H, W, H, scene_t, 1.0f, in_static);
}

static guint64 steps_of (struct FxSet *set)
{
    return set->chains[0]->sim->steps;
}

/* ---- 1 */
static gboolean shape_checks (GLuint in)
{
    struct FxSet *set = build_set (TSIM ("\"push\":1", "0"));
    if (!set) {
        result (FALSE, "the test's simulation effect is built", NULL);
        return FALSE;
    }
    EffectChain *c = set->chains[0];
    struct FxSim *s = c->sim;
    CHECK (s != NULL && fx_has_simulation (set), "a pass list with a swap is a simulation");
    if (!s) {
        fx_free_for_object (set);
        return FALSE;
    }
    CHECK (s->n_step == 2 && s->n_present == 1 && s->reuse_ok && !s->present_vol,
           "two step passes, one presenting pass whose picture can be shown again (%d, %d)", s->n_step, s->n_present);
    CHECK (c->n_targets == 4 && !fx_find_target (c, "_rt_TsLit") && c->passes[2].is_copy && !c->passes[2].program,
           "the pass and the target of a variant that is not in use are left out (%d targets)", c->n_targets);
    CHECK (s->n_fn == 2, "the effect file's clear functions are known (%d)", s->n_fn);
    CHECK (!fx_is_static (set) && !fx_cullable (set) && (fx_dynamic_inputs (set) & FX_DYN_FEEDBACK) &&
           fx_uses_pointer (set), "never static, never culled, carries its picture, follows the pointer");
    frame_begin (0.0, FALSE, 0, 0);
    GLuint out = run (set, in, TRUE);
    EffTarget *va = fx_find_target (c, "_rt_TsVelA"), *da = fx_find_target (c, "_rt_TsDyeA");
    if (s->broken) {
        printf ("skip the simulation checks: this context cannot render to RG16F\n");
        fx_free_for_object (set);
        return FALSE;
    }
    CHECK (out && out != in && out == c->out_tex, "the first frame draws the layer through the presenting pass");
    CHECK (va && va->w == GW && va->h == GH && va->vw == GW && va->vh == GH && !va->pool,
           "fit 32 on a 64x48 layer: a 32x24 grid of the chain's own (%dx%d)", va ? va->w : 0, va ? va->h : 0);
    CHECK (da && da->w == W / 2 && da->h == H / 2 && da->vw == W / 2 && !da->pool, "the dye at half the layer (%dx%d)",
           da ? da->w : 0, da ? da->h : 0);
    CHECK (fx_rt_fmt && GPOINTER_TO_INT (g_hash_table_lookup (fx_rt_fmt, GUINT_TO_POINTER (va->tex))) == FX_FMT_RG16F,
           "the velocity grid is a float target");
    CHECK (steps_of (set) == 0 && field_sum (set, "_rt_TsVelA", 2) == 0.0 && field_sum (set, "_rt_TsVelB", 2) == 0.0 &&
           field_sum (set, "_rt_TsDyeA", 4) == 0.0, "no time has passed: no step, every field holds its clear value");
    for (int p = 0; p < c->n_passes; p++)
        if (c->passes[p].kp_free != 1) {
            result (FALSE, "no pass of a simulation is marked as drawn once and kept", NULL);
            break;
        }
    /* memory: 2 x RG16F 32x24 (4 bytes a texel), 2 x RGBA8 32x24, the output */
    FxSimStats st;
    fx_sim_stats (set, &st);
    const gint64 want = 2 * (gint64) GW * GH * 4 + 2 * (gint64) (W / 2) * (H / 2) * 4 + (gint64) W * H * 4;
    CHECK (st.chains == 1 && st.off == 0 && st.targets == 5 && st.bytes == want && st.grid_w == GW && st.grid_h == GH &&
           st.field_w == W / 2 && st.field_h == H / 2 && st.step_hz == 60.0,
           "accounting: 5 targets, %" G_GINT64_FORMAT " bytes (want %" G_GINT64_FORMAT ")", st.bytes, want);
    gint64 tb = 0;
    int tn = 0;
    fx_targets_stats (&tb, &tn);
    CHECK (tn >= 5 && tb >= want, "... and they are part of the thread's effect target total (%d, %" G_GINT64_FORMAT ")",
           tn, tb);
    {
        /* fx_specialize (on with the presets Balanced and Low) gives a pass
         * another program while the wallpaper runs: fxs_switch () empties
         * the pass's tables and calls fx_cache_uniforms () once more, long
         * after the chain was built. The pass must come out as it was
         * built: the pointer's path, its state and the frame-to-layer
         * matrix. (Done by hand here, on a set that is freed right after.) */
        EffPass *p = &c->passes[0];
        int before[3] = { 0, 0, 0 }, after[3] = { 0, 0, 0 };
        for (int u = 0; u < p->n_dyn; u++) {
            before[0] += p->dyn[u].kind == FXU_POINTER_LAST;
            before[1] += p->dyn[u].kind == FXU_POINTER_STATE;
            before[2] += p->dyn[u].kind == FXU_TEXPROJ_INV;
        }
        g_free (p->dyn);
        g_free (p->dyn_last);
        g_free (p->consts);
        p->dyn = NULL;
        p->dyn_last = NULL;
        p->consts = NULL;
        p->n_dyn = p->n_consts = 0;
        fx_cache_uniforms (p);
        for (int u = 0; u < p->n_dyn; u++) {
            after[0] += p->dyn[u].kind == FXU_POINTER_LAST;
            after[1] += p->dyn[u].kind == FXU_POINTER_STATE;
            after[2] += p->dyn[u].kind == FXU_TEXPROJ_INV;
        }
        CHECK (before[0] == 1 && before[1] == 1 && before[2] == 1 && !memcmp (before, after, sizeof (before)),
               "a pass linked again after the build keeps its pointer path, pointer state and frame-to-layer matrix "
               "(%d %d %d -> %d %d %d)", before[0], before[1], before[2], after[0], after[1], after[2]);
    }
    fx_free_for_object (set);
    /* the lit variant: its pass and its target are there */
    set = build_set (TSIM ("\"push\":1", "1"));
    CHECK (set && set->chains[0]->sim && set->chains[0]->n_targets == 5 && set->chains[0]->sim->n_step == 3 &&
           set->chains[0]->passes[2].program, "with the combo set the variant's pass and target exist");
    if (set)
        fx_free_for_object (set);
    return TRUE;
}

/* a fresh set through n frames of dt with a moving pointer; the hash of
 * every frame's fields and picture */
static guint64 scripted (GLuint in, int n, double dt, gboolean with_pointer, guint64 *steps)
{
    struct FxSet *set = build_set (TSIM ("\"push\":1", "0"));
    guint8 *px = g_malloc (NPX);
    guint64 hsh = 1469598103934665603ull;
    for (int f = 0; set && f < n; f++) {
        const float x = 0.5f + 0.3f * (float) cos (f * 0.13), y = 0.5f + 0.3f * (float) sin (f * 0.09);
        frame_begin (dt, with_pointer, x, y);
        GLuint o = run (set, in, (f & 3) != 3);
        const guint64 sh = state_hash (set);
        read_tex (o, px);
        hsh = fnv (hsh, &sh, sizeof (sh));
        hsh = fnv (hsh, px, NPX);
    }
    if (steps)
        *steps = set ? steps_of (set) : 0;
    g_free (px);
    if (set)
        fx_free_for_object (set);
    return hsh;
}

/* ---- 2 */
static void determinism_checks (GLuint in)
{
    guint64 s1 = 0, s2 = 0;
    const guint64 a = scripted (in, 40, 1.0 / 60.0, TRUE, &s1), b = scripted (in, 40, 1.0 / 60.0, TRUE, &s2);
    CHECK (a == b && s1 == s2 && s1 == 39, "the same 40 frames twice: the same fields and pictures in every frame "
           "(%" G_GUINT64_FORMAT " steps)", s1);
    const guint64 c = scripted (in, 40, 1.0 / 60.0, FALSE, NULL);
    CHECK (a != c, "... and the pointer's way is part of them");
}

/* a fresh set, frames of dt until it has run `upto` steps; hashes[k] = the
 * fields after k steps, for the k that ended a frame (0 = not seen) */
static guint64 rate_run (GLuint in, double dt, int seconds2, guint64 *hashes, int upto)
{
    struct FxSet *set = build_set (TSIM ("\"push\":0", "0"));
    const int frames = (int) floor (seconds2 / dt + 0.5);
    for (int f = 0; set && f < frames; f++) {
        frame_begin (dt, FALSE, 0, 0);
        run (set, in, TRUE);
        const guint64 k = steps_of (set);
        if (k <= (guint64) upto && !hashes[k])
            hashes[k] = state_hash (set);
    }
    const guint64 total = set ? steps_of (set) : 0;
    if (set)
        fx_free_for_object (set);
    return total;
}

/* ---- 3 */
static void rate_checks (GLuint in)
{
    enum { UPTO = 110 };
    static guint64 h30[UPTO + 1], h60[UPTO + 1], h144[UPTO + 1];
    const guint64 n30 = rate_run (in, 1.0 / 30.0, 2, h30, UPTO);
    const guint64 n60 = rate_run (in, 1.0 / 60.0, 2, h60, UPTO);
    const guint64 n144 = rate_run (in, 1.0 / 144.0, 2, h144, UPTO);
    CHECK (llabs ((long long) n30 - 120) <= 2 && llabs ((long long) n60 - 120) <= 1 && llabs ((long long) n144 - 120) <= 1,
           "two seconds are 120 steps at 30, 60 and 144 frames per second (%" G_GUINT64_FORMAT ", %" G_GUINT64_FORMAT
           ", %" G_GUINT64_FORMAT ")", n30, n60, n144);
    int same = 0, differ = 0, compared = 0;
    for (int k = 2; k <= UPTO; k += 2) {
        if (!h30[k] || !h60[k] || !h144[k])
            continue;
        compared++;
        if (h30[k] == h60[k] && h60[k] == h144[k])
            same++;
        else
            differ++;
    }
    CHECK (compared >= 40 && differ == 0, "after the same number of steps the fields are the same bits at all three "
           "frame rates (%d step counts compared, %d equal, %d not)", compared, same, differ);
    CHECK (h60[20] && h60[100] && h60[20] != h60[100], "... and they do move");
}

/* ---- 4: two sets in the same frames, one of them under a shortcut */
static void shortcut_checks (GLuint in)
{
    struct FxSet *plain = build_set (TSIM ("\"push\":1", "0")), *sc = build_set (TSIM ("\"push\":1", "0"));
    if (!plain || !sc)
        return;
    guint held0 = 0, held1 = 0;
    guint64 rf = 0, rd = 0;
    guint rpass = 0, rrect = 0;
    fx_region_stats (NULL, NULL, NULL, NULL, TRUE);
    fx_pass_stats_held (&held0, FALSE);
    fx_set_cache_enabled (TRUE);
    gboolean epoch = FALSE, twice_ok = TRUE;
    for (int f = 0; f < 45; f++) {
        const float x = 0.5f + 0.25f * (float) sin (f * 0.2), y = 0.3f + 0.1f * (float) cos (f * 0.15);
        /* frames 15..29: every animated set is asked to be held on two of three frames */
        q_div = f >= 15 && f < 30 ? 3 : 1;
        q_all = TRUE;
        frame_begin (1.0 / 60.0, TRUE, x, y);
        run (plain, in, TRUE);
        if (f < 15) {
            /* the caller reads a small part of the result only */
            const float need[1][4] = { { 0.70f, 0.70f, 0.95f, 0.95f } };
            fx_set_need (need, 1);
        } else if (f >= 30) {
            /* "Skip hidden areas": only a corner of the layer is visible */
            const float clip[4] = { 0.0f, 0.0f, 0.2f, 0.2f };
            fx_set_clip_uv (clip, 1);
        }
        guint e = 0;
        epoch |= fx_set_hold_epoch (sc, &e);
        run (sc, in, TRUE);
        if (f == 20) {
            /* a comparison draws the run again: no second step in the frame */
            const guint64 k = steps_of (sc);
            run (sc, in, TRUE);
            twice_ok = steps_of (sc) == k;
        }
    }
    q_div = 1;
    q_all = FALSE;
    fx_region_stats (&rf, &rd, &rpass, &rrect, FALSE);
    fx_pass_stats_held (&held1, FALSE);
    CHECK (steps_of (plain) == 44 && steps_of (sc) == 44, "a step in every frame under every shortcut (%" G_GUINT64_FORMAT
           ", %" G_GUINT64_FORMAT ")", steps_of (plain), steps_of (sc));
    CHECK (state_hash (plain) == state_hash (sc), "the fields are the same bits with and without a region, a hold "
           "divisor and a clip");
    CHECK (rpass == 0 && rf == rd, "no pass was restricted to a region (%u passes, %" G_GUINT64_FORMAT " of %"
           G_GUINT64_FORMAT " pixels)", rpass, rd, rf);
    CHECK (held1 == held0 && !epoch, "no pass was held by the effect update rate");
    CHECK (twice_ok, "a second run in the same frame runs no second step");
    CHECK (!sc->cached && !sc->pfx.valid && !sc->held, "nothing of the set is kept by a cache");
    fx_free_for_object (plain);
    fx_free_for_object (sc);
}

/* ---- 5 */
static void present_checks (GLuint in)
{
    struct FxSet *still = build_set (TSIM ("\"push\":0", "0")), *moving = build_set (TSIM ("\"push\":0", "0"));
    if (!still || !moving)
        return;
    guint8 *a = g_malloc (NPX), *b = g_malloc (NPX), *last = g_malloc (NPX);
    guint64 st0 = 0;
    guint pr0 = 0, re0 = 0, pr1 = 0, re1 = 0;
    fx_sim_thread_stats (&st0, &pr0, &re0, FALSE);
    int zero_frames = 0, same_as_step = 0, same_both = 0, no_pass = 0;
    guint64 k_last = 0;
    for (int f = 0; f < 72; f++) {
        frame_begin (1.0 / 144.0, FALSE, 0, 0);
        guint r0 = 0, r1 = 0, r2 = 0;
        fx_pass_stats (&r0, NULL, FALSE);
        read_tex (run (still, in, TRUE), a);     /* the input stands still */
        fx_pass_stats (&r1, NULL, FALSE);
        read_tex (run (moving, in, FALSE), b);   /* it may change under its name */
        fx_pass_stats (&r2, NULL, FALSE);
        const guint64 k = steps_of (still);
        if (f > 0 && k == k_last) {
            zero_frames++;
            same_as_step += !memcmp (a, last, NPX);
            same_both += !memcmp (a, b, NPX);
            no_pass += r1 == r0 && r2 - r1 == 1;
        }
        memcpy (last, a, NPX);
        k_last = k;
    }
    fx_sim_thread_stats (NULL, &pr1, &re1, FALSE);
    CHECK (zero_frames >= 35 && same_as_step == zero_frames, "a frame without a step shows the picture of the last step "
           "(%d of %d such frames)", same_as_step, zero_frames);
    CHECK (same_both == zero_frames, "... whether it is drawn again or shown again");
    CHECK (no_pass == zero_frames && re1 - re0 >= (guint) zero_frames && pr1 - pr0 >= (guint) zero_frames,
           "... with no pass over a still input and one pass (the presenting one) otherwise (%d of %d)", no_pass,
           zero_frames);
    CHECK (steps_of (still) == steps_of (moving) && state_hash (still) == state_hash (moving),
           "the fields do not depend on how the picture was shown");
    g_free (a);
    g_free (b);
    g_free (last);
    fx_free_for_object (still);
    fx_free_for_object (moving);
}

static void frames (struct FxSet *set, GLuint in, int n, float k)
{
    for (int f = 0; f < n; f++) {
        frame_begin (1.0 / 60.0, FALSE, 0, 0);
        fx_run_for_object (set, in, W, H, W, H, scene_t, k, TRUE);
    }
}

/* ---- 6 */
static void reset_checks (GLuint in)
{
    struct FxSet *set = build_set (TSIM ("\"push\":0", "0"));
    if (!set)
        return;
    struct FxSim *s = set->chains[0]->sim;
    frames (set, in, 31, 1.0f);
    const guint r0 = s->resets;
    const double v0 = field_sum (set, "_rt_TsVelA", 2), d0 = field_sum (set, "_rt_TsDyeA", 4);
    CHECK (v0 > 0 && d0 > 0 && steps_of (set) == 30, "thirty steps: the fields have moved (%.2f, %.2f)", v0, d0);
    /* a constant a script sets: nothing starts again */
    const float emit = 55.0f;
    gboolean set_ok = fx_set_constant (set, 0, -1, "emit", &emit, 1);
    frames (set, in, 1, 1.0f);
    CHECK (set_ok && s->resets == r0 && steps_of (set) == 31 && field_sum (set, "_rt_TsVelA", 2) > v0 * 0.5,
           "a constant set by a script does not reset the fields");
    /* a function of the effect file */
    CHECK (fx_sim_call (set, -1, "clearDye") && !fx_sim_call (set, -1, "nothing") &&
           field_sum (set, "_rt_TsDyeA", 4) == 0.0 && field_sum (set, "_rt_TsDyeB", 4) == 0.0 &&
           field_sum (set, "_rt_TsVelA", 2) > 0 && s->resets == r0,
           "the function clearDye clears the dye and leaves the velocity");
    frames (set, in, 10, 1.0f);
    /* another working size: the dye is carried over, the grid stays */
    const GLuint grid = target_tex (set, "_rt_TsVelA");
    const double d1 = field_sum (set, "_rt_TsDyeA", 4);
    frames (set, in, 1, 0.5f);
    EffTarget *da = fx_find_target (set->chains[0], "_rt_TsDyeA");
    CHECK (da->w == W / 4 && da->h == H / 4 && s->resets == r0 && target_tex (set, "_rt_TsVelA") != 0 &&
           (target_tex (set, "_rt_TsVelA") == grid || target_tex (set, "_rt_TsVelB") == grid) &&
           field_sum (set, "_rt_TsDyeA", 4) > d1 * 0.1,
           "half the working size: the dye targets shrink and keep their field, the grid is untouched (%dx%d)", da->w,
           da->h);
    frames (set, in, 1, 1.0f);
    /* the quality level: live, carried over */
    CHECK (fx_sim_set_quality ("reduced") && !fx_sim_set_quality ("nonsense") && !strcmp (fx_sim_quality (), "reduced"),
           "the quality level is set by name");
    frames (set, in, 2, 1.0f);
    da = fx_find_target (set->chains[0], "_rt_TsDyeA");
    EffTarget *va = fx_find_target (set->chains[0], "_rt_TsVelA");
    CHECK (da->w == W / 4 && da->h == H / 4 && da->vw == W / 2 && va->w == GW && va->h == GH && s->resets == r0 &&
           field_sum (set, "_rt_TsDyeA", 4) > 0, "reduced: the dye at half its declared size, the grid as it was, "
           "nothing reset (%dx%d)", da->w, da->h);
    fx_sim_set_quality ("low");
    const guint64 k0 = steps_of (set);
    frames (set, in, 60, 1.0f);
    CHECK (llabs ((long long) (steps_of (set) - k0) - 30) <= 1, "low: 30 steps in a second of 60 frames (%" G_GUINT64_FORMAT
           ")", steps_of (set) - k0);
    fx_sim_set_quality ("full");
    frames (set, in, 2, 1.0f);
    da = fx_find_target (set->chains[0], "_rt_TsDyeA");
    CHECK (da->w == W / 2 && s->resets == r0, "full again: the dye back at its declared size, nothing reset");
    /* fx_sim_reset */
    fx_sim_reset (set);
    CHECK (s->resets == r0 + 1 && field_sum (set, "_rt_TsVelA", 2) == 0.0 && field_sum (set, "_rt_TsVelB", 2) == 0.0 &&
           field_sum (set, "_rt_TsDyeA", 4) == 0.0 && field_sum (set, "_rt_TsDyeB", 4) == 0.0,
           "fx_sim_reset: every field at its clear value");
    frames (set, in, 6, 1.0f);
    CHECK (field_sum (set, "_rt_TsVelA", 2) > 0, "... and the simulation goes on from there");
    /* the switch */
    const guint serial0 = fx_picture_serial ();
    gint64 tb0 = 0, tb1 = 0;
    int tn0 = 0, tn1 = 0;
    fx_targets_stats (&tb0, &tn0);
    fx_sim_set_enabled (FALSE);
    frame_begin (1.0 / 60.0, FALSE, 0, 0);
    GLuint o = run (set, in, TRUE);
    fx_targets_stats (&tb1, &tn1);
    FxSimStats st;
    fx_sim_stats (set, &st);
    CHECK (o == in && fx_is_static (set) && !(fx_dynamic_inputs (set) & FX_DYN_FEEDBACK) && fx_cullable (set) &&
           st.off == 1 && st.targets == 0 && tn1 == tn0 - 5 && fx_picture_serial () != serial0,
           "switched off: the layer's own picture, a static set, its five targets freed (%d -> %d), a frame asked for",
           tn0, tn1);
    frame_begin (1.0 / 60.0, FALSE, 0, 0);
    CHECK (run (set, in, TRUE) == in, "... and it stays that way");
    fx_sim_set_enabled (TRUE);
    frame_begin (1.0 / 60.0, FALSE, 0, 0);
    o = run (set, in, TRUE);
    CHECK (o != in && !fx_is_static (set) && steps_of (set) > 0 && field_sum (set, "_rt_TsVelA", 2) == 0.0 &&
           field_sum (set, "_rt_TsDyeA", 4) == 0.0, "switched on again: it starts from its clear values");
    frames (set, in, 5, 1.0f);
    CHECK (field_sum (set, "_rt_TsVelA", 2) > 0, "... and flows");
    fx_free_for_object (set);
}

/* ---- 7: `with` gets the pointer's push, `without` (push 0) does not; both
 * run in the same frames, so the difference of their fields is the push */
typedef void (*PtrScript) (int f, gboolean *present, float *x, float *y);

static double pushed (GLuint in, int n, PtrScript ps, const float *map, double *cu, double *cv)
{
    struct FxSet *with = build_set (TSIM ("\"push\":1", "0")), *without = build_set (TSIM ("\"push\":0", "0"));
    double d = -1;
    for (int f = 0; with && without && f < n; f++) {
        gboolean present = TRUE;
        float x = 0.5f, y = 0.5f;
        ps (f, &present, &x, &y);
        frame_begin (1.0 / 60.0, present, x, y);
        if (map)
            fx_set_pointer_map (map);
        run (with, in, TRUE);
        if (map)
            fx_set_pointer_map (map);
        run (without, in, TRUE);
    }
    if (with && without)
        d = vel_diff (with, without, cu, cv);
    if (with)
        fx_free_for_object (with);
    if (without)
        fx_free_for_object (without);
    return d;
}

static void ps_rest (int f, gboolean *present, float *x, float *y)
{
    (void) f; (void) present;
    *x = 0.7f;
    *y = 0.2f;
}

static void ps_absent (int f, gboolean *present, float *x, float *y)
{
    /* (positions that move: a pointer that is not there pushes nothing) */
    *present = FALSE;
    *x = 0.2f + 0.02f * (float) f;
    *y = 0.2f;
}

/* a short stroke to the right near the top right corner, ending at frame 6 */
static void ps_stroke (int f, gboolean *present, float *x, float *y)
{
    (void) present;
    *x = 0.60f + 0.03f * (float) MIN (f, 6);
    *y = 0.20f;
}

/* seen at the left, gone for five frames, back at the right: no stroke */
static void ps_gap (int f, gboolean *present, float *x, float *y)
{
    *present = f < 3 || f >= 8;
    *x = f < 8 ? 0.15f : 0.85f;
    *y = 0.8f;
}

/* resting at the left, then at the right in the next frame */
static void ps_jump (int f, gboolean *present, float *x, float *y)
{
    (void) present;
    *x = f < 5 ? 0.1f : 0.9f;
    *y = 0.8f;
}

static void pointer_gl_checks (GLuint in)
{
    double cu = 0, cv = 0;
    double d = pushed (in, 12, ps_rest, NULL, NULL, NULL);
    CHECK (d == 0.0, "a resting pointer pushes nothing (%.4f)", d);
    d = pushed (in, 12, ps_absent, NULL, NULL, NULL);
    CHECK (d == 0.0, "a pointer that is not there pushes nothing, wherever it is said to be (%.4f)", d);
    d = pushed (in, 8, ps_stroke, NULL, &cu, &cv);
    CHECK (d > 1.0 && fabs (cu - 0.69) < 0.12 && fabs (cv - 0.20) < 0.10,
           "a stroke near the top right pushes the fluid there: top of the frame = first rows of the grid "
           "(push %.2f around %.2f, %.2f)", d, cu, cv);
    /* the layer is the right half of the frame: the same stroke lands
     * further left on it (frame x 0.60..0.78 = layer u 0.20..0.56) */
    const float right[9] = { 0.5f, 0, 0, 0, 1, 0, 0.5f, 0, 1 };
    d = pushed (in, 8, ps_stroke, right, &cu, &cv);
    CHECK (d > 1.0 && fabs (cu - 0.38) < 0.14 && fabs (cv - 0.20) < 0.10,
           "... and through the layer's place in the frame (push %.2f around %.2f, %.2f)", d, cu, cv);
    d = pushed (in, 14, ps_gap, NULL, NULL, NULL);
    CHECK (d == 0.0, "gone and back somewhere else: no push along the way between (%.4f)", d);
    d = pushed (in, 12, ps_jump, NULL, NULL, NULL);
    CHECK (d == 0.0, "a jump across the picture in one frame: no push (%.4f)", d);
}

/* ---- 8: an effect file that asks for more than a simulation gets */
static void limit_checks (GLuint in)
{
    /* the test's effect with "fit": 4096 on its velocity pair */
    char **parts = g_strsplit (tsim_effect, "\"fit\":32", -1);
    char *big = g_strjoinv ("\"fit\":4096", parts);
    g_strfreev (parts);
    put ("effects/tbig/effect.json", big);
    g_free (big);
    /* (nothing of earlier checks left in the pool: the totals below are this set's) */
    fx_targets_release ();
    gint64 tb0 = 0, tb1 = 0;
    int tn0 = 0, tn1 = 0;
    fx_targets_stats (&tb0, &tn0);
    struct FxSet *set = build_set ("[{\"file\":\"effects/tbig/effect.json\",\"passes\":[{},{},{\"combos\":{\"LIT\":0}},{}]}]");
    if (!set || !set->chains[0] || !set->chains[0]->sim) {
        result (FALSE, "the effect with a fit of 4096 is built", NULL);
        if (set)
            fx_free_for_object (set);
        return;
    }
    EffectChain *c = set->chains[0];
    frame_begin (0.0, FALSE, 0, 0);
    GLuint o = run (set, in, TRUE);
    EffTarget *va = fx_find_target (c, "_rt_TsVelA"), *vb = fx_find_target (c, "_rt_TsVelB");
    EffTarget *da = fx_find_target (c, "_rt_TsDyeA");
    if (c->sim->broken || !va || !vb || !da) {
        result (FALSE, "the effect with a fit of 4096 runs", NULL);
        fx_free_for_object (set);
        return;
    }
    const int gw = WALPE_FLUID_FIT_MAX, gh = WALPE_FLUID_FIT_MAX * H / W;
    CHECK (o != in && va->fit == WALPE_FLUID_FIT_MAX && va->fit_asked == 4096 && va->w == gw && va->h == gh &&
           vb->w == gw && vb->h == gh && va->vw == gw && va->vh == gh,
           "\"fit\": 4096 in an effect file: the grid has %d cells on its longer side (%dx%d, the shaders are told "
           "%dx%d)", WALPE_FLUID_FIT_MAX, va->w, va->h, va->vw, va->vh);
    CHECK (c->sim->limit_said, "... and the chain has said so (one line, at its first run)");
    FxSimStats st;
    fx_sim_stats (set, &st);
    fx_targets_stats (&tb1, &tn1);
    gint64 want = 2 * (gint64) gw * gh * 4 + 2 * (gint64) (W / 2) * (H / 2) * 4 + (gint64) W * H * 4;
    CHECK (st.targets == 5 && st.bytes == want && st.grid_w == gw && st.grid_h == gh && tn1 - tn0 == 5 &&
           tb1 - tb0 == want, "the reduced targets are what is allocated and counted: %" G_GINT64_FORMAT " bytes in the "
           "set, %" G_GINT64_FORMAT " more in the thread's total (want %" G_GINT64_FORMAT ")", st.bytes, tb1 - tb0, want);
    frames (set, in, 3, 1.0f);
    const double v0 = field_sum (set, "_rt_TsVelA", 2);
    /* as with a driver whose largest texture has 512 texels a side: the
     * grid shrinks in proportion and keeps its field, the smaller targets
     * stay, nothing starts again and nothing more is logged */
    const guint r0 = c->sim->resets;
    fxsim_tex_max_v = 512;
    frames (set, in, 1, 1.0f);
    va = fx_find_target (c, "_rt_TsVelA");
    da = fx_find_target (c, "_rt_TsDyeA");
    fx_sim_stats (set, &st);
    fx_targets_stats (&tb1, &tn1);
    want = 2 * (gint64) 512 * 384 * 4 + 2 * (gint64) (W / 2) * (H / 2) * 4 + (gint64) W * H * 4;
    CHECK (v0 > 0 && va->w == 512 && va->h == 384 && va->vw == 512 && da->w == W / 2 && da->h == H / 2 &&
           c->out_w == W && c->out_h == H && c->sim->resets == r0 && field_sum (set, "_rt_TsVelA", 2) > 0 &&
           st.bytes == want && tb1 - tb0 == want && tn1 - tn0 == 5,
           "no target is larger than the driver's largest texture: the grid at %dx%d with its field, %" G_GINT64_FORMAT
           " bytes counted (want %" G_GINT64_FORMAT ")", va->w, va->h, tb1 - tb0, want);
    fxsim_tex_max_v = 0;   /* asked of the driver again */
    frames (set, in, 1, 1.0f);
    va = fx_find_target (c, "_rt_TsVelA");
    CHECK (va->w == gw && va->h == gh && c->sim->resets == r0 && steps_of (set) >= 4,
           "... and back at the real limit the grid is %dx%d again", va->w, va->h);
    fx_free_for_object (set);
}

/* ---- 9 */
static void fallback_checks (GLuint in)
{
    /* as on a driver that cannot render to RG16F */
    const guint8 was = fx_fmt_ok[FX_FMT_RG16F];
    fx_fmt_ok[FX_FMT_RG16F] = 2;
    struct FxSet *set = build_set (TSIM ("\"push\":1", "0"));
    if (set) {
        frame_begin (1.0 / 60.0, FALSE, 0, 0);
        GLuint o = run (set, in, TRUE);
        FxSimStats st;
        fx_sim_stats (set, &st);
        CHECK (o == in && set->chains[0]->sim->broken && st.off == 1 && st.targets == 0,
               "no float targets: the simulation is left out, the layer shown as it is, nothing kept");
        frame_begin (1.0 / 60.0, FALSE, 0, 0);
        CHECK (run (set, in, TRUE) == in && fx_is_static (set), "... and the set counts as static from then on");
        fx_free_for_object (set);
    }
    fx_fmt_ok[FX_FMT_RG16F] = was;
}

/* ---- 10 */
static void plain_checks (GLuint in)
{
    struct FxSet *set = build_set ("[{\"file\":\"effects/tplain/effect.json\"}]");
    if (!set) {
        result (FALSE, "the effect without a swap is built", NULL);
        return;
    }
    EffectChain *c = set->chains[0];
    gboolean last_kind = FALSE;
    int ptr_kinds = 0;
    for (int p = 0; p < c->n_passes; p++)
        for (int u = 0; u < c->passes[p].n_dyn; u++) {
            last_kind |= c->passes[p].dyn[u].kind >= FXU_POINTER_LAST;
            ptr_kinds += c->passes[p].dyn[u].kind == FXU_POINTER;
        }
    frame_begin (1.0 / 60.0, TRUE, 0.3f, 0.3f);
    GLuint o = run (set, in, TRUE);
    EffTarget *t = fx_find_target (c, "_rt_TpBuf");
    CHECK (!c->sim && !fx_has_simulation (set) && !last_kind && ptr_kinds == 2,
           "an effect without a swap is no simulation: both pointer uniforms are the frame's position");
    CHECK (o != in && t && t->w == W && t->h == H && t->fit == 0, "... its \"fit\" and \"clear\" are not looked at (%dx%d)",
           t ? t->w : 0, t ? t->h : 0);
    fx_free_for_object (set);
}

static void rm_rf (const char *path)
{
    GDir *dir = g_dir_open (path, 0, NULL);
    if (dir) {
        const char *n;
        while ((n = g_dir_read_name (dir)) != NULL) {
            char *c = g_build_filename (path, n, NULL);
            if (g_file_test (c, G_FILE_TEST_IS_DIR) && !g_file_test (c, G_FILE_TEST_IS_SYMLINK))
                rm_rf (c);
            else
                g_unlink (c);
            g_free (c);
        }
        g_dir_close (dir);
    }
    g_rmdir (path);
}

static gboolean gl_run (EGLDisplay dpy)
{
    EGLint maj, min, nc = 0;
    EGLConfig cfg;
    const EGLint attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                            EGL_NONE };
    if (dpy == EGL_NO_DISPLAY || !eglInitialize (dpy, &maj, &min) || !eglBindAPI (EGL_OPENGL_API) ||
        !eglChooseConfig (dpy, attr, &cfg, 1, &nc) || nc < 1) {
        printf ("skip GL checks on one EGL display (no usable GL config)\n");
        return FALSE;
    }
    const EGLint ca[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
                          EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                          EGL_NONE };
    EGLContext ctx = eglCreateContext (dpy, cfg, EGL_NO_CONTEXT, ca);
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent (dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        printf ("skip GL checks on one EGL display (no surfaceless GL 3.3 context)\n");
        if (ctx != EGL_NO_CONTEXT)
            eglDestroyContext (dpy, ctx);
        eglTerminate (dpy);
        return FALSE;
    }
    printf ("GL: %s / %s\n", glGetString (GL_RENDERER), glGetString (GL_VERSION));
    walpe_glutil_init ();
    GError *err = NULL;
    tmpdir = g_dir_make_tmp ("walpe-fxsim-XXXXXX", &err);
    if (!tmpdir) {
        result (FALSE, "temporary project", err ? err->message : NULL);
        return TRUE;
    }
    put ("effects/tsim/effect.json", tsim_effect);
    put_shader ("tsim_force", force_vert, force_frag);
    put_shader ("tsim_dye", quad_vert, dye_frag);
    put_shader ("tsim_lit", quad_vert, lit_frag);
    put_shader ("tsim_show", mvp_vert, show_frag);
    put ("effects/tplain/effect.json", tplain_effect);
    put_shader ("tplain_a", mvp_vert, tplain_a_frag);
    put_shader ("tplain_b", mvp_vert, tplain_b_frag);
    fx_sim_set_enabled (TRUE);
    fx_sim_set_quality ("full");
    GLuint in = smooth_tex ();
    if (shape_checks (in)) {
        determinism_checks (in);
        rate_checks (in);
        shortcut_checks (in);
        present_checks (in);
        reset_checks (in);
        pointer_gl_checks (in);
        limit_checks (in);
        fallback_checks (in);
    }
    plain_checks (in);
    glDeleteTextures (1, &in);
    fx_targets_release ();
    gint64 tb = 0;
    int tn = 0;
    fx_targets_stats (&tb, &tn);
    CHECK (tn == 0 && tb == 0, "no effect target is left when the sets are freed and the pool is released (%d, %"
           G_GINT64_FORMAT " bytes)", tn, tb);
    rm_rf (tmpdir);
    g_clear_pointer (&tmpdir, g_free);
    eglMakeCurrent (dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext (dpy, ctx);
    eglTerminate (dpy);
    return TRUE;
}

int main (void)
{
    /* the checks set every mode themselves */
    g_unsetenv ("WALPE_FLUID");
    g_unsetenv ("WALPE_FLUID_QUALITY");
    g_unsetenv ("WALPE_FLUID_POINTER_Y");
    g_unsetenv ("WALPE_FX_RT_FORMATS");
    g_unsetenv ("WALPE_FX_RT16F");
    g_unsetenv ("WALPE_FX_CACHE");
    g_unsetenv ("WALPE_FX_REGION");
    g_unsetenv ("WALPE_FX_REGION_CHECK");
    g_unsetenv ("WALPE_FX_SPEC");
    g_unsetenv ("WALPE_FX_QUALITY");
    g_unsetenv ("WALPE_FX_SHARE");
    step_checks ();
    fit_checks ();
    pointer_checks ();
    matrix_checks ();
    condition_checks ();
    int ran = 0;
    if (epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_EXT_device_enumeration") &&
        epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_EXT_platform_device")) {
        EGLDeviceEXT dev[8];
        EGLint nd = 0;
        if (eglQueryDevicesEXT (8, dev, &nd))
            for (EGLint i = 0; i < nd && !ran; i++)
                ran += gl_run (eglGetPlatformDisplayEXT (EGL_PLATFORM_DEVICE_EXT, dev[i], NULL));
    }
    if (!ran && epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_MESA_platform_surfaceless"))
        ran += gl_run (eglGetPlatformDisplayEXT (EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL));
    if (!ran)
        printf ("skip GL checks (no EGL device / surfaceless platform)\n");
    printf ("%s (%d check(s), %d failure(s))\n", fails ? "FAILED" : "PASSED", checks, fails);
    return fails ? 1 : 0;
}
