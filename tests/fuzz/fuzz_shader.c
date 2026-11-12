/* fuzz_shader - the text stages an effect shader goes through before a
 * compiler sees it (src/engine/scenenode_effects.c and the fx_*.h it
 * includes): reading the file, #include expansion, the [COMBO] and uniform
 * annotations in comments, the built source, the source repairs, the time
 * scan, the scans of the specialiser and of the fusing. The stages are the
 * ones of tests/fx_hostile_test.c (text_pipeline); no OpenGL.
 * The input is a fragment shader; a line "//@vertex" starts a vertex
 * shader of its own (else a plain one is used). */
#include "../../src/engine/scenenode_effects.c"
#include "fuzz_common.h"
#include <glib/gstdio.h>

static const char *dir;
static char *sdir;

static const char plain_vertex[] =
    "uniform mat4 g_ModelViewProjectionMatrix;\nattribute vec3 a_Position;\nattribute vec2 a_TexCoord;\n"
    "varying vec4 v_TexCoord;\nvoid main() {\n"
    "\tgl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);\n"
    "\tv_TexCoord = a_TexCoord.xyxy;\n}\n";

int LLVMFuzzerInitialize (int *argc, char ***argv)
{
    (void) argc; (void) argv;
    fuzz_quiet ();
    dir = fuzz_dir ("shader");
    sdir = g_build_filename (dir, "shaders", NULL);
    g_mkdir_with_parents (sdir, 0700);
    char *h = g_build_filename (sdir, "common.h", NULL);
    g_file_set_contents (h, "#define M_PI 3.14159265359\n#include \"common_blending.h\"\n"
                            "float greyscale(vec3 c) { return dot(c, vec3(0.3, 0.6, 0.1)); }\n", -1, NULL);
    g_free (h);
    h = g_build_filename (sdir, "common_blending.h", NULL);
    g_file_set_contents (h, "vec3 ApplyBlending(const int m, in vec3 a, in vec3 b, float o) { return mix(a, b, o); }\n",
                         -1, NULL);
    g_free (h);
    return 0;
}

static char *expand (const char *body)
{
    GString *o = g_string_new (NULL);
    GHashTable *seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    fx_expand_into (o, body, sdir, dir, seen, 0);
    g_hash_table_unref (seen);
    return g_string_free (o, FALSE);
}

int LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
    /* as fx_build_pass reads a shader file: bounded, made UTF-8, a string */
    char *fraw = fx_read_shader (fuzz_file (0, data, size));
    if (!fraw)
        return 0;
    char *vraw = NULL, *cut = strstr (fraw, "//@vertex");
    if (cut) {
        *cut = 0;
        vraw = cut + strlen ("//@vertex");
    }
    char *body[2];
    for (int k = 0; k < 2; k++) {
        char *h = fx_hoist_decls (k ? fraw : vraw ? vraw : plain_vertex);
        body[k] = expand (h);
        g_free (h);
    }
    GHashTable *combos = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    fx_collect_combo_defaults (body[0], combos);
    fx_collect_combo_defaults (body[1], combos);
    for (int u = 0; u < FX_MAX_BINDS; u++) {
        g_free (fx_sampler_default (body[1], u));
        char *c = fx_sampler_combo (body[1], u);
        if (c)
            g_hash_table_insert (combos, c, GINT_TO_POINTER (1));
    }
    char *vsrc = fx_build_source (body[0], combos, TRUE);
    char *fsrc = fx_build_source (body[1], combos, FALSE);
    g_hash_table_unref (combos);
    WalpeFxTime t = { 0 };
    walpe_fx_time_scan (body[1], NULL, NULL, &t);
    walpe_fx_time_scan (body[0], NULL, NULL, &t);
    fx_fragment_may_discard (body[1]);
    static const char *const keys[] = { "material", "default", "combo" };
    static const char *const names[] = { "g_Speed", "g_Texture1", "g_Tint", "g_UserAlpha", "g_Bounds" };
    for (guint i = 0; i < G_N_ELEMENTS (names); i++)
        for (guint k = 0; k < G_N_ELEMENTS (keys); k++) {
            g_free (fx_shader_anno_t (body[1], names[i], keys[k], NULL));
            g_free (fx_uniform_anno (body[1], body[0], names[i], keys[k], GL_FLOAT));
        }
    fx_prepass (&vsrc, &fsrc, "fuzz");
    g_free (fxs_active_lines (fsrc));
    g_free (fxs_scan_text (fsrc));
    g_free (fx_active_code (vsrc));
    guint *map = NULL;
    g_free (fxs_active_map (fsrc, 0, &map));
    g_free (map);
    for (int k = 0; k < 2; k++) {
        char *r = fxf_resolve (k ? fsrc : vsrc, !k);
        if (r) {
            GArray *g = fxf_globals (r);
            const char *hd = NULL, *bd = NULL, *en = NULL;
            fxf_main_span (r, &hd, &bd, &en);
            if (k)
                g_free (fxf_warp_function (r, g, 0));
            g_array_unref (g);
            g_free (r);
        }
    }
    g_free (fxs_cost_count_source (fsrc));
    gint64 st = 0;
    fx_expansion_sizes (fsrc, &st);
    g_free (vsrc);
    g_free (fsrc);
    g_free (body[0]);
    g_free (body[1]);
    g_free (fraw);
    return 0;
}
