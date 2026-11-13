/* udmabuf_shim - a stand-in for /dev/udmabuf, preloaded into KWin by
 * scripts/kwincheck.py (the test `daemon-kwin`) on a machine that has
 * neither a DRM render node nor that device: a container, most of all.
 *
 * KWin 6.8 and newer draw with OpenGL only, on a "render device": a DRM
 * node, or the software device, which exists only when
 * open ("/dev/udmabuf") works. KWin and Mesa's software rasteriser use the
 * device to turn a memfd into a dma-buf, which the rasteriser then maps.
 * A second descriptor of the memfd does for that:
 *   open ("/dev/udmabuf")                   a marker memfd
 *   fstat (marker)                          says "character device 10:125"
 *   ioctl (marker, UDMABUF_CREATE, {memfd, offset 0})   dup of that memfd
 *                                           (another offset: EINVAL, and the
 *                                           caller copies instead)
 *   ioctl (fd, DMA_BUF_IOCTL_SYNC) on what is no dma-buf   0
 * Nothing else changes: the windows, their stacking, the protocols and the
 * script interface are KWin's own, and its drawing is its usual OpenGL
 * path on the software rasteriser. Never loaded where one of the two
 * devices exists, and into no other program. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/* the functions are defined here under their own names: no inline open ()
 * of the same name, and fstat () is not another name of fstat64 () */
#undef _FORTIFY_SOURCE
#undef _FILE_OFFSET_BITS
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define MARK "walpe-kwincheck-udmabuf"
#define UDMABUF_CREATE_REQ 0x40187542UL     /* _IOW ('u', 0x42, struct udmabuf_create) */
#define DMA_BUF_SYNC_REQ 0x40086200UL       /* _IOW ('b', 0, struct dma_buf_sync) */
struct udmabuf_create_s { uint32_t memfd, flags; uint64_t offset, size; };

#define EXPORT __attribute__ ((visibility ("default")))

static int is_dev_path (const char *p)
{
    return p && !strcmp (p, "/dev/udmabuf");
}

static int fake_open (void)
{
    return memfd_create (MARK, MFD_CLOEXEC);
}

static int is_marker (int fd)
{
    char link[64], buf[64];
    snprintf (link, sizeof link, "/proc/self/fd/%d", fd);
    ssize_t n = readlink (link, buf, sizeof buf - 1);
    if (n <= 0)
        return 0;
    buf[n] = 0;
    return strstr (buf, "memfd:" MARK) != NULL;
}

/* (the mode is read whether or not the flags ask for one, as the C library
 * itself does on the machines this is for) */
#define OPEN_LIKE(name, decl, pass)                                     \
    EXPORT int name decl                                                \
    {                                                                   \
        static int (*real) decl;                                        \
        if (!real)                                                      \
            real = (int (*) decl) dlsym (RTLD_NEXT, #name);             \
        if (is_dev_path (path))                                         \
            return fake_open ();                                        \
        va_list ap;                                                     \
        va_start (ap, flags);                                           \
        mode_t mode = va_arg (ap, mode_t);                              \
        va_end (ap);                                                    \
        return real pass;                                               \
    }

OPEN_LIKE (open, (const char *path, int flags, ...), (path, flags, mode))
OPEN_LIKE (open64, (const char *path, int flags, ...), (path, flags, mode))
OPEN_LIKE (openat, (int dfd, const char *path, int flags, ...), (dfd, path, flags, mode))
OPEN_LIKE (openat64, (int dfd, const char *path, int flags, ...), (dfd, path, flags, mode))

EXPORT int fstat (int fd, struct stat *st)
{
    static int (*real) (int, struct stat *);
    if (!real)
        real = (int (*) (int, struct stat *)) dlsym (RTLD_NEXT, "fstat");
    int r = real (fd, st);
    if (r == 0 && S_ISREG (st->st_mode) && st->st_size == 0 && is_marker (fd)) {
        st->st_mode = S_IFCHR | 0660;
        st->st_rdev = makedev (10, 125);
    }
    return r;
}

EXPORT int fstat64 (int fd, struct stat64 *st)
{
    static int (*real) (int, struct stat64 *);
    if (!real)
        real = (int (*) (int, struct stat64 *)) dlsym (RTLD_NEXT, "fstat64");
    int r = real (fd, st);
    if (r == 0 && S_ISREG (st->st_mode) && st->st_size == 0 && is_marker (fd)) {
        st->st_mode = S_IFCHR | 0660;
        st->st_rdev = makedev (10, 125);
    }
    return r;
}

EXPORT int ioctl (int fd, unsigned long req, ...)
{
    static int (*real) (int, unsigned long, ...);
    if (!real)
        real = (int (*) (int, unsigned long, ...)) dlsym (RTLD_NEXT, "ioctl");
    va_list ap;
    va_start (ap, req);
    void *arg = va_arg (ap, void *);
    va_end (ap);
    if ((req & 0xffffffffUL) == UDMABUF_CREATE_REQ && is_marker (fd)) {
        struct udmabuf_create_s *c = arg;
        if (!c || c->offset != 0) {
            errno = EINVAL;
            return -1;
        }
        return fcntl ((int) c->memfd, F_DUPFD_CLOEXEC, 0);
    }
    int r = real (fd, req, arg);
    if (r < 0 && errno == ENOTTY && (req & 0xffffffffUL) == DMA_BUF_SYNC_REQ)
        return 0;
    return r;
}
