/* weassets.c - Wallpaper Engine builtin assets locator (see weassets.h) */
#include "weassets.h"
#include "workshop.h"
#include "util.h"
#include <string.h>

static GMutex lock;
static char *configured;        /* config.json we_assets (expanded) */
static char *hint;              /* wallpaper dir being opened */
static const char *resolved;    /* never freed once published */
static const char *source = "none";
static gboolean searched;       /* negative cache: reset by config/hint */

static gboolean looks_like_assets (const char *d)
{
    static const char *marks[] = { "shaders", "effects", "materials", NULL };
    for (int i = 0; marks[i]; i++) {
        char *p = g_build_filename (d, marks[i], NULL);
        gboolean ok = g_file_test (p, G_FILE_TEST_IS_DIR);
        g_free (p);
        if (ok)
            return TRUE;
    }
    return FALSE;
}

char *walpe_we_assets_check (const char *dir)
{
    if (!dir || !*dir)
        return NULL;
    char *d = walpe_expand_path (dir);
    if (!g_file_test (d, G_FILE_TEST_IS_DIR)) {
        g_free (d);
        return NULL;
    }
    if (looks_like_assets (d))
        return d;
    char *sub = g_build_filename (d, "assets", NULL);
    g_free (d);
    if (g_file_test (sub, G_FILE_TEST_IS_DIR) && looks_like_assets (sub))
        return sub;
    g_free (sub);
    return NULL;
}

static char *from_workshop_path (const char *h)
{
    const char *marker = h ? strstr (h, "/steamapps/workshop/") : NULL;
    if (!marker)
        return NULL;
    char *prefix = g_strndup (h, (gsize) (marker - h));
    char *cand = g_build_filename (prefix, "steamapps", "common", "wallpaper_engine", NULL);
    g_free (prefix);
    char *r = walpe_we_assets_check (cand);
    g_free (cand);
    return r;
}

static char *from_steam (void)
{
    char **roots = walpe_steam_library_roots ();
    char *r = NULL;
    for (int i = 0; roots[i] && !r; i++) {
        char *cand = g_build_filename (roots[i], "steamapps", "common", "wallpaper_engine", NULL);
        r = walpe_we_assets_check (cand);
        g_free (cand);
    }
    g_strfreev (roots);
    return r;
}

/* lock held */
static void resolve_locked (void)
{
    if (resolved || searched)
        return;
    searched = TRUE;
    char *r = NULL;
    const char *src = "none";
    const char *env = g_getenv ("WALPE_WE_ASSETS");
    if (env && *env) {
        r = walpe_we_assets_check (env);
        if (r)
            src = "env";
        else
            g_warning ("WALPE_WE_ASSETS=%s is not a Wallpaper Engine assets dir "
                       "(no shaders/, effects/ or materials/ in it or in its assets/)", env);
    }
    if (!r && configured) {
        r = walpe_we_assets_check (configured);
        if (r)
            src = "config";
        else
            g_warning ("config we_assets=%s is not a Wallpaper Engine assets dir", configured);
    }
    if (!r && (r = from_workshop_path (hint)))
        src = "workshop-path";
    if (!r && (r = from_steam ()))
        src = "steam";
    if (r) {
        g_atomic_pointer_set (&resolved, r); /* never freed: callers keep pointers */
        source = src;
        g_message ("WE assets dir (%s): %s", src, r);
    } else {
        g_debug ("WE assets dir: none found (set WALPE_WE_ASSETS or we_assets)");
    }
}

const char *walpe_we_assets_dir (void)
{
    const char *r = g_atomic_pointer_get (&resolved);
    if (r)
        return r;
    g_mutex_lock (&lock);
    resolve_locked ();
    r = resolved;
    g_mutex_unlock (&lock);
    return r;
}

const char *walpe_we_assets_source (void)
{
    walpe_we_assets_dir ();
    g_mutex_lock (&lock);
    const char *s = source;
    g_mutex_unlock (&lock);
    return s;
}

void walpe_we_assets_set_configured (const char *dir)
{
    g_mutex_lock (&lock);
    g_free (configured);
    configured = dir && *dir ? walpe_expand_path (dir) : NULL;
    /* a new setting re-runs the search (an env var still wins) */
    g_atomic_pointer_set (&resolved, NULL);
    source = "none";
    searched = FALSE;
    g_mutex_unlock (&lock);
}

void walpe_we_assets_hint (const char *dir)
{
    if (!dir)
        return;
    g_mutex_lock (&lock);
    if (g_strcmp0 (hint, dir)) {
        g_free (hint);
        hint = g_strdup (dir);
        if (!resolved)
            searched = FALSE; /* only a miss is retried with the new path */
    }
    g_mutex_unlock (&lock);
}
