/* portable.h - what the code may assume about the CPU, in one place.
 *
 * WalPE is built for x86-64 and aarch64 (both little-endian, LP64). The
 * two differ where C leaves the choice to the platform:
 *   - plain `char` is signed on x86-64 and unsigned on aarch64: bytes of a
 *     file or of UTF-8 text are read through guint8 / guchar, never
 *     compared or used as an index as `char`;
 *   - a float converted to an integer type that cannot hold it (or a NaN)
 *     is undefined in C and in fact gives INT_MIN on x86 and a saturated
 *     value (0 for NaN) on ARM: walpe_d2i () of util.h is the defined form
 *     (it answers what x86 answers), fx_f2i () the saturating one;
 *   - a store is seen by other threads in program order on x86 and in any
 *     order on ARM: everything shared between threads goes through a lock
 *     or an atomic operation;
 *   - the kernel's page size is 4 KiB on x86-64 and 4, 16 or 64 KiB on
 *     aarch64: walpe_page_size (), never a constant;
 *   - a*b+c may be computed as one fused operation on aarch64; the build
 *     sets -ffp-contract=off so that C arithmetic rounds the same on both.
 *
 * The readers below take bytes at any address (no alignment is assumed:
 * a cast of a byte pointer to a wider type is undefined when the address
 * is odd, and an atomic or exclusive access to such an address faults on
 * ARM) and name their byte order. Wallpaper Engine's files (.pkg, .tex,
 * .mdl) are little-endian, MP4 boxes big-endian. */
#pragma once
#include <glib.h>
#include <string.h>
#include <unistd.h>

G_BEGIN_DECLS

/* Textures, video planes of more than 8 bits and the premultiply loop are
 * handed to GL as the bytes lie in memory, and cairo's ARGB32 is a native
 * 32-bit word: a big-endian build would show wrong colours without any
 * error. It is refused here and by meson.build instead. */
#if G_BYTE_ORDER != G_LITTLE_ENDIAN
#error "WalPE supports little-endian targets only (x86-64, aarch64)"
#endif

static inline guint16 walpe_rd16le (const void *p)
{
    const guint8 *b = (const guint8 *) p;
    return (guint16) (b[0] | (b[1] << 8));
}

static inline guint32 walpe_rd32le (const void *p)
{
    const guint8 *b = (const guint8 *) p;
    return (guint32) b[0] | ((guint32) b[1] << 8) | ((guint32) b[2] << 16) | ((guint32) b[3] << 24);
}

static inline guint64 walpe_rd64le (const void *p)
{
    const guint8 *b = (const guint8 *) p;
    return (guint64) walpe_rd32le (b) | ((guint64) walpe_rd32le (b + 4) << 32);
}

static inline guint16 walpe_rd16be (const void *p)
{
    const guint8 *b = (const guint8 *) p;
    return (guint16) ((b[0] << 8) | b[1]);
}

static inline guint32 walpe_rd32be (const void *p)
{
    const guint8 *b = (const guint8 *) p;
    return ((guint32) b[0] << 24) | ((guint32) b[1] << 16) | ((guint32) b[2] << 8) | (guint32) b[3];
}

static inline guint64 walpe_rd64be (const void *p)
{
    const guint8 *b = (const guint8 *) p;
    return ((guint64) walpe_rd32be (b) << 32) | (guint64) walpe_rd32be (b + 4);
}

/* IEEE 754 single, little-endian, bit for bit (a NaN stays that NaN) */
static inline float walpe_rdf32le (const void *p)
{
    guint32 u = walpe_rd32le (p);
    float f;
    memcpy (&f, &u, sizeof (f));
    return f;
}

static inline void walpe_wr32le (void *p, guint32 v)
{
    guint8 *b = (guint8 *) p;
    b[0] = (guint8) v;
    b[1] = (guint8) (v >> 8);
    b[2] = (guint8) (v >> 16);
    b[3] = (guint8) (v >> 24);
}

/* the kernel's page size in bytes (a power of two) */
static inline gsize walpe_page_size (void)
{
    long n = sysconf (_SC_PAGESIZE);
    return n > 0 ? (gsize) n : 4096;
}

/* Bytes that two threads should not share when each writes its own part
 * (false sharing). 64 on x86-64 and on most aarch64 cores; cores with
 * 128-byte lines lose a little speed with 64, nothing else. */
#define WALPE_CACHELINE 64

G_END_DECLS
