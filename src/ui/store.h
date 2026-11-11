/* store.h - wallpaper library scanning for the browser UI */
#pragma once
#include <glib-object.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <json-glib/json-glib.h>

#define WALPE_TYPE_ENTRY (walpe_entry_get_type ())
G_DECLARE_FINAL_TYPE (WalpeEntry, walpe_entry, WALPE, ENTRY, GObject)

struct _WalpeEntry {
    GObject parent;
    char *path;        /* absolute: project dir or media file */
    char *title;
    char *type;        /* "scene"/"video"/"gif"/"image"/"web"/"unsupported" */
    char *preview;     /* absolute thumbnail path or NULL */
    gint64 preview_mtime, preview_size;   /* of that file when it was scanned */
    char *description;
    char *id;          /* folder / file name (workshop id for workshop items) */
    char *search;      /* casefolded "title id type" for the library filter */
    gboolean steam;    /* listed from the user's Steam library (read-only) */
    gboolean has_props; /* project.json has general.properties (user options) */
    guint n_options;    /* how many of them the options pane shows */
    /* The parsed project.json is NOT kept per entry (about 40 kB each on a
     * real library: 40 MB for 1000 wallpapers that are only listed):
     * walpe_entry_load_props reads it again for the one entry whose
     * options are shown. NULL until then / after unload. */
    JsonNode *props_root;   /* owner of props (project.json) or NULL */
    JsonObject *props;      /* general.properties schema, borrowed */
    JsonObject *localization; /* general.localization, borrowed (may be NULL) */
};

/* parse the entry's project.json again and set props / localization
 * (FALSE: it has none, or the file is gone); unload drops the tree. A
 * caller that needs the objects longer takes its own reference
 * (json_object_ref) before unloading. */
gboolean walpe_entry_load_props (WalpeEntry *e);
void     walpe_entry_unload_props (WalpeEntry *e);

GList *walpe_store_scan (const char *dir);      /* GList<WalpeEntry*> */
/* the WalPE library plus the user's Steam WE libraries (read-only, listed
 * once per folder name). Pure file I/O + JSON: safe on a worker thread. */
GList *walpe_store_scan_library (const char *dir);
void    walpe_store_entry_free (WalpeEntry *e);
/* write a project.json wrapper around a loose media file inside dest_dir */
char *walpe_store_import_file (const char *src, const char *dest_dir, GError **err);
/* symlink a workshop project folder into dest_dir; returns new link path */
char *walpe_store_import_dir (const char *src, const char *dest_dir, GError **err);
/* "Add folder": src is a project folder (it has a project.json) or a folder
 * of project folders; each is linked into dest_dir. Returns how many were
 * added; *failed = how many were not, *first_err = why the first of those
 * was not (g_free; NULL when none failed) */
guint walpe_store_import_folder (const char *src, const char *dest_dir, guint *failed,
                                 char **first_err);
/* The engine answers "open" before it loads the wallpaper; a load that
 * fails shows only in its status ("load_error" per monitor, "load_restored"
 * when the wallpaper before it is back). state = the "state" object of a
 * status reply. Returns one sentence for the first such monitor (g_free),
 * NULL when every monitor loaded what it was given. */
char *walpe_store_load_error_text (JsonObject *state);
/* default Steam Workshop location for Wallpaper Engine (appid 431960) */
char *walpe_store_steam_dir (void);
