/* gifcheck.h - the frames of a GIF checked against its picture, before a
 * decoder gets the file.
 *
 * Every image of a GIF says where it lies on the picture its header
 * declares (the logical screen): left, top, width and height, 16 bits each.
 * The format requires every image to fit inside that picture, but nothing
 * in the file makes it so, and the size limits on pictures (image.h,
 * tex.c, glutil.c, thumbs.c) are on the declared picture only: they say
 * nothing about an image that the file places elsewhere or declares
 * larger. The rule of the format is therefore checked here, on the file
 * itself, and not left to whichever decoder reads it afterwards.
 *
 * So a GIF is refused, before any of it is decoded, when
 *   - its picture is 0 pixels wide or high,
 *   - an image is 0 pixels wide or high,
 *   - an image does not lie inside the picture (left + width or
 *     top + height beyond it).
 * With every image inside the picture, the limits on the picture hold for
 * the frames as well. A refused file fails to load like any other damaged
 * picture. Nothing is clipped or repaired: the pixels come from the
 * decoder, and an image cut to the picture is no longer what its data
 * describes.
 *
 * The blocks are walked the way the decoder walks them, with what it
 * tolerates: a file that ends early (what is complete is checked), and
 * bytes between blocks (skipped up to the next block start). Two spellings
 * that decoders do not read alike - so that what is checked here need not
 * be what they decode - are refused too: an extension with label 0, and a
 * graphic control or application extension whose first data block is
 * empty. Data that does not start with "GIF8" is not a GIF and passes: its
 * own loader decides. */
#pragma once
#include <glib.h>

G_BEGIN_DECLS

/* d[0..len) = the file. FALSE with err set: a GIF as described above. */
gboolean walpe_gif_layout_ok (const guint8 *d, gsize len, GError **err);

/* The same for a file, read in pieces (a long GIF is not held in memory
 * for this). Also FALSE when the file cannot be read or is not a regular
 * file: what cannot be checked is not handed on. */
gboolean walpe_gif_file_layout_ok (const char *path, GError **err);

G_END_DECLS
