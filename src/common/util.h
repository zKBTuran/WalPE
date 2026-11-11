/* util.h - small shared helpers */
#pragma once
#include <glib.h>
#include "common/api.h"
#include <gio/gio.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

char  *walpe_expand_path (const char *path);            /* ~ expansion, returns absolute */
char  *walpe_read_file   (const char *path, gsize *len); /* g_free() result */
gboolean walpe_file_exists (const char *path);
/* TRUE when path lies below the directory root once "." and ".." are folded,
 * by the text of its name and - when something exists there - also after
 * symbolic links are resolved (a link inside a wallpaper that points out of
 * it is not "inside") */
gboolean walpe_path_is_inside (const char *path, const char *root);
/* a file that a wallpaper's own json / shader names: root/name when that is
 * a regular file below root, else NULL. The name is always relative to root
 * (a leading '/' does not make it absolute) and cannot leave it with "..".
 * g_free() */
char  *walpe_asset_path  (const char *root, const char *name);
char  *walpe_dir_of      (const char *path);            /* g_free() */
/* A number of a wallpaper file as an int. The C cast is undefined when the
 * value does not fit or is not a number ("id": 1e30, "maxcount": 1e999, a
 * size of "nan nan"); what x86 does then - INT_MIN - is what every build
 * does with this, so nothing changes where the cast happened to work. */
static inline int walpe_d2i (double v)
{
    return v > -2147483649.0 && v < 2147483648.0 ? (int) v : G_MININT;
}
char  *walpe_json_escape (const char *s);               /* escape "\ for inline JSON, g_free() */

/* dir is a directory of this user that nobody else can create files in:
 * owned by us, no write permission for group or others. What is found in
 * it by name (the control socket, its lock, a module) is then ours or
 * root's. A symbolic link to such a directory counts. */
gboolean walpe_dir_is_private (const char *dir);
/* data into path for the owner alone (mode 0600 whatever the umask, also
 * when path existed with a wider mode), atomically: written to a new file
 * next to it, flushed to disk, renamed over path. A symbolic link at path
 * is replaced, not followed. len < 0: strlen. */
gboolean walpe_write_private (const char *path, const char *data, gssize len, GError **err);

/* json helpers - all tolerant of missing/mistyped nodes */
WALPE_API const char   *wpj_str   (JsonObject *o, const char *k, const char *dflt);
WALPE_API double        wpj_num   (JsonObject *o, const char *k, double dflt);
WALPE_API gboolean      wpj_bool  (JsonObject *o, const char *k, gboolean dflt);
WALPE_API JsonObject   *wpj_obj   (JsonObject *o, const char *k);
JsonArray    *wpj_arr   (JsonObject *o, const char *k);
guint         wpj_arr_len (JsonArray *a);
JsonObject   *wpj_arr_obj (JsonArray *a, guint i);
gboolean      wpj_num2  (JsonObject *o, const char *k, double *x, double *y); /* [x,y] or scalar */

/* parse a json file; returns root node (caller json_node_unref) or NULL */
WALPE_API JsonNode *wpj_parse_file (const char *path);
/* parse from memory; node returned or NULL */
JsonNode *wpj_parse_data (const char *data, gssize len);

G_END_DECLS
