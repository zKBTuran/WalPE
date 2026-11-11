/* Effect passes restricted to what is read later (src/engine/fx_region.h).
 *
 *   fx-region-test [--print-models [<project dir> shake|shake_old|waves|waves_old|iris]]
 *
 * 1. No GL: which lookups count as "at the fragment's own texture
 *    coordinate" (and which do not: computed coordinates, a varying the
 *    vertex shader changes, macros, a sampler handed to a function ...),
 *    the rectangle arithmetic (union, growth, pixels rounded outwards, the
 *    limit, "not worth it"), the cut of a layer quad by the view, the
 *    rectangles around a puppet mesh.
 * 2. With an EGL device or Mesa's surfaceless platform (skipped without):
 *    small effects written to a temporary project, through the public
 *    executor API. For every chain: the picture inside the part the caller
 *    asked for is byte for byte the picture of the unrestricted run, also
 *    when everything a restricted pass leaves out holds a poison colour;
 *    fewer pixels are shaded; a chain with a pass that reads anywhere
 *    keeps the passes in front of it whole; kept outputs (static sets),
 *    feedback chains, folded transforms and the off switch are never
 *    restricted.
 * 3. With the Wallpaper Engine assets as well (WALPE_WE_ASSETS or the
 *    configured / detected install; skipped without): every combination
 *    of combos of WE's shake, water waves and iris shaders that
 *    fx_region_models.h lists is recognised (a different WE version shows
 *    as a note, not as a failure: the passes are then simply not
 *    restricted), and chains of them - with strong settings, black / white
 *    masks, script-changed constants, many points in time - give the same
 *    bytes restricted and whole. On a machine with an NVIDIA card this is
 *    the check of the reach bounds on that driver.
 * --print-models: table rows for fx_region_models.h (development): one
 *    shader family built in a project dir that holds a package's own
 *    copies of the two files.
 *
 * The engine file is included so the static functions are reachable. */
#include "../src/engine/scenenode_effects.c"
#include "../src/engine/puppet.h"
#include <epoxy/egl.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gstdio.h>
#include <stdio.h>

static int fails, checks, notes;
static gboolean print_models;
static const char *models_dir, *models_kind;

static void result (gboolean ok, const char *what, const char *detail)
{
    checks++;
    if (!ok)
        fails++;
    printf ("%s  %s%s%s\n", ok ? "ok  " : "FAIL", what, detail ? ": " : "", detail ? detail : "");
}

/* ------------------------------------------------------------ no GL */

static const char *VS_PLAIN =
    "uniform mat4 g_ModelViewProjectionMatrix;\n"
    "attribute vec3 a_Position;\nattribute vec2 a_TexCoord;\n"
    "varying vec4 v_TexCoord;\n"
    "void main() {\n"
    "  gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);\n"
    "  v_TexCoord = a_TexCoord.xyxy;\n"
    "}\n";

static void traits (const char *vs, const char *fs, EffPass *pass)
{
    GHashTable *combos = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    char *v = fx_build_source (vs, combos, TRUE), *f = fx_build_source (fs, combos, FALSE);
    memset (pass, 0, sizeof (*pass));
    fx_vertex_traits (pass, v);
    fxr_traits (pass, v, f, "test", combos);
    g_hash_table_unref (combos);
    g_free (v);
    g_free (f);
}

static void source_checks (void)
{
    EffPass p;
    traits (VS_PLAIN,
            "varying vec4 v_TexCoord;\nuniform sampler2D g_Texture0;\nuniform float g_A;\n"
            "void main() { vec4 c = texSample2D(g_Texture0, v_TexCoord.xy); c.a *= g_A; gl_FragColor = c; }\n",
            &p);
    result (p.rg_plain && (p.rg_local & 1), "a lookup at v_TexCoord.xy is local", NULL);
    traits (VS_PLAIN,
            "varying vec4 v_TexCoord;\nuniform sampler2D g_Texture0;\nuniform sampler2D g_Texture1;\n"
            "void main() { vec4 c = texSample2D(g_Texture0, v_TexCoord.xy);\n"
            "  vec4 m = texSample2D(g_Texture1, v_TexCoord.zw);\n"
            "  gl_FragColor = c * m + texSample2D(g_Texture1, v_TexCoord.xy * 2.0); }\n",
            &p);
    result ((p.rg_local & 1) && !(p.rg_local & 2), "two units: one local, one read at a computed place", NULL);
    result ((p.rg_local & 0xfc) == 0xfc, "samplers the code never names count as never read", NULL);
    traits (VS_PLAIN,
            "varying vec4 v_TexCoord;\nuniform sampler2D g_Texture0;\nuniform float g_Time;\n"
            "void main() { gl_FragColor = texSample2D(g_Texture0, v_TexCoord.xy + vec2(sin(g_Time), 0.0)); }\n",
            &p);
    result (p.rg_plain && !(p.rg_local & 1), "a displaced lookup is not local", NULL);
    traits (VS_PLAIN,
            "varying vec4 v_TexCoord;\nuniform sampler2D g_Texture0;\n"
            "void main() { vec2 uv = v_TexCoord.xy; uv += 0.01; gl_FragColor = texSample2D(g_Texture0, uv); }\n",
            &p);
    result (!(p.rg_local & 1), "a local copy of the coordinate is not trusted", NULL);
    traits (VS_PLAIN,
            "varying vec4 v_TexCoord;\nuniform sampler2D g_Texture0;\n"
            "#define SAMPLE(uv) texSample2D(g_Texture0, uv)\n"
            "void main() { gl_FragColor = SAMPLE(v_TexCoord.xy * 1.5); }\n",
            &p);
    result (p.rg_local == 0, "a macro that can hide a lookup: nothing is local", NULL);
    traits (VS_PLAIN,
            "varying vec4 v_TexCoord;\nuniform sampler2D g_Texture0;\n"
            "vec4 fetch(sampler2D s, vec2 uv) { return texSample2D(s, uv + 0.25); }\n"
            "void main() { gl_FragColor = fetch(g_Texture0, v_TexCoord.xy); }\n",
            &p);
    result (!(p.rg_local & 1), "a sampler handed to a function is not local", NULL);
    traits (VS_PLAIN,
            "varying vec4 v_TexCoord;\nuniform sampler2D g_Texture0;\n"
            "void main() { gl_FragColor = texelFetch(g_Texture0, ivec2(3, 4), 0) + texSample2D(g_Texture0, v_TexCoord.xy); }\n",
            &p);
    result (!(p.rg_local & 1), "texelFetch spoils the unit", NULL);
    traits (VS_PLAIN,
            "varying vec4 v_TexCoord;\nuniform sampler2D g_Texture0;\nuniform sampler2D g_Texture1;\n"
            "void main() { gl_FragColor = texSample2D(g_Texture0, v_TexCoord.zw) + texSample2D(g_Texture1, v_TexCoord.xyz.xy); }\n",
            &p);
    result ((p.rg_local & 1) && !(p.rg_local & 2), ".zw of a_TexCoord.xyxy is local, an odd swizzle is not", NULL);
    /* vertex shaders that touch the coordinate */
    static const char *vs_scaled =
        "uniform mat4 g_ModelViewProjectionMatrix;\nuniform vec4 g_Texture1Resolution;\n"
        "attribute vec3 a_Position;\nattribute vec2 a_TexCoord;\nvarying vec4 v_TexCoord;\n"
        "void main() {\n  gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);\n"
        "  v_TexCoord = a_TexCoord.xyxy;\n  v_TexCoord.z *= g_Texture1Resolution.z / max(g_Texture1Resolution.x, 1.0);\n}\n";
    traits (vs_scaled,
            "varying vec4 v_TexCoord;\nuniform sampler2D g_Texture0;\nuniform sampler2D g_Texture1;\n"
            "void main() { gl_FragColor = texSample2D(g_Texture0, v_TexCoord.xy) * texSample2D(g_Texture1, v_TexCoord.zw); }\n",
            &p);
    result ((p.rg_local & 1) && !(p.rg_local & 2), "a varying half the vertex shader scales is not local, the other half is", NULL);
    static const char *vs_moved =
        "uniform mat4 g_ModelViewProjectionMatrix;\n"
        "attribute vec3 a_Position;\nattribute vec2 a_TexCoord;\nvarying vec4 v_TexCoord;\n"
        "void main() {\n  gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);\n"
        "  v_TexCoord.xy = a_TexCoord * 0.5 + 0.25;\n  v_TexCoord.zw = a_TexCoord;\n}\n";
    traits (vs_moved,
            "varying vec4 v_TexCoord;\nuniform sampler2D g_Texture0;\nuniform sampler2D g_Texture1;\n"
            "void main() { gl_FragColor = texSample2D(g_Texture0, v_TexCoord.xy) * texSample2D(g_Texture1, v_TexCoord.zw); }\n",
            &p);
    result (!(p.rg_local & 1) && (p.rg_local & 2), "v.xy = f (a_TexCoord) is not local, v.zw = a_TexCoord is", NULL);
    static const char *vs_vec2 =
        "uniform mat4 g_ModelViewProjectionMatrix;\n"
        "attribute vec3 a_Position;\nattribute vec2 a_TexCoord;\nvarying vec2 v_Uv;\n"
        "void main() {\n  gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);\n"
        "  v_Uv = a_TexCoord;\n}\n";
    traits (vs_vec2,
            "varying vec2 v_Uv;\nuniform sampler2D g_Texture0;\n"
            "void main() { gl_FragColor = texSample2D(g_Texture0, v_Uv); }\n",
            &p);
    result (p.rg_plain && (p.rg_local & 1), "a vec2 varying set to a_TexCoord is local", NULL);
    static const char *vs_warp =
        "uniform mat4 g_ModelViewProjectionMatrix;\nuniform float g_Time;\n"
        "attribute vec3 a_Position;\nattribute vec2 a_TexCoord;\nvarying vec4 v_TexCoord;\n"
        "void main() {\n  vec3 at = a_Position;\n  at.x += sin(g_Time) * 10.0;\n"
        "  gl_Position = mul(vec4(at, 1.0), g_ModelViewProjectionMatrix);\n"
        "  v_TexCoord = a_TexCoord.xyxy;\n}\n";
    traits (vs_warp,
            "varying vec4 v_TexCoord;\nuniform sampler2D g_Texture0;\n"
            "void main() { gl_FragColor = texSample2D(g_Texture0, v_TexCoord.xy); }\n",
            &p);
    result (!p.rg_plain && p.rg_local == 0, "a vertex shader that moves the quad: nothing is local", NULL);
    float ru, rv, weak;
    result (!fxr_reach (&p, 0, &ru, &rv, &weak), "... and its reach is unknown", NULL);

    /* rectangles */
    FxNeed a = { FXR_RECTS, 1, { { 0.25f, 0.25f, 0.5f, 0.5f } } };
    FxNeed g = fxr_need_grow (&a, 0.1f, 0.3f);
    result (g.mode == FXR_RECTS && fabsf (g.r[0][0] - 0.15f) < 1e-6f && g.r[0][1] == 0.0f &&
            fabsf (g.r[0][2] - 0.6f) < 1e-6f && fabsf (g.r[0][3] - 0.8f) < 1e-6f,
            "growth is per axis and stays inside 0..1", NULL);
    g = fxr_need_grow (&a, NAN, 0.0f);
    result (g.mode == FXR_FULL, "a margin that is no number means everything", NULL);
    FxNeed n = { FXR_NONE, 0, { { 0 } } };
    fxr_need_add (&n, &a);
    result (n.mode == FXR_RECTS && n.n == 1, "nothing + a rectangle = the rectangle", NULL);
    for (int i = 0; i < 9; i++) {
        FxNeed b = { FXR_RECTS, 1, { { 0.1f * (float) i, 0.0f, 0.1f * (float) i + 0.05f, 0.05f } } };
        fxr_need_add (&n, &b);
    }
    gboolean covered = n.mode == FXR_RECTS && n.n <= FXR_MAX_RECTS;
    for (int i = 0; covered && i < 9; i++) {
        gboolean in = FALSE;
        for (int k = 0; k < n.n; k++)
            in |= n.r[k][0] <= 0.1f * (float) i + 1e-6f && n.r[k][2] >= 0.1f * (float) i + 0.05f - 1e-6f &&
                  n.r[k][1] <= 0.0f && n.r[k][3] >= 0.05f;
        covered = in;
    }
    result (covered, "more rectangles than fit: joined, nothing lost", NULL);
    FxNeed full = { FXR_FULL, 0, { { 0 } } };
    fxr_need_add (&n, &full);
    result (n.mode == FXR_FULL, "anything + everything = everything", NULL);
    int px[FXR_MAX_RECTS][4];
    guint64 area = 0;
    FxNeed q = { FXR_RECTS, 1, { { 0.101f, 0.2f, 0.299f, 0.401f } } };
    {
        /* a rectangle has to be worth its two calls */
        FxNeed half = { FXR_RECTS, 1, { { 0.0f, 0.0f, 0.5f, 1.0f } } };
        result (fxr_min_px () == 32768 && fxr_pixels (&half, 256, 128, px, &area) == 0 &&
                fxr_pixels (&half, 512, 128, px, &area) == 1 && area == 32768,
                "a restriction that saves fewer than 32768 pixels a rectangle is not made", NULL);
        FxNeed halves = { FXR_RECTS, 2, { { 0.0f, 0.0f, 0.25f, 1.0f }, { 0.5f, 0.0f, 0.75f, 1.0f } } };
        result (fxr_pixels (&halves, 512, 128, px, &area) == 0 && fxr_pixels (&halves, 1024, 128, px, &area) == 2,
                "... per rectangle: two that are not worth it are joined, or the target is drawn whole", NULL);
        FxNeed near = { FXR_RECTS, 2, { { 0.0f, 0.0f, 0.30f, 1.0f }, { 0.31f, 0.0f, 0.5f, 1.0f } } };
        result (fxr_pixels (&near, 2000, 1000, px, &area) == 1 && px[0][0] == 0 && px[0][2] == 1000 && px[0][3] == 1000,
                "two rectangles with a small gap between them are drawn as one", NULL);
    }
    /* (everything below - and the GL checks, whose targets are 256 x 192 -
     * is about the same bytes, not about what a rectangle is worth) */
    g_atomic_int_set (&fxr_min_saved, 0);
    int k = fxr_pixels (&q, 100, 200, px, &area);
    result (k == 1 && px[0][0] == 10 && px[0][1] == 40 && px[0][2] == 20 && px[0][3] == 41 && area == 820,
            "pixels are rounded outwards", NULL);
    FxNeed big = { FXR_RECTS, 1, { { 0.0f, 0.0f, 1.0f, 0.95f } } };
    result (fxr_pixels (&big, 100, 200, px, &area) == 0, "less than a sixteenth to save: the whole target", NULL);
    result (fxr_pixels (&q, 8, 8, px, &area) == 0, "a tiny target is drawn whole", NULL);
    FxNeed two = { FXR_RECTS, 2, { { 0.0f, 0.0f, 0.5f, 0.25f }, { 0.0f, 0.25f, 0.5f, 0.5f } } };
    k = fxr_pixels (&two, 100, 100, px, &area);
    result (k == 1 && px[0][2] == 50 && px[0][3] == 50 && area == 2500,
            "two rectangles whose union is a rectangle become one", NULL);
    FxNeed lsh = { FXR_RECTS, 2, { { 0.0f, 0.0f, 0.5f, 0.25f }, { 0.0f, 0.2f, 0.25f, 0.5f } } };
    k = fxr_pixels (&lsh, 100, 100, px, &area);
    result (k == 2 && area == 50 * 25 + 25 * 30, "an L stays two rectangles (the shared strip is drawn twice)", NULL);
    FxNeed inv = { FXR_RECTS, 1, { { 0.5f, 0.5f, 0.5f, 0.5f } } };
    result (fxr_pixels (&inv, 100, 100, px, &area) == 0, "an empty rectangle restricts nothing", NULL);
    float bad[1][4] = { { 0.5f, 0.1f, 0.2f, 0.9f } };
    fx_set_region_enabled (TRUE);
    fx_set_need ((const float (*)[4]) bad, 1);
    result (fxr_caller_need.mode == FXR_NONE, "an inverted rectangle from the caller says nothing", NULL);
    float nanr[1][4] = { { 0.1f, NAN, 0.2f, 0.9f } };
    fx_set_need ((const float (*)[4]) nanr, 1);
    result (fxr_caller_need.mode == FXR_NONE, "a NaN from the caller says nothing", NULL);
    /* the centres of the pixels a need covers: what the filter margin is
     * counted from (one texel a pass at equal sizes) */
    {
        FxNeed c = fxr_need_centres (&q, 100, 200);
        result (c.mode == FXR_RECTS && c.n == 1 && fabsf (c.r[0][0] - 0.105f) < 1e-6f && fabsf (c.r[0][2] - 0.295f) < 1e-6f &&
                fabsf (c.r[0][1] - 40.5f / 200) < 1e-6f && fabsf (c.r[0][3] - 80.5f / 200) < 1e-6f,
                "pixel centres of a need: first to last pixel", NULL);
        FxNeed g = fxr_need_grow (&c, 0.55f / 100, 0.55f / 200);
        k = fxr_pixels (&g, 100, 200, px, &area);
        result (k == 1 && px[0][0] == 9 && px[0][1] == 39 && px[0][2] == 22 && px[0][3] == 43,
                "a local read at equal sizes needs exactly one texel more on each side", NULL);
        c = fxr_need_centres (&inv, 100, 100);
        result (c.mode == FXR_FULL, "no pixel to count from: everything", NULL);
    }
    /* marked cells of a map -> rectangles that hold every marked cell */
    {
        guint32 grid[32] = { 0 };
        FxNeed cv;
        fxr_cover (grid, &cv);
        result (cv.mode == FXR_NONE, "an empty map has no strong part", NULL);
        for (int y = 4; y < 9; y++)
            grid[y] = 0x000003f0u;            /* columns 4..9 */
        for (int y = 20; y < 30; y++)
            grid[y] = 0x0ff00000u;            /* columns 20..27 */
        fxr_cover (grid, &cv);
        float a = 0.0f;
        for (int i = 0; i < cv.n; i++)
            a += (cv.r[i][2] - cv.r[i][0]) * (cv.r[i][3] - cv.r[i][1]);
        result (cv.mode == FXR_RECTS && cv.n == 2 && fabsf (a - (6 * 5 + 8 * 10) / 1024.0f) < 1e-6f,
                "two blocks of marked cells are two rectangles of exactly their size", NULL);
        gboolean all = TRUE, disjoint = TRUE;
        guint32 sd = 12345;
        for (int round = 0; round < 40; round++) {
            for (int y = 0; y < 32; y++) {
                grid[y] = 0;
                for (int x = 0; x < 32; x++) {
                    sd = sd * 1664525u + 1013904223u;
                    /* from a few spots to salt and pepper */
                    if ((sd >> 16) % 1000u < (guint32) (round < 20 ? 4 + round : 20 * (round - 19)))
                        grid[y] |= 1u << x;
                }
            }
            fxr_cover (grid, &cv);
            if (cv.mode == FXR_FULL)
                continue;
            for (int y = 0; y < 32; y++)
                for (int x = 0; x < 32; x++) {
                    if (!(grid[y] >> x & 1u))
                        continue;
                    gboolean in = FALSE;
                    for (int i = 0; i < cv.n; i++)
                        in |= cv.r[i][0] * 32 <= x && x + 1 <= cv.r[i][2] * 32 && cv.r[i][1] * 32 <= y && y + 1 <= cv.r[i][3] * 32;
                    all &= cv.mode == FXR_RECTS && in;
                }
            for (int i = 0; i < cv.n; i++)
                for (int j = i + 1; j < cv.n; j++)
                    disjoint &= !(cv.r[i][0] < cv.r[j][2] && cv.r[j][0] < cv.r[i][2] && cv.r[i][1] < cv.r[j][3] &&
                                  cv.r[j][1] < cv.r[i][3]);
            all &= cv.n <= FXR_MAX_RECTS;
        }
        result (all, "random maps: every marked cell is inside one of at most six rectangles", NULL);
        result (disjoint, "... and the rectangles do not overlap", NULL);
    }
}

/* ---------------------------------------------------------------- GL */

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

/* a single-pass effect "name" from its two shader bodies */
static void put_effect (const char *name, const char *vert, const char *frag)
{
    char *e = g_strdup_printf ("effects/%s/effect.json", name);
    char *ej = g_strdup_printf ("{\"passes\":[{\"material\":\"materials/effects/%s.json\"}]}", name);
    char *m = g_strdup_printf ("materials/effects/%s.json", name);
    char *mj = g_strdup_printf ("{\"passes\":[{\"shader\":\"effects/%s\"}]}", name);
    char *v = g_strdup_printf ("shaders/effects/%s.vert", name), *f = g_strdup_printf ("shaders/effects/%s.frag", name);
    put (e, ej);
    put (m, mj);
    put (v, vert);
    put (f, frag);
    g_free (e); g_free (ej); g_free (m); g_free (mj); g_free (v); g_free (f);
}

#define W 256
#define H 192

static GLuint noise_tex (int w, int h, guint32 seed, gboolean binary)
{
    guint8 *px = g_malloc ((gsize) w * h * 4);
    guint32 s = seed;
    for (int i = 0; i < w * h * 4; i++) {
        s = s * 1664525u + 1013904223u;
        px[i] = binary ? ((s >> 28) & 1 ? 255 : 0) : (guint8) (s >> 24);
    }
    GLuint t = 0;
    glGenTextures (1, &t);
    glBindTexture (GL_TEXTURE_2D, t);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    g_free (px);
    return t;
}

static void put_png (const char *rel, int w, int h, guint32 seed, gboolean binary)
{
    guint8 *px = g_malloc ((gsize) w * h * 4);
    guint32 s = seed;
    for (int i = 0; i < w * h * 4; i++) {
        s = s * 1664525u + 1013904223u;
        /* blocks of 8x8 so that a mask has areas, not salt and pepper */
        int x = (i / 4) % w, y = (i / 4) / w;
        guint32 b = (guint32) (x / 8) * 2654435761u ^ (guint32) (y / 8) * 40503u ^ seed;
        b = b * 1664525u + 1013904223u;
        px[i] = (i % 4) == 3 ? 255 : binary ? ((b >> 28) & 1 ? 255 : 0) : (guint8) ((b >> 24) ^ (s >> 30));
    }
    GdkPixbuf *pb = gdk_pixbuf_new_from_data (px, GDK_COLORSPACE_RGB, TRUE, 8, w, h, w * 4, NULL, NULL);
    char *p = g_build_filename (tmpdir, rel, NULL), *d = g_path_get_dirname (p);
    g_mkdir_with_parents (d, 0700);
    gdk_pixbuf_save (pb, p, "png", NULL, NULL);
    g_free (p);
    g_free (d);
    g_object_unref (pb);
    g_free (px);
}

/* a map with value (r0, g0) everywhere and (r1, g1) inside the rectangle
 * (u0, v0)-(u1, v1), two texels of ramp between them */
static void put_patch_png (const char *rel, int w, int h, int r0, int g0, int r1, int g1,
                           float u0, float v0, float u1, float v1)
{
    guint8 *px = g_malloc ((gsize) w * h * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const float dx = fmaxf (u0 * w - (x + 0.5f), (x + 0.5f) - u1 * w);
            const float dy = fmaxf (v0 * h - (y + 0.5f), (y + 0.5f) - v1 * h);
            const float in = CLAMP (1.0f - fmaxf (dx, dy) / 2.0f, 0.0f, 1.0f);
            guint8 *q = px + ((gsize) y * w + x) * 4;
            q[0] = (guint8) lroundf (r0 + (r1 - r0) * in);
            q[1] = (guint8) lroundf (g0 + (g1 - g0) * in);
            q[2] = 0;
            q[3] = 255;
        }
    GdkPixbuf *pb = gdk_pixbuf_new_from_data (px, GDK_COLORSPACE_RGB, TRUE, 8, w, h, w * 4, NULL, NULL);
    char *p = g_build_filename (tmpdir, rel, NULL), *d = g_path_get_dirname (p);
    g_mkdir_with_parents (d, 0700);
    gdk_pixbuf_save (pb, p, "png", NULL, NULL);
    g_free (p);
    g_free (d);
    g_object_unref (pb);
    g_free (px);
}

static gpointer build_set (const char *dir, const char *effects_json)
{
    char *wrapped = g_strdup_printf ("{\"effects\":%s}", effects_json);
    JsonNode *root = wpj_parse_data (wrapped, -1);
    g_free (wrapped);
    if (!root)
        return NULL;
    gpointer set = fx_build_for_object (dir, wpj_arr (json_node_get_object (root), "effects"), "test", NULL);
    json_node_unref (root);
    return set;
}

static void read_tex (GLuint tex, int w, int h, guint8 *out)
{
    glBindTexture (GL_TEXTURE_2D, tex);
    int tw = 0, th = 0;
    glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
    glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
    memset (out, 0, (gsize) w * h * 4);
    if (tw == w && th == h)
        glGetTexImage (GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, out);
}

/* every target the pool holds, filled with a colour no effect here makes:
 * what a restricted pass leaves out then holds it */
static void poison_pool (void)
{
    GLint fb = 0;
    glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &fb);
    glDisable (GL_SCISSOR_TEST);
    for (guint i = 0; fx_pool && i < fx_pool->len; i++) {
        FxPoolRt *e = &g_array_index (fx_pool, FxPoolRt, i);
        if (!e->tex)
            continue;
        glBindFramebuffer (GL_FRAMEBUFFER, e->fbo);
        glClearColor (1.0f, 0.0f, 1.0f, 0.37f);
        glClear (GL_COLOR_BUFFER_BIT);
    }
    glBindFramebuffer (GL_FRAMEBUFFER, (GLuint) fb);
}

typedef struct {
    guint64 px_full, px_drawn;
    guint restricted, rects;
    gboolean out_is_input;
} RunInfo;

/* one run of the set over `in` at time t; need = NULL (everything) or n
 * rectangles; the result's pixels in out */
static void run (gpointer set, GLuint in, double t, const float (*need)[4], int n, guint8 *out, RunInfo *ri)
{
    fx_set_frame_info (W, H, 0.5f, 0.5f, 1.0f / 30);
    fx_region_stats (NULL, NULL, NULL, NULL, TRUE);
    if (need)
        fx_set_need (need, n);
    GLuint o = fx_run_for_object (set, in, W, H, W, H, t, 1.0f, FALSE);
    fx_region_stats (&ri->px_full, &ri->px_drawn, &ri->restricted, &ri->rects, TRUE);
    ri->out_is_input = o == in;
    read_tex (o, W, H, out);
    GLboolean sc = glIsEnabled (GL_SCISSOR_TEST);
    if (sc) {
        result (FALSE, "the scissor test is off again after a run", NULL);
        glDisable (GL_SCISSOR_TEST);
    }
}

/* differing bytes inside the rectangles (texture coordinates): every
 * texel a linear sample taken inside a rectangle can read, i.e. the two
 * around each place */
static int diff_inside (const guint8 *a, const guint8 *b, const float (*need)[4], int n, int *outside_stale)
{
    int d = 0, stale = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            gboolean in = FALSE;
            for (int k = 0; k < n; k++) {
                int x0 = (int) floorf (need[k][0] * W - 0.5f), x1 = (int) floorf (need[k][2] * W - 0.5f) + 2;
                int y0 = (int) floorf (need[k][1] * H - 0.5f), y1 = (int) floorf (need[k][3] * H - 0.5f) + 2;
                in |= x >= x0 && x < x1 && y >= y0 && y < y1;
            }
            const guint8 *pa = a + ((gsize) y * W + x) * 4, *pb = b + ((gsize) y * W + x) * 4;
            if (in)
                d += memcmp (pa, pb, 4) != 0;
            else
                stale += pb[0] == 255 && pb[1] == 0 && pb[2] == 255;
        }
    if (outside_stale)
        *outside_stale = stale;
    return d;
}

/* The chain drawn whole and drawn for `need`, at several times: the same
 * bytes inside the need; restricted = what the last run reports. */
static gboolean same_inside (const char *dir, const char *effects_json, GLuint in, const float (*need)[4], int n,
                             RunInfo *last, int *stale, char **detail)
{
    static guint8 a[W * H * 4], b[W * H * 4];
    gpointer set = build_set (dir, effects_json);
    if (!set) {
        *detail = g_strdup ("the set did not build");
        return FALSE;
    }
    gboolean ok = TRUE;
    static const double times[] = { 0.0, 0.37, 1.91, 7.3, 33.33, 123.4 };
    for (guint i = 0; ok && i < G_N_ELEMENTS (times); i++) {
        RunInfo full, part;
        fx_set_region_enabled (FALSE);
        run (set, in, times[i], need, n, a, &full);
        fx_set_region_enabled (TRUE);
        poison_pool ();
        run (set, in, times[i], need, n, b, &part);
        int d = diff_inside (a, b, need, n, stale);
        if (d || full.restricted || full.out_is_input || part.out_is_input) {
            ok = FALSE;
            *detail = g_strdup_printf ("t = %.2f: %d pixel(s) differ inside the need; switch off: %u restricted pass(es)",
                                       times[i], d, full.restricted);
        }
        *last = part;
    }
    fx_free_for_object (set);
    return ok;
}

static const char *FS_TINT =
    "varying vec4 v_TexCoord;\nuniform sampler2D g_Texture0;\n"
    "uniform vec3 g_Tint; // {\"material\":\"tint\",\"default\":\"0.9 0.7 0.8\"}\n"
    "void main() { vec4 c = texSample2D(g_Texture0, v_TexCoord.xy); c.rgb *= g_Tint; gl_FragColor = c; }\n";
static const char *FS_WOBBLE =
    "varying vec4 v_TexCoord;\nuniform sampler2D g_Texture0;\nuniform float g_Time;\n"
    "uniform float g_Amp; // {\"material\":\"amp\",\"default\":0.21}\n"
    "void main() { vec2 uv = v_TexCoord.xy + g_Amp * vec2(sin(g_Time * 3.0 + v_TexCoord.y * 40.0), cos(g_Time + v_TexCoord.x * 31.0));\n"
    "  gl_FragColor = texSample2D(g_Texture0, uv); }\n";
static const char *FS_PARTIAL =
    "varying vec4 v_TexCoord;\nuniform sampler2D g_Texture0;\n"
    "void main() { vec4 c = texSample2D(g_Texture0, v_TexCoord.xy); if (c.r > 0.75) discard; gl_FragColor = c.bgra; }\n";

static void gl_checks (void)
{
    tmpdir = g_dir_make_tmp ("walpe-fx-region-XXXXXX", NULL);
    if (!tmpdir) {
        result (FALSE, "temporary directory", NULL);
        return;
    }
    put_effect ("tint", VS_PLAIN, FS_TINT);
    put_effect ("wob", VS_PLAIN, FS_WOBBLE);
    put_effect ("part", VS_PLAIN, FS_PARTIAL);
    /* a two-pass effect through a half-size target that reads the effect's
     * input again in its second pass, both at the fragment's own place */
    put ("effects/two/effect.json",
         "{\"passes\":[{\"material\":\"materials/effects/two_a.json\",\"target\":\"_rt_half\"},"
         "{\"material\":\"materials/effects/two_b.json\",\"bind\":[{\"name\":\"_rt_half\",\"index\":0},"
         "{\"name\":\"previous\",\"index\":1}]}],\"fbos\":[{\"name\":\"_rt_half\",\"scale\":2,\"format\":\"rgba8888\"}]}");
    put ("materials/effects/two_a.json", "{\"passes\":[{\"shader\":\"effects/two_a\"}]}");
    put ("materials/effects/two_b.json", "{\"passes\":[{\"shader\":\"effects/two_b\"}]}");
    put ("shaders/effects/two_a.vert", VS_PLAIN);
    put ("shaders/effects/two_b.vert", VS_PLAIN);
    put ("shaders/effects/two_a.frag",
         "varying vec4 v_TexCoord;\nuniform sampler2D g_Texture0;\n"
         "void main() { gl_FragColor = texSample2D(g_Texture0, v_TexCoord.xy).gbra; }\n");
    put ("shaders/effects/two_b.frag",
         "varying vec4 v_TexCoord;\nuniform sampler2D g_Texture0;\nuniform sampler2D g_Texture1;\n"
         "void main() { gl_FragColor = mix(texSample2D(g_Texture0, v_TexCoord.xy), texSample2D(g_Texture1, v_TexCoord.xy), 0.375); }\n");
    GLuint in = noise_tex (W, H, 4711, FALSE);
    static const float mid[1][4] = { { 0.30f, 0.35f, 0.62f, 0.71f } };
    static const float three[3][4] = { { 0.05f, 0.05f, 0.30f, 0.20f }, { 0.55f, 0.10f, 0.95f, 0.45f },
                                       { 0.20f, 0.60f, 0.70f, 0.97f } };
    RunInfo ri;
    int stale = 0;
    char *detail = NULL;

    const char *local3 = "[{\"file\":\"effects/tint/effect.json\"},{\"file\":\"effects/tint/effect.json\","
                         "\"passes\":[{\"constantshadervalues\":{\"tint\":\"0.5 1 0.25\"}}]},"
                         "{\"file\":\"effects/tint/effect.json\"}]";
    gboolean ok = same_inside (tmpdir, local3, in, mid, 1, &ri, &stale, &detail);
    result (ok, "three local passes, one rectangle: same bytes inside", detail);
    g_clear_pointer (&detail, g_free);
    result (ri.restricted == 3 && ri.px_drawn * 2 < ri.px_full, "... all three restricted, less than half the pixels shaded", NULL);
    result (stale > W * H / 4, "... and what they leave out is really left out (the poison is still there)", NULL);

    ok = same_inside (tmpdir, local3, in, three, 3, &ri, &stale, &detail);
    result (ok, "three local passes, three rectangles: same bytes inside", detail);
    g_clear_pointer (&detail, g_free);
    result (ri.restricted == 3 && ri.rects == 9, "... three rectangles a pass", NULL);
    {
        /* the filter margin is needed: without it a chain through a
         * half-size target reads texels nobody drew */
        const char *twop0 = "[{\"file\":\"effects/two/effect.json\"},{\"file\":\"effects/two/effect.json\"}]";
        fxr_test_margin = 0.0f;
        ok = same_inside (tmpdir, twop0, in, three, 3, &ri, &stale, &detail);
        result (!ok, "with the filter margin removed on purpose the bytes differ (the check is sensitive)", NULL);
        g_clear_pointer (&detail, g_free);
        fxr_test_margin = 1.0f;
    }

    const char *wob_last = "[{\"file\":\"effects/tint/effect.json\"},{\"file\":\"effects/tint/effect.json\"},"
                           "{\"file\":\"effects/wob/effect.json\"}]";
    ok = same_inside (tmpdir, wob_last, in, mid, 1, &ri, &stale, &detail);
    result (ok, "a pass that reads anywhere at the end: same bytes inside", detail);
    g_clear_pointer (&detail, g_free);
    result (ri.restricted == 1, "... only that pass is restricted, the passes it reads stay whole", NULL);

    const char *wob_mid = "[{\"file\":\"effects/tint/effect.json\"},{\"file\":\"effects/wob/effect.json\"},"
                          "{\"file\":\"effects/tint/effect.json\"},{\"file\":\"effects/tint/effect.json\"}]";
    ok = same_inside (tmpdir, wob_mid, in, mid, 1, &ri, &stale, &detail);
    result (ok, "a pass that reads anywhere in the middle: same bytes inside", detail);
    g_clear_pointer (&detail, g_free);
    result (ri.restricted == 3, "... it and the passes behind it are restricted, the one in front is not", NULL);

    const char *twop = "[{\"file\":\"effects/tint/effect.json\"},{\"file\":\"effects/two/effect.json\"},"
                       "{\"file\":\"effects/tint/effect.json\"}]";
    ok = same_inside (tmpdir, twop, in, three, 3, &ri, &stale, &detail);
    result (ok, "a two-pass effect through a half-size target, reading its input twice: same bytes inside", detail);
    g_clear_pointer (&detail, g_free);
    result (ri.restricted == 4, "... all four passes restricted", NULL);

    const char *partial = "[{\"file\":\"effects/tint/effect.json\"},{\"file\":\"effects/part/effect.json\"},"
                          "{\"file\":\"effects/tint/effect.json\"}]";
    ok = same_inside (tmpdir, partial, in, three, 3, &ri, &stale, &detail);
    result (ok, "a pass that discards (cleared first): same bytes inside", detail);
    g_clear_pointer (&detail, g_free);
    result (ri.restricted == 3, "... restricted, with its clears", NULL);

    /* never restricted: a static set over a static input keeps its output */
    {
        static guint8 a[W * H * 4], b[W * H * 4];
        gpointer set = build_set (tmpdir, local3);
        fx_set_frame_info (W, H, 0.5f, 0.5f, 1.0f / 30);
        fx_set_region_enabled (TRUE);
        fx_region_stats (NULL, NULL, NULL, NULL, TRUE);
        fx_set_need (mid, 1);
        GLuint o1 = fx_run_for_object (set, in, W, H, W, H, 0.0, 1.0f, TRUE);
        guint r1 = 0, r2 = 0;
        fx_region_stats (NULL, NULL, &r1, NULL, TRUE);
        read_tex (o1, W, H, a);
        fx_set_need (three, 3);
        GLuint o2 = fx_run_for_object (set, in, W, H, W, H, 1.0, 1.0f, TRUE);
        fx_region_stats (NULL, NULL, &r2, NULL, TRUE);
        read_tex (o2, W, H, b);
        result (r1 == 0 && r2 == 0 && o1 == o2 && !memcmp (a, b, sizeof (a)),
                "a static set over a static input (its output is kept): drawn whole, served again whole", NULL);
        fx_free_for_object (set);
    }
    /* never restricted: the off switch */
    {
        static guint8 a[W * H * 4];
        gpointer set = build_set (tmpdir, local3);
        fx_set_region_enabled (FALSE);
        run (set, in, 0.5, mid, 1, a, &ri);
        result (ri.restricted == 0 && ri.px_drawn == ri.px_full, "switched off: nothing is restricted", NULL);
        fx_set_region_enabled (TRUE);
        /* a need consumed by one run does not reach the next */
        run (set, in, 0.5, mid, 1, a, &ri);
        guint first = ri.restricted;
        run (set, in, 0.6, NULL, 0, a, &ri);
        result (first == 3 && ri.restricted == 0, "the caller's rectangles are for one run only", NULL);
        fx_free_for_object (set);
    }
    /* the static prefix in front of an animated chain is kept: whole; the
     * chains behind it are restricted */
    {
        static guint8 a[W * H * 4], b[W * H * 4];
        gpointer set = build_set (tmpdir, wob_last);
        fx_set_frame_info (W, H, 0.5f, 0.5f, 1.0f / 30);
        fx_set_region_enabled (FALSE);
        GLuint o = fx_run_for_object (set, in, W, H, W, H, 2.5, 1.0f, TRUE);
        read_tex (o, W, H, a);
        fx_free_for_object (set);
        set = build_set (tmpdir, wob_last);
        fx_set_region_enabled (TRUE);
        fx_region_stats (NULL, NULL, NULL, NULL, TRUE);
        fx_set_need (mid, 1);
        fx_run_for_object (set, in, W, H, W, H, 1.0, 1.0f, TRUE);     /* draws the prefix */
        guint r1 = 0, r2 = 0;
        fx_region_stats (NULL, NULL, &r1, NULL, TRUE);
        poison_pool ();
        fx_set_need (three, 3);
        o = fx_run_for_object (set, in, W, H, W, H, 2.5, 1.0f, TRUE); /* reuses it, other rectangles */
        fx_region_stats (NULL, NULL, &r2, NULL, TRUE);
        read_tex (o, W, H, b);
        result (r1 == 1 && r2 == 1 && diff_inside (a, b, three, 3, NULL) == 0,
                "a static prefix is drawn whole and reused for other rectangles; the animated chain is restricted", NULL);
        fx_free_for_object (set);
    }
    /* the plan is kept with the set and made again when what it was made
     * from changes: the rectangles, a constant, the switch */
    {
        static guint8 a[W * H * 4], b[W * H * 4];
        const char *anim = "[{\"file\":\"effects/tint/effect.json\"},{\"file\":\"effects/wob/effect.json\"},"
                           "{\"file\":\"effects/tint/effect.json\"},{\"file\":\"effects/tint/effect.json\"}]";
        gpointer set = build_set (tmpdir, anim);
        struct FxSet *fs = set;
        RunInfo r1, r2, r3, r4, r5;
        fx_set_region_enabled (TRUE);
        run (set, in, 1.0, mid, 1, a, &r1);
        const guint id1 = fs->rg.id;
        run (set, in, 2.0, mid, 1, a, &r2);
        const guint id2 = fs->rg.id;
        result (fs->rg.valid && fs->rg.ok && id1 && id1 == id2 && r1.restricted == 3 && r2.restricted == 3 &&
                r1.px_drawn == r2.px_drawn, "the same rectangles again: the plan of the last run is used", NULL);
        run (set, in, 2.0, three, 3, a, &r3);
        const guint id3 = fs->rg.id;
        result (id3 != id2 && r3.px_drawn != r2.px_drawn && r3.rects == 9, "other rectangles: a new plan", NULL);
        const float half[3] = { 0.5f, 0.25f, 1.0f };
        fx_set_constant (set, 3, 0, "tint", half, 3);
        poison_pool ();
        run (set, in, 2.0, three, 3, b, &r4);
        const guint id4 = fs->rg.id;
        fx_set_region_enabled (FALSE);
        run (set, in, 2.0, three, 3, a, &r5);
        result (id4 != id3 && r5.restricted == 0 && diff_inside (a, b, three, 3, NULL) == 0,
                "a constant set by a script: a new plan, same bytes inside as with the switch off", NULL);
        fx_set_region_enabled (TRUE);
        poison_pool ();
        run (set, in, 2.0, three, 3, b, &r5);
        result (r5.restricted == 3 && fs->rg.id == id4 && diff_inside (a, b, three, 3, NULL) == 0,
                "switched on again: the kept plan still fits, same bytes inside", NULL);
        fx_free_for_object (set);
    }
    /* kept passes: a pass that reads only a material texture (never the
     * picture, no time) is drawn once; its target is the chain's own */
    {
        static guint8 a[W * H * 4], b[W * H * 4];
        put ("effects/keep/effect.json",
             "{\"passes\":[{\"material\":\"materials/effects/keep_m.json\",\"target\":\"_rt_m\",\"bind\":[{\"name\":\"previous\",\"index\":0}]},"
             "{\"material\":\"materials/effects/keep_c.json\",\"bind\":[{\"name\":\"previous\",\"index\":0},"
             "{\"name\":\"_rt_m\",\"index\":1}]}],\"fbos\":[{\"name\":\"_rt_m\",\"scale\":2,\"format\":\"rgba8888\"}]}");
        put ("materials/effects/keep_m.json", "{\"passes\":[{\"shader\":\"effects/keep_m\",\"textures\":[null,\"masks/km\"]}]}");
        put ("materials/effects/keep_c.json", "{\"passes\":[{\"shader\":\"effects/keep_c\"}]}");
        put_png ("materials/masks/km.png", 64, 64, 4242, FALSE);
        put ("shaders/effects/keep_m.vert", VS_PLAIN);
        put ("shaders/effects/keep_c.vert", VS_PLAIN);
        /* (declares the picture's sampler like WE's "coc" pass and never reads it) */
        put ("shaders/effects/keep_m.frag",
             "varying vec4 v_TexCoord;\nuniform sampler2D g_Texture0;\nuniform sampler2D g_Texture1;\n"
             "uniform float g_Gain; // {\"material\":\"gain\",\"default\":0.75}\n"
             "void main() { gl_FragColor = texSample2D(g_Texture1, v_TexCoord.xy) * g_Gain; }\n");
        put ("shaders/effects/keep_c.frag",
             "varying vec4 v_TexCoord;\nuniform sampler2D g_Texture0;\nuniform sampler2D g_Texture1;\n"
             "void main() { gl_FragColor = mix(texSample2D(g_Texture0, v_TexCoord.xy), texSample2D(g_Texture1, v_TexCoord.xy), 0.5); }\n");
        const char *keep = "[{\"file\":\"effects/wob/effect.json\"},{\"file\":\"effects/keep/effect.json\"},"
                           "{\"file\":\"effects/tint/effect.json\"}]";
        GLuint in2 = noise_tex (W, H, 99, FALSE);
        gpointer set = build_set (tmpdir, keep);
        struct FxSet *fs = set;
        EffectChain *kc = fs && fs->n > 1 ? fs->chains[1] : NULL;
        guint run1 = 0, run2 = 0, run3 = 0, run4 = 0, run5 = 0, c2 = 0;
        gboolean same = TRUE;
        fx_set_region_enabled (FALSE);
        fx_set_static_pass_enabled (TRUE);
        fx_pass_stats (NULL, NULL, TRUE);
        run (set, in, 0.3, NULL, 0, b, &ri);
        fx_pass_stats (&run1, NULL, TRUE);
        result (kc && kc->passes[0].kp_free == 2 && kc->passes[1].kp_free == 1 && kc->targets[0].keep &&
                !kc->targets[0].pool && kc->targets[0].kp_valid,
                "a pass that reads a mask only is recognised; its target is the chain's own", NULL);
        /* later frames, another input picture, the pool overwritten: the
         * pass is not drawn again and the result is the one of a full run */
        const double ts[3] = { 1.1, 2.9, 7.7 };
        for (int f = 0; f < 3; f++) {
            poison_pool ();
            fx_set_static_pass_enabled (TRUE);
            fx_pass_stats (NULL, NULL, TRUE);
            run (set, f == 1 ? in2 : in, ts[f], NULL, 0, b, &ri);
            fx_pass_stats (&run2, &c2, TRUE);
            fx_set_static_pass_enabled (FALSE);
            run (set, f == 1 ? in2 : in, ts[f], NULL, 0, a, &ri);
            fx_pass_stats (&run3, NULL, TRUE);
            same &= !memcmp (a, b, sizeof (a)) && run2 == run1 - 1 && c2 == 1 && run3 == run1;
        }
        result (same, "the kept pass is drawn once: one pass less per frame, the same bytes as with it drawn", NULL);
        /* a constant of the chain changes: drawn again, once */
        const float gain = 0.4f;
        fx_set_static_pass_enabled (TRUE);
        run (set, in, 8.0, NULL, 0, b, &ri);   /* (kept again after the runs with the switch off) */
        fx_set_constant (set, 1, 0, "gain", &gain, 1);
        fx_pass_stats (NULL, NULL, TRUE);
        run (set, in, 9.0, NULL, 0, b, &ri);
        fx_pass_stats (&run4, NULL, TRUE);
        run (set, in, 9.0, NULL, 0, b, &ri);
        fx_pass_stats (&run5, NULL, TRUE);
        fx_set_static_pass_enabled (FALSE);
        run (set, in, 9.0, NULL, 0, a, &ri);
        result (run4 == run1 && run5 == run1 - 1 && !memcmp (a, b, sizeof (a)),
                "a script changes a constant of the kept pass: drawn again once, then kept; same bytes", NULL);
        /* the check is sensitive: with the target overwritten behind the
         * engine's back the picture changes (i.e. the pass really is skipped) */
        fx_set_static_pass_enabled (TRUE);
        run (set, in, 9.0, NULL, 0, b, &ri);
        if (kc) {
            GLint fb = 0;
            glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &fb);
            glBindFramebuffer (GL_FRAMEBUFFER, kc->targets[0].fbo);
            glClearColor (1, 0, 1, 1);
            glClear (GL_COLOR_BUFFER_BIT);
            glBindFramebuffer (GL_FRAMEBUFFER, (GLuint) fb);
        }
        run (set, in, 9.0, NULL, 0, b, &ri);
        result (memcmp (a, b, sizeof (a)) != 0, "... and it is not drawn behind the engine's back (a spoiled target shows)", NULL);
        /* with the region restriction on as well */
        fx_free_for_object (set);
        set = build_set (tmpdir, keep);
        fx_set_region_enabled (TRUE);
        fx_set_static_pass_enabled (TRUE);
        run (set, in, 0.5, three, 3, b, &ri);
        poison_pool ();
        run (set, in, 3.5, three, 3, b, &ri);
        guint rr = ri.restricted;
        fx_set_region_enabled (FALSE);
        fx_set_static_pass_enabled (FALSE);
        run (set, in, 3.5, three, 3, a, &ri);
        result (rr >= 2 && diff_inside (a, b, three, 3, NULL) == 0,
                "kept pass and restricted passes together: same bytes where the result is read", NULL);
        fx_set_region_enabled (TRUE);
        fx_set_static_pass_enabled (TRUE);
        fx_free_for_object (set);
        glDeleteTextures (1, &in2);
    }
    /* a pass that multiplies what it reads by a constant that is exactly
     * zero does not read it: the passes that only fill those targets are
     * not drawn (the depth-of-field effect left on with opacity 0). Own
     * shaders of the same shape; their hash is named as such a pass. */
    {
        static guint8 a[W * H * 4], b[W * H * 4];
        static const float whole[1][4] = { { 0.0f, 0.0f, 1.0f, 1.0f } };
        const char *gate_ej =
            "{\"passes\":[{\"material\":\"materials/effects/gate_a.json\",\"target\":\"_rt_g1\",\"bind\":[{\"name\":\"previous\",\"index\":0}]},"
            "{\"material\":\"materials/effects/gate_b.json\",\"target\":\"_rt_g2\",\"bind\":[{\"name\":\"_rt_g1\",\"index\":0}]},"
            "{\"material\":\"materials/effects/%s.json\",\"bind\":[{\"name\":\"_rt_g2\",\"index\":0},{\"name\":\"previous\",\"index\":1},"
            "{\"name\":\"_rt_g1\",\"index\":2}]}],\"fbos\":[{\"name\":\"_rt_g1\",\"scale\":2,\"format\":\"rgba8888\"},"
            "{\"name\":\"_rt_g2\",\"scale\":2,\"format\":\"rgba8888\"}]}";
        char *ej = g_strdup_printf (gate_ej, "gate_c");
        put ("effects/gate/effect.json", ej);
        g_free (ej);
        ej = g_strdup_printf (gate_ej, "gate_x");
        put ("effects/gatex/effect.json", ej);
        g_free (ej);
        put ("materials/effects/gate_a.json", "{\"passes\":[{\"shader\":\"effects/gate_a\"}]}");
        put ("materials/effects/gate_b.json", "{\"passes\":[{\"shader\":\"effects/gate_b\"}]}");
        put ("materials/effects/gate_c.json", "{\"passes\":[{\"shader\":\"effects/gate_c\"}]}");
        put ("materials/effects/gate_x.json", "{\"passes\":[{\"shader\":\"effects/gate_x\"}]}");
        put ("shaders/effects/gate_a.vert", VS_PLAIN);
        put ("shaders/effects/gate_b.vert", VS_PLAIN);
        put ("shaders/effects/gate_c.vert", VS_PLAIN);
        put ("shaders/effects/gate_x.vert", VS_PLAIN);
        put ("shaders/effects/gate_a.frag",
             "varying vec4 v_TexCoord;\nuniform sampler2D g_Texture0;\n"
             "void main() { gl_FragColor = texSample2D(g_Texture0, v_TexCoord.xy).bgra; }\n");
        put ("shaders/effects/gate_b.frag",
             "varying vec4 v_TexCoord;\nuniform sampler2D g_Texture0;\n"
             "void main() { gl_FragColor = texSample2D(g_Texture0, v_TexCoord.xy).gbra * 0.75; }\n");
#define GATE_FS(extra) \
             "varying vec4 v_TexCoord;\nuniform sampler2D g_Texture0;\nuniform sampler2D g_Texture1;\nuniform sampler2D g_Texture2;\n" \
             "uniform float u_alpha; // {\"material\":\"Opacity\",\"default\":1}\n" \
             "uniform float u_strength; // {\"material\":\"Strength\",\"default\":1}\n" \
             "void main() { vec4 albedo = texSample2D(g_Texture0, v_TexCoord.xy);\n" \
             "  float depth = texSample2D(g_Texture2, v_TexCoord.xy).r;\n" \
             "  vec4 base = texSample2D(g_Texture1, v_TexCoord.xy);\n" \
             "  albedo = mix(base, vec4(mix(base.rgb, albedo.rgb, 1.0 - depth), albedo.a), u_alpha * clamp(log(u_strength * 5.0 + 1.0), 0.0, 1.0));\n" \
             "  gl_FragColor = albedo" extra "; }\n"
        put ("shaders/effects/gate_c.frag", GATE_FS (""));
        /* not such a pass: what it reads on unit 0 shows whatever the opacity is */
        put ("shaders/effects/gate_x.frag", GATE_FS (" + texSample2D(g_Texture0, v_TexCoord.xy) * 0.25"));
        const char *gate = "[{\"file\":\"effects/wob/effect.json\"},{\"file\":\"effects/gate/effect.json\"},"
                           "{\"file\":\"effects/tint/effect.json\"}]";
        const char *gatex = "[{\"file\":\"effects/wob/effect.json\"},{\"file\":\"effects/gatex/effect.json\"},"
                            "{\"file\":\"effects/tint/effect.json\"}]";
        guint64 hc = 0, hx = 0;
        gpointer set = build_set (tmpdir, gate);
        struct FxSet *fs = set;
        if (fs && fs->n > 1 && fs->chains[1] && fs->chains[1]->n_passes == 3)
            hc = fs->chains[1]->passes[2].rg_hash;
        fx_free_for_object (set);
        set = build_set (tmpdir, gatex);
        fs = set;
        if (fs && fs->n > 1 && fs->chains[1] && fs->chains[1]->n_passes == 3)
            hx = fs->chains[1]->passes[2].rg_hash;
        fx_free_for_object (set);
        fxr_test_gate_hash = hc;
        set = build_set (tmpdir, gate);
        fs = set;
        const float zero = 0.0f, some = 0.3f, minus = -1.0f, one = 1.0f;
        guint n1 = 0, n2 = 0, n3 = 0, n4 = 0, n5 = 0;
        fx_set_region_enabled (TRUE);
        fx_pass_stats (NULL, NULL, TRUE);
        run (set, in, 0.4, whole, 1, b, &ri);
        fx_pass_stats (&n1, NULL, TRUE);
        fx_set_constant (set, 1, 2, "Opacity", &zero, 1);
        poison_pool ();
        run (set, in, 1.4, whole, 1, b, &ri);
        fx_pass_stats (&n2, NULL, TRUE);
        fx_set_region_enabled (FALSE);
        run (set, in, 1.4, whole, 1, a, &ri);
        fx_pass_stats (&n3, NULL, TRUE);
        result (hc && fs->chains[1]->passes[2].rg_model == FXR_M_DOF_MIX && n1 == 5 && n2 == 3 && n3 == 5 &&
                !memcmp (a, b, sizeof (a)),
                "opacity exactly 0: the two passes that only feed the mix are not drawn; the same bytes everywhere", NULL);
        fx_set_region_enabled (TRUE);
        fx_set_constant (set, 1, 2, "Opacity", &some, 1);
        poison_pool ();
        run (set, in, 2.4, whole, 1, b, &ri);
        fx_pass_stats (&n4, NULL, TRUE);
        fx_set_region_enabled (FALSE);
        run (set, in, 2.4, whole, 1, a, &ri);
        result (n4 == 5 && !memcmp (a, b, sizeof (a)), "opacity above 0 again: all passes drawn, the same bytes", NULL);
        fx_set_region_enabled (TRUE);
        fx_set_constant (set, 1, 2, "Opacity", &zero, 1);
        fx_set_constant (set, 1, 2, "Strength", &minus, 1);
        fx_pass_stats (NULL, NULL, TRUE);
        run (set, in, 3.4, whole, 1, b, &ri);
        fx_pass_stats (&n5, NULL, TRUE);
        result (n5 == 5, "opacity 0 with a strength whose logarithm is no number: nothing is left out", NULL);
        fx_set_constant (set, 1, 2, "Strength", &one, 1);
        /* the built-in check sees such runs too */
        guint r0 = 0, b0 = 0, r1 = 0, b1 = 0;
        fx_set_region_check (1);
        fx_region_check_stats (&r0, &b0);
        run (set, in, 4.4, whole, 1, b, &ri);
        fx_region_check_stats (&r1, &b1);
        result (r1 == r0 + 1 && b1 == b0, "the built-in check compares a run with passes left out: no difference", NULL);
        fx_free_for_object (set);
        /* ... and catches a shader that is wrongly named as such a pass */
        fxr_test_gate_hash = hx;
        set = build_set (tmpdir, gatex);
        fx_set_constant (set, 1, 2, "Opacity", &zero, 1);
        g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "*fx region check*differ*");
        poison_pool ();
        run (set, in, 4.4, whole, 1, b, &ri);
        g_test_assert_expected_messages ();
        fx_region_check_stats (&r0, &b0);
        result (hx && hx != hc && r0 == r1 + 1 && b0 == b1 + 1,
                "... and reports a pass whose lookups do count (the check is sensitive)", NULL);
        fx_set_region_check (0);
        fxr_test_gate_hash = 0;
        fx_free_for_object (set);
    }
    /* WALPE_FX_REGION_CHECK, the comparison built into the engine: silent
     * on a right plan, loud on a wrong one */
    {
        static guint8 a[W * H * 4];
        const char *twop0 = "[{\"file\":\"effects/two/effect.json\"},{\"file\":\"effects/two/effect.json\"}]";
        gpointer set = build_set (tmpdir, twop0);
        guint runs0 = 0, bad0 = 0, runs1 = 0, bad1 = 0, runs2 = 0, bad2 = 0;
        fx_set_region_enabled (TRUE);
        fx_set_region_check (1);
        fx_region_check_stats (&runs0, &bad0);
        poison_pool ();
        run (set, in, 0.7, three, 3, a, &ri);
        run (set, in, 1.7, mid, 1, a, &ri);
        fx_region_check_stats (&runs1, &bad1);
        result (runs1 == runs0 + 2 && bad1 == bad0, "the built-in check compares restricted runs with whole ones: no difference", NULL);
        fx_free_for_object (set);
        set = build_set (tmpdir, twop0);
        fxr_test_margin = 0.0f;
        g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "*fx region check*differ*");
        poison_pool ();
        run (set, in, 0.7, three, 3, a, &ri);
        g_test_assert_expected_messages ();
        fxr_test_margin = 1.0f;
        fx_region_check_stats (&runs2, &bad2);
        result (runs2 == runs1 + 1 && bad2 == bad1 + 1, "... and it reports a plan that is made too small on purpose", NULL);
        fx_set_region_check (0);
        fx_free_for_object (set);
    }
    /* the layer's colour (the copy in front of the first chain) is only
     * made where the first chain reads it */
    {
        static guint8 a[W * H * 4], b[W * H * 4];
        const char *anim = "[{\"file\":\"effects/tint/effect.json\"},{\"file\":\"effects/part/effect.json\"}]";
        static const float col[3] = { 0.9f, 0.5f, 0.7f };
        gpointer set = build_set (tmpdir, anim);
        RunInfo full, part;
        fx_set_input_tint (set, col);
        fx_set_region_enabled (FALSE);
        run (set, in, 1.5, three, 3, a, &full);
        fx_set_region_enabled (TRUE);
        poison_pool ();
        run (set, in, 1.5, three, 3, b, &part);
        result (full.restricted == 0 && part.restricted == 3 && part.px_full == (guint64) W * H * 3 &&
                diff_inside (a, b, three, 3, NULL) == 0,
                "a coloured layer: the tinted copy and both passes restricted, same bytes inside", NULL);
        fx_free_for_object (set);
    }
    glDeleteTextures (1, &in);
}

/* ---- WE's own shaders ---------------------------------------------------- */

static int model_of (const char *dir, const char *effects_json, guint64 *hash)
{
    gpointer set = build_set (dir, effects_json);
    struct FxSet *s = set;
    int m = -1;
    if (s && s->n > 0 && s->chains[0] && s->chains[0]->n_passes > 0) {
        m = s->chains[0]->passes[0].rg_model;
        *hash = s->chains[0]->passes[0].rg_hash;
    }
    if (set)
        fx_free_for_object (set);
    return m;
}

/* every combination of combos of one shader family, built in project dir
 * `dir` (a package's own copies of the two files, or none = the install's):
 * counts the passes that got the model the family's rule gives, prints the
 * table rows with --print-models */
static void enumerate (const char *dir, const char *kind, int *listed, int *missing)
{
    if (!strcmp (kind, "shake") || !strcmp (kind, "shake_old")) {
        gboolean old = !strcmp (kind, "shake_old");
        for (int v = 0; v < 24; v++) {
            int noise = v & 1, toff = (v >> 1) & 1, mask = (v >> 2) & 1, dirc = v >> 3;
            char *j = g_strdup_printf ("[{\"file\":\"effects/shake/effect.json\",\"passes\":[{\"combos\":{\"NOISE\":%d,"
                                       "\"DIRECTION\":%d,\"TIMEOFFSET\":%d,\"MASK\":%d}}]}]", noise, dirc, toff, mask);
            guint64 h = 0;
            int want = !old ? FXR_M_SHAKE : noise ? FXR_M_SHAKE_OLDN : FXR_M_SHAKE_OLD;
            int m = model_of (dir, j, &h);
            if (print_models)
                printf ("    { 0x%016" G_GINT64_MODIFIER "xull, %s },\n", h,
                        !old ? "FXR_M_SHAKE" : noise ? "FXR_M_SHAKE_OLDN" : "FXR_M_SHAKE_OLD");
            *listed += m == want;
            *missing += m != want;
            g_free (j);
        }
    } else if (!strcmp (kind, "waves") || !strcmp (kind, "waves_old")) {
        gboolean old = !strcmp (kind, "waves_old");
        for (int v = 0; v < 16; v++) {
            int mask = v & 1, toff = (v >> 1) & 1, persp = (v >> 2) & 1, dual = v >> 3;
            if (old && (persp || dual))
                continue;   /* (the old shaders have no DUALWAVES; PERSPECTIVE 1 has no bound) */
            char *j = g_strdup_printf ("[{\"file\":\"effects/waterwaves/effect.json\",\"passes\":[{\"combos\":{\"MASK\":%d,"
                                       "\"TIMEOFFSET\":%d,\"PERSPECTIVE\":%d,\"DUALWAVES\":%d}}]}]", mask, toff, persp, dual);
            guint64 h = 0;
            int want = old ? FXR_M_WAVES_OLD : dual ? FXR_M_WAVES2 : FXR_M_WAVES;
            int m = model_of (dir, j, &h);
            if (print_models)
                printf ("    { 0x%016" G_GINT64_MODIFIER "xull, %s },\n", h,
                        old ? "FXR_M_WAVES_OLD" : dual ? "FXR_M_WAVES2" : "FXR_M_WAVES");
            *listed += m == want;
            *missing += m != want;
            g_free (j);
        }
    } else if (!strcmp (kind, "iris")) {
        for (int v = 0; v < 4; v++) {
            int mask = v & 1, bg = v >> 1;
            char *j = g_strdup_printf ("[{\"file\":\"effects/iris/effect.json\",\"passes\":[{\"combos\":{\"MASK\":%d,"
                                       "\"BACKGROUND\":%d}}]}]", mask, bg);
            guint64 h = 0;
            int m = model_of (dir, j, &h);
            if (print_models)
                printf ("    { 0x%016" G_GINT64_MODIFIER "xull, FXR_M_IRIS },\n", h);
            *listed += m == FXR_M_IRIS;
            *missing += m != FXR_M_IRIS;
            g_free (j);
        }
    }
}

static void we_checks (void)
{
    if (!walpe_we_assets_dir ()) {
        printf ("skip the checks on Wallpaper Engine's own shaders (no assets: WALPE_WE_ASSETS)\n");
        return;
    }
    int listed = 0, missing = 0;
    if (print_models) {
        enumerate (models_dir ? models_dir : tmpdir, models_kind ? models_kind : "shake", &listed, &missing);
        return;
    }
    put_png ("materials/masks/m1.png", 64, 64, 77, TRUE);
    put_png ("materials/masks/m2.png", 64, 64, 1234, FALSE);
    put_png ("materials/masks/m3.png", 32, 32, 999, TRUE);
    /* the install's own versions of the three shaders, every combination */
    enumerate (tmpdir, "shake", &listed, &missing);
    enumerate (tmpdir, "waves", &listed, &missing);
    enumerate (tmpdir, "iris", &listed, &missing);
    if (print_models)
        return;
    if (missing) {
        notes++;
        printf ("note  %d of %d shader variants of this Wallpaper Engine install are not in fx_region_models.h "
                "(another version of its shake / waterwaves / iris shaders): passes with them are not restricted\n",
                missing, listed + missing);
    } else {
        result (TRUE, "every combination of the install's shake, waterwaves and iris shaders is recognised", NULL);
    }
    {
        guint64 h = 0;
        result (model_of (tmpdir, "[{\"file\":\"effects/shake/effect.json\",\"passes\":[{\"combos\":{\"AUDIOPROCESSING\":1}}]}]", &h) ==
                FXR_M_NONE, "shake with audio response is not in the table (its reach is not bounded there)", NULL);
        result (model_of (tmpdir, "[{\"file\":\"effects/waterripple/effect.json\"}]", &h) == FXR_M_NONE,
                "a displacement shader nobody worked through has no model", NULL);
    }
    if (!listed)
        return;
    GLuint in = noise_tex (W, H, 31337, FALSE);
    static const float mid[1][4] = { { 0.40f, 0.40f, 0.60f, 0.62f } };
    static const float three[3][4] = { { 0.08f, 0.06f, 0.30f, 0.22f }, { 0.60f, 0.12f, 0.93f, 0.40f },
                                       { 0.25f, 0.62f, 0.66f, 0.95f } };
    RunInfo ri;
    int stale = 0;
    char *detail = NULL;
    /* strong settings: the lookups go as far as the shaders let them. A
     * binary flow / opacity mask gives the extremes of the mask factor. */
    const char *shakes =
        "[{\"file\":\"effects/shake/effect.json\",\"passes\":[{\"textures\":[null,\"masks/m1\"],"
        "\"constantshadervalues\":{\"strength\":0.3,\"speed\":7,\"bounds\":\"0 1\",\"friction\":\"1 1\"}}]},"
        "{\"file\":\"effects/shake/effect.json\",\"passes\":[{\"combos\":{\"DIRECTION\":1,\"NOISE\":1},"
        "\"textures\":[null,\"masks/m2\",null,\"masks/m3\"],"
        "\"constantshadervalues\":{\"strength\":0.25,\"speed\":3.3,\"bounds\":\"0.1 0.6\",\"friction\":\"0.3 2.5\"}}]},"
        "{\"file\":\"effects/shake/effect.json\",\"passes\":[{\"combos\":{\"DIRECTION\":2},\"textures\":[null,\"masks/m3\",\"masks/m2\"],"
        "\"constantshadervalues\":{\"strength\":0.2,\"speed\":1.7}}]}]";
    gboolean ok = same_inside (tmpdir, shakes, in, mid, 1, &ri, &stale, &detail);
    result (ok, "three shakes (all directions, noise, time offset and opacity maps), strong: same bytes inside", detail);
    g_clear_pointer (&detail, g_free);
    result (ri.restricted == 3 && ri.px_drawn < ri.px_full, "... all three restricted", NULL);
    /* the comparison must see a bound that is too small */
    fxr_test_reach = 0.25f;
    ok = same_inside (tmpdir, shakes, in, mid, 1, &ri, &stale, &detail);
    result (!ok, "... and with the bounds cut to a quarter on purpose the bytes differ (the check is sensitive)", NULL);
    g_clear_pointer (&detail, g_free);
    fxr_test_reach = 1.0f;
    const char *waves =
        "[{\"file\":\"effects/waterwaves/effect.json\",\"passes\":[{\"textures\":[null,\"masks/m1\"],"
        "\"constantshadervalues\":{\"strength\":0.35,\"speed\":9,\"scale\":60,\"direction\":0.7,\"exponent\":0.6}}]},"
        "{\"file\":\"effects/waterwaves/effect.json\",\"passes\":[{\"combos\":{\"DUALWAVES\":1},"
        "\"constantshadervalues\":{\"strength\":0.3,\"speed\":4,\"scale\":25,\"direction\":2.2,\"speed2\":7,\"scale2\":90,"
        "\"direction2\":-1.0,\"exponent2\":2.0}}]},"
        "{\"file\":\"effects/pulse/effect.json\"}]";
    ok = same_inside (tmpdir, waves, in, three, 3, &ri, &stale, &detail);
    result (ok, "water waves (masked, dual) and a pulse, strong: same bytes inside", detail);
    g_clear_pointer (&detail, g_free);
    result (ri.restricted == 3, "... all three restricted", NULL);
    const char *iris =
        "[{\"file\":\"effects/iris/effect.json\",\"passes\":[{\"textures\":[null,\"masks/m1\"],"
        "\"constantshadervalues\":{\"scale\":\"9 7\",\"speed\":1.9,\"noiseamount\":2.0,\"rough\":0.9}}]},"
        "{\"file\":\"effects/iris/effect.json\",\"passes\":[{\"constantshadervalues\":{\"scale\":\"10 10\",\"noiseamount\":1.5}}]},"
        "{\"file\":\"effects/pulse/effect.json\",\"passes\":[{\"textures\":[null,null,\"masks/m3\"]}]}]";
    ok = same_inside (tmpdir, iris, in, three, 3, &ri, &stale, &detail);
    result (ok, "iris (masked and not) and a masked pulse, strong: same bytes inside", detail);
    g_clear_pointer (&detail, g_free);
    result (ri.restricted == 3, "... all three restricted", NULL);
    /* the main test scene (2901582518) in small: the chain of its character layer */
    const char *arona =
        "[{\"file\":\"effects/pulse/effect.json\",\"passes\":[{\"textures\":[null,null,\"masks/m1\"]}]},"
        "{\"file\":\"effects/shake/effect.json\",\"passes\":[{\"textures\":[null,\"masks/m2\"],\"constantshadervalues\":{\"strength\":0.05,\"speed\":10}}]},"
        "{\"file\":\"effects/shine/effect.json\"},"
        "{\"file\":\"effects/iris/effect.json\",\"passes\":[{\"textures\":[null,\"masks/m3\"],\"constantshadervalues\":{\"scale\":\"1.166 1.166\"}}]},"
        "{\"file\":\"effects/shake/effect.json\",\"passes\":[{\"combos\":{\"DIRECTION\":1},\"textures\":[null,\"masks/m1\",null,\"masks/m3\"],"
        "\"constantshadervalues\":{\"bounds\":\"0.99 0.997\",\"strength\":0.23}}]},"
        "{\"file\":\"effects/pulse/effect.json\",\"passes\":[{\"textures\":[null,null,\"masks/m2\"]}]},"
        "{\"file\":\"effects/waterwaves/effect.json\",\"passes\":[{\"textures\":[null,\"masks/m1\"],\"constantshadervalues\":{\"strength\":0.1,\"scale\":25,\"speed\":3}}]},"
        "{\"file\":\"effects/shake/effect.json\",\"passes\":[{\"combos\":{\"DIRECTION\":1},\"textures\":[null,\"masks/m2\"],"
        "\"constantshadervalues\":{\"friction\":\"1.2 1.2\",\"strength\":0.12}}]}]";
    ok = same_inside (tmpdir, arona, in, three, 3, &ri, &stale, &detail);
    result (ok, "pulse, shake, shine (5 passes), iris, shake, pulse, water waves, shake: same bytes inside", detail);
    g_clear_pointer (&detail, g_free);
    result (ri.restricted >= 6, "... the passes from the shine's last one on are restricted", NULL);
    /* painted maps: the displacement is full where the map is strong and
     * (next to) nothing elsewhere - what the passes in front have to draw
     * follows the paint. flow.png: "no flow" (127, 127) with one patch of
     * strong flow and a soft rim; spot.png: black with one white patch. */
    put_patch_png ("materials/masks/flow.png", 64, 64, 127, 127, 255, 0, 0.25f, 0.30f, 0.50f, 0.60f);
    put_patch_png ("materials/masks/spot.png", 64, 64, 0, 0, 255, 255, 0.55f, 0.20f, 0.80f, 0.50f);
    {
        static const float away[1][4] = { { 0.62f, 0.66f, 0.90f, 0.95f } };
        const char *painted =
            "[{\"file\":\"effects/pulse/effect.json\"},"
            "{\"file\":\"effects/shake/effect.json\",\"passes\":[{\"textures\":[null,\"masks/flow\"],"
            "\"constantshadervalues\":{\"strength\":0.3,\"speed\":7}}]},"
            "{\"file\":\"effects/pulse/effect.json\"}]";
        const char *noisy =
            "[{\"file\":\"effects/pulse/effect.json\"},"
            "{\"file\":\"effects/shake/effect.json\",\"passes\":[{\"textures\":[null,\"masks/m1\"],"
            "\"constantshadervalues\":{\"strength\":0.3,\"speed\":7}}]},"
            "{\"file\":\"effects/pulse/effect.json\"}]";
        RunInfo rp, rn, re;
        ok = same_inside (tmpdir, painted, in, away, 1, &rp, &stale, &detail);
        result (ok, "a shake with a painted flow map, read away from the paint: same bytes inside", detail);
        g_clear_pointer (&detail, g_free);
        ok = same_inside (tmpdir, noisy, in, away, 1, &rn, &stale, &detail);
        result (ok, "the same with flow painted everywhere: same bytes inside", detail);
        g_clear_pointer (&detail, g_free);
        result (rp.restricted == 3 && rn.restricted == 3 && rp.px_drawn * 10 < rn.px_drawn * 9,
                "... and the painted map lets the pass in front draw less (the reach only counts where there is flow)", NULL);
        ok = same_inside (tmpdir, painted, in, mid, 1, &re, &stale, &detail);
        result (ok, "read across the rim of the paint: same bytes inside", detail);
        g_clear_pointer (&detail, g_free);
        result (re.restricted == 3, "... all three restricted", NULL);
        fxr_test_no_strong = TRUE;
        ok = same_inside (tmpdir, painted, in, mid, 1, &re, &stale, &detail);
        result (!ok, "... and with the paint ignored on purpose the bytes differ (the check is sensitive)", NULL);
        g_clear_pointer (&detail, g_free);
        fxr_test_no_strong = FALSE;
        const char *spots =
            "[{\"file\":\"effects/pulse/effect.json\"},"
            "{\"file\":\"effects/waterwaves/effect.json\",\"passes\":[{\"textures\":[null,\"masks/spot\"],"
            "\"constantshadervalues\":{\"strength\":0.35,\"speed\":9,\"scale\":60,\"direction\":0.7}}]},"
            "{\"file\":\"effects/iris/effect.json\",\"passes\":[{\"textures\":[null,\"masks/spot\"],"
            "\"constantshadervalues\":{\"scale\":\"9 7\",\"speed\":1.9,\"noiseamount\":2.0,\"rough\":0.9}}]},"
            "{\"file\":\"effects/pulse/effect.json\"}]";
        ok = same_inside (tmpdir, spots, in, three, 3, &re, &stale, &detail);
        result (ok, "water waves and iris with a painted opacity map: same bytes inside", detail);
        g_clear_pointer (&detail, g_free);
        result (re.restricted == 4, "... all four restricted", NULL);
        fxr_test_no_strong = TRUE;
        ok = same_inside (tmpdir, spots, in, three, 3, &re, &stale, &detail);
        result (!ok, "... and with the paint ignored on purpose the bytes differ", NULL);
        g_clear_pointer (&detail, g_free);
        fxr_test_no_strong = FALSE;
    }
    /* a script changes a constant: the bound follows at the next run */
    {
        static guint8 a[W * H * 4], b[W * H * 4];
        gpointer set = build_set (tmpdir, shakes);
        RunInfo r1, r2;
        fx_set_region_enabled (TRUE);
        run (set, in, 1.0, mid, 1, a, &r1);
        const float big = 0.5f, nanv = NAN;
        fx_set_constant (set, 2, 0, "strength", &big, 1);
        run (set, in, 1.0, mid, 1, b, &r2);
        fx_set_region_enabled (FALSE);
        run (set, in, 1.0, mid, 1, a, &ri);
        result (r2.px_drawn > r1.px_drawn && diff_inside (a, b, mid, 1, NULL) == 0,
                "a script raises a shake's strength: more is drawn in front of it, same bytes inside", NULL);
        fx_set_region_enabled (TRUE);
        fx_set_constant (set, 2, 0, "strength", &nanv, 1);
        run (set, in, 1.0, mid, 1, b, &r2);
        result (r2.restricted == 1, "a constant that is no number: the passes in front are drawn whole", NULL);
        fx_free_for_object (set);
    }
    glDeleteTextures (1, &in);
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
    if (!print_models)
        printf ("GL: %s / %s\n", glGetString (GL_RENDERER), glGetString (GL_VERSION));
    walpe_glutil_init ();
    if (!print_models)
        gl_checks ();
    else
        tmpdir = g_dir_make_tmp ("walpe-fx-region-XXXXXX", NULL);
    if (tmpdir)
        we_checks ();
    fx_targets_release ();
    if (tmpdir) {
        rm_rf (tmpdir);
        g_clear_pointer (&tmpdir, g_free);
    }
    eglMakeCurrent (dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext (dpy, ctx);
    eglTerminate (dpy);
    return TRUE;
}

int main (int argc, char **argv)
{
    print_models = argc > 1 && !strcmp (argv[1], "--print-models");
    if (print_models && argc > 3) {
        models_dir = argv[2];    /* a project dir with its own shaders/effects/<name>.frag|vert */
        models_kind = argv[3];   /* shake | shake_old | waves | waves_old | iris */
    }
    /* the checks set every mode themselves */
    g_unsetenv ("WALPE_FX_SHARE");
    g_unsetenv ("WALPE_FX_SPEC");
    g_unsetenv ("WALPE_FX_QUALITY");
    g_unsetenv ("WALPE_FX_REGION");
    g_unsetenv ("WALPE_FX_REGION_RECTS");
    g_unsetenv ("WALPE_FX_REGION_CHECK");
    g_unsetenv ("WALPE_FX_REGION_MINPX");
    g_unsetenv ("WALPE_FX_STATIC_PASS");
    g_unsetenv ("WALPE_FX_CACHE");
    if (!print_models)
        source_checks ();
    int ran = 0;
    if (epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_EXT_device_enumeration") &&
        epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_EXT_platform_device")) {
        EGLDeviceEXT dev[8];
        EGLint nd = 0;
        if (eglQueryDevicesEXT (8, dev, &nd))
            for (EGLint i = 0; i < nd && !(print_models && ran); i++)
                ran += gl_run (eglGetPlatformDisplayEXT (EGL_PLATFORM_DEVICE_EXT, dev[i], NULL));
    }
    if (!ran && epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_MESA_platform_surfaceless"))
        ran += gl_run (eglGetPlatformDisplayEXT (EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL));
    if (print_models)
        return 0;
    if (!ran)
        printf ("skip GL checks (no EGL device / surfaceless platform)\n");
    printf ("%s (%d check(s), %d failure(s), %d note(s))\n", fails ? "FAILED" : "PASSED", checks, fails, notes);
    return fails ? 1 : 0;
}
