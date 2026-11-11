/* gifcheck.c - the frames of a GIF checked against its picture (gifcheck.h) */
#include "gifcheck.h"
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* the file's bytes in order: all of them in memory, or read from a
 * descriptor one buffer at a time */
typedef struct {
    int fd;             /* -1: there is no more than p[0..n) */
    const guint8 *p;
    gsize n, i;         /* bytes at p, how many of them are taken */
    gboolean failed;    /* a read failed (not: the file ended) */
    guint8 buf[16384];
} GifIn;

/* the next k bytes, into out when it is not NULL; FALSE: the data ends
 * before that */
static gboolean in_take (GifIn *in, guint8 *out, gsize k)
{
    while (k) {
        if (in->i == in->n) {
            if (in->fd < 0)
                return FALSE;
            gssize got;
            do
                got = read (in->fd, in->buf, sizeof in->buf);
            while (got < 0 && errno == EINTR);
            if (got <= 0) {
                in->failed = got < 0;
                in->fd = -1;
                return FALSE;
            }
            in->p = in->buf;
            in->n = (gsize) got;
            in->i = 0;
        }
        gsize m = MIN (k, in->n - in->i);
        if (out) {
            memcpy (out, in->p + in->i, m);
            out += m;
        }
        in->i += m;
        k -= m;
    }
    return TRUE;
}

static guint le16 (const guint8 *b)
{
    return (guint) b[0] | (guint) b[1] << 8;
}

static gboolean refuse (GError **err, const char *fmt, ...) G_GNUC_PRINTF (2, 3);
static gboolean refuse (GError **err, const char *fmt, ...)
{
    if (err) {
        va_list ap;
        va_start (ap, fmt);
        char *what = g_strdup_vprintf (fmt, ap);
        va_end (ap);
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "the GIF is damaged: %s", what);
        g_free (what);
    }
    return FALSE;
}

/* TRUE where the data ends: every block that is complete has been checked,
 * and a decoder gets no more of the file than this did */
static gboolean gif_walk (GifIn *in, GError **err)
{
    guint8 h[13];
    if (!in_take (in, h, 4) || memcmp (h, "GIF8", 4))
        return TRUE;                            /* not a GIF */
    if (!in_take (in, h + 4, 2))
        return TRUE;
    if (memcmp (h, "GIF87a", 6) && memcmp (h, "GIF89a", 6))
        return refuse (err, "neither a GIF87a nor a GIF89a file");
    if (!in_take (in, h + 6, 7))
        return TRUE;
    guint sw = le16 (h + 6), sh = le16 (h + 8);
    if (!sw || !sh)
        return refuse (err, "its picture is %u x %u pixels", sw, sh);
    if ((h[10] & 0x80) && !in_take (in, NULL, (gsize) 3 << ((h[10] & 7) + 1)))
        return TRUE;                            /* global colour table */
    guint image = 0;
    for (;;) {
        guint8 b, size;
        if (!in_take (in, &b, 1) || b == 0x3B)  /* the trailer: nothing after it is read */
            return TRUE;
        if (b == 0x21) {                        /* extension: label, data blocks */
            guint8 label;
            if (!in_take (in, &label, 1) || !in_take (in, &size, 1))
                return TRUE;
            if (!label || (!size && (label == 0xF9 || label == 0xFF)))
                return refuse (err, "an extension block (label %u) that decoders do not read "
                               "alike", (guint) label);
            while (size)
                if (!in_take (in, NULL, size) || !in_take (in, &size, 1))
                    return TRUE;
        } else if (b == 0x2C) {                 /* image: descriptor, colour table, data blocks */
            guint8 d[9];
            if (!in_take (in, d, 9))
                return TRUE;
            guint x = le16 (d), y = le16 (d + 2), w = le16 (d + 4), ht = le16 (d + 6);
            image++;
            if (!w || !ht || x + w > sw || y + ht > sh)
                return refuse (err, "image %u is %u x %u pixels at %u, %u and does not lie "
                               "inside the %u x %u picture", image, w, ht, x, y, sw, sh);
            if ((d[8] & 0x80) && !in_take (in, NULL, (gsize) 3 << ((d[8] & 7) + 1)))
                return TRUE;
            if (!in_take (in, &size, 1))        /* LZW code size */
                return TRUE;
            do
                if (!in_take (in, &size, 1) || !in_take (in, NULL, size))
                    return TRUE;
            while (size);
        }
        /* any other byte: the decoder looks for a block start in the next one */
    }
}

gboolean walpe_gif_layout_ok (const guint8 *d, gsize len, GError **err)
{
    GifIn in = { .fd = -1, .p = d, .n = d ? len : 0 };
    return gif_walk (&in, err);
}

gboolean walpe_gif_file_layout_ok (const char *path, GError **err)
{
    /* O_NONBLOCK, then fstat: a FIFO of that name is not waited for */
    int fd = path ? g_open (path, O_RDONLY | O_CLOEXEC | O_NONBLOCK, 0) : -1;
    struct stat sb;
    if (fd < 0 || fstat (fd, &sb) != 0 || !S_ISREG (sb.st_mode)) {
        int e = fd < 0 ? errno : EINVAL;
        if (fd >= 0)
            close (fd);
        g_set_error (err, G_IO_ERROR, g_io_error_from_errno (e), "cannot read %s: %s",
                     path ? path : "(no file)", fd < 0 ? g_strerror (e) : "not a regular file");
        return FALSE;
    }
    GifIn *in = g_new0 (GifIn, 1);
    in->fd = fd;
    gboolean ok = gif_walk (in, err);
    if (ok && in->failed) {
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_FAILED, "cannot read %s", path);
        ok = FALSE;
    }
    g_free (in);
    close (fd);
    return ok;
}
