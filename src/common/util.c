#include "util.h"
#include "gifcheck.h"
#include <glib/gstdio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

static char *expand_path_raw (const char *path)
{
    if (!path || !*path)
        return NULL;
    if (path[0] == '~') {
        const char *rest = path + 1;
        if (*rest == '/' || *rest == '\0') {
            const char *home = g_get_home_dir ();
            return g_build_filename (home, rest[0] ? rest + 1 : "", NULL);
        }
        /* ~user */
        char *user = g_strndup (rest, strcspn (rest, "/"));
        /* (getpwnam_r: paths are expanded on the main thread and, for a
         * wallpaper's options, on render threads; getpwnam answers from
         * one static buffer) */
        /* (the buffer grows while the entry does not fit: getpwnam had no
         * limit, and an entry of a directory service can be longer than
         * any fixed size; ERANGE read as "no such user" left the path
         * unexpanded without a word) */
        struct passwd pwbuf, *pw = NULL;
        gsize blen = 4096;
        char *buf = g_malloc (blen);
        int rc;
        while ((rc = getpwnam_r (user, &pwbuf, buf, blen, &pw)) == ERANGE && blen < 1024 * 1024) {
            blen *= 2;
            buf = g_realloc (buf, blen);
        }
        if (rc != 0)
            pw = NULL;
        g_free (user);
        char *out = pw ? g_build_filename (pw->pw_dir, strchr (rest, '/') ? strchr (rest, '/') + 1 : "", NULL)
                       : g_strdup (path);
        g_free (buf);   /* (pw points into it) */
        return out;
    }
    if (path[0] != '/') {
        char *cwd = g_get_current_dir ();
        char *abs = g_build_filename (cwd, path, NULL);
        g_free (cwd);
        return abs;
    }
    return g_strdup (path);
}

/* One spelling per file. A wallpaper's path is a key: for its options in
 * config.json, for "which monitor shows it", for the scene's own lookups.
 * `walpe-ctl open dir/` (tab completion), "dir//x" or "a/./b" named the
 * same wallpaper as the GUI's "dir" but matched none of them: options set
 * in the GUI were saved and never applied. Redundant separators, "." and
 * ".." components and the trailing separator go; symbolic links are not
 * resolved (the path stays the one the user knows). */
char *walpe_expand_path (const char *path)
{
    char *abs = expand_path_raw (path);
    if (!abs || abs[0] != '/')
        return abs;
#if GLIB_CHECK_VERSION (2, 58, 0)
    char *canon = g_canonicalize_filename (abs, NULL);
    g_free (abs);
    return canon;
#else
    for (gsize n = strlen (abs); n > 1 && abs[n - 1] == '/'; n--)
        abs[n - 1] = 0;
    return abs;
#endif
}

char *walpe_read_file (const char *path, gsize *len)
{
    char *data = NULL;
    /* regular files only: a device or FIFO named by a wallpaper
     * ("/dev/zero" as a texture) would be read until memory runs out */
    if (!path || !g_file_test (path, G_FILE_TEST_IS_REGULAR))
        return NULL;
    if (!g_file_get_contents (path, &data, len, NULL))
        return NULL;
    return data;
}

gboolean walpe_file_exists (const char *path)
{
    return path && g_file_test (path, G_FILE_TEST_EXISTS);
}

/* path, which the text of its name puts below root, is still below root
 * once symbolic links are resolved. Nothing there (yet): the text decides. */
static gboolean real_path_inside (const char *path, const char *root)
{
    char *rp = realpath (path, NULL);
    if (!rp)
        return errno == ENOENT || errno == ENOTDIR;
    char *rr = realpath (root, NULL);
    gboolean in = FALSE;
    if (rr) {
        gsize n = strlen (rr);
        while (n > 1 && rr[n - 1] == G_DIR_SEPARATOR)
            n--;
        in = n == 1 ? rp[1] != '\0'
                    : strncmp (rp, rr, n) == 0 && rp[n] == G_DIR_SEPARATOR && rp[n + 1] != '\0';
    }
    free (rp);
    free (rr);
    return in;
}

gboolean walpe_path_is_inside (const char *path, const char *root)
{
    if (!path || !*path || !root || !*root)
        return FALSE;
    /* by its text first: "." and ".." are folded */
    char *cp = g_canonicalize_filename (path, NULL);
    char *cr = g_canonicalize_filename (root, NULL);
    gsize n = strlen (cr);
    while (n > 1 && cr[n - 1] == G_DIR_SEPARATOR)
        n--;
    gboolean in = n == 1 ? cp[0] == G_DIR_SEPARATOR && cp[1] != '\0'
                         : strncmp (cp, cr, n) == 0 && cp[n] == G_DIR_SEPARATOR && cp[n + 1] != '\0';
    /* ... then as the file system resolves it. WalPE's own Workshop install
     * and the .pkg extraction write no symbolic links, but a folder Steam
     * downloaded, or one the user unpacked from an archive, can hold one:
     * "materials/a.tex" -> a file anywhere the user can read was opened as
     * the wallpaper's own texture, json or font. A link that stays inside
     * the wallpaper (or a wallpaper folder that is itself reached through
     * links) is fine.
     * The name as given is resolved, not the folded one: the kernel takes
     * "linkdir/../x" through the link's target, the folding does not. */
    if (in)
        in = real_path_inside (path, cr);
    g_free (cp);
    g_free (cr);
    return in;
}

char *walpe_asset_path (const char *root, const char *name)
{
    if (!root || !*root || !name || !*name)
        return NULL;
    char *p = g_build_filename (root, name, NULL);
    if (walpe_path_is_inside (p, root) && g_file_test (p, G_FILE_TEST_IS_REGULAR))
        return p;
    g_free (p);
    return NULL;
}

gboolean walpe_dir_is_private (const char *dir)
{
    GStatBuf st;
    return dir && *dir && g_stat (dir, &st) == 0 && S_ISDIR (st.st_mode) &&
           st.st_uid == getuid () && (st.st_mode & (S_IWGRP | S_IWOTH)) == 0;
}

gboolean walpe_write_private (const char *path, const char *data, gssize len, GError **err)
{
    if (!path || !data) {
        g_set_error_literal (err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "nothing to write");
        return FALSE;
    }
    gsize n = len < 0 ? strlen (data) : (gsize) len, off = 0;
    /* a name of its own (O_EXCL, never an existing file or a link), 0600
     * from the first moment */
    char *tmp = g_strconcat (path, ".XXXXXX", NULL);
    int fd = g_mkstemp_full (tmp, O_WRONLY | O_CLOEXEC, 0600);
    gboolean ok = fd >= 0;
    if (ok) {
        fchmod (fd, 0600);   /* a umask may have taken the owner's bits too */
        while (ok && off < n) {
            ssize_t w = write (fd, data + off, n - off);
            if (w < 0 && errno == EINTR)
                continue;
            ok = w > 0;
            off += w > 0 ? (gsize) w : 0;
        }
        ok = ok && fsync (fd) == 0;
        ok = (close (fd) == 0) && ok;
        ok = ok && g_rename (tmp, path) == 0;
    }
    if (!ok) {
        int e = errno;
        if (fd >= 0)
            g_unlink (tmp);
        g_set_error (err, G_IO_ERROR, g_io_error_from_errno (e), "cannot write %s: %s", path,
                     g_strerror (e));
    }
    g_free (tmp);
    return ok;
}

char *walpe_dir_of (const char *path)
{
    char *abs = walpe_expand_path (path);
    char *dir = g_path_get_dirname (abs);
    g_free (abs);
    return dir;
}

/* ------------------------------------------------------------------ jpeg */

/* The header is walked the way libjpeg's own reader walks it (jdmarker.c):
 * anything up to an FF and any number of FF before a marker, segments by
 * their length, up to the first start-of-scan. Whether the decoder then
 * keeps the whole picture is libjpeg's rule (jdinput.c: a progressive
 * frame, or fewer components in the first scan than in the frame), the
 * size of the store its coefficient controller's (jdcoefct.c). */
guint64 walpe_jpeg_store_bytes (const guint8 *d, gsize len)
{
    if (!d || len < 4 || d[0] != 0xFF || d[1] != 0xD8)
        return G_MAXUINT64;
    gsize i = 2;
    guint w = 0, h = 0, ncomp = 0, hs[10], vs[10];
    gboolean progressive = FALSE, have_frame = FALSE;
    for (;;) {
        while (i < len && d[i] != 0xFF)
            i++;
        while (i < len && d[i] == 0xFF)
            i++;
        if (i >= len)
            return G_MAXUINT64;
        guint8 m = d[i++];
        if (m == 0 || m == 0x01 || (m >= 0xD0 && m <= 0xD7))
            continue;           /* a stuffed FF, TEM, RSTn: no segment follows */
        if (i + 2 > len)
            return G_MAXUINT64;
        gsize seg = ((gsize) d[i] << 8) | d[i + 1];
        if (seg < 2 || seg > len - i)
            return G_MAXUINT64;
        const guint8 *p = d + i + 2;
        gsize pl = seg - 2;
        if (m == 0xC0 || m == 0xC1 || m == 0xC2 || m == 0xC9 || m == 0xCA) {
            /* frame header: precision, height, width, components (id, h/v, table) */
            if (have_frame || pl < 6)
                return G_MAXUINT64;
            h = ((guint) p[1] << 8) | p[2];
            w = ((guint) p[3] << 8) | p[4];
            ncomp = p[5];
            if (!w || !h || !ncomp || ncomp > G_N_ELEMENTS (hs) || pl != 6 + 3 * (gsize) ncomp)
                return G_MAXUINT64;
            for (guint c = 0; c < ncomp; c++) {
                hs[c] = p[7 + 3 * c] >> 4;
                vs[c] = p[7 + 3 * c] & 15;
                if (hs[c] < 1 || hs[c] > 4 || vs[c] < 1 || vs[c] > 4)
                    return G_MAXUINT64;
            }
            progressive = m == 0xC2 || m == 0xCA;
            have_frame = TRUE;
        } else if (m == 0xDA) {
            if (!have_frame || pl < 1 || p[0] < 1 || p[0] > ncomp)
                return G_MAXUINT64;
            if (!progressive && p[0] == ncomp)
                return 0;
            break;
        } else if (m != 0xC4 && m != 0xCC && m != 0xDB && m != 0xDC && m != 0xDD && m != 0xFE &&
                   !(m >= 0xE0 && m <= 0xEF)) {
            return G_MAXUINT64; /* not DHT, DAC, DQT, DNL, DRI, COM or APPn */
        }
        i += seg;
    }
    guint hmax = 1, vmax = 1;
    for (guint c = 0; c < ncomp; c++) {
        hmax = MAX (hmax, hs[c]);
        vmax = MAX (vmax, vs[c]);
    }
    guint64 total = 0;
    for (guint c = 0; c < ncomp; c++) {
        guint64 wb = ((guint64) w * hs[c] + 8 * hmax - 1) / (8 * hmax);
        guint64 hb = ((guint64) h * vs[c] + 8 * vmax - 1) / (8 * vmax);
        wb = (wb + hs[c] - 1) / hs[c] * hs[c];
        hb = (hb + vs[c] - 1) / vs[c] * vs[c];
        total += wb * hb * 128;
    }
    return total;
}

guint64 walpe_jpeg_store_bytes_file (const char *path)
{
    /* the first scan comes after the tables and the application segments
     * (colour profile, Exif, XMP): a few KB as a rule, megabytes at times.
     * Read in growing steps until the walk has an answer. */
    const gsize head_max = (gsize) 32 << 20;
    FILE *f = path ? fopen (path, "rb") : NULL;
    if (!f)
        return G_MAXUINT64;
    GByteArray *buf = g_byte_array_new ();
    guint64 r = G_MAXUINT64;
    gsize want = 64 * 1024;
    for (;;) {
        gsize old = buf->len;
        g_byte_array_set_size (buf, (guint) want);
        gsize got = fread (buf->data + old, 1, want - old, f);
        g_byte_array_set_size (buf, (guint) (old + got));
        r = walpe_jpeg_store_bytes (buf->data, buf->len);
        if (r != G_MAXUINT64 || got < want - old || want >= head_max)
            break;
        want *= 2;
    }
    g_byte_array_unref (buf);
    fclose (f);
    return r;
}

/* -------------------------------------------------------------- pictures */

const char *walpe_picture_kind (const guint8 *d, gsize len)
{
    if (!d)
        return NULL;
    if (len >= 3 && !memcmp (d, "\xff\xd8\xff", 3))
        return "jpeg";
    if (len >= 8 && !memcmp (d, "\x89PNG\r\n\x1a\n", 8))
        return "png";
    if (len >= 6 && (!memcmp (d, "GIF87a", 6) || !memcmp (d, "GIF89a", 6)))
        return "gif";
    if (len >= 12 && !memcmp (d, "RIFF", 4) && !memcmp (d + 8, "WEBP", 4))
        return "webp";
    if (len >= 16 && !memcmp (d + 4, "ftyp", 4)) {
        /* the major brand, or one of the compatible brands after it */
        gsize box = ((gsize) d[0] << 24) | ((gsize) d[1] << 16) | ((gsize) d[2] << 8) | d[3];
        box = MIN (MIN (box, len), 128);
        for (gsize o = 8; o + 4 <= box; o += o == 8 ? 8 : 4)
            if (!memcmp (d + o, "avif", 4) || !memcmp (d + o, "avis", 4))
                return "avif";
        return NULL;
    }
    if (len >= 2 && d[0] == 'B' && d[1] == 'M')
        return "bmp";
    return NULL;
}

static guint32 bmp_le32 (const guint8 *p)
{
    return (guint32) p[0] | ((guint32) p[1] << 8) | ((guint32) p[2] << 16) | ((guint32) p[3] << 24);
}

gboolean walpe_bmp_header_ok (const guint8 *d, gsize len, guint64 size)
{
    if (!d || len < 2 || d[0] != 'B' || d[1] != 'M')
        return TRUE;
    if (len < 18)
        return FALSE;
    guint32 off = bmp_le32 (d + 10), hs = bmp_le32 (d + 14);
    if ((hs != 12 && hs != 40 && hs != 52 && hs != 56 && hs != 64 && hs != 108 && hs != 124) ||
        off > size)
        return FALSE;
    if (hs >= 40) {
        if (len < 54)
            return FALSE;       /* shorter than its own header */
        guint bpp = (guint) d[28] | ((guint) d[29] << 8);
        if (bpp <= 8 && bmp_le32 (d + 46) > (1u << bpp))
            return FALSE;
    }
    return TRUE;
}

gboolean walpe_bmp_file_header_ok (const char *path)
{
    guint8 h[54];
    GStatBuf st;
    FILE *f = path && g_stat (path, &st) == 0 && S_ISREG (st.st_mode) ? fopen (path, "rb") : NULL;
    if (!f)
        return TRUE;
    gsize n = fread (h, 1, sizeof h, f);
    fclose (f);
    return walpe_bmp_header_ok (h, n, (guint64) st.st_size);
}

typedef struct {
    const char *kind;
    WalpePictureSizeFunc size;
    gpointer user;
    gboolean refused;
} PictureLoad;

static void picture_size_prepared (GdkPixbufLoader *ld, int w, int h, gpointer user)
{
    PictureLoad *pl = user;
    int sw = 0, sh = 0;
    if (pl->size && !pl->size (pl->kind, w, h, &sw, &sh, pl->user)) {
        pl->refused = TRUE;
        gdk_pixbuf_loader_set_size (ld, 0, 0);     /* the loaders stop before a pixel exists */
        return;
    }
    if (sw > 0 && sh > 0)
        gdk_pixbuf_loader_set_size (ld, sw, sh);
}

/* the loader for the format the first bytes name, when it is one of kinds */
static GdkPixbufLoader *picture_loader (const guint8 *head, gsize n, guint64 size,
                                        const char *const *kinds, PictureLoad *pl, GError **err)
{
    const char *kind = walpe_picture_kind (head, n);
    gboolean accepted = FALSE;
    for (int i = 0; kind && kinds && kinds[i]; i++)
        accepted |= !strcmp (kind, kinds[i]);
    if (!accepted) {
        g_set_error_literal (err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                             "not a picture of an accepted format");
        return NULL;
    }
    /* head = the file's first bytes, size = its whole length */
    if (!strcmp (kind, "bmp") && !walpe_bmp_header_ok (head, n, size)) {
        g_set_error_literal (err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                             "a BMP whose header sizes are not those of a BMP file");
        return NULL;
    }
    GdkPixbufLoader *ld = gdk_pixbuf_loader_new_with_type (kind, NULL);
    if (!ld && !strcmp (kind, "avif"))
        ld = gdk_pixbuf_loader_new_with_type ("heif/avif", NULL);   /* libheif's name for it */
    if (!ld) {
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                     "this gdk-pixbuf has no loader for %s pictures", kind);
        return NULL;
    }
    pl->kind = kind;
    g_signal_connect (ld, "size-prepared", G_CALLBACK (picture_size_prepared), pl);
    return ld;
}

static GdkPixbuf *picture_finish (GdkPixbufLoader *ld, PictureLoad *pl, gboolean ok, GError *lerr,
                                  GError **err)
{
    if (ok && !pl->refused)
        ok = gdk_pixbuf_loader_close (ld, lerr ? NULL : &lerr);
    else
        gdk_pixbuf_loader_close (ld, NULL);
    GdkPixbuf *pb = ok && !pl->refused ? gdk_pixbuf_loader_get_pixbuf (ld) : NULL;
    if (pb)
        g_object_ref (pb);
    g_object_unref (ld);
    if (!pb && pl->refused)
        g_set_error_literal (err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                             "the picture's size is not accepted");
    else if (!pb)
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "the picture cannot be decoded (%s)",
                     lerr ? lerr->message : "no picture in it");
    g_clear_error (&lerr);
    return pb;
}

static gboolean picture_jpeg_ok (const PictureLoad *pl, guint64 store, GError **err)
{
    if (strcmp (pl->kind, "jpeg") || store <= WALPE_JPEG_STORE_MAX)
        return TRUE;
    g_set_error_literal (err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                         "a JPEG that has to be held whole to be decoded, and too large for that");
    return FALSE;
}

GdkPixbuf *walpe_picture_decode (const guint8 *d, gsize len, const char *const *kinds,
                                 WalpePictureSizeFunc size, gpointer user, GError **err)
{
    PictureLoad pl = { NULL, size, user, FALSE };
    GdkPixbufLoader *ld = picture_loader (d, len, len, kinds, &pl, err);
    if (!ld)
        return NULL;
    if (!picture_jpeg_ok (&pl, !strcmp (pl.kind, "jpeg") ? walpe_jpeg_store_bytes (d, len) : 0, err)) {
        gdk_pixbuf_loader_close (ld, NULL);
        g_object_unref (ld);
        return NULL;
    }
    /* a GIF whose frames are not all inside its picture, or have no pixels,
     * is not given to the decoder (gifcheck.h) */
    if (!strcmp (pl.kind, "gif") && !walpe_gif_layout_ok (d, len, err)) {
        gdk_pixbuf_loader_close (ld, NULL);
        g_object_unref (ld);
        return NULL;
    }
    gboolean ok = TRUE;
    GError *lerr = NULL;
    for (gsize off = 0; ok && !pl.refused && off < len; off += 65536)
        ok = gdk_pixbuf_loader_write (ld, d + off, MIN ((gsize) 65536, len - off), &lerr);
    return picture_finish (ld, &pl, ok, lerr, err);
}

GdkPixbuf *walpe_picture_decode_file (const char *path, const char *const *kinds,
                                      WalpePictureSizeFunc size, gpointer user, GError **err)
{
    /* regular files only (a FIFO under a picture's name would park the
     * caller), read in pieces: the file is as large as its author likes */
    GStatBuf st;
    FILE *f = path && g_stat (path, &st) == 0 && S_ISREG (st.st_mode) ? fopen (path, "rb") : NULL;
    if (!f) {
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "%s: not a file that can be read",
                     path ? path : "");
        return NULL;
    }
    guint8 *buf = g_malloc (65536);
    gsize n = fread (buf, 1, 65536, f);
    PictureLoad pl = { NULL, size, user, FALSE };
    GdkPixbufLoader *ld = picture_loader (buf, n, (guint64) st.st_size, kinds, &pl, err);
    if (ld && !picture_jpeg_ok (&pl, !strcmp (pl.kind, "jpeg") ? walpe_jpeg_store_bytes_file (path) : 0,
                                err)) {
        gdk_pixbuf_loader_close (ld, NULL);
        g_clear_object (&ld);
    }
    if (ld && !strcmp (pl.kind, "gif") && !walpe_gif_file_layout_ok (path, err)) {   /* as above */
        gdk_pixbuf_loader_close (ld, NULL);
        g_clear_object (&ld);
    }
    GdkPixbuf *pb = NULL;
    if (ld) {
        gboolean ok = TRUE;
        GError *lerr = NULL;
        while (ok && !pl.refused && n > 0) {
            ok = gdk_pixbuf_loader_write (ld, buf, n, &lerr);
            n = ok && !pl.refused ? fread (buf, 1, 65536, f) : 0;
        }
        pb = picture_finish (ld, &pl, ok, lerr, err);
    }
    g_free (buf);
    fclose (f);
    return pb;
}

/* ------------------------------------------------------------------ json */

const char *wpj_str (JsonObject *o, const char *k, const char *dflt)
{
    if (!o) return dflt;
    JsonNode *n = json_object_get_member (o, k);
    if (!n || json_node_is_null (n) || !JSON_NODE_HOLDS_VALUE (n))
        return dflt;
    const char *s = json_node_get_string (n);
    return s ? s : dflt;
}

static JsonNode *numeric_node (JsonObject *o, const char *k)
{
    if (!o) return NULL;
    JsonNode *n = json_object_get_member (o, k);
    if (!n || json_node_is_null (n))
        return NULL;
    GType t = json_node_get_value_type (n);
    if (t == G_TYPE_DOUBLE || t == G_TYPE_INT64 || t == G_TYPE_INT)
        return n;
    return NULL;
}

double wpj_num (JsonObject *o, const char *k, double dflt)
{
    JsonNode *n = numeric_node (o, k);
    return n ? json_node_get_double (n) : dflt;
}

gboolean wpj_bool (JsonObject *o, const char *k, gboolean dflt)
{
    if (!o) return dflt;
    JsonNode *n = json_object_get_member (o, k);
    if (!n || json_node_is_null (n) || !JSON_NODE_HOLDS_VALUE (n))
        return dflt;
    GType t = json_node_get_value_type (n);
    if (t == G_TYPE_BOOLEAN) return json_node_get_boolean (n);
    if (t == G_TYPE_DOUBLE || t == G_TYPE_INT64) return json_node_get_double (n) != 0.0;
    return dflt;
}

JsonObject *wpj_obj (JsonObject *o, const char *k)
{
    if (!o) return NULL;
    JsonNode *n = json_object_get_member (o, k);
    if (!n || json_node_is_null (n) || !JSON_NODE_HOLDS_OBJECT (n))
        return NULL;
    return json_node_get_object (n);
}

JsonArray *wpj_arr (JsonObject *o, const char *k)
{
    if (!o) return NULL;
    JsonNode *n = json_object_get_member (o, k);
    if (!n || json_node_is_null (n) || !JSON_NODE_HOLDS_ARRAY (n))
        return NULL;
    return json_node_get_array (n);
}

guint wpj_arr_len (JsonArray *a)
{
    return a ? json_array_get_length (a) : 0;
}

JsonObject *wpj_arr_obj (JsonArray *a, guint i)
{
    if (!a || i >= json_array_get_length (a))
        return NULL;
    JsonNode *n = json_array_get_element (a, i);
    if (!n || json_node_is_null (n) || !JSON_NODE_HOLDS_OBJECT (n))
        return NULL;
    return json_node_get_object (n);
}

gboolean wpj_num2 (JsonObject *o, const char *k, double *x, double *y)
{
    JsonArray *a = wpj_arr (o, k);
    if (a && json_array_get_length (a) >= 2) {
        JsonNode *e0 = json_array_get_element (a, 0);
        JsonNode *e1 = json_array_get_element (a, 1);
        if (JSON_NODE_HOLDS_VALUE (e0) && JSON_NODE_HOLDS_VALUE (e1)) {
            *x = json_node_get_double (e0);
            *y = json_node_get_double (e1);
            return TRUE;
        }
    }
    JsonNode *n = numeric_node (o, k);
    if (n) {
        *x = *y = json_node_get_double (n);
        return TRUE;
    }
    return FALSE;
}

/* json-glib's parser recurses once per nested array / object and has no
 * depth limit before 1.10: one IPC line of 150000 '[' - or a scene.json
 * built that way - overflowed the stack and killed the process. Real files
 * nest a few dozen levels at most. Brackets inside strings do not count.
 *
 * The count has to read the text as the parser's scanner does, and
 * json-glib up to 1.8 reads more than JSON: 'single-quoted' strings
 * (without escapes) and a comment that runs from one '/' to the next. A
 * '"' inside one of those opened a string for this count only, so every
 * bracket behind it went uncounted: 150000 '[' after the five bytes of a
 * comment holding one quote crashed exactly as before. The plain reading
 * (double-quoted strings only) is exact up to the first ', / or # outside
 * a string; a text that has one - no file Wallpaper Engine writes does - is
 * counted again under every reading a scanner may have of quotes, slashes
 * and hashes (36: also the C and line comments other readers of "JSON with
 * comments" know), and the deepest reading decides. tests/parsers_test.c
 * holds this against the installed json-glib with random texts. */
#define WPJ_MAX_DEPTH 512

enum { WPJ_SQ_NONE, WPJ_SQ_RAW, WPJ_SQ_ESC };    /* 'strings': not known, without, with \ escapes */
enum { WPJ_SL_NONE, WPJ_SL_PAIR, WPJ_SL_LINE };  /* a '/': nothing, a comment to the next '/', "//" to end of line */

/* the deepest nesting of d under one reading (sq, slash as above; cc: C
 * comments; hash: '#' to end of line). lax (plain reading only): set when
 * a ', / or # stands outside a double-quoted string */
static int json_depth_as (const char *d, gsize len, int sq, gboolean cc, int slash, gboolean hash,
                          gboolean *lax)
{
    int depth = 0, deepest = 0;
    for (gsize i = 0; i < len; i++) {
        char c = d[i];
        if (c == '"' || (c == '\'' && sq != WPJ_SQ_NONE)) {
            gboolean esc = c == '"' || sq == WPJ_SQ_ESC;
            for (i++; i < len && d[i] != c; i++)
                if (esc && d[i] == '\\')
                    i++;
        } else if (c == '/' && cc && i + 1 < len && d[i + 1] == '*') {
            for (i += 2; i + 1 < len && !(d[i] == '*' && d[i + 1] == '/'); i++)
                ;
            i++;
        } else if (c == '/' && slash == WPJ_SL_PAIR) {
            for (i++; i < len && d[i] != '/'; i++)
                ;
        } else if ((c == '/' && slash == WPJ_SL_LINE && i + 1 < len && d[i + 1] == '/') ||
                   (c == '#' && hash)) {
            for (i++; i < len && d[i] != '\n'; i++)
                ;
        } else if (c == '[' || c == '{') {
            if (++depth > deepest)
                deepest = depth;
        } else if ((c == ']' || c == '}') && depth > 0) {
            depth--;
        } else if (lax && (c == '\'' || c == '/' || c == '#')) {
            *lax = TRUE;
        }
    }
    return deepest;
}

gboolean wpj_depth_ok (const char *d, gsize len, int max_depth)
{
    gboolean lax = FALSE;
    if (json_depth_as (d, len, WPJ_SQ_NONE, FALSE, WPJ_SL_NONE, FALSE, &lax) > max_depth)
        return FALSE;
    if (!lax)
        return TRUE;
    for (int sq = WPJ_SQ_NONE; sq <= WPJ_SQ_ESC; sq++)
        for (int cc = 0; cc < 2; cc++)
            for (int slash = WPJ_SL_NONE; slash <= WPJ_SL_LINE; slash++)
                for (int hash = 0; hash < 2; hash++)
                    if (json_depth_as (d, len, sq, cc, slash, hash, NULL) > max_depth)
                        return FALSE;
    return TRUE;
}

/* One value per text. json-glib (seen with 1.8.0) reads on behind the
 * first top-level value: a second array or object is parsed, never becomes
 * the root and is never freed - its whole tree, about thirty times its
 * text. A scene.json of an empty object followed by one of 30 MiB loaded
 * as an empty scene and left some 900 MiB allocated at every load; the
 * library scan reads every project.json the same way, the daemon its
 * request lines.
 *
 * The parser reports where arrays and objects begin and end. One that
 * begins at the top level when a value is already complete is such a
 * second value. The text is then blanked under the parser, which reads the
 * buffer it was given in place: the parse ends at once in an error the
 * parser cleans up after, and the first value - the root, as before - is
 * what the caller gets. (A parser that worked on a copy would read on to
 * the end; the result is the same.)
 *
 * The handlers cost a fifth of the parse time, so only a text that can
 * hold a second value gets them: json_single_plain() is the plain reading
 * (see above) of "white space, one array or object, nothing that opens
 * another", and no ', / or # outside a string that another reading could
 * make something else of. Every file Wallpaper Engine writes is that. */
typedef struct {
    int depth;        /* arrays / objects open */
    int closed;       /* top-level ones complete */
    char *text;
    gsize len;
    JsonNode *first;  /* the root when a second value began */
    gboolean extra;
} WpjOne;

static void wpj_one_start (JsonParser *p, gpointer user)
{
    WpjOne *o = user;
    if (o->depth++ > 0 || o->extra)
        return;
    JsonNode *root = json_parser_get_root (p);
    /* the first value, unless a plain value came before it (then the root
     * is that value: no array, no object) */
    if (!o->closed && (!root || JSON_NODE_HOLDS_OBJECT (root) || JSON_NODE_HOLDS_ARRAY (root)))
        return;
    o->extra = TRUE;
    o->first = root ? json_node_copy (root) : NULL;
    memset (o->text, ' ', o->len);
}

static void wpj_one_end (JsonParser *p, gpointer container, gpointer user)
{
    (void) p; (void) container;
    WpjOne *o = user;
    if (o->depth > 0 && --o->depth == 0)
        o->closed++;
}

static gboolean json_single_plain (const char *d, gsize len)
{
    int depth = 0, tops = 0;
    for (gsize i = 0; i < len; i++) {
        char c = d[i];
        if (c == '"') {
            if (depth == 0 && tops == 0)
                return FALSE;   /* a value in front of the first array / object */
            for (i++; i < len && d[i] != '"'; i++)
                if (d[i] == '\\')
                    i++;
        } else if (c == '[' || c == '{') {
            if (depth++ == 0 && ++tops > 1)
                return FALSE;
        } else if (c == ']' || c == '}') {
            if (depth > 0)
                depth--;
        } else if (c == '\'' || c == '/' || c == '#') {
            return FALSE;
        } else if (depth == 0 && tops == 0 && c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            return FALSE;
        }
    }
    return TRUE;
}

/* text: writable, len bytes; its content is undefined afterwards */
JsonNode *wpj_parse_first (char *text, gsize len, GError **err)
{
    WpjOne one = { .text = text, .len = len };
    g_autoptr (JsonParser) p = json_parser_new ();
    g_signal_connect (p, "object-start", G_CALLBACK (wpj_one_start), &one);
    g_signal_connect (p, "array-start", G_CALLBACK (wpj_one_start), &one);
    g_signal_connect (p, "object-end", G_CALLBACK (wpj_one_end), &one);
    g_signal_connect (p, "array-end", G_CALLBACK (wpj_one_end), &one);
    GError *e = NULL;
    gboolean ok = json_parser_load_from_data (p, text, (gssize) len, &e);
    if (one.extra) {
        g_clear_error (&e);   /* (of the blanked text, not of the file) */
        return one.first;
    }
    if (!ok) {
        g_propagate_error (err, e);
        return NULL;
    }
    JsonNode *root = json_parser_get_root (p);
    return root ? json_node_copy (root) : NULL;
}

/* the parse itself; *msg (if asked for) = why the text was refused, to be
 * freed by the caller; a text that parses but is no object gives NULL
 * without a message, as before. own: the same text, writable (it may be
 * blanked: then nothing is returned in *msg), or NULL. */
static JsonNode *wpj_parse_quiet (const char *data, gssize len, char **msg, char *own)
{
    if (msg)
        *msg = NULL;
    if (!wpj_depth_ok (data, (gsize) len, WPJ_MAX_DEPTH)) {
        if (msg)
            *msg = g_strdup_printf ("nested deeper than %d levels", WPJ_MAX_DEPTH);
        return NULL;
    }
    g_autoptr (GError) err = NULL;
    JsonNode *root = NULL;
    if (json_single_plain (data, (gsize) len)) {
        g_autoptr (JsonParser) p = json_parser_new ();
        if (json_parser_load_from_data (p, data, len, &err)) {
            JsonNode *r = json_parser_get_root (p);
            root = r ? json_node_copy (r) : NULL;
        }
    } else {
        g_autofree char *copy = own ? NULL : g_memdup2 (data, (gsize) len);
        root = wpj_parse_first (own ? own : copy, (gsize) len, &err);
    }
    if (err) {
        if (msg)
            *msg = g_strdup (err->message);
        return NULL;
    }
    if (root && !JSON_NODE_HOLDS_OBJECT (root))
        g_clear_pointer (&root, json_node_unref);
    return root;
}

JsonNode *wpj_parse_data (const char *data, gssize len)
{
    if (!data) return NULL;
    if (len < 0)
        len = (gssize) strlen (data);
    g_autofree char *msg = NULL;
    JsonNode *n = wpj_parse_quiet (data, len, &msg, NULL);
    if (!n && msg)
        g_warning ("json parse failed: %s", msg);
    return n;
}

/* Wallpaper Engine's own JSON reader accepts a comma before the closing
 * ']' or '}' and its files rely on it (the builtin fluid simulation's
 * effect.json, hand-edited Workshop scenes); json-glib does not. Blank
 * such commas (outside strings; same length, so the positions in a later
 * error message still fit). TRUE when one was found. */
static gboolean json_blank_trailing_commas (char *d, gsize len)
{
    gboolean in_str = FALSE, any = FALSE;
    for (gsize i = 0; i < len; i++) {
        char c = d[i];
        if (in_str) {
            if (c == '\\')
                i++;
            else if (c == '"')
                in_str = FALSE;
        } else if (c == '"') {
            in_str = TRUE;
        } else if (c == ',') {
            gsize j = i + 1;
            while (j < len && (d[j] == ' ' || d[j] == '\t' || d[j] == '\r' || d[j] == '\n'))
                j++;
            if (j < len && (d[j] == ']' || d[j] == '}')) {
                d[i] = ' ';
                any = TRUE;
            }
        }
    }
    return any;
}

/* A JSON file a wallpaper brings is read whole and becomes a tree of about
 * twenty times its size: one of a gigabyte (cheap to ship, the Workshop
 * download is compressed) took all memory before anything looked at it.
 * The largest scene.json in 88 packaged Workshop scenes is 1.5 MB. */
#define WPJ_MAX_FILE_BYTES ((goffset) 32 * 1024 * 1024)

JsonNode *wpj_parse_file (const char *path)
{
    gsize len = 0;
    GStatBuf st;
    if (path && g_stat (path, &st) == 0 && (goffset) st.st_size > WPJ_MAX_FILE_BYTES) {
        g_warning ("json: %s is %.0f MiB, more than a wallpaper's json file may have (%d MiB); "
                   "not read", path, st.st_size / 1048576.0, (int) (WPJ_MAX_FILE_BYTES >> 20));
        return NULL;
    }
    char *data = walpe_read_file (path, &len);
    if (!data)
        return NULL;
    if ((goffset) len > WPJ_MAX_FILE_BYTES) {   /* grew between the look and the read */
        g_free (data);
        return NULL;
    }
    /* Wallpaper Engine json files sometimes carry a UTF-8 BOM */
    if (len >= 3 && (guchar) data[0] == 0xEF && (guchar) data[1] == 0xBB && (guchar) data[2] == 0xBF) {
        len -= 3;
        memmove (data, data + 3, len);
    }
    /* files only (IPC lines and shader annotations stay strict): a file
     * that fails as written is tried once more without trailing commas */
    g_autofree char *msg = NULL;
    JsonNode *n = wpj_parse_quiet (data, (gssize) len, &msg, data);
    if (!n && msg && json_blank_trailing_commas (data, len))
        n = wpj_parse_quiet (data, (gssize) len, NULL, data);
    if (!n && msg)
        g_warning ("json parse failed: %s", msg);
    g_free (data);
    return n;
}

/* escape " and \ so a path can be embedded in a hand-built JSON string */
char *walpe_json_escape (const char *s)
{
    if (!s)
        return g_strdup ("");
    GString *g = g_string_new (NULL);
    for (const char *p = s; *p; p++) {
        if ((guchar) *p < 0x20) {
            /* control chars must be escaped in JSON; a raw \n in a file
             * name also split the newline-delimited IPC request */
            g_string_append_printf (g, "\\u%04x", (guchar) *p);
            continue;
        }
        if (*p == '"' || *p == '\\')
            g_string_append_c (g, '\\');
        g_string_append_c (g, *p);
    }
    return g_string_free (g, FALSE);
}
