/* weassets.h - locate the Wallpaper Engine install's builtin assets/ dir
 * (shaders/common*.h, effects/<name>/, materials/util, particle textures,
 * models/util/ json). WalPE ships none of these files; scenes that use
 * builtin effects need the user's own WE install.
 *
 * Resolution order (first hit wins; every candidate must look like an
 * assets dir - it has shaders/ or effects/ or materials/ - or be the
 * install root containing one as assets/):
 *   1. $WALPE_WE_ASSETS
 *   2. config.json "we_assets" (walpe-ctl set we_assets=<dir>)
 *   3. the opened wallpaper's path: .../steamapps/workshop/content/431960/<id>
 *      -> .../steamapps/common/wallpaper_engine/assets
 *   4. every Steam library: ~/.local/share/Steam, ~/.steam/steam,
 *      ~/.steam/root, Flatpak Steam, and the "path" entries of their
 *      libraryfolders.vdf -> <lib>/steamapps/common/wallpaper_engine/assets
 * Thread-safe; returned strings are never freed (stable for the process). */
#pragma once
#include <glib.h>

G_BEGIN_DECLS

/* resolved assets dir, or NULL when no install was found */
const char *walpe_we_assets_dir (void);
/* how it was found: "env" | "config" | "workshop-path" | "steam" | "none" */
const char *walpe_we_assets_source (void);
/* the configured dir (config.json "we_assets"; NULL/"" = auto-detect).
 * Re-runs the search on the next walpe_we_assets_dir(). */
void walpe_we_assets_set_configured (const char *dir);
/* the wallpaper directory being opened (rule 3) */
void walpe_we_assets_hint (const char *dir);
/* normalise a user-given dir: the assets dir itself, or an install root
 * holding assets/. g_free; NULL when it is neither */
char *walpe_we_assets_check (const char *dir);

G_END_DECLS
