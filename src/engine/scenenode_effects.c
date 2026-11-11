/* scenenode_effects.c - Tier 2: WE effect pass-graph executor.
 *
 * Runs effect.json pass chains shipped in pkgs / the WE install. Each
 * pass = a material (shader + combos + textures) rendering into a named
 * FBO target; textures bind by name ("previous" = input texture, target
 * names = earlier FBOs). Uniforms: instance constantshadervalues first
 * (keyed by the shader annotation's "material" name), then the shader's
 * own // {"default":...} annotations; g_Texture0-3 bound from slots +
 * pass binds; g_TextureNResolution from the bound texture dims; g_Time
 * from a content-local clock; g_ModelViewProjectionMatrix = identity
 * (passes run in texture space; the caller composites with the object
 * transform).
 *
 * WE shaders are GLSL-120-era source (varying/attribute/texture2D/
 * gl_FragColor + texSample2D) — compiled through a GL3 compatibility
 * shim. Audio-reactive uniforms receive 0 (silent renderer). */
#include "scenenode.h"
#include "scenenode_effects.h"
#include "propprobe.h"
#include "glutil.h"
#include "clip.h"
#include "gpuprof.h"   /* GPUPROF hooks: WALPE_PROF_PASS_* around the pass draws */
#include "fxclass.h"
#include "fx_keys.h"   /* statements of stock shaders, recognised by key */
#include "common/quality.h"
#include "common/tex.h"
#include "common/util.h"
#include "common/premult.h"
#include <json-glib/json-glib.h>
#include <string.h>
#include <sys/stat.h>
#include <glib/gstdio.h>
#include <math.h>
#include <stdlib.h>
#include <time.h>

#define FX_MAX_PASSES 32
/* g_Time as the shaders get it (fx_shader_time): see there */
#define FX_TIME_HARD_S 16384.0   /* never beyond: float steps of 0.98 ms up to here */
#define FX_TIME_SOFT_S 4200.0    /* folded back at the next unseen moment from here on */
#define FX_TIME_LO_S 600.0       /* a fold lands in [LO, LO + PERIOD) */
#define FX_TIME_PERIOD_S 3600.0
#define FX_TIME_GAP_US 500000    /* no frame for this long: the fold is not seen */
#define FX_MAX_TARGETS 16
#define FX_MAX_BINDS 8
/* render-target slot references resolved at run time */
enum { FX_RT_NONE, FX_RT_INPUT, FX_RT_FULLFRAME };
/* WE system user textures (media integration album art) */
enum { FX_SYS_NONE, FX_SYS_THUMB, FX_SYS_PREV };

/* ----------------------------------------------------------- resolution */

/* WE resolves effect files through layered roots: the project (pkg), the
 * builtin effect's own dir (assets/effects/<name>/ keeps materials/ and
 * shaders/ of that effect), then the install's assets/. The effect root is
 * set while one chain is being built (same thread, synchronous). */
static __thread const char *fx_effroot;

/* refs come from the wallpaper's json: each one is looked up below a root
 * and never outside it (no absolute path, no ".."), regular files only */
static char *fx_try (const char *root, const char *ref)
{
    return walpe_asset_path (root, ref);
}

static char *fx_resolve (const char *dir, const char *ref)
{
    if (!ref || !*ref)
        return NULL;
    char *r = fx_try (dir, ref);
    if (!r) {
        char *b = g_build_filename (dir, "assets", NULL);
        r = fx_try (b, ref);
        g_free (b);
    }
    if (!r)
        r = fx_try (fx_effroot, ref);
    if (!r)
        r = fx_try (walpe_we_assets_dir (), ref); /* builtin effects */
    return r;
}

/* resolve a texture reference the way WE does: project root, project
 * materials/ (refs like "effects/waterripplenormal" and "masks/x" live
 * there), the builtin effect's dir, then the WE install. Builtin effect
 * textures may ship only as the editor's source .png (+ .tex-json). */
static char *fx_tex_path (const char *dir, const char *ref)
{
    static const char *fmt[] = { "%s", "%s.tex", "materials/%s.tex", "materials/%s",
                                 "materials/%s.png", "%s.png" };
    const char *roots[3] = { dir, fx_effroot, walpe_we_assets_dir () };
    for (int d = 0; d < 3; d++) {
        if (!roots[d])
            continue;
        for (guint f = 0; f < G_N_ELEMENTS (fmt); f++) {
            char *rel = g_strdup_printf (fmt[f], ref);
            char *p = walpe_asset_path (roots[d], rel);
            g_free (rel);
            if (p)
                return p;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------ shader src */

/* Shader text comes with the wallpaper. Wallpaper Engine's own shaders are
 * a few KiB, the largest Workshop ones some tens of KiB: one file beyond
 * FX_MAX_SHADER_FILE is not read, and one shader with its includes stops
 * growing at FX_MAX_SOURCE (the includes behind that stay as written and
 * the compile log names them; the effect fails like any other that does
 * not compile). Before, a header that included itself under many
 * spellings of its name, or a few large headers, made one source of
 * gigabytes - built, copied and scanned on the render thread. */
#define FX_MAX_SHADER_FILE (1024 * 1024)
#define FX_MAX_SOURCE (4 * 1024 * 1024)

/* a float from a wallpaper's JSON as an int: saturating (the plain cast
 * is undefined beyond the int range and for NaN) */
static inline int fx_f2i (double v)
{
    return v >= 2147483520.0 ? 2147483520 : v <= -2147483520.0 ? -2147483520 : v == v ? (int) v : 0;
}

static char *fx_read_shader (const char *path)
{
    gsize len = 0;
    char *data = NULL;
    GStatBuf st;
    if (g_stat (path, &st) != 0 || !S_ISREG (st.st_mode))
        return NULL;
    if (st.st_size > FX_MAX_SHADER_FILE) {
        g_warning ("fx: shader file %s has %" G_GINT64_FORMAT " bytes (limit %d): not read", path,
                   (gint64) st.st_size, FX_MAX_SHADER_FILE);
        return NULL;
    }
    if (!g_file_get_contents (path, &data, &len, NULL))
        return NULL;
    if (len > FX_MAX_SHADER_FILE) {     /* grew since the stat */
        g_free (data);
        return NULL;
    }
    /* GLSL is ASCII, but comments come in whatever code page the author
     * used (Latin-1, GBK). GRegex, which the source repairs use, refuses
     * text that is not UTF-8 and returns NULL (a crash in fx_uint_to_int).
     * Such bytes become '?': same length, line and column numbers stay. */
    const char *bad = NULL;
    for (const char *p = data; !g_utf8_validate (p, -1, &bad); p = bad + 1)
        data[bad - data] = '?';
    return data;
}

/* first path an #include name resolves to: the including shader's dir,
 * the project's shaders/, the builtin effect's shaders/, the install's
 * shaders/ (common*.h live there), the install root */
static char *fx_find_include (const char *inc, const char *shader_dir, const char *dir)
{
    const char *we = walpe_we_assets_dir ();
    char *cands[5] = {
        g_build_filename (shader_dir, inc, NULL),
        g_build_filename (dir, "shaders", inc, NULL),
        fx_effroot ? g_build_filename (fx_effroot, "shaders", inc, NULL) : NULL,
        we ? g_build_filename (we, "shaders", inc, NULL) : NULL,
        we ? g_build_filename (we, inc, NULL) : NULL,
    };
    char *hit = NULL;
    for (int i = 0; i < 5; i++) {
        /* "../x.h" may climb, but only inside the wallpaper, the builtin
         * effect or the install: a shader cannot pull in any other file */
        if (!hit && cands[i] && g_file_test (cands[i], G_FILE_TEST_IS_REGULAR) &&
            (walpe_path_is_inside (cands[i], dir) ||
             (fx_effroot && walpe_path_is_inside (cands[i], fx_effroot)) ||
             (we && walpe_path_is_inside (cands[i], we))))
            hit = g_strdup (cands[i]);
        g_free (cands[i]);
    }
    return hit;
}

/* recursive #include expander. Include-once per shader (WE headers have no
 * guards and include each other: common_composite.h pulls common.h and
 * common_blending.h again). Unresolved includes stay as written so the
 * compile log names them. */
static void fx_expand_into (GString *out, const char *body, const char *shader_dir,
                            const char *dir, GHashTable *seen, int depth)
{
    const char *p = body;
    while (*p) {
        const char *line_end = strchr (p, '\n');
        gsize llen = line_end ? (gsize) (line_end - p) + 1 : strlen (p);
        const char *s = p;
        while (s < p + llen && (*s == ' ' || *s == '\t'))
            s++;
        if (!strncmp (s, "#require", 8) && (s[8] == ' ' || s[8] == '\t')) {
            /* "#require LightingV1": Wallpaper Engine's request for an
             * engine code module (the builtin fluid simulation, the *4
             * model shaders), no GLSL directive - the compiler stops at
             * it. Keep the line as a comment (line numbers stay); what
             * the module would define is only used under combos that
             * are off unless a scene turns its lighting on. */
            g_string_append (out, "//");
            g_string_append_len (out, p, llen);
            p += llen;
            continue;
        }
        if (depth < 8 && out->len < FX_MAX_SOURCE && !strncmp (s, "#include", 8)) {
            const char *q1 = memchr (s + 8, '"', (gsize) (p + llen - s - 8));
            const char *q2 = q1 ? memchr (q1 + 1, '"', (gsize) (p + llen - q1 - 1)) : NULL;
            if (q1 && q2) {
                char *inc = g_strndup (q1 + 1, (gsize) (q2 - q1 - 1));
                char *path = fx_find_include (inc, shader_dir, dir);
                g_free (inc);
                if (path) {
                    /* once per file, whatever the include line calls it
                     * ("x.h", "./x.h", ".//x.h", "a/../x.h", a link) */
                    char *real = realpath (path, NULL);
                    if (real) {
                        g_free (path);
                        path = g_strdup (real);
                        free (real);
                    }
                }
                /* a file that is in already is not read again */
                char *content = path && !g_hash_table_contains (seen, path) ? fx_read_shader (path)
                                                                            : path ? g_strdup ("") : NULL;
                if (content) {
                    if (!g_hash_table_contains (seen, path)) {
                        g_hash_table_add (seen, g_strdup (path));
                        fx_expand_into (out, content, shader_dir, dir, seen, depth + 1);
                        g_string_append_c (out, '\n');
                    }
                    g_free (content);
                    g_free (path);
                    p += llen;
                    continue;
                }
                g_free (path);
            }
        }
        g_string_append_len (out, p, llen);
        p += llen;
    }
}

/* ---- WE dialect -> GLSL 330: declaration order + duplicate declarations */

static gboolean fx_line_is (const char *s, gsize n, const char *kw)
{
    gsize k = strlen (kw);
    while (n && (*s == ' ' || *s == '\t'))
        s++, n--;
    return n > k && !strncmp (s, kw, k) && (s[k] == ' ' || s[k] == '\t');
}

static gboolean fx_line_is_decl (const char *s, gsize n)
{
    return fx_line_is (s, n, "uniform") || fx_line_is (s, n, "varying") ||
           fx_line_is (s, n, "attribute");
}

static const char *fx_skip_ws (const char *s, const char *e)
{
    while (s < e && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n'))
        s++;
    return s;
}

/* WE compiles the whole translation unit with declarations visible to the
 * included headers (common_blur.h samples g_Texture0, which the effect
 * declares AFTER its #include). GLSL needs declare-before-use: move the
 * main file's top-level uniform/varying/attribute declarations that follow
 * its first #include up in front of it. Declarations inside an #if block
 * move with their whole block when the block holds nothing else and does
 * not test macros the headers define (FORMAT_*). Only declarations before
 * the first function definition are considered. */
static char *fx_hoist_decls (const char *body)
{
    const char *first_inc = NULL;
    GString *moved = g_string_new (NULL), *rest = g_string_new (NULL);
    const char *p = body;
    int pp = 0, brace = 0;
    gboolean in_func = FALSE;
    const char *blk_start = NULL;     /* depth-0 #if block being collected */
    gboolean blk_pure = TRUE;
    while (*p) {
        const char *le = strchr (p, '\n');
        gsize n = le ? (gsize) (le - p) + 1 : strlen (p);
        const char *s = fx_skip_ws (p, p + n);
        gboolean is_pp = s < p + n && *s == '#';
        if (!first_inc && is_pp && !strncmp (s, "#include", 8) && pp == 0)
            first_inc = p;
        if (!in_func && brace == 0 && !is_pp) {
            /* a '(' on a non-declaration line at file scope starts the
             * first function: nothing after it is hoisted */
            if (memchr (p, '(', n) && !fx_line_is_decl (p, n) &&
                !(s + 2 < p + n && s[0] == '/' && s[1] == '/'))
                in_func = TRUE;
        }
        for (gsize i = 0; i < n; i++) {
            if (p[i] == '/' && i + 1 < n && p[i + 1] == '/')
                break;
            if (p[i] == '{') brace++;
            else if (p[i] == '}') brace--;
        }
        gboolean eligible = first_inc && !in_func && brace == 0;
        if (is_pp && (!strncmp (s, "#if", 3))) {
            if (pp == 0 && eligible) {
                blk_start = p;
                blk_pure = TRUE;
            }
            pp++;
        } else if (is_pp && !strncmp (s, "#endif", 6)) {
            pp--;
            if (pp == 0 && blk_start) {
                gsize bl = (gsize) (p + n - blk_start);
                char *blk = g_strndup (blk_start, bl);
                if (blk_pure && !strstr (blk, "FORMAT_"))
                    g_string_append (moved, blk);
                else
                    g_string_append (rest, blk);
                g_free (blk);
                blk_start = NULL;
                p += n;
                continue;
            }
        } else if (blk_start && !is_pp) {
            const char *e = p + n;
            gboolean blank = fx_skip_ws (p, e) == e ||
                             (s + 1 < e && s[0] == '/' && s[1] == '/');
            if (!blank && !fx_line_is_decl (p, n))
                blk_pure = FALSE;
        }
        if (!blk_start) {
            if (eligible && pp == 0 && fx_line_is_decl (p, n) && p > first_inc)
                g_string_append_len (moved, p, n);
            else
                g_string_append_len (rest, p, n);
        }
        p += n;
    }
    if (blk_start)
        g_string_append (rest, blk_start); /* unterminated #if: keep as is */
    char *out;
    if (!first_inc || moved->len == 0) {
        out = g_strdup (body);
    } else {
        /* rest = body minus moved lines; put moved before the first
         * #include (its offset in rest equals its offset in body: nothing
         * before it was moved) */
        gsize off = (gsize) (first_inc - body);
        GString *o = g_string_new_len (rest->str, (gssize) off);
        g_string_append (o, moved->str);
        g_string_append (o, rest->str + off);
        out = g_string_free (o, FALSE);
    }
    g_string_free (moved, TRUE);
    g_string_free (rest, TRUE);
    return out;
}

/* comment out a uniform/varying/attribute that re-declares a name already
 * declared unconditionally earlier (headers and effects both declare
 * g_Texture0Resolution, v_TexCoord, ...: a GLSL redeclaration error) */
static void fx_dedupe_decls (char *src)
{
    GHashTable *seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    int pp = 0;
    for (char *p = src; *p;) {
        char *le = strchr (p, '\n');
        gsize n = le ? (gsize) (le - p) + 1 : strlen (p);
        const char *s = fx_skip_ws (p, p + n);
        if (*s == '#') {
            if (!strncmp (s, "#if", 3))
                pp++;
            else if (!strncmp (s, "#endif", 6))
                pp--;
        } else if (fx_line_is_decl (p, n)) {
            /* name = last identifier before ';' / '[' / '=' */
            const char *semi = memchr (p, ';', n);
            if (semi) {
                const char *e = semi;
                const char *br = memchr (p, '[', (gsize) (semi - p));
                const char *eq = memchr (p, '=', (gsize) (semi - p));
                if (br) e = br;
                if (eq && eq < e) e = eq;
                while (e > p && (e[-1] == ' ' || e[-1] == '\t'))
                    e--;
                const char *b = e;
                while (b > p && (g_ascii_isalnum (b[-1]) || b[-1] == '_'))
                    b--;
                if (e > b) {
                    char *name = g_strndup (b, (gsize) (e - b));
                    if (g_hash_table_contains (seen, name)) {
                        /* turn the line into a comment in place (keeps
                         * line numbers for the compile log) */
                        p[0] = '/';
                        p[1] = '/';
                        g_free (name);
                    } else if (pp == 0) {
                        g_hash_table_add (seen, name);
                    } else {
                        g_free (name);
                    }
                }
            }
        }
        p += n;
    }
    g_hash_table_unref (seen);
}

/* strchr () / strstr () for a position that only moves forward through
 * one text: the answer is kept for as long as it lies at or behind the
 * position asked for, so the text is walked once however often it is
 * asked. The annotation scans below look from every "[COMBO]" /
 * "sampler2D" for the next '{', ';', line end and key: with plain
 * strchr () a shader of such words without the character looked for was
 * walked to its end once per word (1 MiB of "sampler2D g_Texture1" lines:
 * 5 s per call of the scan, and it runs for every sampler unit of every
 * pass; 4 MiB with includes: 80 s). */
typedef struct { const char *at; gboolean none; } FxAhead;

static const char *fx_ahead_chr (FxAhead *a, const char *p, char c)
{
    if (a->none)
        return NULL;
    if (!a->at || a->at < p) {
        a->at = strchr (p, c);
        a->none = !a->at;
    }
    return a->at;
}

static const char *fx_ahead_str (FxAhead *a, const char *p, const char *s)
{
    if (a->none)
        return NULL;
    if (!a->at || a->at < p) {
        a->at = strstr (p, s);
        a->none = !a->at;
    }
    return a->at;
}

/* an annotation is one short JSON object behind a declaration (the stock
 * ones have under 300 bytes): longer text between the braces is not read */
#define FX_MAX_ANNOTATION 4096

/* the combos a shader declares: // [COMBO] {"combo":"NAME","default":N} */
static void fx_collect_combo_defaults (const char *src, GHashTable *combos)
{
    FxAhead open = { NULL, FALSE }, nl = { NULL, FALSE };
    const char *close_eol = NULL, *close = NULL;   /* the last '}' of the line that ends at close_eol */
    gboolean close_known = FALSE;
    for (const char *p = src; (p = strstr (p, "[COMBO]")) != NULL; p += 7) {
        const char *b1 = fx_ahead_chr (&open, p, '{');
        const char *eol = fx_ahead_chr (&nl, p, '\n');
        if (!b1 || (eol && b1 > eol))
            continue;
        /* the last '}' between b1 and the line end: found once per line */
        if (!close_known || close_eol != eol) {
            close = eol ? g_strrstr_len (b1, eol - b1, "}") : strrchr (b1, '}');
            close_eol = eol;
            close_known = TRUE;
        }
        const char *b2 = close && close >= b1 ? close : NULL;
        if (!b2 || b2 - b1 >= FX_MAX_ANNOTATION)
            continue;
        char *json = g_strndup (b1, (gsize) (b2 - b1 + 1));
        JsonNode *root = wpj_parse_data (json, -1);
        g_free (json);
        if (!root)
            continue;
        if (JSON_NODE_HOLDS_OBJECT (root)) {
            JsonObject *o = json_node_get_object (root);
            const char *name = wpj_str (o, "combo", NULL);
            if (name && *name && !g_hash_table_contains (combos, name))
                g_hash_table_insert (combos, g_strdup (name),
                                     GINT_TO_POINTER (walpe_d2i (wpj_num (o, "default", 0))));
        }
        json_node_unref (root);
    }
}

/* the combo a sampler toggles when a texture is bound:
 * uniform sampler2D g_TextureN; // {..., "combo":"MASK", ...} */
/* the string value of `key` ("\"combo\"") in the annotation behind the
 * declaration of sampler g_Texture<unit>: the text between the next two
 * quotes behind the key, all on the declaration's line */
static char *fx_sampler_anno_str (const char *src, int unit, const char *key)
{
    char uname[16];
    snprintf (uname, sizeof (uname), "g_Texture%d", unit);
    const gsize ul = strlen (uname), kl = strlen (key);
    FxAhead semi_a = { NULL, FALSE }, nl = { NULL, FALSE }, key_a = { NULL, FALSE },
            q1_a = { NULL, FALSE }, q2_a = { NULL, FALSE };
    for (const char *p = src; (p = strstr (p, "sampler2D")) != NULL; p += 9) {
        const char *semi = fx_ahead_chr (&semi_a, p, ';');
        const char *eol = fx_ahead_chr (&nl, p, '\n');
        if (!semi || (eol && semi > eol))
            continue;
        const char *nm = fx_skip_ws (p + 9, semi);
        if (strncmp (nm, uname, ul) || g_ascii_isalnum (nm[ul]))
            continue;
        /* the key between the ';' and the line end */
        const char *c = fx_ahead_str (&key_a, semi, key);
        if (!c || (eol && c + kl > eol))
            continue;
        const char *q1 = fx_ahead_chr (&q1_a, c + kl, '"');
        const char *q2 = q1 ? fx_ahead_chr (&q2_a, q1 + 1, '"') : NULL;
        if (q1 && q2 && (!eol || q2 < eol))
            return g_strndup (q1 + 1, (gsize) (q2 - q1 - 1));
    }
    return NULL;
}

static char *fx_sampler_combo (const char *src, int unit)
{
    return fx_sampler_anno_str (src, unit, "\"combo\"");
}

/* the default texture a sampler annotation names ("default":"util/noise") */
static char *fx_sampler_default (const char *src, int unit)
{
    return fx_sampler_anno_str (src, unit, "\"default\"");
}

static void fx_combos_overlay (GHashTable *combos, JsonObject *src)
{
    if (!src)
        return;
    GList *ks = json_object_get_members (src);
    for (GList *k = ks; k; k = k->next) {
        JsonNode *vn = json_object_get_member (src, k->data);
        if (!vn || json_node_get_node_type (vn) != JSON_NODE_VALUE)
            continue;
        GValue v = G_VALUE_INIT;
        json_node_get_value (vn, &v);
        int iv = 0;
        gboolean ok = TRUE;
        if (G_VALUE_HOLDS_DOUBLE (&v))
            iv = walpe_d2i (g_value_get_double (&v));
        else if (G_VALUE_HOLDS_INT64 (&v))
            iv = (int) g_value_get_int64 (&v);
        else if (G_VALUE_HOLDS_INT (&v))
            iv = g_value_get_int (&v);
        else if (G_VALUE_HOLDS_BOOLEAN (&v))
            iv = g_value_get_boolean (&v);
        else if (G_VALUE_HOLDS_STRING (&v))
            iv = atoi (g_value_get_string (&v));
        else
            ok = FALSE;
        g_value_unset (&v);
        if (ok)
            g_hash_table_insert (combos, g_strdup (k->data), GINT_TO_POINTER (iv));
    }
    g_list_free (ks);
}

/* GLSL 330 prelude for WE's dialect (GLSL 1.x syntax with HLSL helper
 * names; WE translates it to HLSL, so HLSL semantics apply: mul(v, M) is
 * a row-vector product = M * v in GLSL) */
static const char *fx_prelude_common =
    /* first line of the prelude: the combo #defines end here (fx_fuse.h,
     * fx_cost.h and fx_rewrite.h look for it) */
    "#define GLSL 1\n"
    /* precision qualifiers mean nothing in desktop GLSL */
    "#define lowp\n#define mediump\n#define highp\n"
    /* HLSL's names of the vector and matrix types */
    "#define float2 vec2\n#define float3 vec3\n#define float4 vec4\n"
    "#define int2 ivec2\n#define int3 ivec3\n#define int4 ivec4\n"
    "#define float2x2 mat2\n#define float3x3 mat3\n#define float4x4 mat4\n"
    /* a vector or matrix from one value */
    "#define CAST2(v) (vec2(v))\n#define CAST3(v) (vec3(v))\n#define CAST4(v) (vec4(v))\n"
    "#define CAST2X2(m) (mat2(m))\n#define CAST3X3(m) (mat3(m))\n#define CAST4X4(m) (mat4(m))\n"
    "#define TOGL2(v) vec2(v)\n#define TOGL3(v) vec3(v)\n#define TOGL4(v) vec4(v)\n"
    /* HLSL's functions: mul (v, M) is the row-vector product M * v, fmod
     * truncates towards zero */
    "#define mul(a, b) ((b) * (a))\n"
    "#define fmod(a, b) ((a) - (b) * trunc((a) / (b)))\n"
    "#define saturate(v) clamp(v, 0.0, 1.0)\n"
    "#define log10(v) (log2(v) * 0.30102999566)\n"
    "#define ddx(v) dFdx(v)\n"
    "#define ddy(v) dFdy(v)\n"
    "#define lerp mix\n"
    "#define frac fract\n"
    "#define rsqrt inversesqrt\n"
    "#define atan2 atan\n"
    /* texture lookups */
    "#define texSample2D texture\n"
    "#define texSample2DLod textureLod\n"
    "#define texSample3D texture\n"
    "#define texture2D texture\n";

/* A combo is a macro of the shader: its name is an identifier. The names
 * come from the wallpaper's JSON (a material's and an effect instance's
 * "combos", the shader's own [COMBO] annotations) and are written into the
 * source in front of the shader text. Anything else as a name is left out:
 * a key with a line break in it was a way to put text of any length (a
 * JSON file may have 32 MiB) into the source, past FX_MAX_SOURCE. */
#define FX_MAX_COMBO_NAME 128
static gboolean fx_combo_name_ok (const char *k)
{
    if (!g_ascii_isalpha (k[0]) && k[0] != '_')
        return FALSE;
    gsize n = 1;
    while (n <= FX_MAX_COMBO_NAME && (g_ascii_isalnum (k[n]) || k[n] == '_'))
        n++;
    return n <= FX_MAX_COMBO_NAME && !k[n];
}

/* build the compiled source: version + combos + prelude + body */
static char *fx_build_source (const char *body, GHashTable *combos, gboolean vertex)
{
    GString *s = g_string_new ("#version 330\n");
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init (&it, combos);
    while (g_hash_table_iter_next (&it, &k, &v))
        if (fx_combo_name_ok (k))
            g_string_append_printf (s, "#define %s %d\n", (char *) k, GPOINTER_TO_INT (v));
    g_string_append (s, fx_prelude_common);
    g_string_append (s, vertex
        ? "#define varying out\n#define attribute in\n"
        : "#define varying in\n"
          "out vec4 _we_fragColor;\n#define gl_FragColor _we_fragColor\n"
          "void clip(float x) { if (x < 0.0) discard; }\n"
          "void clip(vec2 x) { if (any(lessThan(x, vec2(0.0)))) discard; }\n"
          "void clip(vec3 x) { if (any(lessThan(x, vec3(0.0)))) discard; }\n"
          "void clip(vec4 x) { if (any(lessThan(x, vec4(0.0)))) discard; }\n");
    g_string_append (s, "#line 1\n");
    g_string_append (s, body);
    char *out = g_string_free (s, FALSE);
    fx_dedupe_decls (out);
    return out;
}

/* ---- HLSL semantics repair ------------------------------------------
 * WE translates its shaders to HLSL, where scalar->vector splats, float->
 * int initializers, bool arithmetic, float '%' and vector truncation in
 * calls are legal. GLSL 330 rejects them. The rewrites below are local and
 * line-scoped (wrap in a constructor / float(), '%' -> mod()); each
 * produces exactly what HLSL would compute. WHEN they run is decided in
 * fx_srcrepair.h: from the shader source before the first compile, on
 * every driver alike; the driver's log only names lines for a fallback. */

/* end of the expression starting at s: first ';' or ',' or unmatched ')'
 * at nesting depth 0 */
static const char *fx_expr_end (const char *s)
{
    int d = 0;
    for (; *s && *s != '\n'; s++) {
        if (*s == '(' || *s == '[')
            d++;
        else if (*s == ')' || *s == ']') {
            if (d == 0)
                return s;
            d--;
        } else if ((*s == ';' || *s == ',') && d == 0)
            return s;
    }
    return s;
}

/* "T name = expr" -> "T name = T(expr)" for the first T declaration in
 * the line (T = the variable's type from the log) */
static char *fx_fix_initializer (const char *line, const char *type)
{
    gsize tl = strlen (type);
    for (const char *p = line; (p = strstr (p, type)) != NULL; p += tl) {
        if ((p > line && (g_ascii_isalnum (p[-1]) || p[-1] == '_')) ||
            g_ascii_isalnum (p[tl]) || p[tl] == '_')
            continue;
        const char *q = p + tl;
        while (*q == ' ' || *q == '\t')
            q++;
        if (!g_ascii_isalpha (*q) && *q != '_')
            continue;
        while (g_ascii_isalnum (*q) || *q == '_')
            q++;
        while (*q == ' ' || *q == '\t')
            q++;
        if (*q != '=' || q[1] == '=')
            continue;
        q++;
        while (*q == ' ' || *q == '\t')
            q++;
        const char *e = fx_expr_end (q);
        while (e > q && (e[-1] == ' ' || e[-1] == '\t'))
            e--;
        return g_strdup_printf ("%.*s%s(%.*s)%s", (int) (q - line), line, type,
                                (int) (e - q), q, e);
    }
    return NULL;
}

/* "return expr;" -> "return T(expr);" */
static char *fx_fix_return (const char *line, const char *type)
{
    const char *p = strstr (line, "return");
    if (!p)
        return NULL;
    const char *q = p + 6;
    while (*q == ' ' || *q == '\t')
        q++;
    const char *e = strchr (q, ';');
    if (!e || e == q)
        return NULL;
    return g_strdup_printf ("%.*s%s(%.*s)%s", (int) (q - line), line, type, (int) (e - q), q, e);
}

/* operand bounds around an operator at op: identifiers, numbers, member
 * access, calls and parenthesised groups */
static const char *fx_operand_start (const char *line, const char *op)
{
    const char *p = op;
    while (p > line && (p[-1] == ' ' || p[-1] == '\t'))
        p--;
    if (p > line && p[-1] == ')') {
        int d = 0;
        do {
            p--;
            if (*p == ')')
                d++;
            else if (*p == '(')
                d--;
        } while (p > line && d > 0);
    }
    while (p > line && (g_ascii_isalnum (p[-1]) || p[-1] == '_' || p[-1] == '.' ||
                        p[-1] == ']' || p[-1] == '[')) {
        p--;
        if (*p == ']') {
            /* back to its '['. A ']' without one ("]+x" at the start of
             * the line) ends at the line start: the step back from there
             * used to leave the buffer by one byte, and the callers read
             * and copied from that address */
            int d = 0;
            do {
                if (*p == ']')
                    d++;
                else if (*p == '[')
                    d--;
                if (d > 0) {
                    if (p == line)
                        break;
                    p--;
                }
            } while (d > 0);
        }
    }
    return p;
}

static const char *fx_operand_end (const char *op)
{
    const char *p = op;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == '-' || *p == '+')
        p++;
    if (*p == '(') {
        int d = 0;
        do {
            if (*p == '(')
                d++;
            else if (*p == ')')
                d--;
            p++;
        } while (*p && *p != '\n' && d > 0);
    }
    while (g_ascii_isalnum (*p) || *p == '_' || *p == '.')
        p++;
    if (*p == '(' || *p == '[') {
        char o = *p, c = o == '(' ? ')' : ']';
        int d = 0;
        do {
            if (*p == o)
                d++;
            else if (*p == c)
                d--;
            p++;
        } while (*p && *p != '\n' && d > 0);
    }
    return p;
}

/* "a % b" -> "mod(float(a), float(b))" (HLSL float modulo; fmod and mod
 * agree for the non-negative operands these shaders use) */
static char *fx_fix_modulo (const char *line)
{
    const char *op = strchr (line, '%');
    const char *cm = strstr (line, "//");
    if (!op || (cm && cm < op) || op[1] == '=')
        return NULL;
    const char *a = fx_operand_start (line, op);
    const char *b = fx_operand_end (op + 1);
    const char *b0 = op + 1;
    while (*b0 == ' ')
        b0++;
    const char *a1 = op;
    while (a1 > a && a1[-1] == ' ')
        a1--;
    if (a1 <= a || b <= b0)
        return NULL;
    return g_strdup_printf ("%.*smod(float(%.*s), float(%.*s))%s", (int) (a - line), line,
                            (int) (a1 - a), a, (int) (b - b0), b0, b);
}

static gboolean fx_has_cmp (const char *s, const char *e)
{
    for (const char *p = s; p < e; p++)
        if ((*p == '<' || *p == '>') ||
            ((*p == '=' || *p == '!') && p + 1 < e && p[1] == '='))
            return TRUE;
    return FALSE;
}

/* bool used as a number: "(a < b)" groups that are not call/if/while
 * arguments -> "float(a < b)"; else a compound assignment whose right
 * side compares: "x += a < b;" -> "x += float(a < b);" */
static char *fx_fix_bool_arith (const char *line)
{
    GString *o = g_string_new (NULL);
    gboolean changed = FALSE;
    const char *p = line;
    while (*p) {
        if (*p == '(') {
            const char *k = p;
            while (k > line && k[-1] == ' ')
                k--;
            gboolean call = k > line && (g_ascii_isalnum (k[-1]) || k[-1] == '_');
            const char *q = p + 1;
            int d = 1;
            gboolean inner = FALSE;
            while (*q && d > 0) {
                if (*q == '(') {
                    d++;
                    inner = TRUE;
                } else if (*q == ')')
                    d--;
                if (d > 0)
                    q++;
            }
            if (!call && !inner && *q == ')' && fx_has_cmp (p + 1, q) &&
                !strstr (line, "if") && !strstr (line, "while") && !strstr (line, "for")) {
                g_string_append (o, "float");
                changed = TRUE;
            }
        }
        g_string_append_c (o, *p);
        p++;
    }
    if (changed)
        return g_string_free (o, FALSE);
    g_string_free (o, TRUE);
    const char *as = NULL;
    for (const char *s = line; *s; s++)
        if ((*s == '+' || *s == '-' || *s == '*' || *s == '/') && s[1] == '=') {
            as = s + 2;
            break;
        }
    if (!as)
        return NULL;
    const char *e = strchr (as, ';');
    if (!e || !fx_has_cmp (as, e))
        return NULL;
    while (*as == ' ')
        as++;
    return g_strdup_printf ("%.*sfloat(%.*s)%s", (int) (as - line), line, (int) (e - as), as, e);
}

/* the declared type of a variable: first "T name" in the source */
static char *fx_var_type (const char *src, const char *name)
{
    /* src = the text BEFORE the line being repaired: the nearest
     * preceding declaration is the one in scope */
    char *pat = g_strdup_printf ("\\b(float|int|uint|bool|[iu]?vec[234]|mat[234])\\s+%s\\b", name);
    GRegex *rx = g_regex_new (pat, 0, 0, NULL);
    g_free (pat);
    GMatchInfo *mi = NULL;
    char *ty = NULL;
    if (rx && g_regex_match (rx, src, 0, &mi)) {
        while (g_match_info_matches (mi)) {
            g_free (ty);
            ty = g_match_info_fetch (mi, 1);
            g_match_info_next (mi, NULL);
        }
    }
    g_match_info_free (mi);
    if (rx)
        g_regex_unref (rx);
    return ty;
}

/* compound assignments with HLSL implicit conversions: "x *= b;" ->
 * "x *= T(b);" with T = the type of x (a swizzle picks floatN). Handles
 * every statement on the line. */
static char *fx_fix_compound (const char *line, const char *src)
{
    GString *o = g_string_new (NULL);
    gboolean changed = FALSE;
    const char *p = line;
    while (*p) {
        const char *semi = strchr (p, ';');
        const char *stop = semi ? semi : p + strlen (p);
        const char *op = NULL;
        for (const char *s = p; s + 1 < stop; s++)
            if ((*s == '+' || *s == '-' || *s == '*' || *s == '/') && s[1] == '=') {
                op = s;
                break;
            }
        char *ty = NULL;
        if (op) {
            const char *l0 = p;
            while (l0 < op && (*l0 == ' ' || *l0 == '\t'))
                l0++;
            const char *l1 = op;
            while (l1 > l0 && l1[-1] == ' ')
                l1--;
            const char *n1 = l0;
            while (n1 < l1 && (g_ascii_isalnum (*n1) || *n1 == '_'))
                n1++;
            if (n1 > l0) {
                char *name = g_strndup (l0, (gsize) (n1 - l0));
                const char *dot = memchr (n1, '.', (gsize) (l1 - n1));
                if (dot && !memchr (n1, '[', (gsize) (l1 - n1))) {
                    int k = (int) (l1 - dot - 1);
                    ty = k == 1 ? g_strdup ("float") : k <= 4 ? g_strdup_printf ("vec%d", k) : NULL;
                } else {
                    ty = fx_var_type (src, name);
                }
                g_free (name);
            }
        }
        if (ty) {
            const char *r = op + 2;
            while (*r == ' ')
                r++;
            gsize tl = strlen (ty);
            if (!(strncmp (r, ty, tl) == 0 && r[tl] == '(')) {
                g_string_append_len (o, p, r - p);
                g_string_append_printf (o, "%s(%.*s)", ty, (int) (stop - r), r);
                changed = TRUE;
            } else {
                g_string_append_len (o, p, stop - p);
            }
            g_free (ty);
        } else {
            g_string_append_len (o, p, stop - p);
        }
        if (!semi)
            break;
        g_string_append_c (o, ';');
        p = semi + 1;
    }
    if (!changed) {
        g_string_free (o, TRUE);
        return NULL;
    }
    return g_string_free (o, FALSE);
}

static char *fx_join_upto (char **lines, guint n)
{
    GString *s = g_string_new (NULL);
    for (guint i = 0; i < n && lines[i]; i++) {
        g_string_append (s, lines[i]);
        g_string_append_c (s, '\n');
    }
    return g_string_free (s, FALSE);
}

/* HLSL mixes uint and int freely: use int throughout the body */
static gboolean fx_uint_to_int (char **lines, guint base, guint nl)
{
    gboolean changed = FALSE;
    GRegex *ru = g_regex_new ("\\buint\\b", 0, 0, NULL);
    GRegex *rv = g_regex_new ("\\buvec([234])\\b", 0, 0, NULL);
    for (guint i = base; i < nl; i++) {
        /* NULL: the line is not UTF-8 (sources that did not come through
         * fx_read_shader) - it stays as it is */
        char *r = g_regex_replace_literal (ru, lines[i], -1, 0, "int", 0, NULL);
        char *r2 = r ? g_regex_replace (rv, r, -1, 0, "ivec\\1", 0, NULL) : NULL;
        g_free (r);
        if (r2 && strcmp (r2, lines[i])) {
            g_free (lines[i]);
            lines[i] = r2;
            changed = TRUE;
        } else {
            g_free (r2);
        }
    }
    g_regex_unref (ru);
    g_regex_unref (rv);
    return changed;
}

static gboolean fx_has_word (const char *line, const char *w)
{
    gsize n = strlen (w);
    for (const char *p = line; (p = strstr (p, w)) != NULL; p += n)
        if ((p == line || (!g_ascii_isalnum (p[-1]) && p[-1] != '_')) &&
            !g_ascii_isalnum (p[n]) && p[n] != '_')
            return TRUE;
    return FALSE;
}

/* the first non-blank character of a line */
static const char *fx_trim (const char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

#include "fx_srcrepair.h"

/* FxTryFn on a GL shader object */
static gboolean fx_try_gl (const char *src, char *log, gsize logsize, gpointer user)
{
    GLuint sh = GPOINTER_TO_UINT (user);
    glShaderSource (sh, 1, &src, NULL);
    glCompileShader (sh);
    GLint ok = 0;
    glGetShaderiv (sh, GL_COMPILE_STATUS, &ok);
    log[0] = 0;
    if (!ok) {
        glGetShaderInfoLog (sh, (GLsizei) logsize, NULL, log);
        fx_log_view (log, logsize);
    }
    return ok != 0;
}

static GLuint fx_compile (char **srcp, GLenum type, const char *name)
{
    GLuint sh = glCreateShader (type);
    char log[4096];
    /* fx_prepass() has already repaired what the source shows. What is
     * left goes through the error-driven fallback: line numbers from the
     * log, candidates from the source, the compiler as the judge. */
    int budget = 48, round = 0;
    gboolean ok = fx_try_gl (*srcp, log, sizeof (log), GUINT_TO_POINTER (sh));
    while (!ok && round < 16 &&
           fx_repair_by_lines (srcp, log, fx_try_gl, GUINT_TO_POINTER (sh), &budget)) {
        round++;
        ok = fx_try_gl (*srcp, log, sizeof (log), GUINT_TO_POINTER (sh));
    }
    if (ok) {
        const char *da = g_getenv ("WALPE_FX_DUMP_ALL");
        if (da && *da) {
            static int an;
            char *base = g_strdup (name);
            g_strdelimit (base, "/ ", '_');
            char *fn = g_strdup_printf ("%s/ok%03d-%s.%s", da, g_atomic_int_add (&an, 1), base,
                                        type == GL_VERTEX_SHADER ? "vert" : "frag");
            g_file_set_contents (fn, *srcp, -1, NULL);
            g_free (fn);
            g_free (base);
        }
        if (round > 0)
            g_message ("fx: shader %s compiled after %d error-driven repair round(s) "
                       "(not visible to the source inspection)", name, round);
        return sh;
    }
    g_warning ("fx: shader %s compile failed: %.400s", name, log);
    const char *dump = g_getenv ("WALPE_FX_DUMP");
    if (dump && *dump) {
        static int dn;
        char *base = g_strdup (name);
        g_strdelimit (base, "/ ", '_');
        char *fn = g_strdup_printf ("%s/%03d-%s.%s", dump, g_atomic_int_add (&dn, 1), base,
                                    type == GL_VERTEX_SHADER ? "vert" : "frag");
        char *txt = g_strdup_printf ("%s\n/* ---- log ----\n%s*/\n", *srcp, log);
        g_file_set_contents (fn, txt, -1, NULL);
        g_free (txt);
        g_free (fn);
        g_free (base);
    }
    glDeleteShader (sh);
    return 0;
}

/* ------------------------------------------------------- uniform values */

/* a json scalar (string or number) as a freshly allocated string */
static char *fx_json_valstr (JsonNode *n)
{
    if (!n || json_node_get_node_type (n) != JSON_NODE_VALUE)
        return NULL;
    GValue v = G_VALUE_INIT;
    json_node_get_value (n, &v);
    char *out = NULL;
    if (G_VALUE_HOLDS_STRING (&v))
        out = g_strdup (g_value_get_string (&v));
    else if (G_VALUE_HOLDS_DOUBLE (&v))
        out = g_strdup_printf ("%.10g", g_value_get_double (&v));
    else if (G_VALUE_HOLDS_INT (&v))
        out = g_strdup_printf ("%d", g_value_get_int (&v));
    else if (G_VALUE_HOLDS_INT64 (&v))
        out = g_strdup_printf ("%" G_GINT64_FORMAT, g_value_get_int64 (&v));
    g_value_unset (&v);
    return out;
}

/* the '}' that closes the object opened at b1, before eol: nested objects
 * count ("require":{"DIRECTDRAW":0}, "options":{...}), braces inside
 * strings do not. NULL when the line ends first. */
static const char *fx_json_obj_end (const char *b1, const char *eol)
{
    int depth = 0;
    gboolean in_str = FALSE;
    for (const char *c = b1; c < eol; c++) {
        if (in_str) {
            if (*c == '\\' && c + 1 < eol)
                c++;
            else if (*c == '"')
                in_str = FALSE;
        } else if (*c == '"') {
            in_str = TRUE;
        } else if (*c == '{') {
            depth++;
        } else if (*c == '}' && --depth == 0) {
            return c;
        }
    }
    return NULL;
}

/* extract `key` from the // {"...":...} annotation of `uniform <t> uname`
 * in a shader body (annotations are single-line, after the ';').
 * Handles string AND numeric values; returns NULL when absent.
 * tword: only declarations of this type ("vec2"); NULL = any. */
static char *fx_shader_anno_t (const char *body, const char *uname, const char *key, const char *tword)
{
    if (!body || !uname || !key)
        return NULL;
    gsize ulen = strlen (uname), tlen = tword ? strlen (tword) : 0;
    const char *p = body;
    while ((p = strstr (p, "uniform")) != NULL) {
        const char *semi = strchr (p, ';');
        if (!semi)
            break;
        /* uname must appear inside this uniform statement, as a whole word */
        const char *nm = g_strstr_len (p, semi - p, uname);
        if (nm && tword) {
            const char *t = p + 7;
            while (t < nm && g_ascii_isspace (*t))
                t++;
            if (!((gsize) (nm - t) > tlen && !strncmp (t, tword, tlen) && g_ascii_isspace (t[tlen])))
                nm = NULL;
        }
        if (nm) {
            char after = nm[ulen];
            if (!g_ascii_isalnum (after) && after != '_') {
                const char *eol = strchr (semi, '\n');
                if (!eol)
                    eol = semi + strlen (semi);
                const char *b1 = memchr (semi, '{', (gsize) (eol - semi));
                if (b1) {
                    /* to the matching brace: the first '}' cut annotations
                     * with a nested object short (lightshafts' g_Point0..3:
                     * no material key, no default - the uniform stayed 0) */
                    const char *b2 = fx_json_obj_end (b1, eol);
                    if (b2 && b2 > b1) {
                        char *json = g_strndup (b1, (gsize) (b2 - b1 + 1));
                        JsonNode *root = wpj_parse_data (json, -1);
                        g_free (json);
                        if (root) {
                            char *out = fx_json_valstr (
                                json_object_get_member (json_node_get_object (root), key));
                            json_node_unref (root);
                            if (out)
                                return out;
                        }
                    }
                }
            }
        }
        p = semi + 1;
    }
    return NULL;
}

/* the annotation value for an active uniform of GL type `type`: from the
 * declaration that has this type first (fragment body, then vertex body),
 * else from any declaration of the name, as before. A name can be declared
 * with another type in the other stage or in a branch that is not compiled:
 * WE's Clouds has "uniform vec2 g_CloudSpeeds" (default "0.01 -0.02") in
 * the vertex shader and "uniform vec4 g_CloudSpeeds" (four numbers) in the
 * fragment shader's perspective branch. */
static char *fx_uniform_anno (const char *fbody, const char *vbody, const char *uname, const char *key,
                              GLenum type)
{
    const char *tw = type == GL_FLOAT ? "float" : type == GL_FLOAT_VEC2 ? "vec2"
                   : type == GL_FLOAT_VEC3 ? "vec3" : type == GL_FLOAT_VEC4 ? "vec4"
                   : type == GL_INT ? "int" : type == GL_BOOL ? "bool" : NULL;
    char *r = NULL;
    if (tw && ((r = fx_shader_anno_t (fbody, uname, key, tw)) || (r = fx_shader_anno_t (vbody, uname, key, tw))))
        return r;
    if ((r = fx_shader_anno_t (fbody, uname, key, NULL)))
        return r;
    return fx_shader_anno_t (vbody, uname, key, NULL);
}

/* instance constantshadervalues lookup: {"key": 0.15 | "1 1" |
 * {"value": ...}} — values may be raw numbers, not just strings */
/* user property values of the scene being built (same thread) */
static __thread JsonObject *fx_user_props;

static char *fx_const_val (JsonObject *cst, const char *k)
{
    if (!cst || !k)
        return NULL;
    JsonNode *n = json_object_get_member (cst, k);
    if (!n)
        return NULL;
    if (json_node_get_node_type (n) == JSON_NODE_OBJECT) {
        /* {"user": propid, "value": fallback}: the user's value wins */
        JsonObject *b = json_node_get_object (n);
        const char *up = wpj_str (b, "user", NULL);
        JsonNode *pv = up && fx_user_props ? json_object_get_member (fx_user_props, up) : NULL;
        if (pv)
            walpe_probe_use (up, "fx.const");
        char *v = pv ? fx_json_valstr (pv) : NULL;
        if (pv && !v && json_node_get_node_type (pv) == JSON_NODE_VALUE &&
            json_node_get_value_type (pv) == G_TYPE_BOOLEAN)
            v = g_strdup (json_node_get_boolean (pv) ? "1" : "0");
        return v ? v : fx_json_valstr (json_object_get_member (b, "value"));
    }
    return fx_json_valstr (n);
}

/* Up to four floats of a constant's value string. Wallpaper Engine's
 * editor writes "0.97 0.997"; shader authors also write annotation
 * defaults (which the editor copies into the scene) as "0.0, 1.0" - the
 * workshop's Simple Audio Bars has three such vec2 - and with "%f %f"
 * alone everything behind the first comma was lost: (0, 0) for bar bounds
 * of "0.0, 1.0", (0.02, 0) for an anti-alias width of "0.02, 0.02", whose
 * zero then made a smoothstep with equal edges. A comma between two
 * values is a separator like the blank; a string without one is read
 * exactly as before. Returns the number of values read. */
static int fx_scan_floats (const char *s, float f[4])
{
    if (G_LIKELY (!strchr (s, ',')))
        return sscanf (s, "%f %f %f %f", &f[0], &f[1], &f[2], &f[3]);
    char *t = g_strdup (s);
    for (char *q = t; *q; q++)
        if (*q == ',')
            *q = ' ';
    int n = sscanf (t, "%f %f %f %f", &f[0], &f[1], &f[2], &f[3]);
    g_free (t);
    return n;
}

/* set one uniform from a value string "1 1" / "0.97 0.997", by the
 * uniform's GLSL TYPE (the old code switched on GL_UNIFORM_SIZE - the array
 * length, 1 for every non-array - so vec2/vec3/vec4 constants were sent
 * with glUniform1f, failed with GL_INVALID_OPERATION and stayed 0).
 * Program must be current. */
static void fx_set_uniform (GLint loc, GLenum type, const char *value)
{
    float f[4] = { 0, 0, 0, 1 };
    if (fx_scan_floats (value, f) < 1)
        return;
    switch (type) {
    case GL_FLOAT:      glUniform1f (loc, f[0]); break;
    case GL_FLOAT_VEC2: glUniform2f (loc, f[0], f[1]); break;
    case GL_FLOAT_VEC3: glUniform3f (loc, f[0], f[1], f[2]); break;
    case GL_FLOAT_VEC4: glUniform4f (loc, f[0], f[1], f[2], f[3]); break;
    case GL_INT:
    case GL_BOOL:       glUniform1i (loc, fx_f2i (f[0])); break;
    default:            break; /* matrices/samplers: no string form */
    }
}

/* ---- GL state during one run of a layer's effects -------------------------
 * Every pass used to set its whole state again: blending and depth test
 * off, the pass VAO, the texture unit back to 0, the viewport, every
 * resolution / texel-size uniform, and each chain ended by binding
 * framebuffer 0 and the layer quad's VAO for a caller that only needs them
 * after the last chain. For one 4K scene (2901582518 at 1920x1080)
 * that was 921 GL calls per frame, 373 of them setting what was already set.
 * Between fx_gls_begin() and fx_gls_end() (fx_run_for_object: no other
 * code issues GL calls in between) the values set are remembered and a
 * call is only made when it changes something. Outside a run (a chain run
 * on its own, the tests) and with WALPE_FX_STATE_CACHE=0 every call is
 * made as before. Texture bindings are forgotten whenever a target is made
 * or deleted (both change bindings behind the shadow's back; a deleted
 * name may be handed out again); framebuffer bindings are not shadowed. */
static __thread struct {
    gboolean on;
    gboolean dirty;               /* a pass ran: fx_gls_end() restores */
    gboolean fixed;               /* blending / depth test are off */
    GLuint vao;                   /* 0 = unknown */
    GLenum unit;                  /* 0 = unknown */
    GLuint tex[FX_MAX_BINDS];     /* 0 = unknown */
    int vp_w, vp_h;               /* 0 = unknown */
} fx_gls;

static void fx_gls_begin (void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = g_getenv ("WALPE_FX_STATE_CACHE");
        enabled = !(e && *e == '0');
    }
    memset (&fx_gls, 0, sizeof (fx_gls));
    fx_gls.on = enabled;
}

static void fx_gls_forget_textures (void)
{
    memset (fx_gls.tex, 0, sizeof (fx_gls.tex));
    fx_gls.unit = 0;   /* a new target was bound on whichever unit was active */
}

static void fx_gls_active (GLenum unit)
{
    if (fx_gls.on && fx_gls.unit == unit)
        return;
    glActiveTexture (unit);
    fx_gls.unit = unit;
}

static void fx_gls_bind_texture (int unit, GLuint tex)
{
    if (fx_gls.on && tex && unit < FX_MAX_BINDS && fx_gls.tex[unit] == tex)
        return;
    fx_gls_active (GL_TEXTURE0 + (GLenum) unit);
    glBindTexture (GL_TEXTURE_2D, tex);
    if (unit < FX_MAX_BINDS)
        fx_gls.tex[unit] = tex;
}

static void fx_gls_bind_vao (GLuint vao)
{
    if (fx_gls.on && fx_gls.vao == vao)
        return;
    glBindVertexArray (vao);
    fx_gls.vao = vao;
}

static void fx_gls_viewport (int w, int h)
{
    if (fx_gls.on && fx_gls.vp_w == w && fx_gls.vp_h == h)
        return;
    glViewport (0, 0, w, h);
    fx_gls.vp_w = w;
    fx_gls.vp_h = h;
}

/* what every pass draws with: no blending, no depth test */
static void fx_gls_pass_state (void)
{
    fx_gls.dirty = TRUE;
    if (fx_gls.on && fx_gls.fixed)
        return;
    glDisable (GL_BLEND);
    glDisable (GL_DEPTH_TEST);
    fx_gls.fixed = TRUE;
}

/* the framebuffer the scene is drawn into: what a run leaves bound
 * (0 = the window; fx_set_scene_target) */
static __thread GLuint fx_scene_fbo;

void fx_set_scene_target (guint fbo)
{
    fx_scene_fbo = fbo;
}

/* a pass (or chain) is done: what the caller expects next. Inside a run
 * this waits for fx_gls_end(). */
static void fx_gls_leave (void)
{
    if (fx_gls.on)
        return;
    glBindFramebuffer (GL_FRAMEBUFFER, fx_scene_fbo);
    walpe_glutil_bind_quad_vao ();
}

static void fx_gls_end (void)
{
    if (!fx_gls.on)
        return;
    fx_gls.on = FALSE;
    if (!fx_gls.dirty)
        return;
    if (fx_gls.unit != GL_TEXTURE0)
        glActiveTexture (GL_TEXTURE0);
    glBindFramebuffer (GL_FRAMEBUFFER, fx_scene_fbo);
    walpe_glutil_bind_quad_vao ();
}

/* per-frame uniforms of a pass: sent when the value differs from what the
 * program holds. A program belongs to one pass; should two passes ever
 * share one (a program cache), each forgets what it sent when the other
 * used the program in between (fx_prog_user, fx_chain_run). */
static __thread GHashTable *fx_prog_user;
static void fx_thread_mark (void);   /* this thread has tables to free when it ends */

/* the value a per-frame uniform was last sent with (EffPass.dyn_last,
 * parallel to EffPass.dyn) */
typedef struct {
    gboolean sent;
    float v[4];
} FxUniLast;

static gboolean fx_uniform_changed (FxUniLast *l, const float *v, int n)
{
    if (fx_gls.on && l->sent && !memcmp (l->v, v, (gsize) n * sizeof (float)))
        return FALSE;
    memcpy (l->v, v, (gsize) n * sizeof (float));
    l->sent = TRUE;
    return TRUE;
}

/* fullscreen pass quad: a_Position vec3 + a_TexCoord vec2, stride 20
 * (thread-local: one GL context per render thread) */
static __thread GLuint pass_vao = 0, pass_vbo = 0;

static void fx_bind_pass_vao (void)
{
    if (!pass_vao) {
        /* v = 0 at clip y = -1, i.e. FBO row 0: every pass output keeps
         * the upload convention (row 0 = image top), so a chain never
         * flips its input. The old v = 1 at y = -1 flipped the image once
         * per pass (odd-pass chains drew upside down vs WALPE_NOFX). */
        static const float verts[4][5] = {
            { -1.0f, -1.0f, 0.0f, 0.0f, 0.0f },
            {  1.0f, -1.0f, 0.0f, 1.0f, 0.0f },
            { -1.0f,  1.0f, 0.0f, 0.0f, 1.0f },
            {  1.0f,  1.0f, 0.0f, 1.0f, 1.0f },
        };
        glGenVertexArrays (1, &pass_vao);
        glGenBuffers (1, &pass_vbo);
        glBindVertexArray (pass_vao);
        glBindBuffer (GL_ARRAY_BUFFER, pass_vbo);
        glBufferData (GL_ARRAY_BUFFER, sizeof (verts), verts, GL_STATIC_DRAW);
        glEnableVertexAttribArray (0);
        glVertexAttribPointer (0, 3, GL_FLOAT, GL_FALSE, 20, (void *) 0);
        glEnableVertexAttribArray (1);
        glVertexAttribPointer (1, 2, GL_FLOAT, GL_FALSE, 20, (void *) 12);
        glBindVertexArray (0);
        fx_gls.vao = 0;
    }
    fx_gls_bind_vao (pass_vao);
}

/* render thread: the layer's own quad for the next fx_run_for_object():
 * centre and size in layer units (scene pixels, y up - the space
 * a_Position lives in for WE's vertex shaders). fold: the caller applies
 * fx_layer_affine() to its draw transform. Consumed by that one run. */
static __thread struct { float cx, cy, w, h; gboolean fold, set; } fx_layer;

/* "Skip hidden areas" (fx_clip.h): the part of a pass's target that
 * something later reads: nothing, n rectangles in texture coordinates
 * (u0, v0, u1, v1), or all of it */
enum { FXN_NONE, FXN_RECT, FXN_FULL };
#define FXN_MAX 6
typedef struct { int mode, n; float r[FXN_MAX][4]; } FxClipNeed;

/* render thread: the part of the next run's result that will be seen
 * (fx_set_clip_uv); consumed by that one run */
static __thread struct { FxClipNeed need; gboolean set; } fx_clip;

void fx_set_clip_uv (const float *uv, int n)
{
    fx_clip.set = n >= 0;
    fx_clip.need.mode = FXN_NONE;
    fx_clip.need.n = 0;
    for (int i = 0; i < n && i < FXN_MAX; i++) {
        const float *r = uv + 4 * i;
        if (!(r[2] > r[0] && r[3] > r[1]))
            continue;
        memcpy (fx_clip.need.r[fx_clip.need.n++], r, 4 * sizeof (float));
        fx_clip.need.mode = FXN_RECT;
    }
    if (n > FXN_MAX)
        fx_clip.need.mode = FXN_FULL;   /* (the caller never has more) */
}

void fx_set_layer_geometry (float cx, float cy, float w, float h, gboolean fold)
{
    fx_layer.cx = cx;
    fx_layer.cy = cy;
    fx_layer.w = w;
    fx_layer.h = h;
    fx_layer.fold = fold;
    fx_layer.set = w > 0 && h > 0;
}

/* pass quad in layer units: same corners, same texture coordinates and
 * (through the matching orthographic matrix) the same clip positions as
 * fx_bind_pass_vao() */
static __thread GLuint lay_vao = 0, lay_vbo = 0;
static __thread float lay_last[4];

static void fx_bind_layer_vao (float cx, float cy, float w, float h, float mvp[16])
{
    if (!lay_vao) {
        glGenVertexArrays (1, &lay_vao);
        glGenBuffers (1, &lay_vbo);
        glBindVertexArray (lay_vao);
        glBindBuffer (GL_ARRAY_BUFFER, lay_vbo);
        glEnableVertexAttribArray (0);
        glVertexAttribPointer (0, 3, GL_FLOAT, GL_FALSE, 20, (void *) 0);
        glEnableVertexAttribArray (1);
        glVertexAttribPointer (1, 2, GL_FLOAT, GL_FALSE, 20, (void *) 12);
        lay_last[2] = -1;
        fx_gls.vao = 0;
    }
    fx_gls_bind_vao (lay_vao);
    if (lay_last[0] != cx || lay_last[1] != cy || lay_last[2] != w || lay_last[3] != h) {
        /* v = 0 (image top) is the layer's +y edge */
        const float x0 = cx - w / 2, x1 = cx + w / 2, yt = cy + h / 2, yb = cy - h / 2;
        const float verts[4][5] = {
            { x0, yt, 0.0f, 0.0f, 0.0f },
            { x1, yt, 0.0f, 1.0f, 0.0f },
            { x0, yb, 0.0f, 0.0f, 1.0f },
            { x1, yb, 0.0f, 1.0f, 1.0f },
        };
        glBindBuffer (GL_ARRAY_BUFFER, lay_vbo);
        glBufferData (GL_ARRAY_BUFFER, sizeof (verts), verts, GL_DYNAMIC_DRAW);
        lay_last[0] = cx;
        lay_last[1] = cy;
        lay_last[2] = w;
        lay_last[3] = h;
    }
    /* layer units -> the target's clip space; FBO row 0 = image top */
    memset (mvp, 0, sizeof (float) * 16);
    mvp[0] = 2.0f / w;
    mvp[5] = -2.0f / h;
    mvp[10] = 1.0f;
    mvp[12] = -cx * 2.0f / w;
    mvp[13] = cy * 2.0f / h;
    mvp[15] = 1.0f;
}

/* --------------------------------------------------------------- chains */

/* per-frame uniforms, resolved once at link time (the old executor
 * re-enumerated every active uniform and re-parsed the shader's JSON
 * annotations for every pass on every frame) */
typedef enum { FXU_TEXTURE, FXU_RESOLUTION, FXU_TIME, FXU_AUDIO, FXU_FRAMETIME,
               FXU_SCREEN, FXU_TEXEL, FXU_TEXEL_HALF, FXU_POINTER, FXU_DAYTIME,
               FXU_MVP_INV } FxUKind;

/* render thread: the spectrum of the frame being drawn (NULL = silence);
 * set by the scene once per frame, read by every pass */
static __thread const WalpeAudioSpectrum *fx_audio_frame;

void fx_set_audio_frame (const WalpeAudioSpectrum *spec)
{
    fx_audio_frame = spec;
}

/* render thread: the scene drawn so far (_rt_FullFrameBuffer), set by the
 * scene while it runs a compose layer's effects; 0 = none */
static __thread struct { GLuint tex; int w, h; } fx_fullframe;

void fx_set_fullframe (guint tex, int w, int h)
{
    fx_fullframe.tex = tex;
    fx_fullframe.w = w;
    fx_fullframe.h = h;
}

/* render thread: media integration album art for this frame (0 = none:
 * the pass keeps its declared default texture) */
static __thread struct { GLuint tex; int w, h; } fx_sys[3];

void fx_set_system_textures (guint thumb, int tw, int th, guint prev, int pw, int ph)
{
    fx_sys[FX_SYS_THUMB].tex = thumb;
    fx_sys[FX_SYS_THUMB].w = tw;
    fx_sys[FX_SYS_THUMB].h = th;
    fx_sys[FX_SYS_PREV].tex = prev;
    fx_sys[FX_SYS_PREV].w = pw;
    fx_sys[FX_SYS_PREV].h = ph;
}

/* render thread: per-frame scene values for builtin uniforms */
static __thread struct { int sw, sh; float px, py, dt; } fx_frame = { 1920, 1080, 0.5f, 0.5f, 1.0f / 30 };

static void fx_pool_new_frame (void);
static inline void fxs_cost_new_frame (void);   /* fx_cost.h */
/* render thread: fx_dyn_values() of this frame, per combination of inputs */
static __thread struct { guint64 v[4]; guint have; } fx_dyn_memo;

/* g_Time for the shaders. The scene clock is a double (seconds since the
 * scene was loaded, pauses excluded); a float has 24 bits: 7.8 ms steps
 * after 18 h on screen, 31 ms after 3 days (32 steps a second whatever the
 * frame rate), 125 ms after 12. Shaders use the value in sin / frac /
 * texture scrolls with speeds of their own, so no fixed period makes a
 * wrap seamless (measured: up to 92 % of the pixels of a frame change at
 * a wrap). So the time is folded back by whole hours
 *  - when nobody sees it: the first frame after the scene was not drawn
 *    for FX_TIME_GAP_US (paused: covered, monitor off, fullscreen
 *    window), once it has passed FX_TIME_SOFT_S;
 *  - and, only if the scene was shown without such a break for hours, at
 *    FX_TIME_HARD_S (one visible jump of the time-driven effects; float
 *    steps stay below 1 ms up to there).
 * A fold never goes below FX_TIME_LO_S: effects that fade in from
 * g_Time = 0 do not start again. Until the first fold (70 minutes at the
 * earliest) the value is the scene time, bit for bit. Per render thread:
 * a thread draws one scene. */
static __thread double fx_time_fold;
static __thread gint64 fx_frame_us;
static __thread gboolean fx_time_gap;

static float fx_shader_time (double time)
{
    if (time < fx_time_fold)
        fx_time_fold = 0;   /* another scene: its clock started again */
    double st = time - fx_time_fold;
    if (st >= FX_TIME_HARD_S || (fx_time_gap && st >= FX_TIME_SOFT_S)) {
        fx_time_fold += floor ((st - FX_TIME_LO_S) / FX_TIME_PERIOD_S) * FX_TIME_PERIOD_S;
        st = time - fx_time_fold;
    }
    return (float) st;
}

void fx_set_frame_info (int screen_w, int screen_h, float pointer_x, float pointer_y,
                        float frametime)
{
    gint64 now_us = g_get_monotonic_time ();
    fx_time_gap = fx_frame_us && now_us - fx_frame_us > FX_TIME_GAP_US;
    fx_frame_us = now_us;
    fx_frame.sw = MAX (1, screen_w);
    fx_frame.sh = MAX (1, screen_h);
    fx_frame.px = pointer_x;
    fx_frame.py = pointer_y;
    fx_frame.dt = frametime > 0 ? frametime : 1.0f / 30;
    fx_dyn_memo.have = 0;   /* new audio / pointer values: fx_dyn_values */
    fx_pool_new_frame ();
    fxs_cost_new_frame ();
}
/* render thread: passes drawn / skipped (served from the static cache) */
static __thread guint fx_passes_run, fx_passes_cached;

/* ---- quality knobs (common/quality.h), render thread ------------------
 * Set by the scene before it draws its layers. With the defaults (divisor
 * 1, post-processing full) nothing below takes a different path than
 * before the knobs existed. */
static __thread struct {
    int div;            /* effect update divisor asked for: 1..3 */
    gboolean all;       /* hold every animated set, not only the slow ones */
    int post;           /* WALPE_POST_* */
    double fps;         /* frames per second the scene is paced at */
    guint frame;        /* the scene's frame counter */
    gboolean compose;   /* the next run is a compose layer's (post passes) */
    guint next_phase;   /* FX_HOLD_STAGGER: spreads the held sets over the frames */
} fx_q = { 1, FALSE, WALPE_POST_FULL, 60.0, 0, FALSE, 0 };
/* Which frames held sets are redrawn on. 0: all on the same ones (frame
 * counter divisible by the divisor), so the frames in between hold every
 * such set at once - with a renderer that skips frames equal to the last
 * one (fx_set_hold_epoch) a scene in which nothing else moves then draws
 * only every Nth frame. 1: sets take turns, which spreads the effect work
 * evenly but leaves no frame without a redraw. */
#define FX_HOLD_STAGGER 0
static __thread FxAcct fx_acct;
/* quality knob fx_res, handed over for one run (as fx_layer) */
static __thread struct { gboolean set; float k_full; float keep[3]; } fx_res_run;

void fx_set_run_res (float k_full, const float keep[3])
{
    fx_res_run.set = k_full > 0;
    fx_res_run.k_full = k_full;
    memcpy (fx_res_run.keep, keep, sizeof (fx_res_run.keep));
}

void fx_set_quality (int rate_div, gboolean rate_all, int post, double fps, guint frame)
{
    fx_q.div = CLAMP (rate_div, 1, 3);
    fx_q.all = rate_all;
    fx_q.post = CLAMP (post, WALPE_POST_FULL, WALPE_POST_OFF);
    fx_q.fps = fps > 0 ? fps : 60.0;
    fx_q.frame = frame;
}

void fx_set_run_compose (gboolean on)
{
    fx_q.compose = on;
}

void fx_acct_take (FxAcct *out)
{
    if (out)
        *out = fx_acct;
    memset (&fx_acct, 0, sizeof (fx_acct));
}

typedef struct {
    GLint loc;
    guint8 kind;
    guint8 unit;
} FxUniform;

/* a constant a script may change at run time (SceneScript material
 * property / setMaterialProperty): keyed by the instance's
 * constantshadervalues name (the annotation's "material" key), else the
 * uniform name */
typedef struct {
    char key[128];
    GLint loc;
    GLenum type;
    float v[4];
    int n;
    gboolean dirty;
} FxConst;

/* fx_region.h: the part of a texture that is read later, as up to
 * FXR_MAX_RECTS rectangles u0, v0, u1, v1 in its texture coordinates */
#define FXR_MAX_RECTS 6
enum { FXR_NONE = 0, FXR_RECTS, FXR_FULL };
typedef struct {
    int mode, n;
    float r[FXR_MAX_RECTS][4];
} FxNeed;

typedef struct {
    GLuint program;
    FxUniform *dyn;                      /* textures/resolutions/time */
    FxUniLast *dyn_last;                 /* per dyn entry: the value last sent */
    FxConst *consts;                     /* scriptable constants */
    int n_consts;
    guint8 sys[FX_MAX_BINDS];            /* "usertextures" system textures:
                                          * FX_SYS_THUMB / FX_SYS_PREV */
    int n_dyn;
    char target[64];                     /* "" = final output */
    struct { int index; char name[64]; } binds[FX_MAX_BINDS];
    int n_binds;
    JsonObject *constants;               /* instance constantshadervalues */
    char *frag_body;                     /* for annotation defaults */
    char *vert_body;
    /* resolved sampler textures (material default overridden by the
     * instance's "textures" array). slot 0 stays 0 = chain input.
     * res = (paddedW, paddedH, imageW, imageH) per g_TextureNResolution */
    struct { GLuint tex; int res[4]; int rt; } slot[FX_MAX_BINDS]; /* rt: FX_RT_* */
    gboolean uses_audio;                 /* reads g_AudioSpectrum* */
    char copy_src[64];                   /* "command":"copy" pass: source target */
    gboolean is_copy;
    gboolean is_swap;                    /* "command":"swap": target and source
                                          * trade places (is_copy is set too) */
    /* vertex shaders that work on a_Position (transform / skew / foliage
     * sway in "Vertex" mode): WE feeds them the layer's quad in layer
     * units (scene pixels, y up) with an orthographic
     * g_ModelViewProjectionMatrix. moves_verts: the shader does more with
     * a_Position than the plain MVP product and is drawn that way
     * (fx_chain_run); mvp_loc: its matrix uniform */
    gboolean moves_verts;
    GLint mvp_loc;
    /* the pass may leave pixels of its target untouched: the fragment
     * shader can discard / clip, or the vertex shader does more than place
     * the unit quad over the whole target. Then the target is cleared
     * before the pass; else every pixel is written (blending is off) and
     * a clear would only write the target a second time (fx_chain_run). */
    gboolean partial;
    /* how the fragment shader reads its samplers (fx_input_traits): bit u
     * of in_straight = every lookup of unit u uses the texture coordinate
     * exactly as the vertex shader received it, so the pass reads that
     * texture texel for texel; bit u of paint_op / paint_flow = unit u is
     * a mask painted in the editor (opacity mask / flow map) that the
     * compiled shader reads */
    guint in_straight, paint_op, paint_flow;
    /* WE's transform effect in Vertex mode: FxConst index of g_Offset /
     * g_Scale / g_Direction and the sign g_Direction has in the shader
     * (0 = not that shader) */
    int xf[3], xf_sign;
    /* fx_clip.h: bit u = every lookup through g_Texture<u> is made at the
     * fragment's own texture coordinate (fx_clip_traits, at build time);
     * need = what this run must fill of the pass's target (fx_chain_plan,
     * valid while its chain is `planned`) */
    guint local_units;
    FxClipNeed need;
    /* quality knobs (effect update rate): how this pass depends on g_Time
     * (fxclass.h) and how sharply a "bounds" threshold cuts its sine */
    WalpeFxTime time;
    double sharp;
    struct FxSpec *spec;                 /* fx_specialize.h: baked constants,
                                          * shared program (NULL = neither) */
    /* fx_region.h: what the pass reads (fxr_traits, build time) and
     * the region it has to fill in the run in progress (rg_serial ==
     * fxr_serial; set by fx_run_for_object, used by fx_chain_run) */
    gboolean rg_plain;                   /* fills its target as the plain quad */
    guint8 rg_local;                     /* bit u: g_Texture<u> is only read at the
                                          * fragment's own texture coordinate */
    guint8 rg_model;                     /* FXR_M_*: reach of unit 0 known */
    int rg_c[4];                         /* FxConst indices the model's bound uses */
    guint64 rg_hash;                     /* of its compiled statements */
    guint8 rg_mask;                      /* FXR_K_*: the map that scales the model's reach */
    int rg_sup_levels;                   /* mip levels rg_sup was made for, -1 = not made */
    FxNeed rg_sup;                       /* where that map is strong (fxr_support) */
    guint8 kp_free;   /* kept passes: 0 = not looked at, 1 = no, 2 = reads nothing that changes */
    guint rg_serial;
    FxNeed rg_need;
} EffPass;

/* fx_specialize.h */
static gboolean fxs_release (EffPass *pass);

typedef struct {
    char name[64];
    GLuint fbo, tex;
    int w, h;                            /* allocated size */
    int vw, vh;                          /* full-quality size: what shaders are told */
    int scale;                           /* fbos[].scale: size = input / scale */
    int pool;                            /* tex/fbo: 0 = the chain's own, else pool
                                          * entry + 1, lent for one run (fx_pool_get) */
    int fmt;                             /* FxRtFormat from fbos[].format */
    /* kept passes (fx_keep_*): keep = the one pass that writes this
     * target reads nothing that changes from frame to frame, so the
     * target is the chain's own and holds that pass's picture until a
     * constant of the chain changes (kp_gen), the pass gets another
     * program or the target is made again */
    gboolean keep, kp_valid;
    guint kp_gen;
    GLuint kp_prog;
} EffTarget;

typedef struct {
    EffPass passes[FX_MAX_PASSES];
    int n_passes;
    EffTarget targets[FX_MAX_TARGETS];
    int n_targets;
    GLuint out_fbo, out_tex;   /* chain final output (input-sized) */
    int out_pool;              /* as EffTarget.pool */
    int alt_out;               /* index in targets[] of the second output of a
                                * chain with more than one pass without a
                                * "target" (they ping-pong), -1 = none */
    int feedback;              /* -1 = not checked; 1 = a pass reads what an
                                * earlier frame left in a target: its targets
                                * are its own for life (fx_chain_feedback) */
    int out_w, out_h;          /* allocated (working resolution) */
    int out_vw, out_vh;        /* full-quality size (uniforms) */
    /* post-processing "off": may this chain (a cosmetic family on a
     * compose layer) be left out? Only once it was measured not to move
     * the picture's level (fx_level_probe): 0 = on probation (it runs and
     * is measured behind its runs 1, 2, 4 .. 32), 1 = leaves the level
     * alone: left out, 2 = changes it: stays for good. lvl_gen = the
     * chain's own gen (its constants) a verdict of 1 belongs to (+1) */
    guint8 lvl_state, lvl_draws, lvl_skip;
    guint lvl_gen;
    gboolean ready;
    int inst_index;            /* index in the object's "effects" array */
    gboolean enabled;          /* runtime visibility (SceneScript) */
    char name[64];             /* instance "name" (IEffect.name) */
    char prof_name[48];        /* GPUPROF: short effect name of the profiler rows */
    int cacheable;             /* -1 = not checked yet; fx_chain_static() */
    gboolean vertex_affine;    /* one pass: transform effect, Vertex mode */
    guint gen;                 /* bumped when a constant or the visibility of
                                * this chain changes (static prefix cache) */
    gboolean planned;          /* "Skip hidden areas": the passes' `need` is set
                                * for the run that follows (fx_chain_plan);
                                * cleared by the run */
    int dyn;                   /* -1 = not checked yet; FX_DYN_* (fx_chain_dyn) */
    /* quality knobs (common/quality.h) */
    char file[96];             /* the effect file it was built from */
    int family;                /* WalpeFxFamily of that name */
    gboolean fast_name;        /* walpe_fx_name_fast: new picture every frame */
    gboolean time_scripted;    /* a script changed one of its constants: its
                                * speed is no longer what was read at build */
    WalpeFxTime time;          /* g_Time terms of all its passes */
    double sharp;              /* largest walpe_fx_bounds_factor of its passes */
    gboolean kp_checked;       /* fx_keep_scan () looked at the passes */
} EffectChain;

static void fx_clip_traits (EffPass *pass, const char *vsrc, const char *fsrc);

static void fx_chain_drop_targets (EffectChain *ch, GLuint keep);
static int fx_rt_format_id (const char *name);   /* fbos[].format -> FxRtFormat */

static void fx_chain_free (EffectChain *ch)
{
    for (int i = 0; i < ch->n_passes; i++) {
        if (ch->passes[i].program && !fxs_release (&ch->passes[i]))
            glDeleteProgram (ch->passes[i].program);
        g_free (ch->passes[i].dyn);
        g_free (ch->passes[i].dyn_last);
        g_free (ch->passes[i].consts);
        if (ch->passes[i].constants)
            json_object_unref (ch->passes[i].constants);
        g_free (ch->passes[i].frag_body);
        g_free (ch->passes[i].vert_body);
        for (int u = 0; u < FX_MAX_BINDS; u++)
            if (ch->passes[i].slot[u].tex)
                walpe_gl_tex_release (ch->passes[i].slot[u].tex);
    }
    fx_chain_drop_targets (ch, 0);
    g_free (ch);
}

/* load one .tex/.png for an effect slot (repeat wrap: phase/noise
 * sampling grows unbounded). res = (paddedW, paddedH, imageW, imageH) —
 * WE's g_TextureNResolution layout (CTexture::setupResolution) */
static GLuint fx_load_texture (const char *dir, const char *ref, int res[4], int *wefmt)
{
    res[0] = res[1] = res[2] = res[3] = 0;
    *wefmt = 0;
    if (!ref || !*ref)
        return 0;
    char *path = fx_tex_path (dir, ref);
    if (!path) {
        g_debug ("fx: texture %s not found", ref);
        return 0;
    }
    /* straight texels, as the shaders sample them: masks, normal maps,
     * flow/phase maps and LUTs are data, and blend textures are mixed by
     * their own alpha in the shader. Premultiplying (as layer art drawn
     * directly is) scaled rgb by alpha - a DXT5 normal map keeps x in
     * alpha, an RGBA mask painted on a transparent canvas turned black. */
    /* A DXT file stays block compressed (it used to be decoded to RGBA8:
     * 4-8 times the memory); its mip chain is then the one stored in the
     * file. WALPE_TEX_DXT_FX=0 decodes as before. */
    static int dxt = -1;
    if (dxt < 0) {
        const char *e = g_getenv ("WALPE_TEX_DXT_FX");
        dxt = walpe_gl_has_s3tc () && !g_getenv ("WALPE_NO_DXT") && !(e && *e == '0');
    }
    WalpeTexInfo ti;   /* shared with the other monitors' contexts */
    GLuint tex = walpe_gl_tex_cached (path, WALPE_TEX_LOAD_NATIVE_RG |
                                      (dxt ? WALPE_TEX_LOAD_NATIVE_DXT : 0), GL_REPEAT, FALSE,
                                      &ti);
    g_free (path);
    if (!tex)
        return 0;
    /* effect textures (noise, gradients, masks) are sampled at whatever
     * scale the shader asks for: build the mip chain their file calls for */
    if (ti.want_mips)
        walpe_gl_tex_ensure_mipmaps (tex);
    res[0] = ti.full_width;
    res[1] = ti.full_height;
    res[2] = ti.width;
    res[3] = ti.height;
    *wefmt = ti.we_format;
    return tex;
}

/* parse a material/instance "textures" member (array of string|null)
 * entry N; returns the ref string or NULL (null / framebuffer slots) */
static const char *fx_slot_ref (JsonNode *tn, int n)
{
    if (!tn || json_node_get_node_type (tn) != JSON_NODE_ARRAY)
        return NULL;
    JsonArray *a = json_node_get_array (tn);
    if ((guint) n >= json_array_get_length (a))
        return NULL;
    JsonNode *e = json_array_get_element (a, (guint) n);
    if (!e || json_node_get_node_type (e) != JSON_NODE_VALUE)
        return NULL;                     /* null slot = chain input */
    const char *s = json_node_get_string (e);
    if (!s || !*s || !strcmp (s, "framebuffer") || !strcmp (s, "previous"))
        return NULL;
    return s;
}

/* Quality knobs, effect update rate: the size of an identifier a g_Time
 * term is multiplied with - the instance's constant (keyed by the
 * annotation's "material" name; a user property's value when bound), else
 * the annotation default. Looked up in the sources, not among the active
 * uniforms: a term in a branch the combos switched off still counts (the
 * larger speed wins, which only makes a chain "faster"). */
static gboolean fx_time_value (const char *ident, double *mag, gpointer user)
{
    EffPass *pass = user;
    char *mkey = fx_shader_anno_t (pass->frag_body, ident, "material", NULL);
    if (!mkey)
        mkey = fx_shader_anno_t (pass->vert_body, ident, "material", NULL);
    char *val = NULL;
    const char *keys[2] = { mkey ? mkey : ident, mkey ? ident : NULL };
    for (int i = 0; i < 2 && !val && keys[i] && pass->constants; i++) {
        JsonNode *n = json_object_get_member (pass->constants, keys[i]);
        if (n && json_node_get_node_type (n) == JSON_NODE_OBJECT) {
            /* {"user": propid, "value": fallback}; a script on it makes
             * the speed unknown */
            JsonObject *b = json_node_get_object (n);
            if (json_object_has_member (b, "script") || json_object_has_member (b, "animation")) {
                g_free (mkey);
                return FALSE;
            }
            const char *up = wpj_str (b, "user", NULL);
            JsonNode *pv = up && fx_user_props ? json_object_get_member (fx_user_props, up) : NULL;
            val = pv ? fx_json_valstr (pv) : NULL;
            if (!val)
                val = fx_json_valstr (json_object_get_member (b, "value"));
        } else if (n) {
            val = fx_json_valstr (n);
        }
    }
    if (!val)
        val = fx_shader_anno_t (pass->frag_body, ident, "default", NULL);
    if (!val)
        val = fx_shader_anno_t (pass->vert_body, ident, "default", NULL);
    g_free (mkey);
    if (!val)
        return FALSE;
    float v[4] = { 0 };
    int n = fx_scan_floats (val, v);
    g_free (val);
    if (n < 1)
        return FALSE;
    double m = 0;
    for (int i = 0; i < n; i++)
        m = MAX (m, fabs ((double) v[i]));
    *mag = m;
    return TRUE;
}

static void fx_pass_time (EffPass *pass)
{
    memset (&pass->time, 0, sizeof (pass->time));
    walpe_fx_time_scan (pass->frag_body, fx_time_value, pass, &pass->time);
    walpe_fx_time_scan (pass->vert_body, fx_time_value, pass, &pass->time);
    pass->sharp = 1;
    /* "bounds" (pulse, shake, swing ...): smoothstep (lo, hi, sine) */
    for (int k = 0; k < pass->n_consts; k++)
        if (!strcmp (pass->consts[k].key, "bounds") && pass->consts[k].n >= 2)
            pass->sharp = MAX (pass->sharp, walpe_fx_bounds_factor (pass->consts[k].v[0],
                                                                    pass->consts[k].v[1]));
}

/* link-time uniform setup: constants (instance constantshadervalues, then
 * annotation defaults), sampler units, identity MVP and silent audio are
 * program state - set ONCE here. Only textures, resolutions and g_Time
 * change per frame; those go into pass->dyn. */
static void fx_cache_uniforms (EffPass *pass)
{
    GLint n = 0;
    glUseProgram (pass->program);
    glGetProgramiv (pass->program, GL_ACTIVE_UNIFORMS, &n);
    pass->dyn = g_new0 (FxUniform, MAX (n, 1));
    pass->dyn_last = g_new0 (FxUniLast, MAX (n, 1));
    pass->n_dyn = 0;
    for (GLint u = 0; u < n; u++) {
        char name[128];
        GLsizei len = 0;
        GLint size = 0;
        GLenum type = 0;
        glGetActiveUniform (pass->program, (GLuint) u, sizeof (name), &len,
                            &size, &type, name);
        char *br = strchr (name, '['); /* arrays: element 0 */
        if (br)
            *br = 0;
        GLint loc = glGetUniformLocation (pass->program, name);
        if (loc < 0)
            continue;
        if (!strncmp (name, "g_Texture", 9) && name[9] >= '0' && name[9] <= '7') {
            int unit = name[9] - '0';
            const char *rest = name + 10;
            if (!strcmp (rest, "Resolution")) {
                pass->dyn[pass->n_dyn++] = (FxUniform) { loc, FXU_RESOLUTION, (guint8) unit };
            } else if (!*rest) {
                glUniform1i (loc, unit);
                pass->dyn[pass->n_dyn++] = (FxUniform) { loc, FXU_TEXTURE, (guint8) unit };
            }
        } else if (!strcmp (name, "g_Time")) {
            pass->dyn[pass->n_dyn++] = (FxUniform) { loc, FXU_TIME, 0 };
        } else if (!strcmp (name, "g_Frametime")) {
            pass->dyn[pass->n_dyn++] = (FxUniform) { loc, FXU_FRAMETIME, 0 };
        } else if (!strcmp (name, "g_Screen")) {
            pass->dyn[pass->n_dyn++] = (FxUniform) { loc, FXU_SCREEN, 0 };
        } else if (!strcmp (name, "g_TexelSize")) {
            pass->dyn[pass->n_dyn++] = (FxUniform) { loc, FXU_TEXEL, 0 };
        } else if (!strcmp (name, "g_TexelSizeHalf")) {
            pass->dyn[pass->n_dyn++] = (FxUniform) { loc, FXU_TEXEL_HALF, 0 };
        } else if (!strcmp (name, "g_PointerPosition") || !strcmp (name, "g_PointerPositionLast") ||
                   !strcmp (name, "g_ParallaxPosition")) {
            pass->dyn[pass->n_dyn++] = (FxUniform) { loc, FXU_POINTER, 0 };
        } else if (!strcmp (name, "g_Daytime")) {
            pass->dyn[pass->n_dyn++] = (FxUniform) { loc, FXU_DAYTIME, 0 };
        } else if (!strncmp (name, "g_AudioSpectrum", 15)) {
            /* g_AudioSpectrum{16,32,64}{Left,Right}[N]: unit encodes the
             * array (0..2 = 16/32/64 left, 3..5 = right) */
            const char *r = name + 15;
            int sz = !strncmp (r, "16", 2) ? 0 : !strncmp (r, "32", 2) ? 1
                   : !strncmp (r, "64", 2) ? 2 : -1;
            int right = strstr (r, "Right") != NULL;
            if (sz >= 0) {
                pass->dyn[pass->n_dyn++] = (FxUniform) { loc, FXU_AUDIO,
                                                         (guint8) (sz + 3 * right) };
                pass->uses_audio = TRUE;
            }
        } else if (!strcmp (name, "g_ModelViewProjectionMatrix")) {
            /* passes run in texture space; the caller composites the chain
             * output with the object transform. Identity with the unit
             * quad; a pass that moves its vertices gets the layer quad and
             * the layer's orthographic matrix per draw (fx_chain_run,
             * moves_verts). g_EffectModelViewProjectionMatrix (the layer
             * on screen, for _rt_FullFrameBuffer lookups) is identity too
             * (generic matrix branch below): the layer's own rect stands
             * in for the screen. */
            static const float ident[16] = { 1, 0, 0, 0,  0, 1, 0, 0,
                                             0, 0, 1, 0,  0, 0, 0, 1 };
            glUniformMatrix4fv (loc, 1, GL_FALSE, ident);
        } else if (!strcmp (name, "g_ModelViewProjectionMatrixInverse") ||
                   !strcmp (name, "g_EffectModelViewProjectionMatrixInverse")) {
            /* clip space -> layer units (scene pixels, y up), set per
             * pass: what a shader gets when it takes a clip-space point
             * (the cursor) back into the layer */
            pass->dyn[pass->n_dyn++] = (FxUniform) { loc, FXU_MVP_INV, 0 };
        } else if (!strcmp (name, "g_TextureReductionScale")) {
            /* texture quality setting: textures are never reduced here */
            fx_set_uniform (loc, type, "1");
        } else {
            /* instance constants first (keyed by the annotation's
             * "material" name), then annotation defaults; both vert and
             * frag bodies carry annotations */
            /* a name the two stages declare with different types is two
             * uniforms (fx_uniform_split_repair): the fragment one carries
             * a prefix and has its annotation in the fragment body under
             * the declared name */
            const char *aname = name, *fbody = pass->frag_body, *vbody = pass->vert_body;
            if (g_str_has_prefix (name, FX_FRAG_UNIFORM_PREFIX)) {
                aname = name + strlen (FX_FRAG_UNIFORM_PREFIX);
                vbody = NULL;
            }
            char *mkey = fx_uniform_anno (fbody, vbody, aname, "material", type);
            char *val = fx_const_val (pass->constants, mkey ? mkey : aname);
            if (!val && mkey)
                val = fx_const_val (pass->constants, aname);
            if (!val)
                val = fx_uniform_anno (fbody, vbody, aname, "default", type);
            if (type == GL_FLOAT || type == GL_FLOAT_VEC2 || type == GL_FLOAT_VEC3 ||
                type == GL_FLOAT_VEC4 || type == GL_INT || type == GL_BOOL) {
                if (!pass->consts)
                    pass->consts = g_new0 (FxConst, MAX (n, 1));
                FxConst *fc = &pass->consts[pass->n_consts++];
                snprintf (fc->key, sizeof (fc->key), "%s", mkey ? mkey : aname);
                fc->loc = loc;
                fc->type = type;
                fc->n = type == GL_FLOAT_VEC2 ? 2 : type == GL_FLOAT_VEC3 ? 3
                      : type == GL_FLOAT_VEC4 ? 4 : 1;
                fc->v[3] = 1;
                if (val)
                    fx_scan_floats (val, fc->v);
            }
            if (val) {
                fx_set_uniform (loc, type, val);
                g_free (val);
            } else if (type == GL_FLOAT_MAT4 || type == GL_FLOAT_MAT3) {
                /* engine matrices (model/view/projection/effect texture
                 * projection and inverses): passes run in texture space */
                static const float id4[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
                static const float id3[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
                if (type == GL_FLOAT_MAT4)
                    glUniformMatrix4fv (loc, 1, GL_FALSE, id4);
                else
                    glUniformMatrix3fv (loc, 1, GL_FALSE, id3);
            } else if (!strncmp (name, "g_Texture", 9) && strstr (name, "Rotation") &&
                       type == GL_FLOAT_VEC4) {
                glUniform4f (loc, 1, 0, 0, 1); /* 2x2 identity */
            } else if (!strcmp (name, "g_Alpha") || !strcmp (name, "g_Brightness") ||
                       !strcmp (name, "g_UserAlpha")) {
                fx_set_uniform (loc, type, "1");
            } else if (!strcmp (name, "g_Color") || !strcmp (name, "g_Color4")) {
                fx_set_uniform (loc, type, "1 1 1 1");
            }
            g_free (mkey);
        }
    }
    glUseProgram (0);
    fx_pass_time (pass);   /* reads the sources and constants: before they go */
    /* sources/constants were only needed for the values above */
    g_clear_pointer (&pass->frag_body, g_free);
    g_clear_pointer (&pass->vert_body, g_free);
    g_clear_pointer (&pass->constants, json_object_unref);
}

/* workshop id of the wallpaper being built (zcompat "maximumprojectid") */
static __thread guint64 fx_project_id;

void fx_set_project_id (guint64 id)
{
    fx_project_id = id;
}

/* WE's own compatibility replacements for known-broken workshop shaders:
 * assets/zcompat/scene/shaders/<effect workshop id>/{config.json,*.frag,
 * *.vert}, used for wallpapers whose id is <= config "maximumprojectid"
 * (newer uploads carry the fixed shader). shader = "workshop/<id>/.../x". */
static gboolean fx_zcompat (const char *shader, char **vp, char **fp)
{
    const char *we = walpe_we_assets_dir ();
    if (!we || !shader)
        return FALSE;
    const char *last = NULL;
    for (const char *p = shader; (p = strstr (p, "workshop/")) != NULL; p += 9)
        last = p + 9;
    if (!last)
        return FALSE;
    gsize nd = strspn (last, "0123456789");
    if (nd == 0 || last[nd] != '/')
        return FALSE;
    char *wid = g_strndup (last, nd);
    char *cdir = g_build_filename (we, "zcompat", "scene", "shaders", wid, NULL);
    char *cfg = g_build_filename (cdir, "config.json", NULL);
    g_free (wid);
    JsonNode *cj = wpj_parse_file (cfg);
    g_free (cfg);
    gboolean ok = FALSE;
    if (cj && JSON_NODE_HOLDS_OBJECT (cj)) {
        JsonObject *co = json_node_get_object (cj);
        const char *maxs = wpj_str (co, "maximumprojectid", NULL);
        guint64 maxid = maxs ? g_ascii_strtoull (maxs, NULL, 10) : G_MAXUINT64;
        const char *base = strrchr (shader, '/') ? strrchr (shader, '/') + 1 : shader;
        char *want_f = g_strconcat (base, ".frag", NULL);
        const char *ff = wpj_str (co, "frag", NULL), *vv = wpj_str (co, "vert", NULL);
        if (fx_project_id && fx_project_id <= maxid && ff && vv && !g_strcmp0 (ff, want_f) &&
            !strchr (ff, '/') && !strchr (vv, '/')) {
            char *f2 = g_build_filename (cdir, ff, NULL), *v2 = g_build_filename (cdir, vv, NULL);
            if (g_file_test (f2, G_FILE_TEST_IS_REGULAR) && g_file_test (v2, G_FILE_TEST_IS_REGULAR)) {
                g_free (*vp);
                g_free (*fp);
                *vp = v2;
                *fp = f2;
                ok = TRUE;
            } else {
                g_free (f2);
                g_free (v2);
            }
        }
        g_free (want_f);
    }
    if (cj)
        json_node_unref (cj);
    g_free (cdir);
    return ok;
}

/* repair, compile and link one pass's sources (repaired in place);
 * 0 when it cannot be built (logged) */
static GLuint fx_build_program (char **vsrc, char **fsrc, const char *shader,
                                const char *dbgname)
{
    GLuint prog = 0;
    /* HLSL-semantics repairs, decided from the sources (fx_srcrepair.h) */
    fx_prepass (vsrc, fsrc, shader);
    gint64 vst = 0, fst = 0;
    if (!fx_expansion_sizes_once (*vsrc, &vst) || !fx_expansion_sizes_once (*fsrc, &fst)) {
        g_warning ("fx: shader %s (%s): its macros expand to more than %d tokens, not compiled",
                   shader, dbgname, FX_MAX_EXPANSION);
        return 0;
    }
    if (MAX (vst, fst) > FX_MAX_STATEMENT) {
        g_warning ("fx: shader %s (%s): a statement of %" G_GINT64_FORMAT " tokens (more than %d), not compiled",
                   shader, dbgname, MAX (vst, fst), FX_MAX_STATEMENT);
        return 0;
    }
    for (int round = 0; round < 2; round++) {
        GLuint vs = fx_compile (vsrc, GL_VERTEX_SHADER, shader);
        GLuint fs = vs ? fx_compile (fsrc, GL_FRAGMENT_SHADER, shader) : 0;
        if (!vs || !fs) {
            glDeleteShader (vs);
            glDeleteShader (fs);
            break;
        }
        prog = glCreateProgram ();
        glAttachShader (prog, vs);
        glAttachShader (prog, fs);
        /* fixed attribute layout matching the pass VAO (a_Position vec3 ,
         * a_TexCoord vec2 ) — without this the linker assigns arbitrary
         * locations and the quad geometry becomes garbage */
        glBindAttribLocation (prog, 0, "a_Position");
        glBindAttribLocation (prog, 1, "a_TexCoord");
        glLinkProgram (prog);
        glDeleteShader (vs);
        glDeleteShader (fs);
        GLint ok = 0;
        glGetProgramiv (prog, GL_LINK_STATUS, &ok);
        if (ok)
            break;
        char log[1024];
        glGetProgramInfoLog (prog, sizeof (log), NULL, log);
        glDeleteProgram (prog);
        prog = 0;
        /* the interface check of fx_prepass() found nothing it could
         * prove; whatever the linker says (the text is not read), widen
         * the narrower side of every varying declared with two widths
         * and link once more */
        if (round > 0 || !fx_varying_repair (vsrc, fsrc, TRUE)) {
            g_warning ("fx: %s link failed: %.400s", dbgname, log);
            break;
        }
    }
    return prog;
}

/* What a pass's vertex shader does with a_Position, read from the
 * compiled part of its source (fx_active_code: the lines the combos
 * leave, comments and blanks removed) - no wallpaper or effect ids:
 *  - WE's own passes either hand a_Position through ("gl_Position =
 *    vec4(a_Position, 1.0)": the unit quad of an effect-internal target)
 *    or multiply it with g_ModelViewProjectionMatrix, possibly through a
 *    local copy. Anything else -> moves_verts: the pass is drawn with the
 *    layer's quad in layer units (skew / foliage sway / transform in
 *    Vertex mode add offsets in scene pixels to the position);
 *  - WE's transform effect in Vertex mode, recognised by its three
 *    statements -> xf_sign: fx_layer_affine() can stand in for the pass. */
/* The clear rule (EffPass.partial) needs more than "a_Position is only
 * used in the MVP product": the vertex shader must write gl_Position
 * exactly once, with exactly that product (or the plain copy). One that
 * scales or offsets the result afterwards (gl_Position.xy *= ...,
 * gl_Position += ...), multiplies the product by something or uses
 * another matrix may not cover its target; it keeps the clear. Reading
 * gl_Position is fine. (None of the 603 vertex shaders of the
 * test library does it.)
 * t = fx_active_code() of the source. */
static gboolean fx_vertex_plain_cover (const char *t)
{
    static const char *full[] = { "gl_Position=mul(vec4(a_Position,1.0),g_ModelViewProjectionMatrix);",
                                  "gl_Position=mul(vec4(a_Position,1),g_ModelViewProjectionMatrix);",
                                  "gl_Position=vec4(a_Position,1.0);",
                                  "gl_Position=vec4(a_Position,1);" };
    int writes = 0;
    gboolean plain = FALSE;
    for (const char *p = t; (p = strstr (p, "gl_Position")) != NULL; p += 11) {
        const char *q = p + 11;
        if (*q == '.')
            for (q++; *q && strchr ("xyzwrgbastpq", *q); q++)
                ;
        gboolean write = (*q == '=' && q[1] != '=') ||
                         (*q && strchr ("+-*/", *q) && q[1] == '=');
        if (!write)
            continue;
        writes++;
        for (guint i = 0; i < G_N_ELEMENTS (full); i++)
            plain |= g_str_has_prefix (p, full[i]);
        /* ... or that product with a local copy of the position (the two
         * spellings of fx_keys.h), and nothing after it */
        if (g_str_has_prefix (p, "gl_Position=")) {
            gsize l = 0;
            if ((fxk_at (p + 12, FXK_MVP_LOCAL, &l) || fxk_at (p + 12, FXK_MVP_LOCAL_INT, &l)) &&
                p[12 + l] == ';')
                plain = TRUE;
        }
    }
    return writes == 1 && plain;
}

static void fx_vertex_traits (EffPass *pass, const char *vsrc)
{
    static const char *plain[] = { "attributevec3a_Position;", "invec3a_Position;",
                                   "mul(vec4(a_Position,1.0),g_ModelViewProjectionMatrix)",
                                   "mul(vec4(a_Position,1),g_ModelViewProjectionMatrix)",
                                   "gl_Position=vec4(a_Position,1.0);",
                                   "gl_Position=vec4(a_Position,1);" };
    char *t = fx_active_code (vsrc);
    int uses = 0, local = 0;
    for (const char *p = t; (p = strstr (p, "a_Position")) != NULL; p += 10)
        uses++;
    for (guint i = 0; i < G_N_ELEMENTS (plain); i++)
        for (const char *p = t; (p = strstr (p, plain[i])) != NULL; p += strlen (plain[i]))
            uses--;
    {
        /* the declaration of a local copy of the position (fx_keys.h) */
        gsize l = 0;
        for (const char *p = t; (p = fxk_find (p, FXK_POS_LOCAL, &l)) != NULL; p += l)
            uses--;
    }
    /* the local copy: declared and used once in the MVP product */
    for (const char *p = t; (p = strstr (p, "position")) != NULL; p += 8)
        local++;
    if (fxk_has (t, FXK_POS_LOCAL) &&
        (local != 2 || !(fxk_has (t, FXK_MVP_LOCAL) || fxk_has (t, FXK_MVP_LOCAL_INT))))
        uses++;
    pass->moves_verts = uses > 0;
    /* anything but the plain MVP product may leave part of the target
     * uncovered (also when fx_vertex_setup later draws the pass with the
     * unit quad after all) */
    if (uses > 0 || !fx_vertex_plain_cover (t))
        pass->partial = TRUE;
    pass->xf_sign = 0;
    /* the transform effect: the position goes through its function, which
     * rotates about the centre, offsets and scales (fx_keys.h) */
    if (fxk_has (t, FXK_XF_APPLY) && fxk_has (t, FXK_XF_RETURN)) {
        if (fxk_has (t, FXK_XF_ROT_NEG))
            pass->xf_sign = -1;
        else if (fxk_has (t, FXK_XF_ROT_POS))
            pass->xf_sign = 1;
    }
    g_free (t);
}

/* The program side of fx_vertex_traits(), once the pass is linked and
 * its constants are cached: the matrix uniform of a pass that moves its
 * vertices, and the transform effect's three constants. */
static void fx_vertex_setup (EffPass *pass)
{
    static const char *xf_names[3] = { "g_Offset", "g_Scale", "g_Direction" };
    for (int i = 0; i < 3; i++) {
        GLint loc = glGetUniformLocation (pass->program, xf_names[i]);
        pass->xf[i] = -1;
        for (int k = 0; loc >= 0 && k < pass->n_consts; k++)
            if (pass->consts[k].loc == loc)
                pass->xf[i] = k;
    }
    pass->mvp_loc = glGetUniformLocation (pass->program, "g_ModelViewProjectionMatrix");
    if (!pass->moves_verts)
        return;
    /* the layer-unit quad needs the pass to project with that one matrix
     * and nothing else (model / view / effect matrices stay identity) */
    GLint n = 0;
    int mats = 0;
    glGetProgramiv (pass->program, GL_ACTIVE_UNIFORMS, &n);
    for (GLint u = 0; u < n; u++) {
        char name[128];
        GLint size = 0;
        GLenum type = 0;
        glGetActiveUniform (pass->program, (GLuint) u, sizeof (name), NULL, &size, &type, name);
        if (type == GL_FLOAT_MAT4 || type == GL_FLOAT_MAT3 || type == GL_FLOAT_MAT2)
            mats++;
    }
    if (pass->mvp_loc < 0 || mats != 1)
        pass->moves_verts = FALSE;
}

/* A fragment shader that can drop fragments (discard, or clip() of the
 * prelude) leaves the cleared colour there: such a pass keeps the clear
 * of its target (EffPass.partial). The body is searched as text, dead
 * #if branches and comments included: a false hit only keeps the clear. */
static gboolean fx_fragment_may_discard (const char *fbody)
{
    return strstr (fbody, "discard") != NULL || strstr (fbody, "clip") != NULL;
}

/* fx_region.h: passes restricted to the pixels that are read later */
static EffTarget *fx_find_target (EffectChain *ch, const char *name);
static int fx_chain_tdiv (const EffectChain *ch);
#include "fx_region.h"

#include "fx_specialize.h"

/* ---- how a pass reads its input (fx_input_displaced) ------------------
 * A pass whose shader samples a texture at `v_TexCoord` / `v_TexCoord.xy`,
 * with that varying written once in the vertex shader, straight from
 * a_TexCoord, reads the texture texel for texel: tint, opacity, blend,
 * colour key, pulse. Every other lookup - a coordinate with an offset
 * (shake, water waves, chromatic aberration), another varying (blur taps),
 * a sampler handed to a function - may land between texels. vt / ft =
 * fx_active_code () of the built sources (no white space, no dead
 * branches). Unknown spellings count as "not straight": the caller then
 * only keeps a texture larger than it had to. */
static gboolean fx_vertex_plain_texcoord (const char *vt)
{
    static const char *plain[] = { "v_TexCoord=a_TexCoord;", "v_TexCoord=a_TexCoord.xy;",
                                   "v_TexCoord=a_TexCoord.xyxy;", "v_TexCoord.xy=a_TexCoord;",
                                   "v_TexCoord.xy=a_TexCoord.xy;" };
    int writes = 0;
    for (const char *p = vt; (p = strstr (p, "v_TexCoord")) != NULL; p += 10) {
        const char *q = p + 10;
        if (g_ascii_isalnum (*q) || *q == '_')
            continue;   /* v_TexCoordMask, v_TexCoordKernel ... */
        gboolean zw = FALSE;
        if (*q == '.') {
            const char *sw = ++q;
            while (*q && strchr ("xyzwrgbastpq", *q))
                q++;
            zw = q - sw == 2 && sw[0] == 'z' && sw[1] == 'w';
        } else if (*q == '[') {
            return FALSE;   /* an array of coordinates */
        }
        gboolean write = (*q == '=' && q[1] != '=') ||
                         (*q && strchr ("+-*/", *q) && q[1] == '=');
        if (!write || zw)
            continue;   /* read, or the second pair (mask coordinates) */
        gboolean ok = FALSE;
        for (guint i = 0; i < G_N_ELEMENTS (plain); i++)
            ok |= g_str_has_prefix (p, plain[i]);
        if (!ok)
            return FALSE;
        writes++;
    }
    return writes == 1;
}

/* *used: the units the compiled fragment shader mentions beyond their
 * declaration */
static guint fx_fragment_straight_units (const char *ft, guint *used)
{
    guint straight = 0;
    *used = 0;
    for (int u = 0; u < FX_MAX_BINDS; u++) {
        char name[24];
        int nl = snprintf (name, sizeof (name), "g_Texture%d", u);
        int lookups = 0;
        gboolean ok = TRUE;
        for (const char *p = ft; (p = strstr (p, name)) != NULL; p += nl) {
            const char *q = p + nl;
            if (g_ascii_isalnum (*q) || *q == '_' || *q == ';')
                continue;   /* g_Texture0Resolution, g_Texture10, the declaration */
            *used |= 1u << u;
            if (*q == ',' && (g_str_has_prefix (q + 1, "v_TexCoord.xy)") ||
                              g_str_has_prefix (q + 1, "v_TexCoord)")))
                lookups++;
            else
                ok = FALSE;
        }
        if (ok && lookups > 0)
            straight |= 1u << u;
    }
    return straight;
}

static void fx_input_traits (EffPass *pass, const char *vsrc, const char *fsrc, const char *fbody)
{
    char *vt = fx_active_code (vsrc), *ft = fx_active_code (fsrc);
    guint used = 0;
    guint straight = fx_fragment_straight_units (ft, &used);
    pass->in_straight = !pass->moves_verts && fx_vertex_plain_texcoord (vt) ? straight : 0;
    pass->paint_op = pass->paint_flow = 0;
    for (int u = 1; u < FX_MAX_BINDS; u++) {
        if (!(used & (1u << u)))
            continue;
        char name[24];
        snprintf (name, sizeof (name), "g_Texture%d", u);
        char *mode = fx_shader_anno_t (fbody, name, "mode", NULL);
        if (!g_strcmp0 (mode, "opacitymask"))
            pass->paint_op |= 1u << u;
        else if (!g_strcmp0 (mode, "flowmask"))
            pass->paint_flow |= 1u << u;
        g_free (mode);
    }
    g_free (vt);
    g_free (ft);
}

/* build one pass: material json -> shader + combos + textures */
static gboolean fx_build_pass (EffPass *pass, JsonObject *matobj, const char *dir,
                               JsonObject *passdef, JsonObject *instpass,
                               const char *dbgname)
{
    memset (pass, 0, sizeof (*pass));
    pass->target[0] = 0;
    JsonObject *p0 = wpj_arr_obj (wpj_arr (matobj, "passes"), 0);
    if (!p0)
        return FALSE;
    const char *shader = wpj_str (p0, "shader", NULL);
    if (!shader)
        return FALSE;

    /* WE shader refs are relative to a shaders/ dir (pkg, builtin effect
     * dir or the install); try the bare spelling as well */
    char *vpath = g_strconcat ("shaders/", shader, ".vert", NULL);
    char *fpath = g_strconcat ("shaders/", shader, ".frag", NULL);
    char *vp = fx_resolve (dir, vpath);
    char *fp = fx_resolve (dir, fpath);
    g_free (vpath);
    g_free (fpath);
    if (!vp || !fp) {
        g_free (vp);
        g_free (fp);
        vpath = g_strconcat (shader, ".vert", NULL);
        fpath = g_strconcat (shader, ".frag", NULL);
        vp = fx_resolve (dir, vpath);
        fp = fx_resolve (dir, fpath);
        g_free (vpath);
        g_free (fpath);
    }
    if (!vp || !fp) {
        g_warning ("fx: %s: shader %s not found (project, effect dir, WE assets %s)",
                   dbgname, shader, walpe_we_assets_dir () ? "searched" : "not found");
        g_free (vp);
        g_free (fp);
        return FALSE;
    }
    if (fx_zcompat (shader, &vp, &fp))
        g_message ("fx: %s: using WE zcompat replacement %s", dbgname, fp);
    char *vraw = fx_read_shader (vp), *fraw = fx_read_shader (fp);
    /* includes resolve against the shader's dir: take it BEFORE freeing
     * fp (the old order read freed memory - ASan heap-use-after-free) */
    char *sdir = g_path_get_dirname (fp);
    g_free (vp);
    g_free (fp);
    char *vbody = NULL, *fbody = NULL;
    if (vraw && fraw) {
        for (int k = 0; k < 2; k++) {
            char *h = fx_hoist_decls (k ? fraw : vraw);
            GString *o = g_string_new (NULL);
            GHashTable *seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
            fx_expand_into (o, h, sdir, dir, seen, 0);
            g_hash_table_unref (seen);
            g_free (h);
            if (k)
                fbody = g_string_free (o, FALSE);
            else
                vbody = g_string_free (o, FALSE);
        }
    }
    g_free (vraw);
    g_free (fraw);
    g_free (sdir);
    if (!vbody || !fbody) {
        g_free (vbody);
        g_free (fbody);
        return FALSE;
    }

    /* sampler slots: instance "textures" override material defaults, then
     * the shader annotation's "default"; slot 0 = the chain input
     * ("previous"/framebuffer). Loaded before compiling: bound textures
     * switch their combos (MASK ...) and TEXnFORMAT. */
    JsonNode *mtn = json_object_get_member (p0, "textures");
    JsonNode *itn = instpass ? json_object_get_member (instpass, "textures") : NULL;
    int slot_fmt[FX_MAX_BINDS] = { 0 };
    gboolean slot_set[FX_MAX_BINDS] = { FALSE };
    for (int u = 1; u < FX_MAX_BINDS; u++) {
        const char *ref = fx_slot_ref (itn, u);
        if (!ref)
            ref = fx_slot_ref (mtn, u);
        char *dref = NULL;
        if (ref) {
            slot_set[u] = TRUE;
        } else {
            dref = fx_sampler_default (fbody, u);
            if (!dref)
                dref = fx_sampler_default (vbody, u);
            ref = dref;
        }
        if (ref && !strncmp (ref, "_alias_", 7)) {
            /* engine-provided textures of WE's lighting (_alias_lightCookie,
             * the shadow atlas): not files, not rendered here - the slot
             * stays empty instead of a "texture not found" warning */
            g_debug ("fx: %s slot %d <- %s (engine texture, not provided)", dbgname, u, ref);
            g_free (dref);
            continue;
        }
        if (ref && !strncmp (ref, "_rt_", 4)) {
            /* engine render targets, bound at run time:
             * _rt_imageLayerComposite_<id>_a = the layer's image as it
             * enters this effect (WE ping-pongs the layer through _a/_b);
             * _rt_FullFrameBuffer = the scene drawn so far (compose
             * layers), else the effect input */
            pass->slot[u].rt = !strncmp (ref, "_rt_FullFrameBuffer", 19) ? FX_RT_FULLFRAME
                                                                       : FX_RT_INPUT;
            g_debug ("fx: %s slot %d <- %s (render target)", dbgname, u, ref);
            g_free (dref);
            continue;
        }
        if (ref) {
            pass->slot[u].tex = fx_load_texture (dir, ref, pass->slot[u].res, &slot_fmt[u]);
            if (pass->slot[u].tex)
                g_debug ("fx: %s slot %d <- %s (%dx%d img, %dx%d padded)", dbgname, u, ref,
                         pass->slot[u].res[2], pass->slot[u].res[3],
                         pass->slot[u].res[0], pass->slot[u].res[1]);
            else
                g_warning ("fx: %s: texture %s (slot %d) not found", dbgname, ref, u);
        }
        g_free (dref);
    }

    /* "usertextures": [null, {"name": "$mediaThumbnail", "type": "system"}]
     * binds WE system textures over a slot while they exist */
    JsonArray *uta = instpass ? wpj_arr (instpass, "usertextures") : NULL;
    for (guint u = 0; u < wpj_arr_len (uta) && u < FX_MAX_BINDS; u++) {
        JsonObject *ut = wpj_arr_obj (uta, u);
        const char *un = ut ? wpj_str (ut, "name", NULL) : NULL;
        if (!g_strcmp0 (un, "$mediaThumbnail"))
            pass->sys[u] = FX_SYS_THUMB;
        else if (!g_strcmp0 (un, "$mediaPreviousThumbnail"))
            pass->sys[u] = FX_SYS_PREV;
        if (pass->sys[u])
            g_debug ("fx: %s slot %u <- system texture %s", dbgname, u, un);
    }

    /* combos: shader [COMBO] defaults < material < instance; texture
     * combos and TEXnFORMAT from the bound slots */
    GHashTable *combos = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    fx_collect_combo_defaults (vbody, combos);
    fx_collect_combo_defaults (fbody, combos);
    for (int u = 1; u < FX_MAX_BINDS; u++) {
        if (!slot_set[u])
            continue;
        char *c = fx_sampler_combo (fbody, u);
        if (!c)
            c = fx_sampler_combo (vbody, u);
        if (c)
            g_hash_table_insert (combos, c, GINT_TO_POINTER (1));
    }
    fx_combos_overlay (combos, wpj_obj (p0, "combos"));
    fx_combos_overlay (combos, instpass ? wpj_obj (instpass, "combos") : NULL);
    for (int u = 0; u < FX_MAX_BINDS; u++) {
        char k[16];
        snprintf (k, sizeof (k), "TEX%dFORMAT", u);
        if (!g_hash_table_contains (combos, k))
            g_hash_table_insert (combos, g_strdup (k), GINT_TO_POINTER (slot_fmt[u]));
    }

    char *vsrc = fx_build_source (vbody, combos, TRUE);
    char *fsrc = fx_build_source (fbody, combos, FALSE);
    fx_vertex_traits (pass, vsrc);
    fx_input_traits (pass, vsrc, fsrc, fbody);
    fxr_traits (pass, vsrc, fsrc, shader, combos);   /* fx_region.h */
    fx_clip_traits (pass, vsrc, fsrc);
    if (fx_fragment_may_discard (fbody))
        pass->partial = TRUE;
    g_hash_table_unref (combos);
    pass->vert_body = vbody; /* kept for annotation defaults */
    pass->frag_body = fbody;
    {
        /* WALPE_FX_DUMP_SRC=dir (diagnostics): every pass's sources as
         * built, before any repair (input for tests/fx_repair_test.c) */
        const char *ds = g_getenv ("WALPE_FX_DUMP_SRC");
        if (ds && *ds) {
            static int sn;
            int k = g_atomic_int_add (&sn, 1);
            char *base = g_strdup (shader);
            g_strdelimit (base, "/ ", '_');
            char *fv = g_strdup_printf ("%s/%04d-%s.vert", ds, k, base);
            char *ff = g_strdup_printf ("%s/%04d-%s.frag", ds, k, base);
            g_file_set_contents (fv, vsrc, -1, NULL);
            g_file_set_contents (ff, fsrc, -1, NULL);
            g_free (fv);
            g_free (ff);
            g_free (base);
        }
    }
    /* specialised and / or shared (fx_specialize.h) */
    GLuint prog = fxs_build_program (pass, &vsrc, &fsrc, shader, dbgname, instpass);
    g_free (vsrc);
    g_free (fsrc);
    if (!prog) {
        g_clear_pointer (&pass->frag_body, g_free);
        g_clear_pointer (&pass->vert_body, g_free);
        return FALSE;
    }
    pass->program = prog;

    /* "target" and "bind" belong to the effect.json pass entry (WE /
     * linux-wallpaperengine layout); the material pass is only a fallback.
     * Reading them from the material alone sent every pass of multi-pass
     * effects (blurs etc.) to the chain output, reading its own target. */
    const char *target = wpj_str (passdef, "target", wpj_str (p0, "target", ""));
    snprintf (pass->target, sizeof (pass->target), "%s", target ? target : "");
    JsonArray *binds = wpj_arr (passdef, "bind");
    if (!binds)
        binds = wpj_arr (p0, "bind");
    guint nb = wpj_arr_len (binds);
    if (nb > FX_MAX_BINDS)
        nb = FX_MAX_BINDS;
    for (guint i = 0; i < nb; i++) {
        JsonObject *b = wpj_arr_obj (binds, i);
        if (!b)
            continue;
        pass->binds[i].index = walpe_d2i (wpj_num (b, "index", (double) i));
        snprintf (pass->binds[i].name, sizeof (pass->binds[i].name), "%s",
                  wpj_str (b, "name", ""));
    }
    pass->n_binds = (int) nb;

    /* instance constant overrides (constantshadervalues) */
    JsonObject *cst = instpass ? wpj_obj (instpass, "constantshadervalues") : NULL;
    if (cst)
        pass->constants = json_object_ref (cst);
    fx_cache_uniforms (pass);
    fxs_after_link (pass);
    fx_vertex_setup (pass);
    fxr_after_link (pass);   /* fx_region.h */
    return TRUE;
}

static EffectChain *fx_chain_build_parsed (const char *dir, JsonObject *instance,
                                           JsonNode *ej, const char *file,
                                           const char *dbgname)
{
    JsonObject *eo = json_node_get_object (ej);
    JsonArray *passes = wpj_arr (eo, "passes");
    guint npass = wpj_arr_len (passes);
    if (npass == 0 || npass > FX_MAX_PASSES) {
        g_warning ("fx: %s: %s has %u passes (supported: 1..%d)", dbgname, file, npass,
                   FX_MAX_PASSES);
        json_node_unref (ej);
        return NULL;
    }

    EffectChain *ch = g_new0 (EffectChain, 1);
    ch->cacheable = -1;
    ch->dyn = -1;
    ch->feedback = -1;
    ch->n_passes = (int) npass;
    /* instance "passes" is an ARRAY parallel to effect.json's passes */
    JsonArray *instpasses = wpj_arr (instance, "passes");

    for (guint i = 0; i < npass; i++) {
        JsonObject *passdef = wpj_arr_obj (passes, i);
        JsonObject *instpass = wpj_arr_obj (instpasses, i);
        const char *cmd = wpj_str (passdef, "command", NULL);
        gboolean swap = !g_strcmp0 (cmd, "swap");
        if (swap || !g_strcmp0 (cmd, "copy")) {
            /* "command": "copy" - target <- source (motion blur keeps the
             * previous frame this way); "swap" - the two trade places */
            EffPass *cp = &ch->passes[i];
            memset (cp, 0, sizeof (*cp));
            cp->is_copy = TRUE;
            cp->is_swap = swap;
            snprintf (cp->target, sizeof (cp->target), "%s", wpj_str (passdef, "target", ""));
            snprintf (cp->copy_src, sizeof (cp->copy_src), "%s", wpj_str (passdef, "source", ""));
            continue;
        }
        char *matref = g_strdup (wpj_str (passdef, "material", NULL));
        char *mpath = matref ? fx_resolve (dir, matref) : NULL;
        if (!mpath)
            g_warning ("fx: %s: %s pass %u material %s not found", dbgname, file, i,
                       matref ? matref : "(none)");
        g_free (matref);
        if (!mpath) {
            fx_chain_free (ch);
            json_node_unref (ej);
            return NULL;
        }
        JsonNode *mj = wpj_parse_file (mpath);
        g_free (mpath);
        if (!mj) {
            fx_chain_free (ch);
            json_node_unref (ej);
            return NULL;
        }
        gboolean ok = fx_build_pass (&ch->passes[i], json_node_get_object (mj),
                                     dir, passdef, instpass, dbgname);
        json_node_unref (mj);
        if (!ok) {
            fx_chain_free (ch);
            json_node_unref (ej);
            return NULL;
        }
    }

    /* FBO targets */
    JsonArray *fbos = wpj_arr (eo, "fbos");
    guint nfb = wpj_arr_len (fbos);
    if (nfb > FX_MAX_TARGETS)
        nfb = FX_MAX_TARGETS;
    for (guint i = 0; i < nfb; i++) {
        JsonObject *f = wpj_arr_obj (fbos, i);
        if (!f)
            continue;
        /* index by the count of VALID entries (a skipped non-object entry
         * used to leave a hole that hid every later target) */
        EffTarget *t = &ch->targets[ch->n_targets++];
        snprintf (t->name, sizeof (t->name), "%s", wpj_str (f, "name", ""));
        t->w = 0; /* sized lazily at first run */
        t->h = 0;
        t->fbo = 0;
        t->tex = 0;
        /* WE fbos[].scale: the target is the input divided by scale (blur
         * chains run at 1/2..1/8 size) */
        t->scale = CLAMP (walpe_d2i (wpj_num (f, "scale", 1.0)), 1, 16);
        /* fbos[].format: the target's pixel format (see FxRtFormat) */
        t->fmt = fx_rt_format_id (wpj_str (f, "format", NULL));
    }
    /* WE runs a layer through two buffers (_rt_imageLayerComposite_<id>_a
     * and _b): a pass without "target" reads the layer as the pass before
     * left it and writes the other buffer. One output is enough while only
     * one pass does that. With two or more (the builtin refraction:
     * effectcomposebackground, then refract) the later pass cleared the
     * earlier one's picture and sampled the texture it was drawing into
     * (undefined; the layer came out empty). Such a chain gets a second
     * output: a target of the output's size that no "target" / "bind" can
     * name; fx_chain_run alternates. */
    ch->alt_out = -1;
    int plain = 0;
    for (int i = 0; i < ch->n_passes; i++)
        plain += !ch->passes[i].is_copy && !ch->passes[i].target[0];
    if (plain > 1 && ch->n_targets < FX_MAX_TARGETS) {
        EffTarget *t = &ch->targets[ch->n_targets];
        memset (t, 0, sizeof (*t));
        snprintf (t->name, sizeof (t->name), "\001alt");
        t->scale = 1;
        ch->alt_out = ch->n_targets++;
    }
    json_node_unref (ej);
    ch->ready = TRUE;
    /* quality knobs: what the effect is by name, how fast it moves */
    snprintf (ch->file, sizeof (ch->file), "%s", file ? file : "");
    ch->family = walpe_fx_family (file);
    ch->fast_name = walpe_fx_name_fast (file);
    ch->sharp = 1;
    for (int i = 0; i < ch->n_passes; i++) {
        const EffPass *ps = &ch->passes[i];
        if (ps->is_copy || !ps->time.uses_time)
            continue;
        ch->time.derived = ch->time.uses_time ? ch->time.derived && ps->time.derived
                                              : ps->time.derived;
        ch->time.uses_time = TRUE;
        ch->time.rate = MAX (ch->time.rate, ps->time.rate);
        ch->sharp = MAX (ch->sharp, ps->sharp);
    }
    {
        /* the transform effect in Vertex mode: one pass whose three
         * constants are known */
        EffPass *p0 = &ch->passes[0];
        ch->vertex_affine = ch->n_passes == 1 && !p0->is_copy && p0->xf_sign != 0 &&
                            p0->xf[0] >= 0 && p0->xf[1] >= 0 && p0->xf[2] >= 0;
    }
    return ch;
}

/* build a full chain from an effect instance (file + instance overrides) */
static EffectChain *fx_chain_build (const char *dir, JsonObject *instance,
                                    const char *dbgname)
{
    const char *file = wpj_str (instance, "file", NULL);
    if (!file)
        return NULL;
    char *epath = fx_resolve (dir, file);
    if (!epath) {
        g_warning ("fx: %s: %s not found (not in the project; WE assets %s)", dbgname, file,
                   walpe_we_assets_dir () ? walpe_we_assets_dir () : "not found");
        return NULL;
    }
    JsonNode *ej = wpj_parse_file (epath);
    /* the effect's own dir is a resolution root for its materials,
     * shaders and textures (builtin assets/effects/<name>/ layout) */
    char *effroot = g_path_get_dirname (epath);
    g_free (epath);
    if (!ej) {
        g_warning ("fx: %s effect.json unparseable", file);
        g_free (effroot);
        return NULL;
    }
    const char *saved_root = fx_effroot;
    fx_effroot = effroot;
    EffectChain *ch = fx_chain_build_parsed (dir, instance, ej, file, dbgname);
    fxs_chain_built (ch, file);
    fx_effroot = saved_root;
    g_free (effroot);
    return ch;
}

/* ---------------------------------------------------------- execution */

/* create a render target: texture + FBO WITH THE TEXTURE ATTACHED (an
 * unattached FBO is incomplete — passes rasterize into the void and the
 * output texture keeps undefined garbage) */
/* render thread: bytes / number of effect render targets alive in this
 * context (pool and owned), for the scene memory line */
static __thread gint64 fx_rt_bytes;
static __thread int fx_rt_count;

static gboolean fx_rt_half_float (void)
{
    /* WALPE_FX_RT16F=1: half-float targets (diagnostics: banding from
     * 8-bit intermediates; WE itself uses RGBA8 unless the scene is HDR) */
    static int rt16 = -1;
    if (rt16 < 0)
        rt16 = g_getenv ("WALPE_FX_RT16F") && *g_getenv ("WALPE_FX_RT16F") == '1';
    return rt16;
}

/* ---- render target formats ----------------------------------------------
 * An effect's fbos[] entries name the format of each target. The names
 * found in Wallpaper Engine 2.8's own effects and in the workshop scenes
 * looked at: "rgba_backbuffer" (most), "rgba8888", "rg88", "r8", "r16f",
 * "rg1616f". Every target used to be RGBA8 whatever the effect asked for:
 * a one- or two-channel target (a mask, a circle-of-confusion map, a flow
 * field) took 2 to 4 times the memory and write bandwidth it needs, and a
 * target declared as float was clamped to 0..1. Now the declared format
 * is used; "rgba_backbuffer", "rgba8888", no format and names not listed
 * here stay RGBA8 (the scene's back buffer is 8 bits per channel here). A
 * format the context cannot render to (float targets on GLES without the
 * extension) falls back to RGBA8, once per format and thread.
 * WALPE_FX_RT_FORMATS=0 restores RGBA8 for every target (A/B).
 * FX_FMT_CAPTURE is not a WE format: the copy of the frame under a
 * compose layer (fx_capture_get), RGB8 so that its alpha reads 1. */
typedef enum {
    FX_FMT_RGBA8 = 0,   /* rgba8888, rgba_backbuffer, default */
    FX_FMT_R8,
    FX_FMT_RG8,
    FX_FMT_R16F,
    FX_FMT_RG16F,
    FX_FMT_CAPTURE,
    FX_FMT_N
} FxRtFormat;

/* bytes = what a texel takes on the GPU (RGB8 is stored as 4) */
static const struct { const char *name; GLint ifmt; GLenum fmt, type; int bytes; } fx_fmt_info[FX_FMT_N] = {
    [FX_FMT_RGBA8]   = { "rgba8888",  GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4 },
    [FX_FMT_R8]      = { "r8",        GL_R8,    GL_RED,  GL_UNSIGNED_BYTE, 1 },
    [FX_FMT_RG8]     = { "rg88",      GL_RG8,   GL_RG,   GL_UNSIGNED_BYTE, 2 },
    [FX_FMT_R16F]    = { "r16f",      GL_R16F,  GL_RED,  GL_FLOAT, 2 },
    [FX_FMT_RG16F]   = { "rg1616f",   GL_RG16F, GL_RG,   GL_FLOAT, 4 },
    [FX_FMT_CAPTURE] = { "(capture)", GL_RGB8,  GL_RGB,  GL_UNSIGNED_BYTE, 4 },
};

/* fx_region.h: can this target only hold finite values (an 8-bit
 * format)? t = NULL: the chain's output or its input */
static gboolean fx_target_unorm8 (const EffTarget *t)
{
    return !fx_rt_half_float () &&
           (!t || (t->fmt >= 0 && t->fmt < FX_FMT_N && fx_fmt_info[t->fmt].type == GL_UNSIGNED_BYTE));
}

/* fbos[].format -> FxRtFormat (RGBA8 for none / unknown / the back buffer) */
static FxRtFormat fx_rt_format_from_name (const char *name)
{
    static int honour = -1;
    if (honour < 0) {
        const char *e = g_getenv ("WALPE_FX_RT_FORMATS");
        honour = !(e && *e == '0');
    }
    if (!honour || !name || !*name)
        return FX_FMT_RGBA8;
    for (int i = FX_FMT_R8; i < FX_FMT_CAPTURE; i++)
        if (!g_ascii_strcasecmp (name, fx_fmt_info[i].name))
            return (FxRtFormat) i;
    return FX_FMT_RGBA8;
}

/* format of each live target, by texture name (fx_drop_rt needs no
 * format argument: its callers do not all know it) */
static __thread GHashTable *fx_rt_fmt;
/* per format: 0 = not tried, 1 = renderable, 2 = not (RGBA8 instead) */
static __thread guint8 fx_fmt_ok[FX_FMT_N];

static int fx_rt_bytes_per_pixel (FxRtFormat f)
{
    if (f == FX_FMT_RGBA8 && fx_rt_half_float ())
        return 8;
    return fx_fmt_info[f].bytes;
}

static void fx_drop_rt (GLuint *fbo, GLuint *tex, int w, int h)
{
    if (*tex) {
        FxRtFormat f = FX_FMT_RGBA8;
        fx_gls_forget_textures ();   /* the name may come back as another texture */
        if (fx_rt_fmt) {
            f = (FxRtFormat) GPOINTER_TO_INT (g_hash_table_lookup (fx_rt_fmt, GUINT_TO_POINTER (*tex)));
            g_hash_table_remove (fx_rt_fmt, GUINT_TO_POINTER (*tex));
        }
        glDeleteTextures (1, tex);
        fx_rt_bytes -= (gint64) w * h * fx_rt_bytes_per_pixel (f);
        fx_rt_count--;
    }
    if (*fbo)
        glDeleteFramebuffers (1, fbo);
    *tex = *fbo = 0;
}

/* *fbo / *tex must be 0 (fx_drop_rt). Returns the format the target got
 * (fmt, or RGBA8 when the context cannot render to fmt). */
static FxRtFormat fx_make_rt_fmt (GLuint *fbo, GLuint *tex, int w, int h, FxRtFormat fmt)
{
    gboolean rt16 = fx_rt_half_float ();
    if (fmt < 0 || fmt >= FX_FMT_N || fx_fmt_ok[fmt] == 2)
        fmt = FX_FMT_RGBA8;
    fx_gls_forget_textures ();   /* binds the new texture on the active unit */
    glGenTextures (1, tex);
    glBindTexture (GL_TEXTURE_2D, *tex);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers (1, fbo);
    glBindFramebuffer (GL_FRAMEBUFFER, *fbo);
    for (;;) {
        if (fmt == FX_FMT_RGBA8)
            glTexImage2D (GL_TEXTURE_2D, 0, rt16 ? GL_RGBA16F : GL_RGBA8, w, h, 0, GL_BGRA,
                          rt16 ? GL_FLOAT : GL_UNSIGNED_BYTE, NULL);
        else
            glTexImage2D (GL_TEXTURE_2D, 0, fx_fmt_info[fmt].ifmt, w, h, 0, fx_fmt_info[fmt].fmt,
                          fx_fmt_info[fmt].type, NULL);
        glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                GL_TEXTURE_2D, *tex, 0);
        if (fmt == FX_FMT_RGBA8 || fx_fmt_ok[fmt] == 1)
            break;
        /* first target of this format in this context: can it be drawn to? */
        if (glCheckFramebufferStatus (GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
            fx_fmt_ok[fmt] = 1;
            break;
        }
        g_message ("fx: render target format %s is not renderable here: RGBA8 instead",
                   fx_fmt_info[fmt].name);
        fx_fmt_ok[fmt] = 2;
        while (glGetError () != GL_NO_ERROR)
            ;
        fmt = FX_FMT_RGBA8;
    }
    if (fmt != FX_FMT_RGBA8) {
        if (!fx_rt_fmt) {
            fx_rt_fmt = g_hash_table_new (NULL, NULL);
            fx_thread_mark ();
        }
        g_hash_table_insert (fx_rt_fmt, GUINT_TO_POINTER (*tex), GINT_TO_POINTER ((int) fmt));
    }
    fx_rt_bytes += (gint64) w * h * fx_rt_bytes_per_pixel (fmt);
    fx_rt_count++;
    return fmt;
}

static void fx_make_rt (GLuint *fbo, GLuint *tex, int w, int h)
{
    fx_make_rt_fmt (fbo, tex, w, h, FX_FMT_RGBA8);
}

static int fx_rt_format_id (const char *name)
{
    return (int) fx_rt_format_from_name (name);
}

/* ---- render target pool ------------------------------------------------
 * Every chain used to own its output target and its declared targets for
 * as long as the scene lived: a scene with 50 effect layers held 50+
 * layer-sized targets although each is only needed while its layer is
 * being drawn. Now a target is lent from a per-thread pool for one run of
 * a layer's chains (fx_run_for_object) and handed back as soon as the
 * next chain has read it; the result of a run stays valid until the next
 * run starts (the caller draws it at once). Every pass clears and redraws
 * its whole target, so whose pixels were in a lent target before does not
 * matter. Kept out of the pool (owned by their chain, as before):
 *  - chains with feedback (a pass reads a target before a pass of the
 *    same run wrote it: last frame's picture);
 *  - the output that a cache keeps over frames: the last chain of a
 *    static set, the last chain of a static prefix. Their other targets
 *    go when the run is over (fx_chain_drop_targets).
 * Idle pool entries are freed after FX_POOL_IDLE_US, with the scene
 * (fx_targets_release) and when the scene is paused. */
#define FX_POOL_IDLE_US (5 * G_USEC_PER_SEC)

typedef struct {
    GLuint fbo, tex;
    int w, h;
    FxRtFormat want;      /* the format asked for (the key) */
    FxRtFormat fmt;       /* the format it has (RGBA8 when `want` is not renderable) */
    gboolean busy;
    gboolean pinned;      /* lent over runs (fx_capture_get): only
                           * fx_pool_put / fx_pool_free (0) end the loan */
    gint64 used_us;
    guint used_frame;     /* fx_pool_frame when it was last lent */
} FxPoolRt;

static __thread GArray *fx_pool;
static __thread guint fx_pool_frame;   /* counts fx_pool_new_frame () */
/* the most targets the pool held at once / their bytes (WALPE_FX_POOL_LOG) */
static __thread int fx_pool_peak_n;
static __thread gint64 fx_pool_peak_bytes;

/* A render thread's own tables (the target pool, the targets' formats,
 * which pass last used a program) live as long as the thread and were
 * never freed: LeakSanitizer listed the three at exit, once per render
 * thread. They go when the thread ends (their GL objects are gone by
 * then: the content's gl_free released them). */
static void fx_thread_free (gpointer p)
{
    (void) p;
    g_clear_pointer (&fx_pool, g_array_unref);
    g_clear_pointer (&fx_rt_fmt, g_hash_table_unref);
    g_clear_pointer (&fx_prog_user, g_hash_table_unref);
}
static GPrivate fx_thread_key = G_PRIVATE_INIT (fx_thread_free);

static void fx_thread_mark (void)
{
    if (!g_private_get (&fx_thread_key))
        g_private_set (&fx_thread_key, GINT_TO_POINTER (1));
}

/* a target of w x h in format fmt, lent until fx_pool_put / the next
 * fx_pool_begin_run; returns entry + 1. Targets are shared by size AND
 * format only: a pass samples its inputs over the whole texture (0..1,
 * clamped at the edges), so a larger target cannot stand in for a
 * smaller one. */
static int fx_pool_get_fmt (int w, int h, FxRtFormat fmt, GLuint *fbo, GLuint *tex)
{
    if (!fx_pool) {
        fx_pool = g_array_new (FALSE, TRUE, sizeof (FxPoolRt));
        fx_thread_mark ();
    }
    int slot = -1;
    FxPoolRt *r = NULL;
    for (guint i = 0; i < fx_pool->len && !r; i++) {
        FxPoolRt *e = &g_array_index (fx_pool, FxPoolRt, i);
        if (!e->tex) {
            if (slot < 0)
                slot = (int) i;
        } else if (!e->busy && e->w == w && e->h == h && e->want == fmt) {
            r = e;
            slot = (int) i;
        }
    }
    if (!r) {
        if (slot < 0) {
            FxPoolRt z = { 0 };
            g_array_append_val (fx_pool, z);
            slot = (int) fx_pool->len - 1;
        }
        r = &g_array_index (fx_pool, FxPoolRt, slot);
        r->fmt = fx_make_rt_fmt (&r->fbo, &r->tex, w, h, fmt);
        r->want = fmt;
        r->w = w;
        r->h = h;
        int n = 0;
        gint64 bytes = 0;
        for (guint i = 0; i < fx_pool->len; i++) {
            FxPoolRt *e = &g_array_index (fx_pool, FxPoolRt, i);
            if (e->tex) {
                n++;
                bytes += (gint64) e->w * e->h * fx_rt_bytes_per_pixel (e->fmt);
            }
        }
        fx_pool_peak_n = MAX (fx_pool_peak_n, n);
        fx_pool_peak_bytes = MAX (fx_pool_peak_bytes, bytes);
    }
    r->busy = TRUE;
    r->pinned = FALSE;
    r->used_us = g_get_monotonic_time ();
    r->used_frame = fx_pool_frame;
    *fbo = r->fbo;
    *tex = r->tex;
    return slot + 1;
}

static void fx_pool_put (int entry)
{
    if (fx_pool && entry >= 1 && (guint) entry <= fx_pool->len) {
        FxPoolRt *e = &g_array_index (fx_pool, FxPoolRt, entry - 1);
        e->busy = e->pinned = FALSE;
    }
}

/* a new run: what the previous one lent out has been drawn */
static void fx_pool_begin_run (void)
{
    for (guint i = 0; fx_pool && i < fx_pool->len; i++) {
        FxPoolRt *e = &g_array_index (fx_pool, FxPoolRt, i);
        if (!e->pinned)
            e->busy = FALSE;
    }
}

/* free pool entries: all (older_us = 0) or the idle ones not used since
 * older_us */
static void fx_pool_free (gint64 older_us)
{
    for (guint i = 0; fx_pool && i < fx_pool->len; i++) {
        FxPoolRt *e = &g_array_index (fx_pool, FxPoolRt, i);
        if (e->tex && (!older_us || (!e->busy && e->used_us < older_us))) {
            fx_drop_rt (&e->fbo, &e->tex, e->w, e->h);
            e->busy = e->pinned = FALSE;
        }
    }
}

/* WALPE_FX_POOL_LOG=1 (diagnostics): the pool of this thread, every 300
 * frames: what it holds now and the most it ever held */
static void fx_pool_log (void)
{
    static int on = -1;
    static __thread guint frames;
    if (on < 0)
        on = g_getenv ("WALPE_FX_POOL_LOG") != NULL;
    if (!on || ++frames % 300 != 20 || !fx_pool)
        return;
    GString *ln = g_string_new (NULL);
    int n = 0;
    gint64 bytes = 0;
    for (guint i = 0; i < fx_pool->len; i++) {
        FxPoolRt *e = &g_array_index (fx_pool, FxPoolRt, i);
        if (!e->tex)
            continue;
        n++;
        bytes += (gint64) e->w * e->h * fx_rt_bytes_per_pixel (e->fmt);
        g_string_append_printf (ln, " %dx%d %s", e->w, e->h, fx_fmt_info[e->fmt].name);
    }
    g_message ("fx pool: %d targets = %.1f MiB (peak %d = %.1f MiB); all targets of this thread "
               "%d = %.1f MiB:%s", n, bytes / 1048576.0, fx_pool_peak_n,
               fx_pool_peak_bytes / 1048576.0, fx_rt_count, fx_rt_bytes / 1048576.0, ln->str);
    g_string_free (ln, TRUE);
}

/* a new frame: everything lent in the last one has been drawn; entries
 * nobody asked for in a while go */
static void fx_pool_new_frame (void)
{
    /* WALPE_FX_POOL_IDLE_MS=n: how long an unused pool target is kept
     * (default 5000; at least 50). Shorter = less memory after a scene
     * calms down, more allocations when layers come and go. */
    static gint64 idle_us = -1;
    if (idle_us < 0) {
        const char *e = g_getenv ("WALPE_FX_POOL_IDLE_MS");
        gint64 ms = e && *e ? g_ascii_strtoll (e, NULL, 10) : 0;
        idle_us = ms > 0 ? MAX (ms, 50) * 1000 : FX_POOL_IDLE_US;
    }
    /* a capture nobody handed back (the layer loop left early) */
    for (guint i = 0; fx_pool && i < fx_pool->len; i++)
        g_array_index (fx_pool, FxPoolRt, i).pinned = FALSE;
    fx_pool_begin_run ();
    fx_pool_free (g_get_monotonic_time () - idle_us);
    fx_pool_frame++;
    fx_pool_log ();
}

/* ---- compose captures ----------------------------------------------------
 * A compose layer copies the frame drawn so far (or the part under it)
 * into a texture and runs its effects on that. Every such layer used to
 * own an RGBA8 texture of the copied size for the life of the scene, and
 * after each copy the alpha channel was overwritten with 1 by a masked
 * clear (the window's alpha is not the scene's): one more full write of
 * the texture per layer and frame. Now the copy goes into a target lent
 * from the pool until the layer has been drawn (compose layers of one
 * size share one), and the target has no alpha channel (RGB8): what is
 * sampled from it has alpha 1 by definition, on any driver and whatever
 * the window surface holds. Where RGB8 cannot be rendered to (the format
 * is optional as a render target in desktop GL) the target is RGBA8 and
 * *opaque comes back FALSE: the caller then sets alpha itself as before.
 * WALPE_FX_CAPTURE_RGBA=1 forces that path (A/B).
 * A compose layer that moves or scales copies a rectangle of another size
 * every frame; so that the pool does not collect one target per size for
 * its idle time, a capture of a new size first frees the captures nobody
 * used in this frame or the one before (the old per-layer texture was
 * re-made in place: at most one per layer).
 * The texture name of a capture stands for different pictures over time
 * (and for different layers): nothing may be cached under it. Compose
 * layers run with input_static = FALSE, so neither the static cache nor
 * the prefix cache keys by it today. */
int fx_capture_get (int w, int h, guint *fbo, guint *tex, gboolean *opaque)
{
    static int rgba = -1;
    if (rgba < 0) {
        const char *e = g_getenv ("WALPE_FX_CAPTURE_RGBA");
        rgba = e && *e == '1';
    }
    GLuint f = 0, t = 0;
    const FxRtFormat want = rgba ? FX_FMT_RGBA8 : FX_FMT_CAPTURE;
    gboolean have = FALSE;
    for (guint i = 0; fx_pool && i < fx_pool->len && !have; i++) {
        FxPoolRt *p = &g_array_index (fx_pool, FxPoolRt, i);
        have = p->tex && !p->busy && p->w == w && p->h == h && p->want == want;
    }
    for (guint i = 0; fx_pool && i < fx_pool->len && !have && !rgba; i++) {
        FxPoolRt *p = &g_array_index (fx_pool, FxPoolRt, i);
        if (p->tex && !p->busy && p->want == FX_FMT_CAPTURE && p->used_frame + 1 < fx_pool_frame)
            fx_drop_rt (&p->fbo, &p->tex, p->w, p->h);
    }
    int entry = fx_pool_get_fmt (w, h, want, &f, &t);
    FxPoolRt *e = &g_array_index (fx_pool, FxPoolRt, entry - 1);
    e->pinned = TRUE;
    *fbo = f;
    *tex = t;
    if (opaque)
        *opaque = e->fmt == FX_FMT_CAPTURE;
    return entry;
}

void fx_capture_put (int entry)
{
    fx_pool_put (entry);
}

void fx_targets_release (void)
{
    fx_pool_free (0);
}

void fx_targets_stats (gint64 *bytes, int *count)
{
    if (bytes)
        *bytes = fx_rt_bytes;
    if (count)
        *count = fx_rt_count;
}

static gboolean fx_chain_feedback (EffectChain *ch);

/* free the chain's own targets, except the one holding texture `keep`
 * (0 = all); targets lent from the pool are only forgotten */
static void fx_chain_drop_targets (EffectChain *ch, GLuint keep)
{
    for (int i = 0; i < ch->n_targets; i++) {
        EffTarget *t = &ch->targets[i];
        if (t->pool) {
            t->pool = 0;
            t->tex = t->fbo = 0;
        } else if (t->tex != keep || !keep) {
            fx_drop_rt (&t->fbo, &t->tex, t->w, t->h);
        }
    }
    if (ch->out_pool) {
        ch->out_pool = 0;
        ch->out_tex = ch->out_fbo = 0;
    } else if (ch->out_tex != keep || !keep) {
        fx_drop_rt (&ch->out_fbo, &ch->out_tex, ch->out_w, ch->out_h);
    }
}

/* one target of the chain for this run: its own (own) or lent */
static void fx_target_ensure_fmt (GLuint *fbo, GLuint *tex, int *pool, int *cw, int *chh, int w,
                                  int h, gboolean own, FxRtFormat fmt)
{
    if (*pool) {
        /* lent for an earlier run: no longer this chain's */
        *pool = 0;
        *tex = *fbo = 0;
    }
    if (own) {
        if (*tex && (*cw != w || *chh != h))
            fx_drop_rt (fbo, tex, *cw, *chh);
        if (!*tex)
            fx_make_rt_fmt (fbo, tex, w, h, fmt);
    } else {
        if (*tex)
            fx_drop_rt (fbo, tex, *cw, *chh);   /* was its own: the pool serves it now */
        *pool = fx_pool_get_fmt (w, h, fmt, fbo, tex);
    }
    *cw = w;
    *chh = h;
}

static void fx_target_ensure (GLuint *fbo, GLuint *tex, int *pool, int *cw, int *chh, int w, int h,
                              gboolean own)
{
    fx_target_ensure_fmt (fbo, tex, pool, cw, chh, w, h, own, FX_FMT_RGBA8);
}

static EffTarget *fx_find_target (EffectChain *ch, const char *name);

/* where the chain's result ends up: index of the target the last drawing
 * pass writes, -1 = the chain output (what fx_chain_run returns) */
static int fx_chain_result_target (EffectChain *ch)
{
    int res = -1;
    for (int i = 0; i < ch->n_passes; i++) {
        EffPass *p = &ch->passes[i];
        if (p->is_copy)
            continue;
        EffTarget *t = p->target[0] ? fx_find_target (ch, p->target) : NULL;
        if (p->target[0] && !t)
            continue;   /* not run */
        res = t ? (int) (t - ch->targets) : -1;
    }
    return res;
}

/* w/h = full-quality (input) size; k = working-resolution factor: the
 * targets are allocated at k * size, while shaders keep seeing the
 * full-quality sizes (g_TextureNResolution, g_TexelSize), so texel-step
 * effects (blur radii, offsets) cover the same UV distance as at k = 1.
 * own: FX_OWN_ALL = the chain keeps its targets (feedback), FX_OWN_RESULT
 * = only the target that ends up holding its result (a cache keeps that
 * one; the others used to be made for the run and freed right after it,
 * again on every refresh of the cache - a clock text under a static
 * effect made and freed targets ten times a second), FX_OWN_NONE = all
 * lent from the pool for this run. */
enum { FX_OWN_NONE = 0, FX_OWN_ALL = 1, FX_OWN_RESULT = 2 };

static void fx_ensure_targets (EffectChain *ch, int w, int h, float k, int own, int tdiv)
{
    int rw = MAX (1, (int) ceilf (w * k)), rh = MAX (1, (int) ceilf (h * k));
    int res = own == FX_OWN_RESULT ? fx_chain_result_target (ch) : -2;
    /* the chain's final output FBO: always present, input-sized. Effects
     * without declared fbos (single-pass, e.g. waterripple) render here */
    fx_target_ensure (&ch->out_fbo, &ch->out_tex, &ch->out_pool, &ch->out_w, &ch->out_h, rw, rh,
                      own == FX_OWN_ALL || res == -1);
    ch->out_vw = w;
    ch->out_vh = h;
    for (int i = 0; i < ch->n_targets; i++) {
        EffTarget *t = &ch->targets[i];
        /* tdiv > 1 (post-processing "reduced", blur and bloom chains): the
         * chain's inner buffers at 1 / tdiv of their size; the shaders
         * keep seeing the full-quality sizes, so a blur covers the same
         * part of the picture with fewer pixels. The output stays - and so
         * does the second output of a chain whose passes name no target
         * (alt_out; the two-pass blurprecise of older Wallpaper Engine
         * versions that many packages carry): its passes are drawn with
         * the output's viewport and one of the two holds the result. At
         * half size it took the lower left quarter of the picture, which
         * the layer then showed twice as large. */
        const int td = i == ch->alt_out ? 1 : tdiv;
        int tw = MAX (1, rw / (t->scale * td)), th = MAX (1, rh / (t->scale * td));
        t->vw = MAX (1, w / t->scale);
        t->vh = MAX (1, h / t->scale);
        /* the target of a kept pass is the chain's own; made again
         * (another size, dropped while the scene was paused) it holds
         * nothing yet */
        const GLuint tex0 = t->tex;
        const int w0 = t->w, h0 = t->h, pool0 = t->pool;
        fx_target_ensure_fmt (&t->fbo, &t->tex, &t->pool, &t->w, &t->h, tw, th,
                              own == FX_OWN_ALL || res == i || t->keep, (FxRtFormat) t->fmt);
        if (!tex0 || pool0 || t->pool || t->tex != tex0 || t->w != w0 || t->h != h0)
            t->kp_valid = FALSE;
    }
}

static EffTarget *fx_find_target (EffectChain *ch, const char *name);

/* Post-processing "off" leaves a chain out because of its NAME (bloom,
 * film grain, vignette, chromatic aberration). A name does not say what
 * the chain does to the picture: Rainy Day 3465215190 uses a "bloom" with
 * threshold 0 and a dark tint as its colour grading, and without it the
 * whole picture was a third brighter - Low did not look like the
 * wallpaper. So a chain is only left out once it was measured to leave
 * the picture's level alone: behind its runs 1, 2, 4, 8, 16 and 32 after
 * a load (or after one of its constants changed) the mean colour of
 * what it read and of what it drew are compared (FX_LEVEL_N x FX_LEVEL_N
 * bilinear samples each, alpha weighted). One measurement above
 * FX_LEVEL_MAX (of 255, any channel) and the chain stays for good; six
 * below and it is left out from then on. A measurement over a picture of
 * one colour (a scene that fades in from black) says nothing: the schedule
 * starts over 16 runs later. WALPE_FX_POST_LEVEL=0: the rule of the earlier build (by
 * name alone). */
#define FX_LEVEL_N 48
#define FX_LEVEL_MAX 3.0f

static gboolean fx_level_rule (void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = g_getenv ("WALPE_FX_POST_LEVEL");
        on = !(e && *e == '0');
    }
    return on;
}

/* quality knobs: is the chain left out of this run (post-processing "off":
 * a purely cosmetic post pass on a compose layer), and by how much are its
 * inner buffers reduced ("reduced" and "off": blur and bloom chains) */
static gboolean fx_chain_dropped (const EffectChain *ch)
{
    return fx_q.post == WALPE_POST_OFF && fx_q.compose &&
           walpe_fx_family_cosmetic ((WalpeFxFamily) ch->family) &&
           (ch->lvl_state == 1 || !fx_level_rule ());
}

static int fx_chain_tdiv (const EffectChain *ch)
{
    return fx_q.post != WALPE_POST_FULL && walpe_fx_family_reducible ((WalpeFxFamily) ch->family)
               ? 2 : 1;
}

/* pixels the chain's passes shade in one run on a working size of rw x rh
 * with inner buffers at 1 / tdiv; *passes = draws (copy passes not counted) */
static guint64 fx_chain_px (EffectChain *ch, int rw, int rh, int tdiv, guint *passes)
{
    guint64 px = 0;
    guint n = 0;
    for (int i = 0; i < ch->n_passes; i++) {
        EffPass *ps = &ch->passes[i];
        EffTarget *t = ps->target[0] ? fx_find_target (ch, ps->target) : NULL;
        if ((ps->target[0] && !t) || ps->is_copy)
            continue;
        if (t)
            px += (guint64) MAX (1, rw / (t->scale * tdiv)) * MAX (1, rh / (t->scale * tdiv));
        else
            px += (guint64) rw * rh;
        n++;
    }
    if (passes)
        *passes = n;
    return px;
}

static EffTarget *fx_find_target (EffectChain *ch, const char *name)
{
    for (int i = 0; i < ch->n_targets; i++)
        if (!strcmp (ch->targets[i].name, name))
            return &ch->targets[i];
    return NULL;
}

/* ---- kept passes ----------------------------------------------------
 * A pass of a multi-pass effect that writes a target of its own and reads
 * nothing that changes from frame to frame - no g_Time / pointer / audio /
 * screen uniform, no texture but material textures loaded from files (the
 * depth-of-field effect's "circle of confusion" pass reads its mask and its
 * depth map and never the picture) - draws the same picture every frame,
 * whatever the rest of its chain does. It is drawn once; its target stays
 * the chain's own and is used again until a constant of the chain changes
 * (ch->gen), the pass gets another program or the target is made again.
 * Same assumptions as the static chain cache (fx_chain_static), applied to
 * one pass. WALPE_FX_STATIC_PASS=0: draw it every frame as before (A/B). */
static gint fx_keep_on_v = -1;

static gboolean fx_keep_on (void)
{
    int v = g_atomic_int_get (&fx_keep_on_v);
    if (G_UNLIKELY (v < 0)) {
        const char *e = g_getenv ("WALPE_FX_STATIC_PASS");
        v = !(e && *e == '0');
        g_atomic_int_set (&fx_keep_on_v, v);
    }
    return v;
}

void fx_set_static_pass_enabled (gboolean on)
{
    g_atomic_int_set (&fx_keep_on_v, on ? 1 : 0);
}

static void fx_keep_scan (EffectChain *ch)
{
    ch->kp_checked = TRUE;
    for (int i = 0; i < ch->n_passes; i++) {
        EffPass *p = &ch->passes[i];
        EffTarget *t = p->target[0] ? fx_find_target (ch, p->target) : NULL;
        p->kp_free = 1;
        if (!t || p->is_copy || p->is_swap || p->moves_verts || !p->program)
            continue;
        gboolean ok = TRUE;
        /* the only writer of its target (a swap trades the target itself) */
        for (int j = 0; ok && j < ch->n_passes; j++)
            ok = j == i || (strcmp (ch->passes[j].target, p->target) != 0 &&
                            !(ch->passes[j].is_swap && !strcmp (ch->passes[j].copy_src, p->target)));
        /* every uniform the linked program really has: constants (their
         * changes bump ch->gen) and samplers of material textures */
        for (int u = 0; ok && u < p->n_dyn; u++) {
            const int un = p->dyn[u].unit;
            ok = p->dyn[u].kind == FXU_TEXTURE && un >= 1 && un < FX_MAX_BINDS && !p->sys[un] &&
                 p->slot[un].tex != 0;
            for (int b = 0; ok && b < p->n_binds; b++)
                ok = p->binds[b].index != un;   /* "previous" or a target on that unit */
        }
        if (ok) {
            p->kp_free = 2;
            t->keep = TRUE;
        }
    }
}

#include "fx_clip.h"

/* render thread: effect passes of this frame drawn in full / drawn in
 * part / not drawn (fx_clip.h), for WALPE_PIX_STATS */
static __thread int fx_clip_count[3];
/* tests/clip_test.c only (it includes this file): poison = the part of a
 * target a pass leaves out is filled with a colour no scene has, so
 * anything that reads it shows; nopad = fx_run_for_object leaves out the
 * margin for the caller's filter (the test must then see differences) */
static gboolean fx_clip_test_poison, fx_clip_test_nopad;

/* run one chain: input texture in, final output texture out (the caller
 * draws it with the object transform). w/h = working resolution,
 * fullw/fullh = input's padded texture size (g_Texture0Resolution). */
static GLuint fx_chain_run (EffectChain *ch, GLuint input, int w, int h,
                            int fullw, int fullh, double time, float k, const float *lay,
                            int own)
{
    if (fxs_chain_noop (ch, input, w, h, k)) {
        /* every pass provably copies its input (fx_rewrite.h) */
        fx_chain_drop_targets (ch, 0);
        return input;
    }
    if (!ch->kp_checked)
        fx_keep_scan (ch);   /* (before the targets are made: a kept pass owns its target) */
    fx_ensure_targets (ch, w, h, k, own, fx_chain_tdiv (ch));
    GLuint previous = input;
    int prevres[4] = { fullw, fullh, w, h };
    const int inres[4] = { fullw, fullh, w, h };
    GLuint output = input;
    /* passes without a target: the first writes the chain output, each
     * later one the other of {output, second output} (fx_chain_build_parsed) */
    EffTarget *alt = ch->alt_out >= 0 ? &ch->targets[ch->alt_out] : NULL;
    gboolean out_written = FALSE, to_alt = FALSE;

    for (int i = 0; i < ch->n_passes; i++) {
        EffPass *pass = &ch->passes[i];
        EffTarget *target = pass->target[0] ? fx_find_target (ch, pass->target) : NULL;
        if (pass->target[0] && !target)
            continue;
        /* "Skip hidden areas": only the part of the target a later pass
         * or the caller reads is filled (fx_chain_plan; never set unless
         * a clip is at work) */
        int part_px[FXN_MAX][4], part_n = 1;
        const gboolean part = ch->planned && pass->need.mode != FXN_FULL;
        if (pass->is_swap) {
            /* the two targets trade places: what this frame wrote into one
             * is what the next frame reads from the other (the fluid
             * simulation's velocity and dye pairs) */
            EffTarget *src = fx_find_target (ch, pass->copy_src);
            if (src && target && src != target && src->w == target->w && src->h == target->h) {
                GLuint sf = src->fbo, st = src->tex;
                int sp = src->pool;
                src->fbo = target->fbo, src->tex = target->tex, src->pool = target->pool;
                target->fbo = sf, target->tex = st, target->pool = sp;
            }
            continue;
        }
        if (pass->is_copy) {
            EffTarget *src = fx_find_target (ch, pass->copy_src);
            if (src && target) {
                if (part && !(part_n = fx_need_pixels (&pass->need, target->w, target->h, part_px))) {
                    fx_clip_count[2]++;
                    continue;
                }
                WALPE_PROF_PASS_BEGIN (ch->prof_name, ch->inst_index, i, target->w, target->h);
                fxr_scissor_off ();   /* fx_region.h: a blit obeys the scissor */
                glBindFramebuffer (GL_READ_FRAMEBUFFER, src->fbo);
                glBindFramebuffer (GL_DRAW_FRAMEBUFFER, target->fbo);
                if (part && fx_clip_test_poison) {
                    glClearColor (1, 0, 1, 1);
                    glClear (GL_COLOR_BUFFER_BIT);
                }
                if (part)
                    glEnable (GL_SCISSOR_TEST);
                for (int q = 0; q < part_n; q++) {
                    if (part)
                        glScissor (part_px[q][0], part_px[q][1], part_px[q][2], part_px[q][3]);
                    glBlitFramebuffer (0, 0, src->w, src->h, 0, 0, target->w, target->h,
                                       GL_COLOR_BUFFER_BIT, GL_LINEAR);
                }
                if (part)
                    glDisable (GL_SCISSOR_TEST);
                glBindFramebuffer (GL_FRAMEBUFFER, fx_scene_fbo);
                fx_clip_count[part ? 1 : 0]++;
                WALPE_PROF_PASS_END ();   /* GPUPROF */
            }
            continue;
        }
        if (part && !(part_n = fx_need_pixels (&pass->need, target ? target->w : ch->out_w,
                                               target ? target->h : ch->out_h, part_px))) {
            /* nothing reads what it would draw: only where it would have
             * drawn is kept track of (the outputs' ping-pong, below) */
            fx_clip_count[2]++;
            if (!target) {
                if (alt && out_written)
                    to_alt = !to_alt;
                out_written = TRUE;
            }
            goto pass_done;
        }

        /* fx_region.h: nothing reads this pass's target in this run */
        if (target && fxr_serial && pass->rg_serial == fxr_serial && pass->rg_need.mode == FXR_NONE) {
            fxr_n_dead++;
            fx_passes_cached++;
            previous = target->tex;
            prevres[0] = prevres[2] = target->vw;
            prevres[1] = prevres[3] = target->vh;
            output = target->tex;
            continue;
        }
        /* kept passes: its target still holds what this draw would make */
        const gboolean kp = target && pass->kp_free == 2 && !pass->spec;
        if (kp && target->kp_valid && target->kp_gen == ch->gen && target->kp_prog == pass->program &&
            fx_keep_on () && !fxs_cost_on ()) {
            fx_passes_cached++;
            previous = target->tex;
            prevres[0] = prevres[2] = target->vw;
            prevres[1] = prevres[3] = target->vh;
            output = target->tex;
            continue;
        }
        if (target)
            target->kp_valid = FALSE;
        /* GPUPROF: one row per pass and target size */
        WALPE_PROF_PASS_BEGIN (ch->prof_name, ch->inst_index, i,
                               target ? target->w : ch->out_w, target ? target->h : ch->out_h);
        fxs_pre_run (ch, pass);
        glUseProgram (pass->program);
        if (fx_gls.on) {
            /* what this pass last sent is only what the program holds while
             * no other pass used the program since */
            if (!fx_prog_user) {
                fx_prog_user = g_hash_table_new (NULL, NULL);
                fx_thread_mark ();
            }
            if (g_hash_table_lookup (fx_prog_user, GUINT_TO_POINTER (pass->program)) != pass) {
                g_hash_table_insert (fx_prog_user, GUINT_TO_POINTER (pass->program), pass);
                for (int u = 0; u < pass->n_dyn; u++)
                    pass->dyn_last[u].sent = FALSE;
            }
        }
        if (pass->moves_verts) {
            /* a vertex shader that works on a_Position: WE's layer quad
             * in layer units + the orthographic matrix of the layer's
             * render target. Offsets the shader adds are scene pixels;
             * what leaves the target is clipped, as in WE ("Vertex mode
             * only works correctly as the last effect"). Without the
             * layer's geometry: the unit quad as before. */
            static const float ident[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1 };
            float mvp[16];
            if (lay) {
                fx_bind_layer_vao (lay[0], lay[1], lay[2], lay[3], mvp);
                glUniformMatrix4fv (pass->mvp_loc, 1, GL_FALSE, mvp);
            } else {
                fx_bind_pass_vao ();
                glUniformMatrix4fv (pass->mvp_loc, 1, GL_FALSE, ident);
            }
        } else {
            fx_bind_pass_vao ();
        }
        fx_gls_pass_state ();

        /* texture bindings: slot textures (material defaults overridden
         * by the instance), unit 0 default = previous; the pass bind list
         * re-maps units by name ("previous", target names) */
        GLuint unit_tex[FX_MAX_BINDS] = { previous };
        int unit_res[FX_MAX_BINDS][4] = { { 0 } };
        memcpy (unit_res[0], prevres, sizeof (prevres));
        for (int u = 1; u < FX_MAX_BINDS; u++) {
            if (pass->sys[u] && fx_sys[pass->sys[u]].tex) {
                unit_tex[u] = fx_sys[pass->sys[u]].tex;
                unit_res[u][0] = unit_res[u][2] = fx_sys[pass->sys[u]].w;
                unit_res[u][1] = unit_res[u][3] = fx_sys[pass->sys[u]].h;
            } else if (pass->slot[u].tex) {
                unit_tex[u] = pass->slot[u].tex;
                memcpy (unit_res[u], pass->slot[u].res, sizeof (pass->slot[u].res));
            } else if (pass->slot[u].rt == FX_RT_FULLFRAME && fx_fullframe.tex) {
                unit_tex[u] = fx_fullframe.tex;
                unit_res[u][0] = unit_res[u][2] = fx_fullframe.w;
                unit_res[u][1] = unit_res[u][3] = fx_fullframe.h;
            } else if (pass->slot[u].rt) {
                unit_tex[u] = input;
                memcpy (unit_res[u], inres, sizeof (inres));
            }
        }
        for (int b = 0; b < pass->n_binds; b++) {
            int idx = pass->binds[b].index;
            if (idx < 0 || idx >= FX_MAX_BINDS)
                continue;
            if (!strcmp (pass->binds[b].name, "previous")) {
                /* "previous" = what entered this EFFECT (the shine/bloom
                 * combine passes blend their result over it), not the
                 * previous pass's output */
                unit_tex[idx] = input;
                memcpy (unit_res[idx], inres, sizeof (inres));
            } else {
                EffTarget *t = fx_find_target (ch, pass->binds[b].name);
                if (t) {
                    unit_tex[idx] = t->tex;
                    /* render targets are unpadded */
                    unit_res[idx][0] = unit_res[idx][2] = t->vw;
                    unit_res[idx][1] = unit_res[idx][3] = t->vh;
                }
            }
        }

        /* constants a script changed since the last run */
        for (int k = 0; k < pass->n_consts; k++) {
            FxConst *fc = &pass->consts[k];
            if (!fc->dirty)
                continue;
            fc->dirty = FALSE;
            switch (fc->type) {
            case GL_FLOAT:      glUniform1f (fc->loc, fc->v[0]); break;
            case GL_FLOAT_VEC2: glUniform2f (fc->loc, fc->v[0], fc->v[1]); break;
            case GL_FLOAT_VEC3: glUniform3f (fc->loc, fc->v[0], fc->v[1], fc->v[2]); break;
            case GL_FLOAT_VEC4: glUniform4f (fc->loc, fc->v[0], fc->v[1], fc->v[2], fc->v[3]); break;
            default:            glUniform1i (fc->loc, fx_f2i (fc->v[0])); break;
            }
        }

        /* per-frame uniforms only (locations cached at link time) */
        for (int u = 0; u < pass->n_dyn; u++) {
            const FxUniform *fu = &pass->dyn[u];
            FxUniLast *ul = &pass->dyn_last[u];
            switch (fu->kind) {
            case FXU_TEXTURE:
                fx_gls_bind_texture (fu->unit, unit_tex[fu->unit]);
                break;
            case FXU_RESOLUTION: {
                /* (paddedW, paddedH, imageW, imageH) per CTexture */
                const int *r = unit_res[fu->unit];
                const float v[4] = { (float) r[0], (float) r[1], (float) r[2], (float) r[3] };
                if (fx_uniform_changed (ul, v, 4))
                    glUniform4f (fu->loc, v[0], v[1], v[2], v[3]);
                break;
            }
            case FXU_TIME: {
                const float v = fx_shader_time (time);
                if (fx_uniform_changed (ul, &v, 1))
                    glUniform1f (fu->loc, v);
                break;
            }
            case FXU_FRAMETIME:
                if (fx_uniform_changed (ul, &fx_frame.dt, 1))
                    glUniform1f (fu->loc, fx_frame.dt);
                break;
            case FXU_SCREEN: {
                const float v[3] = { (float) fx_frame.sw, (float) fx_frame.sh,
                                     (float) fx_frame.sw / (float) fx_frame.sh };
                if (fx_uniform_changed (ul, v, 3))
                    glUniform3f (fu->loc, v[0], v[1], v[2]);
                break;
            }
            case FXU_TEXEL:
            case FXU_TEXEL_HALF: {
                /* g_TexelSize = one texel of the FRAME (1 / g_Screen.xy),
                 * the same for every pass, not of the pass's target: WE's
                 * blur_h_bloom.vert takes "g_TexelSize.y * 8.0" for its
                 * eighth-size buffer, downsample_quarter_bloom.vert steps
                 * the full frame by it, and workshop blurs derive
                 * "g_TexelSize * g_Texture0Resolution.xy" as the buffer /
                 * screen ratio. Per-target texels made every 1/2..1/16
                 * pass of bloom / bokeh blur 2..16 times too wide. */
                float k = fu->kind == FXU_TEXEL ? 1.0f : 0.5f;
                const float v[2] = { k / (float) fx_frame.sw, k / (float) fx_frame.sh };
                if (fx_uniform_changed (ul, v, 2))
                    glUniform2f (fu->loc, v[0], v[1]);
                break;
            }
            case FXU_MVP_INV: {
                /* inverse of the matrix WE gives the pass: identity for a
                 * pass with an FBO target (clip-space quad), else the
                 * layer's orthographic matrix (layer units -> clip; v = 0,
                 * the image top, sits at clip y = -1 here and layer y is
                 * up, hence the sign). The forward matrix itself stays
                 * identity with the unit quad, which is the same product. */
                float m4[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1 };
                if (!target) {
                    m4[0] = (float) MAX (1, ch->out_vw) / 2.0f;
                    m4[5] = (float) MAX (1, ch->out_vh) / -2.0f;
                }
                const float v[2] = { m4[0], m4[5] };   /* all that varies */
                if (fx_uniform_changed (ul, v, 2))
                    glUniformMatrix4fv (fu->loc, 1, GL_FALSE, m4);
                break;
            }
            case FXU_POINTER: {
                const float v[2] = { fx_frame.px, fx_frame.py };
                if (fx_uniform_changed (ul, v, 2))
                    glUniform2f (fu->loc, v[0], v[1]);
                break;
            }
            case FXU_DAYTIME: {
                GDateTime *now = g_date_time_new_now_local ();
                float d = (float) (g_date_time_get_hour (now) * 3600 + g_date_time_get_minute (now) * 60 +
                                   g_date_time_get_second (now)) / 86400.0f;
                g_date_time_unref (now);
                if (fx_uniform_changed (ul, &d, 1))
                    glUniform1f (fu->loc, d);
                break;
            }
            case FXU_AUDIO: {
                static const float silence[64];
                const WalpeAudioSpectrum *a = fx_audio_frame;
                int sz = fu->unit % 3, right = fu->unit / 3;
                int n = sz == 0 ? 16 : sz == 1 ? 32 : 64;
                const float *v = !a ? silence
                    : sz == 0 ? (right ? a->right16 : a->left16)
                    : sz == 1 ? (right ? a->right32 : a->left32)
                    : (right ? a->right : a->left);
                glUniform1fv (fu->loc, n, v);
                break;
            }
            }
        }
        if (!fx_gls.on)
            glActiveTexture (GL_TEXTURE0);   /* in a run: fx_gls_end() */

        if (target) {
            glBindFramebuffer (GL_FRAMEBUFFER, target->fbo);
            fx_gls_viewport (target->w, target->h);
        } else {
            /* final pass of the chain: render into the chain output FBO
             * (never directly to the screen — the caller composites) */
            if (alt && out_written)
                to_alt = !to_alt;
            out_written = TRUE;
            glBindFramebuffer (GL_FRAMEBUFFER, to_alt ? alt->fbo : ch->out_fbo);
            fx_gls_viewport (ch->out_w, ch->out_h);
        }
        /* The clear before the draw used to run for every pass: the
         * target was written twice per pass (on a 4K scene the
         * clears wrote more bytes per frame than the passes themselves).
         * A pass draws the unit quad over the whole viewport = the whole
         * target with blending, depth test and scissor off, so every
         * pixel is replaced unless the shaders say otherwise
         * (pass->partial) or the pass samples the texture it renders
         * into (reads_own: undefined in GL; with the clear it reads the
         * cleared target on the drivers tested, kept that way).
         * WALPE_FX_CLEAR=1: clear before every pass as before (A/B);
         * WALPE_FX_CLEAR=invalidate: tell the driver instead that the old
         * content is not needed (glInvalidateFramebuffer; nothing to gain
         * on desktop GPUs, a tile-based one saves the tile load). */
        static int clear_mode = -1;   /* 0 = when needed, 1 = always, 2 = invalidate */
        if (clear_mode < 0) {
            const char *e = g_getenv ("WALPE_FX_CLEAR");
            clear_mode = !e || !*e ? 0 : !strcmp (e, "invalidate") ? 2 : *e == '1' ? 1 : 0;
            if (clear_mode == 2 && !(epoxy_is_desktop_gl ()
                                     ? epoxy_gl_version () >= 43 ||
                                       epoxy_has_gl_extension ("GL_ARB_invalidate_subdata")
                                     : epoxy_gl_version () >= 30))
                clear_mode = 0;
        }
        gboolean reads_own = FALSE;
        GLuint draw_tex = target ? target->tex : to_alt ? alt->tex : ch->out_tex;
        for (int u = 0; u < pass->n_dyn && !reads_own; u++)
            reads_own = pass->dyn[u].kind == FXU_TEXTURE && unit_tex[pass->dyn[u].unit] == draw_tex;
        /* fx_region.h: the rectangles of the target this pass has to
         * fill in this run (0 = all of it, as before). Same draw, clipped
         * by the scissor; a clear is clipped the same way. Not in a run of
         * the opt-in "Skip hidden areas" clip (part): such a run has no
         * plan of this kind (fx_run_for_object) and its own rectangles
         * decide. */
        const int rg_tw = target ? target->w : ch->out_w, rg_th = target ? target->h : ch->out_h;
        int rg_px[FXR_MAX_RECTS][4];
        guint64 rg_area = (guint64) rg_tw * rg_th;
        const int rg_n = !part && fxr_serial && pass->rg_serial == fxr_serial && !reads_own && !kp
                             ? fxr_pixels (&pass->rg_need, rg_tw, rg_th, rg_px, &rg_area) : 0;
        if (G_UNLIKELY (part)) {
            /* "Skip hidden areas": once per rectangle that is needed, the
             * rectangle as the scissor box (clear and draw both honour it) */
            fxr_scissor_off ();
            if (fx_clip_test_poison) {
                glClearColor (1, 0, 1, 1);
                glClear (GL_COLOR_BUFFER_BIT);
            }
            glEnable (GL_SCISSOR_TEST);
            for (int q = 0; q < part_n; q++) {
                glScissor (part_px[q][0], part_px[q][1], part_px[q][2], part_px[q][3]);
                if (pass->partial || reads_own || clear_mode == 1) {
                    glClearColor (0, 0, 0, 0);
                    glClear (GL_COLOR_BUFFER_BIT);
                }
                glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
            }
            glDisable (GL_SCISSOR_TEST);
            fx_clip_count[1]++;
        } else if (rg_n) {
            if (!fxr_scissor) {
                glEnable (GL_SCISSOR_TEST);
                fxr_scissor = TRUE;
            }
            if (clear_mode == 2 && !pass->partial) {
                static const GLenum att = GL_COLOR_ATTACHMENT0;
                glInvalidateFramebuffer (GL_FRAMEBUFFER, 1, &att);
            }
            for (int r = 0; r < rg_n; r++) {
                glScissor (rg_px[r][0], rg_px[r][1], rg_px[r][2], rg_px[r][3]);
                if (pass->partial || clear_mode == 1) {
                    glClearColor (0, 0, 0, 0);
                    glClear (GL_COLOR_BUFFER_BIT);
                }
                glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
            }
            fxr_n_restricted++;
            fxr_n_rects += (guint) rg_n;
            /* GPUPROF: the row was opened with the whole target */
            if (WALPE_PROF_ON ())
                walpe_prof_pass_region ((guint64) rg_tw * rg_th, rg_area);
        } else {
            fxr_scissor_off ();
            if (pass->partial || reads_own || clear_mode == 1) {
                glClearColor (0, 0, 0, 0);
                glClear (GL_COLOR_BUFFER_BIT);
            } else if (clear_mode == 2) {
                static const GLenum att = GL_COLOR_ATTACHMENT0;
                glInvalidateFramebuffer (GL_FRAMEBUFFER, 1, &att);
            }
            glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
        }
        fxr_px_full += (guint64) rg_tw * rg_th;
        fxr_px_drawn += rg_area;
        if (fxr_run_frac > 0.0f) {
            fxr_px_asked += (double) rg_tw * rg_th;
            fxr_px_floor += (double) rg_tw * rg_th * fxr_run_frac;
        }
        if (!part)
            fx_clip_count[0]++;
        fx_passes_run++;
        fx_acct.passes_now++;
        fx_acct.px_now += rg_area;
        /* (inside the profiler's pass: the comparison draws of a pass on
         * probation are this pass's cost) */
        if (pass->spec)
            fxr_scissor_off ();   /* (never on with such a pass: fx_run_for_object) */
        fxs_post_draw (ch, pass, i);
        WALPE_PROF_PASS_END ();   /* GPUPROF */
        if (kp && !reads_own && !part) {
            /* kept passes: drawn whole, with the constants of ch->gen
             * (a draw clipped by "Skip hidden areas" is not whole: such a
             * pass is drawn again by the next run) */
            target->kp_valid = TRUE;
            target->kp_gen = ch->gen;
            target->kp_prog = pass->program;
        }

pass_done:
        if (target) {
            previous = target->tex;
            prevres[0] = prevres[2] = target->vw;
            prevres[1] = prevres[3] = target->vh;
            output = target->tex;
        } else {
            previous = to_alt ? alt->tex : ch->out_tex;
            prevres[0] = prevres[2] = ch->out_vw;
            prevres[1] = prevres[3] = ch->out_vh;
            output = previous;
        }
    }

    static int fx_probe = -1;   /* read once: this runs per chain per frame */
    if (fx_probe < 0)
        fx_probe = g_getenv ("WALPE_FX_PROBE") != NULL;
    if (fx_probe) {
        /* debugging aid: corner + center of the chain output, and of the
         * input (straight alpha expected) */
        static int nprobe;
        if (nprobe < 200) {
            nprobe++;
            guint8 oc[4] = { 0 }, oz[4] = { 0 };
            glReadPixels (0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, oc);
            glReadPixels (ch->out_w / 2, ch->out_h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, oz);
            guint8 *buf = g_malloc ((gsize) MAX (1, fullw) * MAX (1, fullh) * 4 + 16);
            fx_gls_forget_textures ();
            glBindTexture (GL_TEXTURE_2D, input);
            int iw = 0, ih = 0;
            glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &iw);
            glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &ih);
            guint8 ic[4] = { 0 };
            if (iw > 0 && ih > 0 && iw <= fullw && ih <= fullh) {
                glGetTexImage (GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf);
                memcpy (ic, buf, 4);
            }
            g_free (buf);
            g_message ("fx probe: %dx%d input corner=%u,%u,%u,%u (tex %dx%d) out corner=%u,%u,%u,%u center=%u,%u,%u,%u",
                       w, h, ic[0], ic[1], ic[2], ic[3], iw, ih, oc[0], oc[1], oc[2], oc[3],
                       oz[0], oz[1], oz[2], oz[3]);
        }
    }
    fx_gls_leave ();
    ch->planned = FALSE;
    walpe_pixstat_fx (fx_clip_count[0], fx_clip_count[1], fx_clip_count[2]);
    fx_clip_count[0] = fx_clip_count[1] = fx_clip_count[2] = 0;
    return output;
}

/* ------------------------------------------------ static chain cache
 * A chain whose output is a pure function of its input texture, its own
 * constants and the render-target sizes renders the same image every
 * frame. Proven static when no pass reads g_Time / g_Frametime /
 * g_Daytime / g_AudioSpectrum* / g_PointerPosition / g_ParallaxPosition,
 * a media system texture or _rt_FullFrameBuffer, and no pass reads a
 * target before an earlier pass of the same run wrote it (feedback from
 * the previous frame). Anything else is never cached. */
static gboolean fx_chain_static (EffectChain *ch)
{
    if (ch->cacheable >= 0)
        return ch->cacheable;
    gboolean ok = TRUE;
    char written[FX_MAX_TARGETS][64];
    int nw = 0;
    for (int i = 0; ok && i < ch->n_passes; i++) {
        EffPass *p = &ch->passes[i];
        if (p->is_swap) {
            ok = FALSE;   /* state carried from frame to frame */
        } else if (p->is_copy) {
            gboolean seen = FALSE;
            for (int w = 0; w < nw; w++)
                seen |= !strcmp (written[w], p->copy_src);
            ok = seen;
        } else {
            for (int u = 0; ok && u < p->n_dyn; u++) {
                int k = p->dyn[u].kind;
                ok = !(k == FXU_TIME || k == FXU_FRAMETIME || k == FXU_AUDIO ||
                       k == FXU_POINTER || k == FXU_DAYTIME);
            }
            for (int u = 0; ok && u < FX_MAX_BINDS; u++)
                ok = !p->sys[u] && p->slot[u].rt != FX_RT_FULLFRAME;
            for (int b = 0; ok && b < p->n_binds; b++) {
                if (!strcmp (p->binds[b].name, "previous"))
                    continue;
                if (!fx_find_target (ch, p->binds[b].name))
                    continue;   /* unbound name: the pass never sees it */
                gboolean seen = FALSE;
                for (int w = 0; w < nw; w++)
                    seen |= !strcmp (written[w], p->binds[b].name);
                ok = seen;
            }
        }
        if (ok && p->target[0] && nw < FX_MAX_TARGETS)
            snprintf (written[nw++], sizeof (written[0]), "%s", p->target);
    }
    ch->cacheable = ok;
    return ok;
}

static gboolean fx_chain_feedback (EffectChain *ch);

/* What the chain's output depends on besides its input texture, its
 * constants and the target sizes (FX_DYN_*; 0 = fx_chain_static). */
static guint fx_chain_dyn (EffectChain *ch)
{
    if (ch->dyn >= 0)
        return (guint) ch->dyn;
    guint m = 0;
    for (int i = 0; i < ch->n_passes; i++) {
        EffPass *p = &ch->passes[i];
        if (p->is_copy)
            continue;
        for (int u = 0; u < p->n_dyn; u++) {
            switch (p->dyn[u].kind) {
            case FXU_TIME:
            case FXU_FRAMETIME: m |= FX_DYN_TIME; break;
            case FXU_AUDIO:     m |= FX_DYN_AUDIO; break;
            case FXU_POINTER:   m |= FX_DYN_POINTER; break;
            case FXU_DAYTIME:   m |= FX_DYN_DAYTIME; break;
            default: break;
            }
        }
        for (int u = 0; u < FX_MAX_BINDS; u++) {
            if (p->sys[u])
                m |= FX_DYN_SYSTEX;
            if (p->slot[u].rt == FX_RT_FULLFRAME)
                m |= FX_DYN_FRAME;
        }
    }
    if (fx_chain_feedback (ch))
        m |= FX_DYN_FEEDBACK;
    if (!m && !fx_chain_static (ch))
        m |= FX_DYN_FEEDBACK;   /* never weaker than the static test */
    ch->dyn = (int) m;
    return m;
}

/* hash of this frame's values of the AUDIO / POINTER inputs in mask (the
 * others have no value that could be compared); one hash per frame and
 * combination, however many sets ask */
static guint64 fx_dyn_values (guint mask)
{
    const guint slot = (mask >> 1) & 3u;   /* AUDIO 2, POINTER 4 */
    if (fx_dyn_memo.have & (1u << slot))
        return fx_dyn_memo.v[slot];
    guint64 h = 1469598103934665603ull;
#define FX_MIX(ptr, len) do { const guint8 *b_ = (const guint8 *) (ptr); \
        for (gsize i_ = 0; i_ < (gsize) (len); i_++) { h ^= b_[i_]; h *= 1099511628211ull; } } while (0)
    if (mask & FX_DYN_AUDIO) {
        const WalpeAudioSpectrum *a = fx_audio_frame;
        if (a) {
            /* the six band arrays, not the time stamp */
            FX_MIX (a->left, sizeof (a->left));
            FX_MIX (a->right, sizeof (a->right));
            FX_MIX (a->left32, sizeof (a->left32));
            FX_MIX (a->right32, sizeof (a->right32));
            FX_MIX (a->left16, sizeof (a->left16));
            FX_MIX (a->right16, sizeof (a->right16));
        } else {
            h ^= 0x51; h *= 1099511628211ull;
        }
    }
    if (mask & FX_DYN_POINTER) {
        FX_MIX (&fx_frame.px, sizeof (float));
        FX_MIX (&fx_frame.py, sizeof (float));
    }
#undef FX_MIX
    fx_dyn_memo.v[slot] = h;
    fx_dyn_memo.have |= 1u << slot;
    return h;
}

static gint fx_cache_on = 1;   /* fx_set_cache_enabled / WALPE_FX_CACHE=0 */

void fx_set_cache_enabled (gboolean on)
{
    const char *e = g_getenv ("WALPE_FX_CACHE");
    if (e && *e)
        on = *e != '0';
    g_atomic_int_set (&fx_cache_on, on ? 1 : 0);
}

void fx_pass_stats (guint *run, guint *cached, gboolean reset)
{
    if (run)
        *run = fx_passes_run;
    if (cached)
        *cached = fx_passes_cached;
    if (reset)
        fx_passes_run = fx_passes_cached = 0;
}

/* -------------------------------------------------- public integration */

#define FX_SOFT_MISSES 30

struct FxSet {
    EffectChain **chains;
    int n;
    int n_failed;       /* enabled effects whose chain could not be built */
    int fold_from;      /* last run: chains from this index on are applied by
                         * the caller (fx_layer_affine), not run; -1 = none */
    int key_fold;       /* static cache: fold_from and the layer quad of */
    float key_lay[4];   /* the run the cached output was made with */
    GLuint out_tex;     /* final composite result */
    int out_w, out_h;
    gboolean ready;
    /* static cache: the last run's output and what it was made from */
    int cacheable;      /* -1 unknown, else every chain static */
    int cullable;       /* 0 unknown, 1 yes, 2 no (fx_cullable) */
    gboolean cached;
    guint gen;          /* bumped by constant/visibility changes, fx_invalidate */
    struct { guint input, gen; int w, h, fullw, fullh, sw, sh; float k; GLuint out;
             float tint[4]; guint64 dyn; } key;
    /* a set that is not static but only follows values that often stand
     * still (the audio spectrum in silence, a resting pointer): its output
     * is kept as well, keyed by those values (key.dyn), once they were the
     * same for two runs in a row (dyn_prev) and until FX_SOFT_MISSES runs
     * in a row each brought new ones - while they move, the output stays
     * in a pooled target */
    guint64 dyn_prev;
    gboolean dyn_seen, soft_keep;
    int soft_misses;
    /* static prefix cache: when a later chain animates, the leading
     * static chains (and the tint pass in front of them) still give the
     * same picture every frame. Their output stays in their own targets,
     * which nothing else writes: the run starts behind them. `end` =
     * first chain not covered, `gen` = gen_ext + the covered chains' gen,
     * out / out_fullw / out_fullh = what the first chain behind reads */
    guint gen_ext;      /* bumped by fx_invalidate only (new input pixels) */
    struct { gboolean valid; guint input, gen; int end, w, h, fullw, fullh, sw, sh; float k;
             float tint[4]; GLuint out; int out_fullw, out_fullh; } pfx;
    /* layer colour (rgb) and opacity (a) applied to the input before the
     * first effect; alpha_after: WALPE_FX_ALPHA=after, the opacity stays
     * with the caller's draw of the result (the earlier build's order) */
    gboolean tinted, alpha_after;
    float tint[4];
    GLuint pre_fbo, pre_tex;
    int pre_pool;       /* as EffTarget.pool */
    int pre_w, pre_h;
    /* quality knobs, effect update rate: with a divisor N the set is
     * redrawn every Nth frame and shows its last output in between
     * (key.out, kept in the last chain's own targets). held = there is
     * such an output; hold_phase = which frames it is redrawn on (see
     * FX_HOLD_STAGGER); hold_epoch counts those redraws. elig = the
     * divisor the set is granted for [0] slow /2, [1] slow /3, [2] all /2,
     * [3] all /3 (1 = redraw every frame), valid for (gen, fps). nom =
     * what its last full run cost with every knob at its default, for the
     * accounting of the frames it is held in. */
    gboolean held;
    guint hold_phase, hold_epoch;
    struct { gboolean valid; guint gen; int fps; guint8 div[4]; } elig;
    struct {
        guint64 px, red, off, hold_px[4], res[3];
        guint passes, passes_off, hold_passes[4];
    } nom;
    /* held prefix: a set with an effect that must be drawn every frame can
     * still hold the chains in front of it. As pfx (the static prefix),
     * but redrawn every `div`th frame; its output stays in the targets of
     * its last chain. */
    struct { gboolean valid; guint input, gen; int end, div, w, h, fullw, fullh, sw, sh; float k;
             float tint[4]; GLuint out; int out_fullw, out_fullh; } hpfx;
    int disp_chain;           /* fx_input_displaced: chain index + 1 the */
    float disp_cov;           /* answer below was worked out for (0 = none) */
    /* fx_region.h: the plan of passes restricted to what is read
     * later, and what it was made from (fx_run_for_object) */
    struct { gboolean valid, ok; guint id, gen, ext; FxNeed caller, tint_need; int w, h, from, post;
             float k; gboolean tint, compose; } rg;
    struct FxFuseSet *fuse;   /* fx_fuse.h: runs of displacement effects
                               * drawn as one pass (fx_quality = fast) */
    guint fxs_mark;           /* fx_fuse.h (fxf_refresh / fxf_settle): passes */
    gboolean fxs_stale;       /* of this set changed programs in the last run */
};

#include "fx_fuse.h"

/* The divisor (1 = redraw every frame) the set tolerates when `want` (2,
 * 3) is asked for. Never held: a chain that carries its own picture from
 * frame to frame (feedback), or reads album art or the frame below. all =
 * every other animated set is held as asked. Else ("slow") only a set
 * whose every animated chain is known to move slowly: it reads no
 * pointer, audio or frame time, its effect is not one that is new in
 * every frame by name, no script changed its constants, and its g_Time
 * terms advance by at most WALPE_FX_SLOW_STEP per redraw at the divisor
 * (fxclass.h); a chain that tolerates only /2 lowers the set to /2. */
static gboolean fx_chain_static (EffectChain *ch);
static gboolean fx_chain_feedback (EffectChain *ch);

/* one chain: 0 = static (never redrawn for time's sake), 1 = must be drawn
 * every frame, 2..want = the divisor it tolerates */
static int fx_chain_hold_div (EffectChain *c, int want, gboolean all, const char **why)
{
    const char *dummy;
    if (!why)
        why = &dummy;
    if (c->feedback < 0)
        c->feedback = fx_chain_feedback (c);
    if (c->feedback == 1) {
        *why = "an effect keeps its own picture from frame to frame";
        return 1;
    }
    gboolean ptr = FALSE, audio = FALSE, frametime = FALSE, timeu = FALSE;
    for (int p = 0; p < c->n_passes; p++) {
        const EffPass *ps = &c->passes[p];
        if (ps->is_copy)
            continue;
        /* (a _rt_FullFrameBuffer slot only shows the frame below on a
         * compose layer, whose input is never static: not held anyway) */
        for (int u = 0; u < FX_MAX_BINDS; u++)
            if (ps->sys[u]) {
                *why = "an effect shows album art";
                return 1;
            }
        for (int u = 0; u < ps->n_dyn; u++) {
            int k = ps->dyn[u].kind;
            ptr |= k == FXU_POINTER;
            audio |= k == FXU_AUDIO;
            frametime |= k == FXU_FRAMETIME;
            timeu |= k == FXU_TIME;
        }
    }
    if (!ptr && !audio && !frametime && !timeu)
        return 0;   /* (g_Daytime alone moves over hours) */
    if (all)
        return want;
    if (ptr || audio || frametime || c->fast_name || c->time_scripted) {
        *why = ptr ? "an effect follows the pointer" : audio ? "an effect reacts to audio"
             : frametime ? "an effect depends on the frame time"
             : c->fast_name ? "an effect is new in every frame (by name)"
             : "a script changes an effect's constants";
        return 1;
    }
    if (!c->time.uses_time || !c->time.derived) {
        /* g_Time reaches the shader in a way the scan could not follow */
        *why = "an effect's speed cannot be told from its shader";
        return 1;
    }
    int dc = walpe_fx_slow_div (&c->time, c->sharp, want, fx_q.fps);
    if (dc < 2)
        *why = "an effect moves too fast for this frame rate";
    return MAX (dc, 1);
}

static int fx_set_hold_eval (struct FxSet *set, int want, gboolean all, const char **why)
{
    int d = want;
    gboolean animated = FALSE;
    const char *dummy;
    if (!why)
        why = &dummy;
    *why = "";
    for (int i = 0; i < set->n; i++) {
        EffectChain *c = set->chains[i];
        if (!c || !c->enabled)
            continue;
        int cd = fx_chain_hold_div (c, d, all, why);
        if (cd == 1)
            return 1;
        if (cd >= 2) {
            animated = TRUE;
            d = MIN (d, cd);
        }
    }
    if (!animated)
        *why = "nothing in it is animated";
    return animated ? d : 1;
}

static const guint8 *fx_set_elig (struct FxSet *set)
{
    int fps = (int) (fx_q.fps + 0.5);
    if (!set->elig.valid || set->elig.gen != set->gen || set->elig.fps != fps) {
        set->elig.valid = TRUE;
        set->elig.gen = set->gen;
        set->elig.fps = fps;
        for (int c = 0; c < 4; c++)
            set->elig.div[c] = (guint8) fx_set_hold_eval (set, 2 + (c & 1), c >= 2, NULL);
    }
    return set->elig.div;
}

gboolean fx_set_hold_epoch (gpointer fxset, guint *epoch)
{
    struct FxSet *set = fxset;
    if (epoch)
        *epoch = 0;
    /* no held output yet (or its target could not be kept): the next run
     * redraws, and so does every run until one is kept */
    if (!set || set->n == 0 || fx_q.div <= 1 || !set->held || !set->key.out)
        return FALSE;
    int hold = fx_set_elig (set)[(fx_q.all ? 2 : 0) + (fx_q.div == 3 ? 1 : 0)];
    if (hold <= 1)
        return FALSE;
    /* due this frame: the run redraws and (if the output is kept again)
     * the epoch moves on; else the held picture is shown as it is */
    gboolean due = (fx_q.frame + set->hold_phase) % (guint) hold == 0;
    if (epoch)
        *epoch = set->hold_epoch + (due ? 1 : 0);
    return TRUE;
}

/* accounting of one run (or held frame) of a set: what it costs at High
 * and what each knob would take off (see FxAcct), as noted at its last
 * full run */
static void fx_acct_set (struct FxSet *set)
{
    fx_acct.px_nom += set->nom.px;
    fx_acct.passes_nom += set->nom.passes;
    fx_acct.px_post_red += set->nom.red;
    fx_acct.px_post_off += set->nom.off;
    fx_acct.passes_post_off += set->nom.passes_off;
    for (int c = 0; c < 4; c++) {
        fx_acct.px_hold[c] += set->nom.hold_px[c];
        fx_acct.passes_hold[c] += set->nom.hold_passes[c];
    }
    for (int l = 0; l < 3; l++)
        fx_acct.px_fxres[l] += set->nom.res[l];
}

/* the layer's own material pass: WE renders an image layer with effects
 * through its material first (genericimage*.frag: texture * g_Color4,
 * flat.frag for solid layers: vec4 (g_Color, g_Alpha), font.frag for
 * text: coverage * g_Color4.a) and runs the effects on that, so tint /
 * colour-key / audio-bar effects see the coloured layer and an effect
 * that adds to the alpha (shine, glow) adds to the layer's opacity
 * instead of being scaled by it afterwards */
static __thread GLuint tint_prog;
static __thread GLint tint_loc_tex, tint_loc_col;

static void fx_tint_program (void)
{
    if (tint_prog)
        return;
    {
        static const char *vs_body =
            "in vec3 a_pos;\nin vec2 a_uv;\nout vec2 v_uv;\n"
            "void main(){ v_uv = a_uv; gl_Position = vec4(a_pos, 1.0); }\n";
        static const char *fs_body =
            "in vec2 v_uv;\nuniform sampler2D u_tex;\nuniform vec4 u_col;\nout vec4 frag;\n"
            "void main(){ vec4 c = texture(u_tex, v_uv); frag = vec4(c.rgb * u_col.rgb, c.a * u_col.a); }\n";
        const char *src[2];
        GLuint sh[2];
        for (int i = 0; i < 2; i++) {
            src[0] = i ? walpe_gl_shader_fs_header () : walpe_gl_shader_header ();
            src[1] = i ? fs_body : vs_body;
            sh[i] = glCreateShader (i ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);
            glShaderSource (sh[i], 2, src, NULL);
            glCompileShader (sh[i]);
        }
        tint_prog = glCreateProgram ();
        glAttachShader (tint_prog, sh[0]);
        glAttachShader (tint_prog, sh[1]);
        glBindAttribLocation (tint_prog, 0, "a_pos");
        glBindAttribLocation (tint_prog, 1, "a_uv");
        glLinkProgram (tint_prog);
        glDeleteShader (sh[0]);
        glDeleteShader (sh[1]);
        tint_loc_tex = glGetUniformLocation (tint_prog, "u_tex");
        tint_loc_col = glGetUniformLocation (tint_prog, "u_col");
    }
}

static void fx_tint_pass (struct FxSet *set, GLuint input, int rw, int rh, gboolean own,
                          const FxNeed *need)
{
    fx_tint_program ();
    /* the tinted copy is read by the first chain only: lent from the pool
     * unless a cache keeps it (a static prefix made of it alone) */
    fx_target_ensure (&set->pre_fbo, &set->pre_tex, &set->pre_pool, &set->pre_w, &set->pre_h,
                      rw, rh, own);
    WALPE_PROF_PASS_BEGIN ("(layer colour)", -1, -1, rw, rh);   /* GPUPROF */
    glUseProgram (tint_prog);
    fx_bind_pass_vao ();
    if (fx_gls.on) {
        fx_gls_pass_state ();
    } else {
        fx_gls.dirty = TRUE;
        glDisable (GL_BLEND);
    }
    fx_gls_bind_texture (0, input);
    glUniform1i (tint_loc_tex, 0);
    glUniform4f (tint_loc_col, set->tint[0], set->tint[1], set->tint[2], set->tint[3]);
    glBindFramebuffer (GL_FRAMEBUFFER, set->pre_fbo);
    fx_gls_viewport (rw, rh);
    /* fx_region.h: only what the first chain reads of the tinted copy */
    int rg_px[FXR_MAX_RECTS][4];
    guint64 rg_area = (guint64) rw * rh;
    const int rg_n = need ? fxr_pixels (need, rw, rh, rg_px, &rg_area) : 0;
    if (rg_n) {
        if (!fxr_scissor) {
            glEnable (GL_SCISSOR_TEST);
            fxr_scissor = TRUE;
        }
        for (int r = 0; r < rg_n; r++) {
            glScissor (rg_px[r][0], rg_px[r][1], rg_px[r][2], rg_px[r][3]);
            glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
        }
        fxr_n_restricted++;
        fxr_n_rects += (guint) rg_n;
        if (WALPE_PROF_ON ())
            walpe_prof_pass_region ((guint64) rw * rh, rg_area);
    } else {
        fxr_scissor_off ();
        glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
    }
    fxr_px_full += (guint64) rw * rh;
    fxr_px_drawn += rg_area;
    if (!fx_gls.on)
        glBindFramebuffer (GL_FRAMEBUFFER, fx_scene_fbo);
    WALPE_PROF_PASS_END ();   /* GPUPROF */
    fx_acct.px_now += rg_area;   /* the layer's own material pass */
    fx_acct.passes_now++;
}

/* mean colour (alpha weighted, 0..255) of FX_LEVEL_N x FX_LEVEL_N bilinear
 * samples of two textures; *flat = the first one is a single colour. One
 * scratch target, two draws, two read-backs; inside a run (the next pass
 * binds its own framebuffer and viewport). */
static gboolean fx_level_means (GLuint a, GLuint b, float ma[3], float mb[3], gboolean *flat)
{
    static const float white[4] = { 1, 1, 1, 1 };
    GLuint tex = 0, fbo = 0;
    guint8 *px = g_malloc (FX_LEVEL_N * FX_LEVEL_N * 4);
    gboolean ok = TRUE;
    fx_tint_program ();
    glGenTextures (1, &tex);
    glBindTexture (GL_TEXTURE_2D, tex);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, FX_LEVEL_N, FX_LEVEL_N, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers (1, &fbo);
    glBindFramebuffer (GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    fx_gls_forget_textures ();
    if (glCheckFramebufferStatus (GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        ok = FALSE;
    glUseProgram (tint_prog);
    fx_bind_pass_vao ();
    if (fx_gls.on) {
        fx_gls_pass_state ();
    } else {
        fx_gls.dirty = TRUE;
        glDisable (GL_BLEND);
    }
    glUniform1i (tint_loc_tex, 0);
    glUniform4fv (tint_loc_col, 1, white);
    fx_gls_viewport (FX_LEVEL_N, FX_LEVEL_N);
    for (int k = 0; ok && k < 2; k++) {
        float *m = k ? mb : ma;
        double sum[3] = { 0, 0, 0 };
        guint8 lo[3] = { 255, 255, 255 }, hi[3] = { 0, 0, 0 };
        fx_gls_bind_texture (0, k ? b : a);
        glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
        glReadPixels (0, 0, FX_LEVEL_N, FX_LEVEL_N, GL_RGBA, GL_UNSIGNED_BYTE, px);
        if (glGetError () != GL_NO_ERROR)
            ok = FALSE;
        for (int i = 0; i < FX_LEVEL_N * FX_LEVEL_N; i++)
            for (int c = 0; c < 3; c++) {
                guint8 v = px[i * 4 + c];
                sum[c] += v * (px[i * 4 + 3] / 255.0);
                lo[c] = MIN (lo[c], v);
                hi[c] = MAX (hi[c], v);
            }
        for (int c = 0; c < 3; c++)
            m[c] = (float) (sum[c] / (FX_LEVEL_N * FX_LEVEL_N));
        if (!k && flat)
            *flat = hi[0] - lo[0] <= 2 && hi[1] - lo[1] <= 2 && hi[2] - lo[2] <= 2;
    }
    if (!fx_gls.on)
        glBindFramebuffer (GL_FRAMEBUFFER, fx_scene_fbo);
    glDeleteFramebuffers (1, &fbo);
    glDeleteTextures (1, &tex);
    fx_gls_forget_textures ();
    g_free (px);
    return ok;
}

/* fx_run_for_object, behind a chain that post-processing "off" would
 * leave out by its family: measure it (see fx_level_rule). in / out =
 * what the chain read and what it drew. */
static void fx_level_probe (struct FxSet *set, EffectChain *ch, GLuint in, GLuint out)
{
    if (ch->lvl_state || !in || !out || in == out)
        return;
    if (ch->lvl_skip) {   /* behind a measurement over one colour */
        ch->lvl_skip--;
        return;
    }
    guint n = ++ch->lvl_draws;
    if (n & (n - 1))
        return;   /* behind runs 1, 2, 4, 8, 16, 32 */
    float mi[3], mo[3];
    gboolean flat = FALSE;
    if (!fx_level_means (in, out, mi, mo, &flat)) {
        ch->lvl_state = 2;   /* cannot tell: it stays */
        return;
    }
    if (flat) {
        /* nothing to see yet: start over, and look again in a while (not
         * in every frame of a long black screen) */
        ch->lvl_draws = 0;
        ch->lvl_skip = 15;
        return;
    }
    float d = 0;
    for (int c = 0; c < 3; c++)
        d = fmaxf (d, fabsf (mo[c] - mi[c]));
    if (d > FX_LEVEL_MAX) {
        ch->lvl_state = 2;
        g_message ("fx: %s on a post-processing layer changes the picture's level (mean %.0f %.0f %.0f "
                   "-> %.0f %.0f %.0f): it stays with post-processing off", ch->file,
                   (double) mi[0], (double) mi[1], (double) mi[2], (double) mo[0], (double) mo[1],
                   (double) mo[2]);
    } else if (n >= 32) {
        ch->lvl_state = 1;
        fxs_picture_changed ();   /* it is left out from the next run on */
    }
    (void) set;
}

void fx_set_input_tint (gpointer fxset, const float *rgb)
{
    struct FxSet *set = fxset;
    if (!set)
        return;
    set->tinted = rgb && (rgb[0] != 1.0f || rgb[1] != 1.0f || rgb[2] != 1.0f);
    for (int i = 0; i < 3; i++)
        set->tint[i] = set->tinted ? rgb[i] : 1.0f;
    set->tint[3] = 1.0f;
}

/* The layer's opacity, after fx_set_input_tint: Wallpaper Engine applies
 * it where it applies the colour, in the layer's own material draw
 * (g_Color4.a / g_UserAlpha / g_Alpha), so the effects work on the
 * already transparent layer. Returns TRUE when the set takes it: the
 * caller then draws the effects' result with opacity 1 (and with the
 * layer's own opacity when the run hands back the untouched input).
 * FALSE with WALPE_FX_ALPHA=after (the earlier build's order: effects on the opaque
 * layer, the opacity in the draw of the result), read when the set is
 * built. */
gboolean fx_set_input_alpha (gpointer fxset, float alpha)
{
    struct FxSet *set = fxset;
    if (!set || set->alpha_after)
        return FALSE;
    alpha = CLAMP (alpha, 0.0f, 1.0f);
    set->tint[3] = alpha;
    if (alpha != 1.0f)
        set->tinted = TRUE;
    return TRUE;
}

static gint fx_stat_ok, fx_stat_fail;

void fx_stats (int *ok, int *failed, gboolean reset)
{
    if (ok)
        *ok = g_atomic_int_get (&fx_stat_ok);
    if (failed)
        *failed = g_atomic_int_get (&fx_stat_fail);
    if (reset) {
        g_atomic_int_set (&fx_stat_ok, 0);
        g_atomic_int_set (&fx_stat_fail, 0);
    }
}

gpointer fx_build_for_object (const char *dir, JsonArray *effects, const char *dbgname,
                              JsonObject *user_props)
{
    fx_user_props = user_props;
    if (!effects || wpj_arr_len (effects) == 0)
        return NULL;
    struct FxSet *set = g_new0 (struct FxSet, 1);
    set->alpha_after = !g_strcmp0 (g_getenv ("WALPE_FX_ALPHA"), "after");
    set->n = (int) wpj_arr_len (effects);
    set->chains = g_new0 (EffectChain *, set->n);
    set->cacheable = -1;
    set->fold_from = set->key_fold = -1;
    set->hold_phase = FX_HOLD_STAGGER ? fx_q.next_phase++ : 0;
    int built = 0;
    for (guint i = 0; i < (guint) set->n; i++) {
        JsonObject *inst = wpj_arr_obj (effects, i);
        if (!inst)
            continue;
        if (!wpj_bool (inst, "visible", TRUE))
            continue;
        {
            /* WALPE_FX_SKIP="shine,pulse": debugging aid - effect files
             * containing any of these substrings are not built */
            const char *skip = g_getenv ("WALPE_FX_SKIP");
            const char *f = wpj_str (inst, "file", "");
            gboolean drop = FALSE;
            if (skip && *skip) {
                char **parts = g_strsplit (skip, ",", -1);
                for (char **q = parts; *q; q++)
                    drop |= **q && strstr (f, *q) != NULL;
                g_strfreev (parts);
            }
            if (drop)
                continue;
        }
        EffectChain *ch = NULL;
        {
            /* WALPE_FX_FAIL="2846673187,foo": test aid - effect files
             * containing any of these substrings fail to build (what an
             * unrepairable shader does on some driver) */
            const char *fail = g_getenv ("WALPE_FX_FAIL");
            const char *f = wpj_str (inst, "file", "");
            gboolean forced = FALSE;
            if (fail && *fail) {
                char **parts = g_strsplit (fail, ",", -1);
                for (char **q = parts; *q; q++)
                    forced |= **q && strstr (f, *q) != NULL;
                g_strfreev (parts);
            }
            if (forced)
                g_warning ("fx: %s: effect %s made to fail (WALPE_FX_FAIL)", dbgname, f);
            else
                ch = fx_chain_build (dir, inst, dbgname);
        }
        if (!ch && wpj_bool (inst, "__walpe_enabled", TRUE))
            set->n_failed++;
        if (ch) {
            /* the caller may pass a filtered copy: "__walpe_index" keeps
             * the position in the scene's effects array (getEffect(i)),
             * "__walpe_enabled" the initial state of a scripted visible */
            ch->inst_index = walpe_d2i (wpj_num (inst, "__walpe_index", (double) i));
            ch->enabled = wpj_bool (inst, "__walpe_enabled", TRUE);
            snprintf (ch->name, sizeof (ch->name), "%s", wpj_str (inst, "name", ""));
            /* GPUPROF: "effects/blur/effect.json" -> "blur" */
            walpe_prof_effect_label (ch->prof_name, sizeof (ch->prof_name),
                                     wpj_str (inst, "file", ""));
        }
        set->chains[built++] = ch; /* NULLs kept: order matters */
        g_atomic_int_inc (ch ? &fx_stat_ok : &fx_stat_fail);
        if (!ch)
            g_message ("fx: %s: effect %s FAILED", dbgname, wpj_str (inst, "file", "?"));
    }
    fx_user_props = NULL;
    set->n = built;
    if (built == 0) {
        g_free (set->chains);
        g_free (set);
        return NULL;
    }
    fxf_build (set, dbgname);
    return set;
}

void fx_free_for_object (gpointer fxset)
{
    struct FxSet *set = fxset;
    if (!set)
        return;
    fxf_free (set);
    for (int i = 0; i < set->n; i++)
        if (set->chains[i])
            fx_chain_free (set->chains[i]);
    g_free (set->chains);
    if (set->out_tex)
        glDeleteTextures (1, &set->out_tex);
    if (!set->pre_pool)
        fx_drop_rt (&set->pre_fbo, &set->pre_tex, set->pre_w, set->pre_h);
    g_free (set);
}

/* run the object's chains over its texture; returns the final texture to
 * draw (or the input when no effects). fullw/fullh = input's padded size */
/* a chain that runs: visible (scripts) and not left out by the
 * post-processing level */
#define FX_ON(c) ((c)->enabled && !fx_chain_dropped (c))
static __thread guint fx_passes_held;

/* Who draws what a run keeps. fx_run_for_object keeps results over frames
 * (the static cache, a held set, the static and the held prefix) in a
 * target that the chain delivering the result owns; every other target
 * of the run is lent from the pool and overwritten by the next layer.
 * "The chain delivering it" is not simply the last enabled one:
 *  - a fused run (fx_fuse.h) is one pass standing in for chains
 *    first..last, run at index `first`;
 *  - a chain whose passes provably copy their input (fx_rewrite.h: an
 *    opacity of 1) is not drawn behind another chain, so the result stays
 *    where the chain in front of it left it.
 * Both exist only with fx_specialize / fx_quality = fast. The loops that
 * pick the keeping chain walk the set in these units; fx_unit_draws says
 * whether the unit starting at chain c writes a target (fed: something
 * of this run was drawn in front of it - a chain or the layer-colour
 * pass - so its input is a target of the working size). */
static inline gboolean fx_unit_draws (EffectChain *c, gboolean fused, gboolean fed)
{
    if (fused || G_LIKELY (!c->n_passes || !c->passes[0].spec || !c->passes[0].spec->identity) ||
        !fxs_chain_identity (c))
        return TRUE;
    /* an identity chain: skipped behind a target, and with fx_quality =
     * fast behind anything; over the layer's own texture the exact mode
     * decides when it runs (fxs_chain_noop) - if it is skipped there, the
     * result is the input and nothing is kept */
    return !fed && !fxs_fast ();
}

static GLuint fx_run_for_object_1 (gpointer fxset, GLuint input, int w, int h,
                                   int fullw, int fullh, double time, float k, gboolean input_static);

/* fx_region.h, WALPE_FX_REGION_CHECK: the pixels of a result texture */
static guint8 *fxr_check_read (GLuint tex, int *w, int *h)
{
    GLuint rf = 0;
    fx_gls_forget_textures ();
    glBindTexture (GL_TEXTURE_2D, tex);
    *w = *h = 0;
    glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, w);
    glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, h);
    if (*w < 1 || *h < 1)
        return NULL;
    guint8 *px = g_malloc ((gsize) *w * (gsize) *h * 4);
    glGenFramebuffers (1, &rf);
    glBindFramebuffer (GL_FRAMEBUFFER, rf);
    glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glReadPixels (0, 0, *w, *h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glBindFramebuffer (GL_FRAMEBUFFER, fx_scene_fbo);
    glDeleteFramebuffers (1, &rf);
    return px;
}

GLuint fx_run_for_object (gpointer fxset, GLuint input, int w, int h,
                          int fullw, int fullh, double time, float k, gboolean input_static)
{
    const int every = fxr_check_every ();
    if (G_LIKELY (!every))
        return fx_run_for_object_1 (fxset, input, w, h, fullw, fullh, time, k, input_static);
    /* WALPE_FX_REGION_CHECK (see fx_region.h): the run, and when it had
     * restricted passes the same run again without the caller's
     * rectangles (they are consumed by the first), compared where the
     * caller reads */
    static __thread guint seq;
    const FxNeed asked = fxr_caller_need;
    const guint r0 = fxr_n_restricted, d0 = fxr_n_dead;
    /* (what else the caller handed over for one run: kept for the second) */
    const __typeof__ (fx_layer) lay0 = fx_layer;
    const __typeof__ (fx_res_run) res0 = fx_res_run;
    GLuint out = fx_run_for_object_1 (fxset, input, w, h, fullw, fullh, time, k, input_static);
    if (asked.mode != FXR_RECTS || (fxr_n_restricted == r0 && fxr_n_dead == d0) || !out || out == input ||
        ++seq % (guint) every != 0)
        return out;
    int aw = 0, ah = 0, bw = 0, bh = 0;
    guint8 *a = fxr_check_read (out, &aw, &ah);
    fx_layer = lay0;
    fx_res_run = res0;
    GLuint whole = fx_run_for_object_1 (fxset, input, w, h, fullw, fullh, time, k, input_static);
    guint8 *b = whole ? fxr_check_read (whole, &bw, &bh) : NULL;
    int diff = 0, maxd = 0;
    if (a && b && aw == bw && ah == bh) {
        guint8 *seen = g_malloc0 ((gsize) aw * ah);
        for (int r = 0; r < asked.n; r++) {
            const int x0 = MAX (0, (int) floorf (asked.r[r][0] * (float) aw - 0.5f));
            const int x1 = MIN (aw, (int) floorf (asked.r[r][2] * (float) aw - 0.5f) + 2);
            const int y0 = MAX (0, (int) floorf (asked.r[r][1] * (float) ah - 0.5f));
            const int y1 = MIN (ah, (int) floorf (asked.r[r][3] * (float) ah - 0.5f) + 2);
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++) {
                    const gsize i = (gsize) y * aw + x;
                    if (seen[i])
                        continue;
                    seen[i] = 1;
                    int d = 0;
                    for (int c = 0; c < 4; c++)
                        d = MAX (d, abs ((int) a[i * 4 + c] - (int) b[i * 4 + c]));
                    diff += d != 0;
                    maxd = MAX (maxd, d);
                }
        }
        g_free (seen);
    } else {
        diff = -1;   /* not the same texture size twice: nothing to compare */
    }
    g_free (a);
    g_free (b);
    const int runs = g_atomic_int_add (&fxr_chk_runs, 1) + 1;
    if (diff > 0) {
        const int bad = g_atomic_int_add (&fxr_chk_bad, 1) + 1;
        if (bad <= 20)
            g_warning ("fx region check: effect set %p (%dx%d): %d pixel(s) differ where the result is read, "
                       "largest difference %d - WALPE_FX_REGION=0 avoids it, please report it",
                       fxset, aw, ah, diff, maxd);
    } else if (diff < 0) {
        g_debug ("fx region check: effect set %p: results of different sizes, not compared", fxset);
    }
    if (runs % 200 == 0)
        g_message ("fx region check: %d run(s) compared, %d differed", runs, g_atomic_int_get (&fxr_chk_bad));
    return whole ? whole : out;
}

static GLuint fx_run_for_object_1 (gpointer fxset, GLuint input, int w, int h,
                                   int fullw, int fullh, double time, float k, gboolean input_static)
{
    /* k > 1: a compose layer shown larger than its size in scene units
     * (targets at the captured pixels, shaders see the layer's size) */
    if (!(k > 0.0f && k < 8.0f))
        k = 1.0f;
    struct FxSet *set = fxset;
    /* the layer's quad, handed over for this run only (taken before the
     * early return, so it can never reach another layer's run) */
    float lay[4] = { fx_layer.cx, fx_layer.cy, fx_layer.w, fx_layer.h };
    gboolean have_lay = fx_layer.set, fold = fx_layer.set && fx_layer.fold;
    fx_layer.set = FALSE;
    /* "Skip hidden areas": the part of the result that will be seen, for
     * this run only too */
    const gboolean have_clip = fx_clip.set;
    const FxClipNeed clip_need = fx_clip.need;
    fx_clip.set = FALSE;
    /* quality knob fx_res: the scale this run has at "full" (k is smaller) */
    const gboolean res_set = fx_res_run.set;
    const float res_k = fx_res_run.k_full;
    float res_keep[3];
    memcpy (res_keep, fx_res_run.keep, sizeof (res_keep));
    fx_res_run.set = FALSE;
    /* fx_region.h: what the caller samples of the result, for this
     * run only (taken before the early returns, like the layer quad) */
    const FxNeed rg_caller = fxr_caller_need;
    fxr_caller_need.mode = FXR_NONE;
    if (!set || set->n == 0)
        return input;
    if (!have_lay)
        memset (lay, 0, sizeof (lay));
    /* post-processing "off": "left out" belongs to the constants the
     * chain was measured with (fx_level_rule); with new ones it runs and
     * is measured again */
    if (G_UNLIKELY (fx_q.post == WALPE_POST_OFF) && fx_q.compose) {
        for (int i = 0; i < set->n; i++) {
            EffectChain *c = set->chains[i];
            if (c && c->lvl_gen != c->gen + 1) {
                c->lvl_gen = c->gen + 1;
                if (c->lvl_state == 1) {
                    c->lvl_state = 0;
                    c->lvl_draws = 0;
                    c->lvl_skip = 0;
                }
            }
        }
    }
    /* WE draws the last displayed effect's pass straight into the scene
     * with the layer's real transform: a transform effect in Vertex mode
     * there moves / scales / rotates the layer on screen, unclipped.
     * Here the chain output is composited by the caller, so the trailing
     * run of such effects is not drawn as passes: the caller folds
     * fx_layer_affine() into its draw transform. (WE clips all but the
     * very last of several; folding the whole trailing run shows what the
     * author asked for instead of an empty layer.) */
    set->fold_from = -1;
    for (int i = set->n - 1; fold && i >= 0; i--) {
        EffectChain *c = set->chains[i];
        if (!c || !FX_ON (c))
            continue;
        if (!c->vertex_affine)
            break;
        set->fold_from = i;
    }
    /* other chains to run, or other layer units, than the cached output
     * was made with */
    if (set->key_fold != set->fold_from || memcmp (set->key_lay, lay, sizeof (lay))) {
        set->cached = FALSE;
        set->held = FALSE;
        set->pfx.valid = FALSE;
    }
    set->key_fold = set->fold_from;
    memcpy (set->key_lay, lay, sizeof (lay));
    if (set->cacheable < 0) {
        set->cacheable = 1;
        for (int i = 0; i < set->n; i++)
            if (set->chains[i] && !fx_chain_static (set->chains[i]))
                set->cacheable = 0;
    }
    /* a set that only follows the audio spectrum or the pointer (struct
     * FxSet, key.dyn): cached like a static one while those stand still */
    guint64 dynv = 0;
    gboolean soft = FALSE;
    if (set->cacheable == 0 && input_static && g_atomic_int_get (&fx_cache_on)) {
        guint dmask = 0;
        for (int i = 0; i < set->n; i++) {
            EffectChain *c = set->chains[i];
            if (c && c->enabled && !(set->fold_from >= 0 && i >= set->fold_from))
                dmask |= fx_chain_dyn (c);
        }
        soft = dmask && !(dmask & ~(guint) (FX_DYN_AUDIO | FX_DYN_POINTER));
        if (soft)
            dynv = fx_dyn_values (dmask);
    }
    gboolean use_cache = input_static && g_atomic_int_get (&fx_cache_on) &&
                         (set->cacheable == 1 || soft);
    /* the output of a soft set is worth a target of its own only while it
     * gets reused */
    if (soft) {
        if (set->dyn_seen && set->dyn_prev == dynv) {
            set->soft_keep = TRUE;
            set->soft_misses = 0;
        } else if (set->soft_keep && ++set->soft_misses > FX_SOFT_MISSES) {
            set->soft_keep = FALSE;
        }
        set->dyn_prev = dynv;
        set->dyn_seen = TRUE;
    } else {
        set->dyn_seen = FALSE;
        set->soft_keep = FALSE;
    }
    gboolean keep_out = use_cache && (!soft || set->soft_keep);
    GLuint check_tex = 0, check_fbo = 0;
    int check_w = 0, check_h = 0;
    /* nothing the last output was made from changed (but time) */
    gboolean key_same = set->key.input == input && set->key.gen == set->gen &&
        set->key.dyn == dynv &&
        set->key.w == w && set->key.h == h && set->key.fullw == fullw &&
        set->key.fullh == fullh && set->key.k == k && set->key.sw == fx_frame.sw &&
        set->key.sh == fx_frame.sh && !memcmp (set->key.tint, set->tint, sizeof (set->tint));
    /* Quality knobs, effect update rate: an animated set over a static
     * input may be redrawn every Nth frame only (N granted per set:
     * fx_set_hold_eval). In between it shows its last output, as long as
     * nothing but time changed; a constant a script sets, the layer's
     * colour, a size or the input changing redraws it at once. With the
     * default divisor of 1 this is never taken. */
    gboolean can_hold = input_static && !use_cache && set->cacheable == 0;
    int hold = 1;
    if (fx_q.div > 1 && can_hold)
        hold = fx_set_elig (set)[(fx_q.all ? 2 : 0) + (fx_q.div == 3 ? 1 : 0)];
    if (hold > 1 && set->held && key_same && set->key.out && set->key.out != input &&
        (fx_q.frame + set->hold_phase) % (guint) hold != 0) {
        fx_acct_set (set);
        fx_passes_held += set->nom.passes;
        return set->key.out;
    }
    if (use_cache && set->cached && key_same) {
        /* WALPE_FX_CACHE_CHECK=1 (diagnostics): every 20th hit, render the
         * chains anyway and compare with the cached image */
        static int check = -1;
        static __thread guint hits;
        if (check < 0)
            check = g_getenv ("WALPE_FX_CACHE_CHECK") != NULL;
        if (!check || ++hits % 20 != 0 || set->key.out == input) {
            for (int i = 0; i < set->n; i++)
                if (set->chains[i] && FX_ON (set->chains[i]))
                    fx_passes_cached += (guint) set->chains[i]->n_passes;
            return set->key.out;
        }
        glBindTexture (GL_TEXTURE_2D, set->key.out);
        glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &check_w);
        glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &check_h);
        fx_make_rt (&check_fbo, &check_tex, check_w, check_h);
        fx_rt_bytes -= (gint64) check_w * check_h * (fx_rt_half_float () ? 8 : 4);
        fx_rt_count--;   /* diagnostics copy: deleted below, not counted */
        GLuint rf = 0;
        glGenFramebuffers (1, &rf);
        glBindFramebuffer (GL_READ_FRAMEBUFFER, rf);
        glFramebufferTexture2D (GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                set->key.out, 0);
        glBindFramebuffer (GL_DRAW_FRAMEBUFFER, check_fbo);
        glBlitFramebuffer (0, 0, check_w, check_h, 0, 0, check_w, check_h, GL_COLOR_BUFFER_BIT,
                           GL_NEAREST);
        glBindFramebuffer (GL_FRAMEBUFFER, fx_scene_fbo);
        glDeleteFramebuffers (1, &rf);
    }
    set->key.input = input;
    set->key.gen = set->gen;
    set->key.w = w;
    set->key.h = h;
    set->key.fullw = fullw;
    set->key.fullh = fullh;
    set->key.k = k;
    set->key.sw = fx_frame.sw;
    set->key.sh = fx_frame.sh;
    set->key.dyn = dynv;
    memcpy (set->key.tint, set->tint, sizeof (set->tint));
    GLuint current = input;
    /* chains that run as passes (folded ones are applied by the caller,
     * who then tints the untouched input as usual) */
    gboolean any = FALSE;
    for (int i = 0; i < set->n; i++)
        any |= set->chains[i] && FX_ON (set->chains[i]) &&
               !(set->fold_from >= 0 && i >= set->fold_from);
    gboolean tint_pass = set->tinted && any;
    /* static prefix (see struct FxSet): the leading static chains of a
     * set that is not static as a whole, over a static input */
    int pfx_end = 0;
    guint pfx_gen = set->gen_ext;
    gboolean pfx_any = FALSE;
    if (input_static && set->cacheable == 0 && !check_tex && g_atomic_int_get (&fx_cache_on)) {
        for (; pfx_end < set->n; pfx_end++) {
            EffectChain *c = set->chains[pfx_end];
            if (set->fold_from >= 0 && pfx_end >= set->fold_from)
                break;
            if (!c)
                continue;
            pfx_gen += c->gen;
            if (!FX_ON (c))
                continue;
            if (!fx_chain_static (c))
                break;
            pfx_any = TRUE;
        }
        if (tint_pass)
            pfx_any = TRUE;   /* the tint pass alone is worth keeping */
    }
    /* Quality knobs, effect update rate, part two: a set that cannot be
     * held as a whole (one of its effects must be drawn every frame) can
     * still hold the chains in front of that effect: their output is kept
     * like the static prefix's and redrawn every hp_div-th frame. hp_end =
     * first chain not covered, hp_last = the chain whose targets keep the
     * output, hp_div = 0: no such prefix. Never with the default divisor. */
    const int in_fullw = fullw, in_fullh = fullh;
    int hp_end = 0, hp_div = 0, hp_last = -1;
    guint hp_gen = set->gen_ext;
    /* the fused runs as they are for this run (rebuilt here when passes
     * changed programs in the last one): everything below that names a
     * chain by its index goes by them */
    fxf_refresh (set);
    if (fx_q.div > 1 && hold <= 1 && can_hold && !check_tex) {
        gboolean fed = tint_pass;
        for (; hp_end < set->n; hp_end++) {
            EffectChain *c = set->chains[hp_end];
            if (set->fold_from >= 0 && hp_end >= set->fold_from)
                break;
            if (!c)
                continue;
            if (FX_ON (c)) {
                /* a fused run stands in for chains hp_end..ul: the prefix
                 * takes all of them or ends in front of the run (the one
                 * pass cannot be held for some of its effects only) */
                int ul = hp_end, ud = 0;
                const gboolean fused = fxf_at (set, hp_end, &ul) != NULL;
                gboolean every = FALSE;
                for (int j = hp_end; j <= ul && !every; j++) {
                    EffectChain *m = set->chains[j];
                    if (!m || !FX_ON (m))
                        continue;
                    int cd = fx_chain_hold_div (m, fx_q.div, fx_q.all, NULL);
                    every = cd == 1;
                    if (cd >= 2)
                        ud = ud ? MIN (ud, cd) : cd;
                }
                if (every)
                    break;
                if (ud)
                    hp_div = hp_div ? MIN (hp_div, ud) : ud;
                if (fx_unit_draws (c, fused, fed)) {
                    hp_last = hp_end;   /* where the unit runs */
                    fed = TRUE;
                }
                for (; hp_end < ul; hp_end++)
                    if (set->chains[hp_end])
                        hp_gen += set->chains[hp_end]->gen;
                c = set->chains[hp_end];
            }
            hp_gen += c->gen;
        }
        if (hp_div < 2 || hp_last < 0)
            hp_end = hp_div = 0, hp_last = -1;
    }
    gboolean hp_hit = hp_div >= 2 && set->hpfx.valid && set->hpfx.end == hp_end &&
        set->hpfx.gen == hp_gen && set->hpfx.div == hp_div && set->hpfx.input == input &&
        set->hpfx.w == w && set->hpfx.h == h && set->hpfx.fullw == fullw &&
        set->hpfx.fullh == fullh && set->hpfx.k == k && set->hpfx.sw == fx_frame.sw &&
        set->hpfx.sh == fx_frame.sh && !memcmp (set->hpfx.tint, set->tint, sizeof (set->tint)) &&
        (fx_q.frame + set->hold_phase) % (guint) hp_div != 0;
    /* which chains keep their targets (see the pool): feedback chains,
     * and the chain whose output a cache holds over frames - the last one
     * that runs when the whole set is cached, the last of a static prefix */
    int last_run = -1, pfx_last = -1;
    {
        /* by units (fx_unit_draws): the index a fused run is run at, and
         * never a chain that is skipped as an identity */
        gboolean fed = tint_pass;
        for (int i = 0; i < set->n; i++) {
            EffectChain *c = set->chains[i];
            if (!c || !FX_ON (c) || (set->fold_from >= 0 && i >= set->fold_from))
                continue;
            int ul = i;
            const gboolean fused = fxf_at (set, i, &ul) != NULL;
            if (fx_unit_draws (c, fused, fed)) {
                last_run = i;
                if (pfx_any && i < pfx_end)
                    pfx_last = i;
                fed = TRUE;
            }
            i = ul;
        }
    }
    fx_pool_begin_run ();
    fx_gls_begin ();
    int first = 0;
    int held = 0;   /* pool entry (+1) of the texture `current` lives in */
    const gboolean pfx_ok = pfx_any && set->pfx.valid && set->pfx.end == pfx_end &&
        set->pfx.gen == pfx_gen &&
        set->pfx.input == input && set->pfx.w == w && set->pfx.h == h &&
        set->pfx.fullw == fullw && set->pfx.fullh == fullh && set->pfx.k == k &&
        set->pfx.sw == fx_frame.sw && set->pfx.sh == fx_frame.sh &&
        !memcmp (set->pfx.tint, set->tint, sizeof (set->tint));
    /* fx_region.h: this run's passes restricted to what is read
     * later. Planned backwards from what the caller samples of the result
     * (rg_caller); only for a run nothing keeps over frames, and only in
     * the default mode of the quality knobs. The static prefix, while it
     * is drawn, is drawn in full (it is kept); chains behind it read all
     * of it. rg_tint = what the first chain reads of the tinted copy. The
     * plan is kept with the set and made again when anything it was made
     * from changes (set->rg: the rectangles, sizes, constants through
     * set->gen, which chains run). */
    FxNeed rg_tint = { FXR_NONE, 0, { { 0 } } };
    fxr_serial = 0;
    fxr_run_frac = 0.0f;
    if (rg_caller.mode == FXR_RECTS) {
        for (int r = 0; r < rg_caller.n; r++)
            fxr_run_frac += (rg_caller.r[r][2] - rg_caller.r[r][0]) * (rg_caller.r[r][3] - rg_caller.r[r][1]);
        fxr_run_frac = CLAMP (fxr_run_frac, 1e-6f, 1.0f);
    }
    if (rg_caller.mode == FXR_RECTS && fxr_enabled () && !keep_out && !check_tex &&
        fx_q.div <= 1 && !set->fuse && set->fold_from < 0 && last_run >= 0 &&
        !fxs_cost_on () /* (WALPE_FX_COST dumps and times whole targets) */) {
        const int from = pfx_any ? pfx_end : 0;
        const gboolean tint = tint_pass && !pfx_any;   /* (a tinted copy the prefix keeps is drawn in full) */
        const guint ext = (fx_fullframe.tex ? 1u : 0u) | (fx_sys[1].tex ? 2u : 0u) | (fx_sys[2].tex ? 4u : 0u);
        if (!(set->rg.valid && set->rg.gen == set->gen && set->rg.w == w && set->rg.h == h &&
              set->rg.k == k && set->rg.from == from && set->rg.tint == tint &&
              set->rg.post == fx_q.post && set->rg.compose == fx_q.compose && set->rg.ext == ext &&
              !memcmp (&set->rg.caller, &rg_caller, sizeof (rg_caller)))) {
            const int prw = MAX (1, (int) ceilf (w * k)), prh = MAX (1, (int) ceilf (h * k));
            int *in_w = g_newa (int, set->n), *in_h = g_newa (int, set->n);
            /* forwards: the size of the texture each chain reads; 0 = one
             * that exists in full (the layer's texture, the static prefix) */
            int cw = 0, chh = 0;
            gboolean ok = TRUE;
            if (tint)
                cw = prw, chh = prh;
            memset (&set->rg, 0, sizeof (set->rg));
            fxr_serial = (guint) g_atomic_int_add (&fxr_next_id, 1) + 1u;
            for (int i = from; ok && i < set->n; i++) {
                EffectChain *c = set->chains[i];
                if (!c || !FX_ON (c))
                    continue;
                if (c->feedback < 0)
                    c->feedback = fx_chain_feedback (c);
                ok = c->feedback != 1 && c->ready;
                for (int q = 0; ok && q < c->n_passes; q++)
                    ok = !c->passes[q].spec;
                in_w[i] = cw;
                in_h[i] = chh;
                if (ok)
                    fxr_result_size (c, prw, prh, &cw, &chh);
            }
            ok = ok && cw > 0 && chh > 0;
            if (ok) {
                /* backwards: a linear sample of the result takes the two
                 * texels around it (see fxr_plan_chain) */
                FxNeed need = fxr_need_grow (&rg_caller, 0.55f / (float) cw, 0.55f / (float) chh);
                for (int i = set->n - 1; ok && i >= from; i--) {
                    EffectChain *c = set->chains[i];
                    if (!c || !FX_ON (c))
                        continue;
                    FxNeed in;
                    ok = fxr_plan_chain (c, &need, prw, prh, in_w[i], in_h[i], &in);
                    if (in_w[i] <= 0)
                        break;   /* reads a texture that exists in full */
                    need = in;
                }
                if (ok && tint && need.mode == FXR_RECTS)
                    set->rg.tint_need = need;
            }
            set->rg.valid = TRUE;
            set->rg.ok = ok;
            set->rg.id = fxr_serial;
            set->rg.gen = set->gen;
            set->rg.caller = rg_caller;
            set->rg.w = w;
            set->rg.h = h;
            set->rg.k = k;
            set->rg.from = from;
            set->rg.tint = tint;
            set->rg.post = fx_q.post;
            set->rg.compose = fx_q.compose;
            set->rg.ext = ext;
            fxr_log_plan (set, ok, from, cw, chh);
            for (int i = from; ok && fxr_log_on > 0 && i < set->n; i++)
                for (int q = 0; set->chains[i] && FX_ON (set->chains[i]) && q < set->chains[i]->n_passes; q++)
                    fxr_log_pass (i, q, &set->chains[i]->passes[q], fxr_serial);
        }
        /* the passes carry the plan's id; 0 = none in this run */
        fxr_serial = set->rg.ok ? set->rg.id : 0;
        if (set->rg.ok)
            rg_tint = set->rg.tint_need;
    }
    /* what the frame costs at High starts behind the static prefix only */
    const int first_hi = pfx_ok ? pfx_end : 0;
    if (hp_hit) {
        /* the held prefix is shown again: start behind it */
        for (int i = 0; i < hp_end; i++)
            if (set->chains[i] && FX_ON (set->chains[i])) {
                if (i < first_hi)
                    fx_passes_cached += (guint) set->chains[i]->n_passes;
                else
                    fx_passes_held += (guint) set->chains[i]->n_passes;
            }
        current = set->hpfx.out;
        fullw = set->hpfx.out_fullw;
        fullh = set->hpfx.out_fullh;
        first = hp_end;
    } else if (pfx_ok) {
        /* nothing the prefix depends on changed: start behind it */
        for (int i = 0; i < pfx_end; i++)
            if (set->chains[i] && FX_ON (set->chains[i]))
                fx_passes_cached += (guint) set->chains[i]->n_passes;
        current = set->pfx.out;
        fullw = set->pfx.out_fullw;
        fullh = set->pfx.out_fullh;
        first = pfx_end;
    } else {
        set->pfx.valid = FALSE;
        if (pfx_any) {
            set->pfx.input = input;
            set->pfx.gen = pfx_gen;
            set->pfx.end = pfx_end;
            set->pfx.w = w;
            set->pfx.h = h;
            set->pfx.fullw = fullw;
            set->pfx.fullh = fullh;
            set->pfx.k = k;
            set->pfx.sw = fx_frame.sw;
            set->pfx.sh = fx_frame.sh;
            memcpy (set->pfx.tint, set->tint, sizeof (set->tint));
        }
        if (tint_pass) {
            fx_tint_pass (set, input, MAX (1, (int) ceilf (w * k)), MAX (1, (int) ceilf (h * k)),
                          pfx_any && pfx_last < 0, rg_tint.mode == FXR_RECTS ? &rg_tint : NULL);
            current = set->pre_tex;
            held = set->pre_pool;
            fullw = w, fullh = h;   /* an unpadded FBO from here on */
        }
    }
    if (!hp_hit) {
        set->hpfx.valid = FALSE;
        if (hp_div >= 2) {
            set->hpfx.input = input;
            set->hpfx.gen = hp_gen;
            set->hpfx.end = hp_end;
            set->hpfx.div = hp_div;
            set->hpfx.w = w;
            set->hpfx.h = h;
            set->hpfx.fullw = in_fullw;
            set->hpfx.fullh = in_fullh;
            set->hpfx.k = k;
            set->hpfx.sw = fx_frame.sw;
            set->hpfx.sh = fx_frame.sh;
            memcpy (set->hpfx.tint, set->tint, sizeof (set->tint));
        }
    }
    {
        /* quality accounting: what this frame costs with every knob at its
         * default (the chains behind the static prefix), what post-
         * processing "reduced" / "off" take off, and what an update
         * divisor could hold: the chains from there up to the first one
         * that must be drawn every frame */
        const float kn = res_set ? res_k : k;
        int rw = MAX (1, (int) ceilf (w * kn)), rh = MAX (1, (int) ceilf (h * kn));
        memset (&set->nom, 0, sizeof (set->nom));
        const gboolean tint_hi = tint_pass && first_hi == 0;
        if (tint_hi) {
            set->nom.px += (guint64) rw * rh;
            set->nom.passes++;
        }
        for (int cmb = 0; can_hold && cmb < 4; cmb++) {
            const int want = 2 + (cmb & 1);
            guint64 hpx = 0;
            guint hpass = 0;
            gboolean got = FALSE;
            for (int i = first_hi; i < set->n; i++) {
                EffectChain *c = set->chains[i];
                if (set->fold_from >= 0 && i >= set->fold_from)
                    break;
                if (!c || !c->enabled)
                    continue;
                int cd = fx_chain_hold_div (c, want, cmb >= 2, NULL);
                if (cd == 1 || (cd >= 2 && cd < want))
                    break;
                guint np = 0;
                hpx += fx_chain_px (c, rw, rh, 1, &np);
                hpass += np;
                got |= cd >= 2;
            }
            if (got) {
                set->nom.hold_px[cmb] = hpx + (tint_hi ? (guint64) rw * rh : 0);
                set->nom.hold_passes[cmb] = hpass + (tint_hi ? 1 : 0);
            }
        }
        for (int i = first_hi; i < set->n; i++) {
            EffectChain *c = set->chains[i];
            if (!c || !c->enabled || (set->fold_from >= 0 && i >= set->fold_from))
                continue;
            guint np = 0;
            guint64 px = fx_chain_px (c, rw, rh, 1, &np);
            set->nom.px += px;
            set->nom.passes += np;
            if (fx_q.compose && walpe_fx_family_cosmetic ((WalpeFxFamily) c->family) &&
                c->lvl_state != 2) {   /* (2: measured to change the level, it stays) */
                set->nom.off += px;
                set->nom.passes_off += np;
            } else if (walpe_fx_family_reducible ((WalpeFxFamily) c->family)) {
                guint64 less = px - fx_chain_px (c, rw, rh, 2, NULL);
                set->nom.red += less;
                set->nom.off += less;
            }
        }
        for (int l = 0; res_set && l < 3; l++)
            set->nom.res[l] = (guint64) (set->nom.px * (1.0 - CLAMP ((double) res_keep[l], 0.0, 1.0)));
    }
    /* the held prefix is complete: kept only when its result sits in a
     * target a chain owns (`held`: not one lent from the pool, which the
     * next layer's run overwrites) */
#define FX_HPFX_DONE() do { \
        set->hpfx.out = current; \
        set->hpfx.out_fullw = fullw; \
        set->hpfx.out_fullh = fullh; \
        set->hpfx.valid = !held && current != input; \
    } while (0)
    /* "Skip hidden areas" (fx_clip.h): from the last chain back, what each
     * pass has to fill. Only with a clip at work (have_clip), and only
     * where nothing keeps a chain's picture beyond this frame or runs the
     * chains in another form: a whole-set cache (use_cache), a held set or
     * held prefix (effect update rate), the static prefix while it is
     * being made, feedback chains, fused runs and specialised programs
     * (fx_fuse.h / fx_specialize.h: their own draws are not planned),
     * inner buffers at reduced resolution (post-processing "reduced"); a
     * transform folded into the layer's matrix moves the result on screen
     * (the caller's rectangle would not fit). Everything else is run as
     * ever. */
    if (G_UNLIKELY (have_clip)) {
        /* a run with this clip does not use the region plan of fx_region.h:
         * the two sets of rectangles are not intersected. With the serial
         * at 0 every pass takes the clip's path or the whole draw, and no
         * pass is left out by that plan. */
        fxr_serial = 0;
        gboolean clip = !use_cache && set->fold_from < 0 && hold <= 1 && hp_div < 2 &&
                        !set->fuse && !check_tex && fx_q.post == WALPE_POST_FULL;
        int crw = MAX (1, (int) ceilf (w * k)), crh = MAX (1, (int) ceilf (h * k));
        /* the caller draws the result with a linear filter: a pixel at
         * the edge of a box reads the texels next to its own as well
         * (a result smaller than its place on screen) */
        FxClipNeed need = fx_clip_test_nopad ? clip_need : fx_need_local (&clip_need, crw, crh);
        for (int i = set->n - 1; i >= first; i--) {
            EffectChain *ch = set->chains[i];
            if (!ch || !FX_ON (ch) || (set->fold_from >= 0 && i >= set->fold_from))
                continue;
            if (ch->feedback < 0)
                ch->feedback = fx_chain_feedback (ch);
            if (clip && (ch->feedback == 1 || ch->vertex_affine ||
                         (pfx_any && first == 0 && i < pfx_end) || need.mode == FXN_FULL))
                clip = FALSE;
            for (int q = 0; clip && q < ch->n_passes; q++)
                if (ch->passes[q].spec || ch->passes[q].is_swap)
                    clip = FALSE;
            if (!clip) {
                fx_chain_unplan (ch);
                continue;
            }
            need = fx_chain_plan (ch, need, crw, crh);
        }
    }
    for (int i = first; i < set->n; i++) {
        if (pfx_any && first == 0 && i == pfx_end) {
            /* the prefix is complete: what the chains behind it read
             * (kept only in a target that is not the pool's) */
            set->pfx.out = current;
            set->pfx.out_fullw = fullw;
            set->pfx.out_fullh = fullh;
            set->pfx.valid = !held;
        }
        if (hp_div >= 2 && first < hp_end && i == hp_end)
            FX_HPFX_DONE ();
        EffectChain *ch = set->chains[i];
        const gboolean runs = ch && FX_ON (ch) && !(set->fold_from >= 0 && i >= set->fold_from);
        /* a run of displacement effects starting here, as one pass */
        int fz_last = i;
        EffectChain *fz = runs ? fxf_at (set, i, &fz_last) : NULL;
        if (G_UNLIKELY (set->fuse != NULL)) {
            /* whichever of the two does not draw keeps no target: the
             * members when the fused pass stands in for them (one of them
             * may own the result it kept before the run was fused), the
             * fused pass when its members run one by one */
            if (fz) {
                for (int j = i; j <= fz_last; j++)
                    if (set->chains[j] && set->chains[j]->out_tex)
                        fx_chain_drop_targets (set->chains[j], 0);
            } else {
                fxf_idle (set, i);
            }
        }
        if (!runs)
            continue;   /* off, or the caller applies it: fx_layer_affine() */
        if (fz)
            ch = fz;
        if (ch->feedback < 0)
            ch->feedback = fx_chain_feedback (ch);
        gboolean keep = (keep_out && i == last_run) || (pfx_any && i == pfx_last);
        /* a held set's output must survive the frames it is shown in:
         * the chain that ends up holding it keeps that target (it is
         * redrawn every Nth frame, so it is not dropped in between) */
        gboolean hold_keep = (hold > 1 && i == last_run) || (hp_div >= 2 && i == hp_last);
        int own = ch->feedback == 1 ? FX_OWN_ALL : (keep || hold_keep) ? FX_OWN_RESULT : FX_OWN_NONE;
        GLuint before = current;
        current = fx_chain_run (ch, current, w, h, fullw, fullh, time, k,
                                have_lay ? lay : NULL, own);
        /* post-processing "off": a chain of a cosmetic family runs until
         * it is known to leave the picture's level alone */
        if (G_UNLIKELY (fx_q.post == WALPE_POST_OFF) && fx_q.compose && !ch->lvl_state &&
            walpe_fx_family_cosmetic ((WalpeFxFamily) ch->family) && fx_level_rule ())
            fx_level_probe (set, ch, before, current);
        if (current != input)
            fullw = w, fullh = h; /* later chains read an unpadded FBO */
        /* this chain's input and its targets other than its result are
         * free for the next chain */
        int res = 0;
        if (ch->out_pool && ch->out_tex == current)
            res = ch->out_pool;
        else
            fx_pool_put (ch->out_pool);
        for (int t = 0; t < ch->n_targets; t++) {
            if (ch->targets[t].pool && ch->targets[t].tex == current)
                res = ch->targets[t].pool;
            else
                fx_pool_put (ch->targets[t].pool);
        }
        if (current != before) {
            fx_pool_put (held);
            held = res;
        }
        if (keep && ch->feedback != 1 && !hold_keep)
            fx_chain_drop_targets (ch, current);   /* a cache holds only the result */
        i = fz_last;
    }
    fxf_settle (set);
    fxr_scissor_off ();   /* fx_region.h */
    fxr_run_frac = 0.0f;
    fxr_serial = 0;
    if (pfx_any && first == 0 && pfx_end >= set->n) {
        /* every chain that runs is static (the animated one is off) */
        set->pfx.out = current;
        set->pfx.out_fullw = fullw;
        set->pfx.out_fullh = fullh;
        set->pfx.valid = !held;
    }
    if (hp_div >= 2 && first < hp_end && hp_end >= set->n)
        FX_HPFX_DONE ();
#undef FX_HPFX_DONE
    fx_gls_end ();
    if (current == set->pre_tex && set->pre_tex) {
        current = input;      /* no chain ran: the caller tints as usual */
        held = 0;
    }
    set->key.out = current;
    /* Kept over frames only when the result sits in a target a chain owns
     * (held = 0), never in one lent from the pool: the next layer's run
     * overwrites that, and the cache would show its picture. The loops
     * above give the delivering chain its own target; this is what holds
     * when one of them guessed wrong (the set is then drawn again in the
     * next frame, as before it had a cache). */
    set->cached = keep_out && !held;
    set->held = FALSE;
    if (hold > 1 && last_run >= 0 && current != input) {
        set->held = !held;
        set->hold_epoch++;
    }
    fx_acct_set (set);
    if (check_tex) {
        /* compare the fresh output with the cached copy (both RGBA8) */
        static gint checks, bad, maxd;
        gsize n = (gsize) check_w * check_h * 4;
        guint8 *a = g_malloc (n), *b = g_malloc (n);
        GLuint rf = 0;
        glGenFramebuffers (1, &rf);
        glBindFramebuffer (GL_FRAMEBUFFER, rf);
        glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, current, 0);
        glReadPixels (0, 0, check_w, check_h, GL_RGBA, GL_UNSIGNED_BYTE, a);
        glBindFramebuffer (GL_FRAMEBUFFER, check_fbo);
        glReadPixels (0, 0, check_w, check_h, GL_RGBA, GL_UNSIGNED_BYTE, b);
        glBindFramebuffer (GL_FRAMEBUFFER, fx_scene_fbo);
        glDeleteFramebuffers (1, &rf);
        int d = 0;
        for (gsize i = 0; i < n; i++)
            d = MAX (d, abs ((int) a[i] - (int) b[i]));
        g_free (a);
        g_free (b);
        glDeleteFramebuffers (1, &check_fbo);
        glDeleteTextures (1, &check_tex);
        g_atomic_int_inc (&checks);
        if (d)
            g_atomic_int_inc (&bad);
        if (d > g_atomic_int_get (&maxd))
            g_atomic_int_set (&maxd, d);
        if (d || g_atomic_int_get (&checks) % 50 == 0)
            g_message ("fx cache check: %d cached outputs re-rendered, %d differed, max diff %d",
                       g_atomic_int_get (&checks), g_atomic_int_get (&bad),
                       g_atomic_int_get (&maxd));
    }
    return current;
}

/* The transform effects the last fx_run_for_object() left to the caller,
 * as one affine map of the layer's local coordinates (scene pixels, y
 * up): x' = m[0] x + m[2] y + m[4], y' = m[1] x + m[3] y + m[5]. It goes
 * between the layer's world transform and its quad. Each effect is
 * transform.vert's applyFx() on the vertex position:
 *   p' = (rotate(p - 0.5, -angle) + offset) * scale + 0.5
 * - rotation about (0.5, 0.5) (the layer centre, half a pixel off), then
 * the offset in pixels, then the scale (which scales the offset too).
 * FALSE (identity) when nothing was left. */
gboolean fx_layer_affine (gpointer fxset, float m[6])
{
    struct FxSet *set = fxset;
    float a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;
    gboolean any = FALSE;
    for (int i = set ? set->fold_from : -1; i >= 0 && i < set->n; i++) {
        EffectChain *ch = set->chains[i];
        if (!ch || !ch->enabled || !ch->vertex_affine)
            continue;
        const EffPass *p = &ch->passes[0];
        const float *off = p->consts[p->xf[0]].v, *sc = p->consts[p->xf[1]].v;
        float r = (float) p->xf_sign * p->consts[p->xf[2]].v[0];
        float cs = cosf (r), sn = sinf (r);
        /* L = S * R, t = S * (off - R * (0.5, 0.5)) + (0.5, 0.5) */
        float la = sc[0] * cs, lc = -sc[0] * sn, lb = sc[1] * sn, ld = sc[1] * cs;
        float lx = sc[0] * (off[0] - (0.5f * cs - 0.5f * sn)) + 0.5f;
        float ly = sc[1] * (off[1] - (0.5f * sn + 0.5f * cs)) + 0.5f;
        /* this effect after the ones before it */
        float na = la * a + lc * b, nb = lb * a + ld * b;
        float nc = la * c + lc * d, nd = lb * c + ld * d;
        float nx = la * tx + lc * ty + lx, ny = lb * tx + ld * ty + ly;
        a = na; b = nb; c = nc; d = nd; tx = nx; ty = ny;
        any = TRUE;
    }
    m[0] = a; m[1] = b; m[2] = c; m[3] = d; m[4] = tx; m[5] = ty;
    return any;
}

/* enabled effects of the set whose chain could not be built */
int fx_failed_count (gpointer fxset)
{
    struct FxSet *set = fxset;
    return set ? set->n_failed : 0;
}

gboolean fx_is_static (gpointer fxset)
{
    struct FxSet *set = fxset;
    if (!set || !set->n)
        return FALSE;
    if (set->cacheable < 0) {
        set->cacheable = 1;
        for (int i = 0; i < set->n; i++)
            if (set->chains[i] && !fx_chain_static (set->chains[i]))
                set->cacheable = 0;
    }
    return set->cacheable == 1;
}

/* a pass reads a target no earlier pass of the same run wrote, or copies
 * from one: the chain carries a picture over from the previous frame */
static gboolean fx_chain_feedback (EffectChain *ch)
{
    char written[FX_MAX_TARGETS][64];
    int nw = 0;
    for (int i = 0; i < ch->n_passes; i++) {
        EffPass *p = &ch->passes[i];
        if (p->is_swap)
            return TRUE;   /* the targets live from frame to frame */
        if (p->is_copy) {
            gboolean seen = FALSE;
            for (int w = 0; w < nw; w++)
                seen |= !strcmp (written[w], p->copy_src);
            if (!seen)
                return TRUE;
        } else {
            for (int b = 0; b < p->n_binds; b++) {
                if (!strcmp (p->binds[b].name, "previous") ||
                    !fx_find_target (ch, p->binds[b].name))
                    continue;
                gboolean seen = FALSE;
                for (int w = 0; w < nw; w++)
                    seen |= !strcmp (written[w], p->binds[b].name);
                if (!seen)
                    return TRUE;
            }
        }
        if (p->target[0] && nw < FX_MAX_TARGETS)
            snprintf (written[nw++], sizeof (written[0]), "%s", p->target);
    }
    return FALSE;
}

void fx_free_targets_for_object (gpointer fxset)
{
    struct FxSet *set = fxset;
    if (!set)
        return;
    for (int i = 0; i < set->n; i++) {
        EffectChain *ch = set->chains[i];
        if (!ch)
            continue;
        if (ch->feedback < 0)
            ch->feedback = fx_chain_feedback (ch);
        if (ch->feedback != 1)
            fx_chain_drop_targets (ch, 0);
    }
    fxf_drop_targets (set);
    if (set->pre_pool) {
        set->pre_pool = 0;
        set->pre_tex = set->pre_fbo = 0;
    } else {
        fx_drop_rt (&set->pre_fbo, &set->pre_tex, set->pre_w, set->pre_h);
    }
    set->cached = FALSE;
    set->held = FALSE;
    set->pfx.valid = FALSE;
    set->hpfx.valid = FALSE;
}

/* ---- quality knobs: queries for the scene ------------------------------ */

gboolean fx_post_dropped (gpointer fxset)
{
    struct FxSet *set = fxset;
    if (!set || fx_q.post != WALPE_POST_OFF)
        return FALSE;
    gboolean any = FALSE;
    for (int i = 0; i < set->n; i++) {
        const EffectChain *c = set->chains[i];
        if (!c || !c->enabled)
            continue;
        if (!walpe_fx_family_cosmetic ((WalpeFxFamily) c->family))
            return FALSE;
        /* not measured yet, or measured to change the level: it runs */
        if (fx_level_rule () && (c->lvl_state != 1 || c->lvl_gen != c->gen + 1))
            return FALSE;
        any = TRUE;
    }
    return any;
}

void fx_acct_compose (gpointer fxset, int vw, int vh, float k, guint64 draw_px, gboolean dropped)
{
    struct FxSet *set = fxset;
    /* the compose layer's own draw: effect work at High, gone with the
     * layer when "off" leaves none of its chains */
    fx_acct.px_nom += draw_px;
    gboolean all_cosmetic = set != NULL;
    for (int i = 0; set && i < set->n; i++) {
        const EffectChain *c = set->chains[i];
        if (c && c->enabled && !walpe_fx_family_cosmetic ((WalpeFxFamily) c->family))
            all_cosmetic = FALSE;
    }
    if (all_cosmetic)
        fx_acct.px_post_off += draw_px;
    if (!dropped) {
        fx_acct.px_now += draw_px;
        return;
    }
    /* the layer was skipped: its chains as they would have run */
    int rw = MAX (1, (int) ceilf (vw * k)), rh = MAX (1, (int) ceilf (vh * k));
    for (int i = 0; set && i < set->n; i++) {
        EffectChain *c = set->chains[i];
        if (!c || !c->enabled)
            continue;
        guint np = 0;
        guint64 px = fx_chain_px (c, rw, rh, 1, &np);
        fx_acct.px_nom += px;
        fx_acct.passes_nom += np;
        fx_acct.px_post_off += px;
        fx_acct.passes_post_off += np;
    }
}

void fx_pass_stats_held (guint *held, gboolean reset)
{
    if (held)
        *held = fx_passes_held;
    if (reset)
        fx_passes_held = 0;
}

void fx_quality_describe (gpointer fxset, const char *layer, GString *out)
{
    struct FxSet *set = fxset;
    if (!set || !out)
        return;
    const guint8 *el = fx_set_elig (set);
    for (int i = 0; i < set->n; i++) {
        EffectChain *c = set->chains[i];
        if (!c)
            continue;
        gboolean st = fx_chain_static (c);
        g_string_append_printf (out, "%s | %s%s | %s", layer ? layer : "?", c->file,
                                c->enabled ? "" : " (off)", st ? "static" : "animated");
        if (!st) {
            if (c->time.uses_time && c->time.derived)
                g_string_append_printf (out, ", rate %.3g/s", c->time.rate * c->sharp);
            else if (c->time.uses_time)
                g_string_append (out, ", rate not derivable");
            if (c->fast_name)
                g_string_append (out, ", new every frame (by name)");
            if (c->time_scripted)
                g_string_append (out, ", constants scripted");
        }
        if (!st)
            g_string_append_printf (out, " | slow: 1/%d", fx_chain_hold_div (c, 3, FALSE, NULL));
        if (c->family)
            g_string_append_printf (out, " | %s", walpe_fx_family_name ((WalpeFxFamily) c->family));
        g_string_append_c (out, '\n');
    }
    const char *why_slow = "", *why_all = "";
    fx_set_hold_eval (set, 3, FALSE, &why_slow);
    fx_set_hold_eval (set, 3, TRUE, &why_all);
    g_string_append_printf (out, "%s | set: update divisor granted slow/2 %d, slow/3 %d, all/2 %d, "
                            "all/3 %d (at %d fps; 1 = every frame)%s%s%s%s\n", layer ? layer : "?",
                            el[0], el[1], el[2], el[3], set->elig.fps,
                            *why_slow ? "; slow: " : "", why_slow, *why_all ? "; all: " : "",
                            why_all);
}

/* The share (0..1) of a painted mask that lets an effect act: an opacity
 * mask above 2 %, a flow map off its rest value (0.498) in red or green.
 * res = (padded w, padded h, image w, image h) of the texture: only the
 * image is looked at. Read from a 96 x 96 sampling of the texture (about
 * 37000 texels): an estimate, a few per cent either way. */
static float fx_mask_coverage (GLuint tex, const int res[4], gboolean flow)
{
    enum { N = 96 };
    GLint fb = 0, vp[4] = { 0, 0, 0, 0 };
    glGetIntegerv (GL_FRAMEBUFFER_BINDING, &fb);
    glGetIntegerv (GL_VIEWPORT, vp);
    GLuint fbo = 0, t = 0;
    glActiveTexture (GL_TEXTURE0);   /* the unit every quad draw binds anew */
    glGenTextures (1, &t);
    glBindTexture (GL_TEXTURE_2D, t);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, N, N, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers (1, &fbo);
    glBindFramebuffer (GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
    float cov = 1.0f;   /* cannot tell: everywhere */
    if (glCheckFramebufferStatus (GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        /* the unit quad holds the texture's (0, 0) at its top left corner:
         * scaled about that corner so that the image part fills the target */
        float sx = res[0] > 0 && res[2] > 0 && res[2] < res[0] ? (float) res[0] / res[2] : 1.0f;
        float sy = res[1] > 0 && res[3] > 0 && res[3] < res[1] ? (float) res[1] / res[3] : 1.0f;
        const float m[9] = { sx, 0, 0, 0, sy, 0, sx - 1.0f, 1.0f - sy, 1 };
        guint8 *px = g_malloc0 ((gsize) N * N * 4);
        glViewport (0, 0, N, N);
        walpe_glutil_draw_quad (tex, TRUE, 1.0f, m, WALPE_BLEND_NONE);
        glPixelStorei (GL_PACK_ALIGNMENT, 4);
        glReadPixels (0, 0, N, N, GL_RGBA, GL_UNSIGNED_BYTE, px);
        int hit = 0;
        for (int i = 0; i < N * N; i++) {
            int r = px[i * 4], g = px[i * 4 + 1];
            hit += flow ? (abs (r - 127) > 2 || abs (g - 127) > 2) : r > 5;
        }
        cov = (float) hit / (float) (N * N);
        g_free (px);
    }
    glBindFramebuffer (GL_FRAMEBUFFER, (GLuint) fb);
    glViewport (vp[0], vp[1], vp[2], vp[3]);
    glDeleteFramebuffers (1, &fbo);
    glDeleteTextures (1, &t);
    return cov;
}

/* the effect that runs first on the layer: the first enabled chain */
static EffectChain *fx_first_chain (struct FxSet *set, int *ci)
{
    for (int i = 0; set && i < set->n; i++)
        if (set->chains[i] && set->chains[i]->enabled) {
            *ci = i;
            return set->chains[i];
        }
    return NULL;
}

/* Does this pass of the layer's first effect read the layer's own texture
 * at displaced coordinates? Only that effect's passes read it: unit 0 of
 * its first drawn pass (`first`), a "previous" bind, an _rt_ slot; every
 * later effect reads a render target. */
static gboolean fx_pass_displaces (EffectChain *c, const EffPass *p, gboolean first)
{
    guint in_units = first ? 1u : 0;
    for (int u = 1; u < FX_MAX_BINDS; u++)
        if (!p->sys[u] && !p->slot[u].tex && p->slot[u].rt == FX_RT_INPUT)
            in_units |= 1u << u;
    for (int b = 0; b < p->n_binds; b++) {
        int idx = p->binds[b].index;
        if (idx < 0 || idx >= FX_MAX_BINDS)
            continue;
        if (!strcmp (p->binds[b].name, "previous"))
            in_units |= 1u << idx;
        else if (fx_find_target (c, p->binds[b].name))
            in_units &= ~(1u << idx);
    }
    return (in_units & ~p->in_straight) != 0;   /* else texel for texel, or not at all */
}

/* How much of the layer the first effect that runs reads at displaced
 * coordinates (scenenode_effects.h). */
float fx_input_displaced (gpointer fxset)
{
    struct FxSet *set = fxset;
    int ci = -1;
    EffectChain *c = fx_first_chain (set, &ci);
    if (!c)
        return 0.0f;
    if (set->disp_chain == ci + 1)
        return set->disp_cov;
    float cov = 0.0f;
    gboolean first = TRUE;
    for (int i = 0; i < c->n_passes; i++) {
        EffPass *p = &c->passes[i];
        if (p->is_copy)
            continue;   /* copies one target into another */
        gboolean disp = fx_pass_displaces (c, p, first);
        first = FALSE;
        if (!disp)
            continue;
        /* displaced where its painted masks let it: all of them have to */
        float pc = 1.0f;
        for (int u = 1; u < FX_MAX_BINDS; u++) {
            guint bit = 1u << u;
            if (((p->paint_op | p->paint_flow) & bit) && p->slot[u].tex && !p->sys[u])
                pc = fminf (pc, fx_mask_coverage (p->slot[u].tex, p->slot[u].res,
                                                  (p->paint_flow & bit) != 0));
        }
        cov = fmaxf (cov, pc);
    }
    set->disp_chain = ci + 1;
    set->disp_cov = cov;
    g_debug ("fx: the first effect (%s) reads the layer's texture at displaced coordinates "
             "over %.1f %% of it", c->file, cov * 100.0);
    return cov;
}

/* The image part of a texture, sampled on an n x n grid (RGBA, n * n * 4
 * bytes): one bilinear lookup per grid point. The caller keeps the
 * framebuffer binding and the viewport. */
static gboolean fx_grid_read (GLuint tex, const int res[4], int n, guint8 *px)
{
    GLuint fbo = 0, t = 0;
    gboolean ok = FALSE;
    glActiveTexture (GL_TEXTURE0);
    glGenTextures (1, &t);
    glBindTexture (GL_TEXTURE_2D, t);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, n, n, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers (1, &fbo);
    glBindFramebuffer (GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
    if (glCheckFramebufferStatus (GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        float sx = res[0] > 0 && res[2] > 0 && res[2] < res[0] ? (float) res[0] / res[2] : 1.0f;
        float sy = res[1] > 0 && res[3] > 0 && res[3] < res[1] ? (float) res[1] / res[3] : 1.0f;
        const float m[9] = { sx, 0, 0, 0, sy, 0, sx - 1.0f, 1.0f - sy, 1 };
        glViewport (0, 0, n, n);
        walpe_glutil_draw_quad (tex, TRUE, 1.0f, m, WALPE_BLEND_NONE);
        glPixelStorei (GL_PACK_ALIGNMENT, 4);
        glReadPixels (0, 0, n, n, GL_RGBA, GL_UNSIGNED_BYTE, px);
        ok = TRUE;
    }
    glBindFramebuffer (GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers (1, &fbo);
    glDeleteTextures (1, &t);
    return ok;
}

/* A copy of a w x h texture `levels` halvings smaller (2 x 2 averages, as
 * the reduced input of a layer's effects holds them); 0 = not possible.
 * The caller keeps the framebuffer binding and the viewport. */
static GLuint fx_halved_copy (GLuint tex, int w, int h, int levels)
{
    /* upside down: a quad draw puts the texture's first row at the top
     * of the target, which is the target texture's last row */
    static const float flip[9] = { 1, 0, 0, 0, -1, 0, 0, 0, 1 };
    GLuint cur = tex, fbo = 0;
    glGenFramebuffers (1, &fbo);
    for (int k = 0; k < levels && cur; k++) {
        w = MAX (1, (w + 1) / 2);
        h = MAX (1, (h + 1) / 2);
        GLuint t = 0;
        glActiveTexture (GL_TEXTURE0);
        glGenTextures (1, &t);
        glBindTexture (GL_TEXTURE_2D, t);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glBindFramebuffer (GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
        gboolean ok = glCheckFramebufferStatus (GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        if (ok) {
            glViewport (0, 0, w, h);
            walpe_glutil_draw_quad (cur, TRUE, 1.0f, flip, WALPE_BLEND_NONE);
        }
        if (cur != tex)
            glDeleteTextures (1, &cur);
        cur = t;
        if (!ok) {
            glDeleteTextures (1, &cur);
            cur = 0;
        }
    }
    glBindFramebuffer (GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers (1, &fbo);
    return cur == tex ? 0 : cur;
}

/* What the layer's picture would lose if its texture were stored `levels`
 * halvings smaller as the input of its effects (scenenode_effects.h): the
 * share of the picture where the first effect reads it at displaced
 * coordinates AND a lookup between the texels gives a value more than
 * 16 / 255 away from the same lookup in the reduced copy. Sampled at
 * 251 x 251 points (63001; 251 divides none of the usual sizes, so the
 * points fall on every position between the texels) with one bilinear
 * lookup each, the way an effect pass reads its input. A smooth picture -
 * a blurred backdrop, a sky - loses nothing; fine detail does (stars, hair,
 * line art). 1 = cannot tell. */
float fx_input_soft_cost (gpointer fxset, guint tex, const int res[4], int levels)
{
    enum { N = 251 };
    struct FxSet *set = fxset;
    int ci = -1;
    EffectChain *c = fx_first_chain (set, &ci);
    if (!c || !tex || levels < 1 || res[0] < 2 || res[1] < 2)
        return 1.0f;
    GLint fb = 0, vp[4] = { 0, 0, 0, 0 };
    glGetIntegerv (GL_FRAMEBUFFER_BINDING, &fb);
    glGetIntegerv (GL_VIEWPORT, vp);
    /* level 0 with plain bilinear filtering, whatever the textures' own
     * state is (a mip chain would hand the grid an average) */
    GLuint smp = 0;
    glGenSamplers (1, &smp);
    glSamplerParameteri (smp, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri (smp, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glSamplerParameteri (smp, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri (smp, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindSampler (0, smp);
    guint8 *a = g_malloc ((gsize) N * N * 4), *b = g_malloc ((gsize) N * N * 4);
    guint8 *m = g_malloc ((gsize) N * N * 4), *act = g_malloc0 ((gsize) N * N);
    guint8 *pact = g_malloc ((gsize) N * N);
    float cost = 1.0f;
    GLuint half = fx_halved_copy (tex, res[0], res[1], levels);
    if (half && fx_grid_read (tex, res, N, a) && fx_grid_read (half, res, N, b)) {
        /* where the first effect displaces its lookups: any of its passes
         * that do, each where all of its painted masks let it */
        gboolean first = TRUE;
        for (int i = 0; i < c->n_passes; i++) {
            EffPass *p = &c->passes[i];
            if (p->is_copy)
                continue;
            gboolean disp = fx_pass_displaces (c, p, first);
            first = FALSE;
            if (!disp)
                continue;
            memset (pact, 1, (gsize) N * N);
            for (int u = 1; u < FX_MAX_BINDS; u++) {
                guint bit = 1u << u;
                if (!((p->paint_op | p->paint_flow) & bit) || !p->slot[u].tex || p->sys[u])
                    continue;
                if (!fx_grid_read (p->slot[u].tex, p->slot[u].res, N, m))
                    continue;   /* cannot tell: everywhere */
                gboolean flow = (p->paint_flow & bit) != 0;
                for (int k = 0; k < N * N; k++) {
                    int r = m[k * 4], g = m[k * 4 + 1];
                    if (!(flow ? (abs (r - 127) > 2 || abs (g - 127) > 2) : r > 5))
                        pact[k] = 0;
                }
            }
            for (int k = 0; k < N * N; k++)
                act[k] |= pact[k];
        }
        int hit = 0;
        for (int k = 0; k < N * N; k++) {
            if (!act[k])
                continue;
            const guint8 *x = a + k * 4, *y = b + k * 4;
            int d = abs (x[3] - y[3]);
            if (x[3] > 8 || y[3] > 8)   /* colour counts where something shows */
                for (int ch = 0; ch < 3; ch++)
                    d = MAX (d, abs (x[ch] - y[ch]));
            hit += d > 16;
        }
        cost = (float) hit / (float) (N * N);
    }
    if (half)
        glDeleteTextures (1, &half);
    glBindSampler (0, 0);
    glDeleteSamplers (1, &smp);
    glBindFramebuffer (GL_FRAMEBUFFER, (GLuint) fb);
    glViewport (vp[0], vp[1], vp[2], vp[3]);
    g_free (a);
    g_free (b);
    g_free (m);
    g_free (act);
    g_free (pact);
    g_debug ("fx: stored %d level(s) smaller, the first effect (%s) would read %.2f %% of the "
             "layer's picture more than 16/255 off", levels, c->file, cost * 100.0);
    return cost;
}

gboolean fx_cullable (gpointer fxset)
{
    struct FxSet *set = fxset;
    if (!set)
        return TRUE;
    if (!set->cullable) {
        set->cullable = 1;
        for (int i = 0; i < set->n; i++) {
            EffectChain *ch = set->chains[i];
            /* a transform effect in Vertex mode is folded into the layer's
             * own matrix by the caller (fx_layer_affine): it may move the
             * layer into view */
            if (ch && (ch->vertex_affine || fx_chain_feedback (ch)))
                set->cullable = 2;
        }
    }
    return set->cullable == 1;
}

void fx_invalidate (gpointer fxset)
{
    struct FxSet *set = fxset;
    if (set) {
        set->gen++;
        set->gen_ext++;
    }
}

/* a constant or the visibility of one chain changed: the whole-set cache
 * is stale, the static prefix only when the chain belongs to it */
static void fx_chain_changed (gpointer fxset, EffectChain *ch)
{
    struct FxSet *set = fxset;
    if (set)
        set->gen++;
    ch->gen++;
}

gboolean fx_uses_audio (gpointer fxset)
{
    struct FxSet *set = fxset;
    if (!set)
        return FALSE;
    for (int i = 0; i < set->n; i++) {
        EffectChain *ch = set->chains[i];
        for (int k = 0; ch && k < ch->n_passes; k++)
            if (ch->passes[k].uses_audio)
                return TRUE;
    }
    return FALSE;
}

gboolean fx_uses_pointer (gpointer fxset)
{
    struct FxSet *set = fxset;
    for (int i = 0; set && i < set->n; i++) {
        EffectChain *ch = set->chains[i];
        for (int p = 0; ch && p < ch->n_passes; p++)
            for (int u = 0; u < ch->passes[p].n_dyn; u++)
                if (ch->passes[p].dyn[u].kind == FXU_POINTER)
                    return TRUE;
    }
    return FALSE;
}

gboolean fx_has_effects (gpointer fxset)
{
    struct FxSet *set = fxset;
    return set && set->n > 0;
}

guint fx_dynamic_inputs (gpointer fxset)
{
    struct FxSet *set = fxset;
    guint m = 0;
    for (int i = 0; set && i < set->n; i++)
        if (set->chains[i] && set->chains[i]->enabled)
            m |= fx_chain_dyn (set->chains[i]);
    return m;
}

guint64 fx_frame_signature (gpointer fxset, gboolean *vol)
{
    struct FxSet *set = fxset;
    if (!set || !set->n)
        return 0;
    guint m = fx_dynamic_inputs (set);
    if (vol && (m & (FX_DYN_TIME | FX_DYN_DAYTIME | FX_DYN_SYSTEX | FX_DYN_FEEDBACK)))
        *vol = TRUE;
    guint64 h = fx_dyn_values (m & (FX_DYN_AUDIO | FX_DYN_POINTER));
    h ^= (guint64) set->gen * 0x9E3779B97F4A7C15ull;
    h ^= (guint64) m << 48;
    return h;
}

/* ----------------------------------------- SceneScript access (render thread) */

static EffectChain *fx_chain_at (gpointer fxset, int inst)
{
    struct FxSet *set = fxset;
    for (int i = 0; set && i < set->n; i++)
        if (set->chains[i] && set->chains[i]->inst_index == inst)
            return set->chains[i];
    return NULL;
}

int fx_find_effect (gpointer fxset, const char *name)
{
    struct FxSet *set = fxset;
    for (int i = 0; set && name && i < set->n; i++)
        if (set->chains[i] && !strcmp (set->chains[i]->name, name))
            return set->chains[i]->inst_index;
    return -1;
}

int fx_effect_count (gpointer fxset)
{
    struct FxSet *set = fxset;
    int n = 0;
    for (int i = 0; set && i < set->n; i++)
        if (set->chains[i])
            n = MAX (n, set->chains[i]->inst_index + 1);
    return n;
}

const char *fx_effect_name (gpointer fxset, int inst)
{
    EffectChain *ch = fx_chain_at (fxset, inst);
    return ch ? ch->name : NULL;
}

int fx_get_enabled (gpointer fxset, int inst)
{
    EffectChain *ch = fx_chain_at (fxset, inst);
    return ch ? ch->enabled : -1;
}

void fx_set_enabled (gpointer fxset, int inst, gboolean on)
{
    EffectChain *ch = fx_chain_at (fxset, inst);
    if (ch && ch->enabled != on) {
        ch->enabled = on;
        fx_chain_changed (fxset, ch);
    }
}

int fx_get_constant (gpointer fxset, int inst, int pass, const char *key, float v[4])
{
    EffectChain *ch = fx_chain_at (fxset, inst);
    for (int p = 0; ch && p < ch->n_passes; p++) {
        if (pass >= 0 && p != pass)
            continue;
        for (int k = 0; k < ch->passes[p].n_consts; k++) {
            FxConst *fc = &ch->passes[p].consts[k];
            if (!strcmp (fc->key, key)) {
                memcpy (v, fc->v, sizeof (float) * 4);
                return fc->n;
            }
        }
    }
    return 0;
}

gboolean fx_set_constant (gpointer fxset, int inst, int pass, const char *key,
                          const float *v, int n)
{
    EffectChain *ch = fx_chain_at (fxset, inst);
    gboolean hit = FALSE;
    for (int p = 0; ch && p < ch->n_passes; p++) {
        if (pass >= 0 && p != pass)
            continue;
        for (int k = 0; k < ch->passes[p].n_consts; k++) {
            FxConst *fc = &ch->passes[p].consts[k];
            if (strcmp (fc->key, key))
                continue;
            gboolean changed = FALSE;
            for (int c = 0; c < fc->n; c++) {
                float nv = c < n ? v[c] : (n == 1 ? v[0] : fc->v[c]);
                changed |= nv != fc->v[c];
                fc->v[c] = nv;
            }
            if (changed) {
                fc->dirty = TRUE;
                ch->time_scripted = TRUE;   /* its speed is the script's now */
                fx_chain_changed (fxset, ch);
            }
            hit = TRUE;
        }
    }
    return hit;
}

gboolean fx_uses_system_textures (gpointer fxset)
{
    struct FxSet *set = fxset;
    for (int i = 0; set && i < set->n; i++) {
        EffectChain *ch = set->chains[i];
        for (int p = 0; ch && p < ch->n_passes; p++)
            for (int u = 0; u < FX_MAX_BINDS; u++)
                if (ch->passes[p].sys[u])
                    return TRUE;
    }
    return FALSE;
}

/* WALPE_PROP_PROBE (propprobe.h): the layer's effect state as a JSON array
 * [{i, name, on, passes: [{constant: [v...], ...}]}] */
void fx_probe_dump (gpointer fxset, JsonBuilder *b)
{
    struct FxSet *set = fxset;
    json_builder_begin_array (b);
    for (int i = 0; set && i < set->n; i++) {
        EffectChain *ch = set->chains[i];
        if (!ch)
            continue;
        json_builder_begin_object (b);
        json_builder_set_member_name (b, "i");
        json_builder_add_int_value (b, ch->inst_index);
        json_builder_set_member_name (b, "name");
        json_builder_add_string_value (b, ch->name);
        json_builder_set_member_name (b, "on");
        json_builder_add_boolean_value (b, ch->enabled);
        json_builder_set_member_name (b, "ready");
        json_builder_add_boolean_value (b, ch->ready);
        json_builder_set_member_name (b, "passes");
        json_builder_begin_array (b);
        for (int p = 0; p < ch->n_passes; p++) {
            EffPass *ep = &ch->passes[p];
            json_builder_begin_object (b);
            for (int k = 0; k < ep->n_consts; k++) {
                FxConst *fc = &ep->consts[k];
                gboolean dup = FALSE;   /* one key can feed several uniforms */
                for (int q = 0; q < k && !dup; q++)
                    dup = !strcmp (ep->consts[q].key, fc->key);
                if (dup || !*fc->key)
                    continue;
                json_builder_set_member_name (b, fc->key);
                json_builder_begin_array (b);
                for (int c = 0; c < fc->n && c < 4; c++)
                    json_builder_add_double_value (b, round ((double) fc->v[c] * 1e4) / 1e4);
                json_builder_end_array (b);
            }
            json_builder_set_member_name (b, "$tex");
            json_builder_begin_array (b);
            for (int u = 1; u < FX_MAX_BINDS; u++)
                json_builder_add_int_value (b, ep->slot[u].res[2] * 10000 + ep->slot[u].res[3]);
            json_builder_end_array (b);
            json_builder_end_object (b);
        }
        json_builder_end_array (b);
        json_builder_end_object (b);
    }
    json_builder_end_array (b);
}
