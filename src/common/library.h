/* library.h - removing one wallpaper from the libraries (GUI "Delete").
 *
 * Nothing here deletes on a caller's say-so: every path is checked against
 * the known library roots first, and again right before it is touched.
 *
 * Known roots (each resolved with realpath):
 *   - the WalPE library (config "wallpapers_dir")
 *   - every Steam library's steamapps/workshop/content/431960
 *   - steamcmd's own .../steamapps/workshop/content/431960, where the
 *     Workshop tab's downloads stay after they were copied into the library
 *
 * What may be removed (never a root, never anything a root lies in):
 *   - <root>/<name>/ with a project.json            a wallpaper folder
 *   - <WalPE library>/[<plain folder>/]<name> that  "Add folder" link: only
 *     is a symlink                                  the link, never its target
 *   - <WalPE library>/[<plain folder>/]<media file> an imported video / GIF /
 *                                                   image, plus the <stem>.jpg
 *                                                   thumbnail of an .mp4
 * The parent directory is resolved with realpath and must BE a root (or a
 * plain folder directly in the WalPE library, for loose media): a ".." in
 * the path, a symlinked parent that leads elsewhere, a mount point, a
 * hidden name, a folder without project.json are all refused.
 *
 * Steam (version 0.12 "unsub", see steamsub.h): only a copy in a Steam CLIENT
 * library comes back after a delete, and only while the account is
 * subscribed to the item. A steamcmd download does not: steamcmd's
 * workshop_download_item fetches an item without subscribing to it.
 *
 * Deleting moves to the trash (g_file_trash: a rename into the trash of the
 * same filesystem, restorable). walpe_library_remove() is the explicit
 * "delete permanently" for when there is no trash: it never follows a
 * symlink and never leaves the filesystem it started on. */
#pragma once
#include <glib.h>
#include <gio/gio.h>
#include "common/config.h"

G_BEGIN_DECLS

typedef enum {
    WALPE_ROOT_LIBRARY,   /* the WalPE library */
    WALPE_ROOT_STEAM,     /* a Steam library's workshop content dir */
    WALPE_ROOT_STEAMCMD,  /* steamcmd's download dir (Workshop tab installs) */
} WalpeRootKind;

typedef enum {
    WALPE_DEL_DIR,    /* wallpaper folder */
    WALPE_DEL_LINK,   /* symlink in the WalPE library: the link only */
    WALPE_DEL_FILE,   /* loose media file */
    WALPE_DEL_THUMB,  /* <stem>.jpg next to an imported <stem>.mp4 */
} WalpeDelKind;

typedef struct {
    char *path;           /* <realpath of the parent>/<name>: what goes */
    WalpeDelKind kind;
    WalpeRootKind root;
    char *link_target;    /* WALPE_DEL_LINK: where it points (left alone) */
    gboolean primary;     /* the entry asked for; FALSE = another copy of the
                           * same workshop item, or a thumbnail */
    gboolean steam_client; /* it lies in a Steam client library's workshop
                            * folder (also when that folder is the WalPE
                            * library itself) */
} WalpeDelTarget;

typedef struct {
    GPtrArray *targets;      /* WalpeDelTarget*; [0] is the entry asked for */
    char *workshop_id;       /* the folder name when it is a workshop id
                              * (digits only, 8 or more) */
    gboolean steam_managed;  /* a target lies in a Steam / steamcmd download
                              * dir */
    gboolean steam_client;   /* a target lies in a Steam client library: the
                              * Steam client downloads it again while the
                              * account is subscribed. Not set for a copy
                              * that only steamcmd downloaded */
    char **forget;           /* every path the daemon / config must forget
                              * (NULL-terminated) */
    char **dependents;       /* titles of the presets in the libraries that are
                              * based on this workshop item (project.json
                              * "dependency"): they stop working without it.
                              * NULL-terminated, may be empty */
} WalpeDelPlan;

/* Check `path` (a library entry as the GUI lists it) and say what deleting
 * it removes: the entry, an imported video's thumbnail, and every other
 * copy of the same workshop item in the known roots (the grid lists an item
 * once; a Steam copy left behind would simply show up again). NULL + err
 * (G_IO_ERROR_NOT_SUPPORTED etc., message for the user) when `path` is not
 * something that may be deleted. library_dir = config wallpapers_dir (NULL =
 * the default). File I/O only: fine on a worker thread. */
WalpeDelPlan *walpe_library_delete_plan (const char *path, const char *library_dir,
                                         GError **err);
void          walpe_del_plan_free (WalpeDelPlan *plan);

/* Move one target to the trash. The target is validated again first. */
gboolean walpe_library_trash (const WalpeDelTarget *t, const char *library_dir,
                              GError **err);
/* Delete one target for good (validated again first). Symlinks are removed,
 * never followed; a folder on another filesystem inside it stops the
 * removal. Only for an explicit "delete permanently". */
gboolean walpe_library_remove (const WalpeDelTarget *t, const char *library_dir,
                               GError **err);

/* TRUE when a and b name the same directory entry (lexically, or through
 * symlinked parent directories). The last component is NOT resolved: a link
 * and its target are different entries. */
gboolean walpe_path_same (const char *a, const char *b);

/* Drop every reference to `path` from the config: `current`, the
 * per-monitor wallpapers (those monitors follow `current` again) and the
 * saved wallpaper options. Returns how many entries changed (0 = nothing
 * to save). Not saved here. */
guint walpe_config_forget_path (WalpeConfig *cfg, const char *path);

/* GUI, daemon not running: load config.json, forget the paths, save.
 * Returns the number of entries changed. */
guint walpe_config_forget_offline (const char *const *paths);

G_END_DECLS
