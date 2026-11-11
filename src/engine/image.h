/* image.h - static image backend */
#pragma once
#include "content.h"

/* Is the picture at path small enough to load? The size comes from the
 * file header (nothing is decoded). A GIF's logical screen or a PNG's
 * dimensions cost nothing in the file, but every pixel is 4 bytes once
 * decoded, copied and uploaded - per animation frame for a GIF: a 155-byte
 * GIF with a 12000x12000 canvas took 1.8 GB and seconds per frame.
 * Above WALPE_PICTURE_MAX_SIDE per side or WALPE_PICTURE_MAX_PIXELS in
 * all: FALSE with err set. So is a file gdk-pixbuf would decode as something
 * that is no picture format of a wallpaper (SVG, XPM, a Windows icon ...).
 * Files gdk-pixbuf does not know (.tex) pass. */
#define WALPE_PICTURE_MAX_SIDE 16384
#define WALPE_PICTURE_MAX_PIXELS (64 * 1000 * 1000)
gboolean walpe_picture_size_ok (const char *path, GError **err);

WalpeContent *walpe_image_new (const char *path, GError **err);
