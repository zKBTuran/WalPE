/* pichead.h - what a picture file is and how large, read by WalPE itself
 * from the file's own first bytes, before gdk-pixbuf is asked anything.
 *
 * Why not ask gdk-pixbuf: a loader made for one name
 * (gdk_pixbuf_loader_new_with_type ("png")) was the way to keep a file
 * away from every decoder but its own, and its size-prepared signal the
 * way to refuse a picture before a pixel of it existed. Both hold where
 * gdk-pixbuf decodes with its own modules (2.42: Ubuntu 24.04). Where it
 * decodes through glycin (2.44: Arch) neither does:
 *   - every loader name is the same module. It writes what it is fed into
 *     a temporary file and hands that to glycin when the loader is closed,
 *     and glycin picks the decoder from the bytes: an SVG document fed to
 *     the "png" loader is rendered by librsvg, a PNM, QOI, DDS, OpenEXR,
 *     HEIC or icon file by those decoders;
 *   - the size is reported at close, and an answer of 0 x 0 ("do not
 *     decode") is not acted on: the picture is decoded at full size in the
 *     loader process and then dropped. 12000 x 12000 pixels that WalPE
 *     refused cost 830 MiB and 4 to 11 seconds there (a GIF 1650 MiB), a
 *     114-byte SVG of 16000 x 16000 two gigabytes in that process and two
 *     in WalPE's own.
 * So the format and the size are settled here first, by rules that do not
 * depend on the gdk-pixbuf underneath, and a file goes to a loader only
 * when these readers took it for one of the caller's formats and its
 * declared size is one the caller accepts. The loaders' own answers stay
 * in place behind that as a second check. */
#pragma once
#include <glib.h>

G_BEGIN_DECLS

/* The format by signature: "png", "jpeg", "gif", "webp", "bmp", "tiff",
 * "avif", "jxl", or NULL. d[0..len) = the file's first bytes (132 of a file
 * that has them: an ISO media file's brands).
 *   - A file with "ftyp" at offset 4 is an ISO media file and nothing else:
 *     "avif" when its major brand or one of the compatible brands is avif
 *     or avis, else NULL - also when its first bytes are another format's
 *     signature ("BM", a JPEG start, a TIFF's). glycin goes by that box
 *     before it looks for those signatures and hands such a file to
 *     libheif.
 *   - "tiff" is the classic file ("II*\0", "MM\0*"); the 64-bit kind
 *     ("II+\0", "MM\0+") is told by walpe_pichead_is_bigtiff ().
 * TGA has no signature: walpe_tga_head (). */
const char *walpe_pichead_kind (const guint8 *d, gsize len);

/* d[0..len) starts a TIFF file of the 64-bit kind */
gboolean walpe_pichead_is_bigtiff (const guint8 *d, gsize len);

/* The size the header declares, in pixels, for a kind of the list above or
 * "tga". TRUE with *w, *h >= 1; FALSE: the header does not say (cut short,
 * not that format's header, or a layout this reader does not follow).
 *
 * For "png", "jpeg", "gif", "bmp", "webp" and "tga" the header is in one
 * fixed place and these are the numbers the decoder goes by; a file for
 * which this says FALSE is one no decoder shows (walpe_pichead_exact ()).
 * For "tiff" (first directory; of a tag that is there twice the larger
 * value), "avif" (the largest "ispe" property) and "jxl" (the codestream's
 * size header) TRUE is an upper bound good for a limit, and FALSE means
 * only that this reader did not find it.
 *
 * The file variant reads what it needs from the file (a JPEG's frame
 * header within the first 32 MiB, a TIFF directory wherever it is). */
gboolean walpe_pichead_size (const char *kind, const guint8 *d, gsize len, int *w, int *h);
gboolean walpe_pichead_file_size (const char *kind, const char *path, int *w, int *h);
gboolean walpe_pichead_exact (const char *kind);

/* A TGA file by its 18-byte header: one of the picture types and depths
 * gdk-pixbuf's TGA decoder takes (colour-mapped 8 bit, true colour 16 / 24 /
 * 32, grey 8 / 16, plain or run-length coded; not interleaved; a colour map
 * of 15, 16, 24 or 32 bits an entry; a colour-mapped picture has a map).
 *
 * head = the header a decoder is given in place of the file's: for a
 * true-colour or grey picture without the colour map (unused there) and
 * with the map fields zero, for a colour-mapped one with the map flag 1.
 * skip_off / skip_len: the bytes of the file to leave out behind it (the
 * unused map). The pixels are the same; the bytes are then a TGA file to
 * every reader - glycin knows one by the map flag and picture type, and
 * took a true-colour picture with a map, or with a map origin, for no
 * picture or for a Windows cursor.
 * FALSE: not such a header. */
typedef struct {
    int w, h;
    guint8 head[18];
    gsize skip_off, skip_len;
    gboolean changed;       /* head / skip differ from the file */
} WalpeTga;
gboolean walpe_tga_head (const guint8 *d, gsize len, WalpeTga *t);

/* The format of a picture file as a wallpaper has them (a layer picture
 * of the old scene format, a particle sprite, a picture chosen as the
 * wallpaper): walpe_pichead_kind () of its first bytes, or
 *   - "tga" for a TGA header (*tga, may be NULL, is filled) of the common
 *     kind - no colour map and picture type 2, 3, 10 or 11, or a colour map
 *     and type 1 or 9, less the first bytes of a Windows cursor - under any
 *     name, and for every other TGA header under a name that ends in
 *     ".tga";
 *   - "tiff" for a 64-bit TIFF under a name that ends in ".tif" / ".tiff".
 * NULL: none of them, or no regular file. The name decides nothing else:
 * a file of other bytes under a picture's name used to go to the loader of
 * that name ("the decoder's to refuse"), and with glycin underneath that
 * loader decodes whatever the bytes are. Every decoder of these formats
 * asks for its signature, so no picture that was shown is lost by it. */
const char *walpe_pichead_file_kind (const char *path, WalpeTga *tga);

/* For a message: what a file that is none of the above looks like by its
 * first bytes - "svg", "xpm", "pnm", "ico", "ani", "heif", "qoi", "dds",
 * "exr", "hdr", "jp2", "xbm" - or NULL. */
const char *walpe_pichead_file_other (const char *path);

G_END_DECLS
