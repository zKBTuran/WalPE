/* image.h - static image backend */
#pragma once
#include "content.h"

/* Is the picture at path small enough to load? The size comes from the
 * file header (nothing is decoded). A GIF's logical screen or a PNG's
 * dimensions cost nothing in the file, but every pixel is 4 bytes once
 * decoded, copied and uploaded - per animation frame for a GIF: a 155-byte
 * GIF with a 12000x12000 canvas took 1.8 GB and seconds per frame.
 * Above WALPE_PICTURE_MAX_SIDE per side or WALPE_PICTURE_MAX_PIXELS in
 * all: FALSE with err set. So is a file that is a picture of a format no
 * wallpaper has (SVG, XPM, a Windows icon, HEIC ...). Format and size are
 * read by WalPE from the file's first bytes (common/pichead.h); gdk-pixbuf
 * is not asked. Files that are no picture format at all (.tex) pass. */
#define WALPE_PICTURE_MAX_SIDE 16384
#define WALPE_PICTURE_MAX_PIXELS (64 * 1000 * 1000)
gboolean walpe_picture_size_ok (const char *path, GError **err);

/* The picture at path opened as gdk-pixbuf's animation (a still picture is
 * one of a single frame), after the check above: only as one of the
 * formats a wallpaper can be, read from the file's own first bytes - a
 * file that is none of them is not handed to gdk-pixbuf to see what it
 * makes of it. NULL with err set otherwise. */
typedef struct _GdkPixbufAnimation GdkPixbufAnimation;
GdkPixbufAnimation *walpe_picture_animation_open (const char *path, GError **err);

WalpeContent *walpe_image_new (const char *path, GError **err);
