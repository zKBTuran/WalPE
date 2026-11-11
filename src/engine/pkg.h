/* pkg.h - Wallpaper Engine .pkg archive extraction (scene.pkg) */
#pragma once
#include <glib.h>

/* extract all entries of a WE .pkg into dest_dir (created as needed).
 * Returns FALSE with err on corrupt archives / IO failure. */
gboolean walpe_pkg_extract (const char *pkg_path, const char *dest_dir, GError **err);

/* cached variant: extracts into <user cache>/walpe/pkg/<project basename>/
 * and reuses it while the .pkg mtime is unchanged; project.json is copied
 * over since it lives beside the pkg. Returns cache dir (g_free) or NULL.
 * The returned dir is held: it is not evicted from the (size-limited)
 * cache until walpe_pkg_cache_release is called for it, once per
 * successful call. */
char *walpe_pkg_extract_cached (const char *pkg_path, const char *project_dir, GError **err);
void walpe_pkg_cache_release (const char *cache_dir);
