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

/* The same, for a wallpaper whose sound is switched off (with_sound FALSE):
 * the copy is made without the package's sound files (walpe_pkg_sound_name)
 * - nothing reads them then. A copy that lacks them and is asked for with
 * sound gets them at that moment. */
char *walpe_pkg_extract_cached_opts (const char *pkg_path, const char *project_dir, gboolean with_sound,
                                     GError **err);
/* the name of an entry the copy of a silent wallpaper is made without:
 * *.mp3 *.ogg *.oga *.opus *.wav *.flac (letter case ignored) - if it also
 * begins like a sound file; an entry of that name with other content is
 * extracted with the rest */
gboolean walpe_pkg_sound_name (const char *entry_path);
/* The sound files a cached copy was made without, extracted now, with the
 * checks of every extraction (any thread; at once when there are none).
 * FALSE: they could not be made (the package is gone or has changed, no
 * room on the disk): the copy is as it was. */
gboolean walpe_pkg_fetch_sound (const char *cache_dir);
/* `name` as a scene writes it (looked up as <name> and assets/<name>, like
 * a file on disk): where that file of the package will be once
 * walpe_pkg_fetch_sound () has run, or NULL when the copy has no such file
 * outstanding (g_free). */
char *walpe_pkg_deferred_path (const char *cache_dir, const char *name);
