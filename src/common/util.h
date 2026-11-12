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
/* The whole of a regular file of at most max bytes, read into memory.
 * For files somebody else may change while they are used (a path a D-Bus
 * peer names): a mapping of a file that is cut short ends the process with
 * SIGBUS the moment the missing part is read; what was read stays what it
 * was. NULL + err when path is no regular file (a FIFO is not waited for),
 * is larger than max (also when it grows beyond it while it is read) or
 * cannot be read. */
GBytes *walpe_read_regular (const char *path, gsize max, GError **err);

/* What libjpeg has to hold of the JPEG in d[0..len) before the first pixel
 * comes out, in bytes. A sequential file whose first scan carries every
 * component is decoded as it is read (a few rows; a reader that asks for a
 * smaller picture never has the full size in memory): 0. Any other file -
 * progressive, or its components in scans of their own - is first read
 * whole into a store of 128 bytes per 8 x 8 block of every component,
 * whatever size was asked for: 600 bytes of file can declare 16384 x 16384
 * and fill half a gigabyte of it with end-of-band runs. G_MAXUINT64: the
 * bytes do not reach a first scan the way libjpeg reads them (cut short,
 * a frame type or segment it refuses). */
guint64 walpe_jpeg_store_bytes (const guint8 *d, gsize len);
/* the same for a file (its head is read, at most 32 MiB of it) */
guint64 walpe_jpeg_store_bytes_file (const char *path);
/* the largest store a preview picture may ask for: what the largest
 * picture of another format costs decoded (8192 x 8192, 4 bytes a pixel) */
#define WALPE_JPEG_STORE_MAX ((guint64) 256 << 20)

/* Pictures that are not a wallpaper's own textures (previews).
 * gdk-pixbuf asked to choose (gdk_pixbuf_loader_new, gdk_pixbuf_new_from_file,
 * gdk_pixbuf_get_file_info) hands the bytes to whichever of its loaders
 * matches them - SVG (librsvg), XPM, PNM, TGA, ICO, ANI, TIFF, whatever the
 * system has - and that loader has parsed them by the time its name can be
 * asked for. Here the format is read from the signature and the bytes go to
 * the loader of that one name, and only when the caller accepts it.
 *
 * walpe_picture_kind: "jpeg", "png", "gif", "webp", "bmp", "avif" or NULL.
 * walpe_picture_decode / _file: the picture (first frame), or NULL + err
 * (G_IO_ERROR_NOT_SUPPORTED: not one of `kinds`, a NULL-terminated list of
 * the names above, or no loader of that name here). `size` (may be NULL)
 * is called once with the declared size before any pixel exists: FALSE
 * refuses the picture, *sw x *sh (when set) is the size it is decoded to.
 * A JPEG that its decoder would hold whole beyond WALPE_JPEG_STORE_MAX is
 * refused as well. */
typedef struct _GdkPixbuf GdkPixbuf;
typedef gboolean (*WalpePictureSizeFunc) (const char *kind, int w, int h, int *sw, int *sh,
                                          gpointer user);
const char *walpe_picture_kind (const guint8 *d, gsize len);
/* A "BM" file before it reaches gdk-pixbuf's BMP decoder. The decoder
 * reserves buffers of sizes it reads from the file's first bytes, each up
 * to 4 GiB, before it has checked anything else: one of the size of the info
 * header (4 bytes at offset 14; also when only the picture's size is asked
 * for), one that reaches up to the pixel data (the offset at 10; pictures
 * without a palette), one for the palette (the count at 46). Here: the info
 * header has one of the sizes the format knows (12, 40, 52, 56, 64, 108,
 * 124; the decoder refuses any other afterwards), the pixel data starts
 * inside the file, and a palette has no more entries than the bits of a
 * pixel can name.
 * d[0..len) = the file's first bytes (54 or more of a file that has them),
 * size = the whole file's length. TRUE for what does not start with "BM". */
gboolean walpe_bmp_header_ok (const guint8 *d, gsize len, guint64 size);
/* the same for a file (TRUE when it cannot be read: whoever opens it next
 * says why) */
gboolean walpe_bmp_file_header_ok (const char *path);
GdkPixbuf *walpe_picture_decode (const guint8 *d, gsize len, const char *const *kinds,
                                 WalpePictureSizeFunc size, gpointer user, GError **err);
GdkPixbuf *walpe_picture_decode_file (const char *path, const char *const *kinds,
                                      WalpePictureSizeFunc size, gpointer user, GError **err);

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
/* the first top-level value of a text (of any type; nothing behind it is
 * read into memory), a new reference, or NULL. text must be writable and is
 * undefined afterwards. No depth check: the caller's. */
JsonNode *wpj_parse_first (char *text, gsize len, GError **err);
/* no array / object of the text is nested deeper than max_depth, under any
 * reading json-glib's scanner may have of quotes and comments (see util.c).
 * To be asked before json-glib parses a text that did not come through
 * wpj_parse_file / wpj_parse_data: its parser recurses once per level. */
gboolean wpj_depth_ok (const char *data, gsize len, int max_depth);

G_END_DECLS
