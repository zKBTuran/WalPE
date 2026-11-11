/* swrender.c - "Render on CPU" without EGL / X11: see swrender.h */
#include "common/swrender.h"
#include "common/util.h"
#include <json-glib/json-glib.h>
#include <string.h>

static int cmp_strptr (gconstpointer a, gconstpointer b)
{
    return strcmp (*(const char *const *) a, *(const char *const *) b);
}

static gboolean contains_ci (const char *hay, const char *needle)
{
    if (!hay || !needle)
        return FALSE;
    char *h = g_ascii_strdown (hay, -1);
    gboolean r = strstr (h, needle) != NULL;
    g_free (h);
    return r;
}

/* ICD.library_path of one vendor file names Mesa's EGL library */
static gboolean vendor_file_is_mesa (const char *path)
{
    JsonNode *root = wpj_parse_file (path);
    if (!root)
        return FALSE;
    gboolean mesa = FALSE;
    if (JSON_NODE_HOLDS_OBJECT (root)) {
        JsonObject *icd = wpj_obj (json_node_get_object (root), "ICD");
        const char *lib = icd ? wpj_str (icd, "library_path", NULL) : NULL;
        if (lib) {
            char *base = g_path_get_basename (lib);
            mesa = contains_ci (base, "egl_mesa");
            g_free (base);
        }
    }
    json_node_unref (root);
    return mesa;
}

char *walpe_sw_find_mesa_vendor (const char *dirs, char **searched)
{
    if (!dirs || !*dirs)
        dirs = g_getenv ("__EGL_VENDOR_LIBRARY_DIRS");
    if (!dirs || !*dirs)
        dirs = WALPE_SW_VENDOR_DIRS;
    if (searched)
        *searched = g_strdup (dirs);
    char **dv = g_strsplit (dirs, ":", -1);
    char *found = NULL;
    for (int i = 0; dv[i] && !found; i++) {
        if (!*dv[i])
            continue;
        GDir *d = g_dir_open (dv[i], 0, NULL);
        if (!d)
            continue;
        /* libglvnd's order: by file name */
        GPtrArray *names = g_ptr_array_new_with_free_func (g_free);
        const char *n;
        while ((n = g_dir_read_name (d)))
            if (g_str_has_suffix (n, ".json"))
                g_ptr_array_add (names, g_strdup (n));
        g_dir_close (d);
        g_ptr_array_sort (names, cmp_strptr);
        for (guint k = 0; k < names->len && !found; k++) {
            char *p = g_build_filename (dv[i], names->pdata[k], NULL);
            if (vendor_file_is_mesa (p))
                found = p;
            else
                g_free (p);
        }
        g_ptr_array_free (names, TRUE);
    }
    g_strfreev (dv);
    return found;
}

int walpe_sw_threads (int configured, int ncpu)
{
    if (configured > 0)
        return MIN (configured, 32);
    if (ncpu < 1)
        ncpu = 1;
    return CLAMP (ncpu / 2, 1, 8);
}

int walpe_sw_env_override (const char *value)
{
    if (!value || !*value)
        return -1;
    static const char *const on[] = { "1", "on", "true", "yes", NULL };
    static const char *const off[] = { "0", "off", "false", "no", NULL };
    for (int i = 0; on[i]; i++)
        if (!g_ascii_strcasecmp (value, on[i]))
            return 1;
    for (int i = 0; off[i]; i++)
        if (!g_ascii_strcasecmp (value, off[i]))
            return 0;
    return -1;
}

char **walpe_sw_env_pairs (const char *vendor_json, int threads, gboolean keep_lp_threads)
{
    GPtrArray *a = g_ptr_array_new ();
#define PAIR(k, v) (g_ptr_array_add (a, g_strdup (k)), g_ptr_array_add (a, (v)))
    /* libglvnd: load Mesa's EGL and nothing else */
    PAIR ("__EGL_VENDOR_LIBRARY_FILENAMES", g_strdup (vendor_json));
    /* Mesa: no hardware driver, the LLVM rasteriser */
    PAIR ("LIBGL_ALWAYS_SOFTWARE", g_strdup ("1"));
    PAIR ("GALLIUM_DRIVER", g_strdup ("llvmpipe"));
    /* GLX users among the child processes (WebKit) follow */
    PAIR ("__GLX_VENDOR_LIBRARY_NAME", g_strdup ("mesa"));
    if (!keep_lp_threads)
        PAIR ("LP_NUM_THREADS", g_strdup_printf ("%d", threads));
#undef PAIR
    g_ptr_array_add (a, NULL);
    return (char **) g_ptr_array_free (a, FALSE);
}

char **walpe_sw_environ (char **env, char **pairs)
{
    for (int i = 0; pairs && pairs[i] && pairs[i + 1]; i += 2)
        env = g_environ_setenv (env, pairs[i], pairs[i + 1], TRUE);
    return env;
}

gboolean walpe_sw_renderer_is_llvmpipe (const char *renderer)
{
    return contains_ci (renderer, "llvmpipe");
}

gboolean walpe_sw_renderer_is_software (const char *renderer)
{
    return contains_ci (renderer, "llvmpipe") || contains_ci (renderer, "softpipe") ||
           contains_ci (renderer, "swrast") || contains_ci (renderer, "software rasterizer");
}

gboolean walpe_sw_parse_probe (const char *out, char **vendor, char **renderer,
                               char **version, char **why)
{
    char *v = NULL, *r = NULL, *ver = NULL, *fail = NULL;
    gboolean drew = FALSE;
    char **lines = g_strsplit (out ? out : "", "\n", -1);
    for (int i = 0; lines[i]; i++) {
        char *eq = strchr (lines[i], '=');
        if (!eq)
            continue;
        *eq = 0;
        const char *k = lines[i], *val = g_strstrip (eq + 1);
        if (!strcmp (k, "vendor") && !v)
            v = g_strdup (val);
        else if (!strcmp (k, "renderer") && !r)
            r = g_strdup (val);
        else if (!strcmp (k, "version") && !ver)
            ver = g_strdup (val);
        else if (!strcmp (k, "fail") && !fail)
            fail = g_strdup (val);
        else if (!strcmp (k, "drew"))
            drew = !strcmp (val, "1");
    }
    g_strfreev (lines);
    gboolean ok = FALSE;
    char *w = NULL;
    if (fail)
        w = g_strdup (fail);
    else if (!r)
        w = g_strdup ("the test of Mesa's EGL ended without a result");
    else if (!walpe_sw_renderer_is_llvmpipe (r))
        w = g_strdup_printf ("Mesa's EGL gives \"%s\", not llvmpipe%s", r,
                             walpe_sw_renderer_is_software (r)
                                 ? " (Mesa built without LLVM: far too slow)" : "");
    else if (!drew)
        w = g_strdup ("llvmpipe could not draw to a window of this X server");
    else
        ok = TRUE;
    g_free (fail);
    if (vendor) *vendor = v; else g_free (v);
    if (renderer) *renderer = r; else g_free (r);
    if (version) *version = ver; else g_free (ver);
    if (why) *why = w; else g_free (w);
    return ok;
}
