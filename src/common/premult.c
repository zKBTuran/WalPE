/* premult.c - RGBA premultiply: exact floor(c*a/255) integer math.
 * Replaces the hand-written AVX2 kernel (lane bugs zeroed rgb); GCC
 * autovectorizes this loop, and it is memory-bound either way. */
#include "premult.h"

void walpe_premultiply_rgba8 (guint8 *buf, gsize n_bytes)
{
    guint32 *p = (guint32 *) (void *) buf;
    gsize px = n_bytes / 4;
    for (gsize i = 0; i < px; i++) {
        guint32 v = p[i];
        guint32 a = v >> 24;
        if (a == 255)
            continue;
        guint32 r = (v & 0xFF) * a, g = ((v >> 8) & 0xFF) * a, b = ((v >> 16) & 0xFF) * a;
        r = (r + 1 + (r >> 8)) >> 8;
        g = (g + 1 + (g >> 8)) >> 8;
        b = (b + 1 + (b >> 8)) >> 8;
        p[i] = r | (g << 8) | (b << 16) | (a << 24);
    }
}
