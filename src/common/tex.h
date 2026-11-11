/* tex.h - Wallpaper Engine .tex texture container parser */
#pragma once
#include <glib.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

/* .tex header flags (RePKG / linux-wallpaperengine TextureFlags) */
enum {
    WALPE_TEXF_NO_INTERPOLATION = 1 << 0,   /* point sampling (pixel art) */
    WALPE_TEXF_CLAMP_UVS = 1 << 1,
    WALPE_TEXF_IS_GIF = 1 << 2,             /* sprite sheet (TEXS table) */
    WALPE_TEXF_VIDEO = 1 << 5,              /* mip 0 is an MP4 */
};
#define WALPE_TEX_MAX_LEVELS 15

typedef enum {
    WALPE_TEX_RGBA8 = 0,   /* raw 8-bit, BGRA byte order in memory */
    WALPE_TEX_DXT1 = 1,
    WALPE_TEX_DXT3 = 2,
    WALPE_TEX_DXT5 = 3,
} WalpeTexFormat;

/* one frame of an animated sprite sheet (TEXS0002/0003 table after the
 * mips): image index, duration (s), and the frame rect in texture pixels
 * as two axis vectors (w1,w2) = x axis, (h2,h1) = y axis (axis aligned:
 * w2 = h2 = 0) */
typedef struct {
    int image;
    float time;
    float x, y, w1, w2, h2, h1;
} WalpeTexFrame;

typedef struct {
    WalpeTexFormat format;
    int width, height;            /* image (real content) size */
    int full_width, full_height;  /* == width/height: padded mips are cropped to the image
                                   * at load (kept for g_TextureNResolution callers) */
    guint8 *data;         /* g_free(); straight alpha (not premultiplied) */
    gsize len;
    /* animated sprite sheet (flag 4 + TEXS table); nframes 0 = static */
    int nframes;
    WalpeTexFrame *frames;  /* g_free()d by walpe_tex_free */
    int frame_w, frame_h;   /* sprite size (TEXS0003 header, else frame 0) */
    /* RGBA8 only: 1 = R8, 2 = RG88 kept native (WALPE_TEX_LOAD_NATIVE_RG);
     * 0 = 4 bytes per pixel */
    int channels;
    /* the container's WE format id (0 RGBA8888, 4 DXT5, 6 DXT3, 7 DXT1,
     * 8 RG88, 9 R8 - WE's FORMAT_* shader constants); 0 for PNG/JPEG */
    int we_format;
    /* sampling, as the author's import settings in the container say:
     * mips = stored mip levels of image 0 (> 1: WE samples the texture
     * trilinear through its mip chain; 1: imported without mipmaps, WE
     * samples it bilinear; 0: not a container, a plain image file) and
     * tex_flags = the header flags (WALPE_TEXF_*) */
    int mips;
    guint tex_flags;
    /* WALPE_TEX_LOAD_NATIVE_DXT only: the stored levels 1..n_levels of the
     * chain, cropped to the image like level 0 (block rows; g_free()d by
     * walpe_tex_free). Other formats get their chain from glGenerateMipmap. */
    int n_levels;
    struct { int w, h; guint8 *data; gsize len; } level[WALPE_TEX_MAX_LEVELS];
    /* walpe_tex_load_level: the stored level these pixels are (0 = full
     * size) and the image's size at level 0 (== width/height for level 0
     * and for everything that is not a container) */
    int mip_level;
    int base_width, base_height;
} WalpeTex;

/* walpe_tex_load_ex flags: keep GPU-native formats instead of expanding
 * to BGRA (upload with walpe_gl_upload_tex). NATIVE_DXT only applies to
 * unpadded or block-aligned images; DXT data is straight alpha. */
enum {
    WALPE_TEX_LOAD_NATIVE_RG = 1 << 0,
    WALPE_TEX_LOAD_NATIVE_DXT = 1 << 1,
    /* expand R8/RG88 the way WE's shaders consume a color texture
     * (ConvertTexture0Format): R8 -> (1,1,1,R), RG88 -> (R,R,R,G) */
    WALPE_TEX_LOAD_WE_COLOR = 1 << 2,
    /* walpe_tex_load_level only: also give a level of an image whose
     * size is not a multiple of 2^level: max(1, w >> level) x
     * max(1, h >> level) texels from the level's top-left, as the stored
     * DXT chain is cropped. The last column / row of such a level was
     * averaged from the image's last texels AND the padding next to them
     * (a fringe up to one texel wide on the right / bottom edge). */
    WALPE_TEX_LOAD_LEVEL_FLOOR = 1 << 3,
};

/* Parse a .tex file. On success returns texture (caller: walpe_tex_free).
 * PNG/JPEG/WEBP payloads embedded in the container are decoded via pixbuf
 * and returned as WALPE_TEX_RGBA8 (byte order RGBA). NULL on failure. */
WalpeTex *walpe_tex_load (const char *path);
WalpeTex *walpe_tex_load_ex (const char *path, guint flags);

/* walpe_tex_load_ex for one stored mip level of a container: the result
 * is that level's image (width/height = the level 0 image size >> level,
 * base_width/base_height = the level 0 size), decoded like level 0 (same
 * flags, same byte order, cropped to the image). Only the level asked for
 * is read and decoded: level 1 of a 4K RGBA texture costs a quarter of the
 * bytes and time of level 0, level 2 a sixteenth.
 * level 0 is walpe_tex_load_ex. For level > 0 the result is NULL - load
 * level 0 instead - unless the file is a container that stores the level
 * as pixels (raw or LZ4; RGBA8888 / R8 / RG88, or DXT without
 * WALPE_TEX_LOAD_NATIVE_DXT), the padded texture halves down to it as GL
 * halves, the image's width and height are both multiples of 2^level
 * (its texels at that level then cover image texels only, never the
 * padding; WALPE_TEX_LOAD_LEVEL_FLOOR lifts this), and it is not a sprite
 * sheet.
 * The pixels are the author's (WE's importer's) minification of level 0,
 * not a 2x2 box average of the level 0 pixels: close to, but not
 * byte-identical with, halving level 0 on the GPU. */
WalpeTex *walpe_tex_load_level (const char *path, guint flags, int level);

/* what a container stores, from its headers alone (no pixel is read) */
typedef struct {
    int width, height;           /* image size at level 0 */
    int tex_width, tex_height;   /* padded texture size at level 0 */
    int we_format;               /* as WalpeTex.we_format */
    guint tex_flags;             /* WALPE_TEXF_* */
    int mips;                    /* stored levels of image 0 */
    int levels;                  /* walpe_tex_load_level loads levels 0 .. levels-1
                                  * (0: a video texture) */
    int levels_floor;            /* the same with WALPE_TEX_LOAD_LEVEL_FLOOR (>= levels) */
    gboolean lz4;                /* level 0 is LZ4 compressed */
    gboolean picture;            /* level 0 is an embedded PNG / JPEG: the result's size
                                  * and pixels come from that picture, and there are no
                                  * loadable smaller levels */
    gsize level_bytes[WALPE_TEX_MAX_LEVELS + 1];   /* file bytes of each of them */
} WalpeTexProbe;
/* FALSE: not a .tex container (a plain picture, unreadable, malformed) */
gboolean walpe_tex_probe (const char *path, WalpeTexProbe *pr);
/* For a container whose level 0 is a PNG / JPEG (WalpeTexProbe.picture):
 * the width and height walpe_tex_load_ex will return, read from the
 * picture's own header - they need not be the container's. Nothing is
 * decoded. FALSE: no such container, or its picture declares no usable
 * size (load it to find out). */
gboolean walpe_tex_picture_size (const char *path, int *w, int *h);
WalpeTex *walpe_tex_from_pixbuf (GdkPixbuf *pb);   /* helper, copies + flattens */
void walpe_tex_free (WalpeTex *t);

/* WE video texture (flag 0x20): an MP4 stored as mip 0. Reads only the
 * header; on TRUE off/len give the MP4's byte range inside the file and
 * w/h the declared size. walpe_tex_load() fails for these. */
gboolean walpe_tex_video_range (const char *path, guint64 *off, guint64 *len,
                                int *w, int *h);
