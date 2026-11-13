/* heaptrim.h - malloc_trim () where the C library has it
 *
 * glibc keeps the pages of freed heap memory resident until it is asked to
 * give them back, so the daemon and the window call malloc_trim (0) after
 * they freed something large (a wallpaper, a display that went to sleep,
 * the pictures of a hidden window). The call is glibc's own: musl (Alpine
 * Linux, Void, postmarketOS) has neither malloc_trim nor mallopt in its
 * <malloc.h>, and its allocator returns freed memory to the system by
 * itself. There the calls compile to nothing.
 *
 * Include this instead of <malloc.h> in a file that calls malloc_trim. */
#ifndef WALPE_HEAPTRIM_H
#define WALPE_HEAPTRIM_H

#include <malloc.h>

#ifndef __GLIBC__
static inline int walpe_no_malloc_trim (size_t pad)
{
    (void) pad;
    return 0;   /* (malloc_trim: "no memory was released") */
}
#define malloc_trim walpe_no_malloc_trim
#endif

#endif
