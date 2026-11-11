/* glutil.h - shared GL helpers: quad, base shader, texture uploads */
#pragma once
#include <glib.h>
#include "common/api.h"
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <epoxy/gl.h>
#include "common/tex.h"

typedef struct { GLuint tex; gboolean premult; int width, height; } WalpeGlTex;

/* one-time init: builds the shared quad VAO + draw program (idempotent) */
WALPE_API void walpe_glutil_init (void);
/* bind the shared 4-vertex strip quad VAO (a_pos=0, a_uv=1) for custom programs */
void walpe_glutil_bind_quad_vao (void);

/* "#version 330" or the matching "#version NNN es" for the current context
 * (call with the context current; used to prepend to shader bodies).
 * The _fs_ variant adds the float precision declaration ES needs in the
 * fragment stage. */
const char *walpe_gl_shader_header (void);
const char *walpe_gl_shader_fs_header (void);

/* BGRA bytes (8 bits per channel) into level 0 of the texture bound to
 * GL_TEXTURE_2D, on either API. _image_ (re)defines the level as w x h
 * (px NULL: storage only), _sub_image_ replaces a part of a level made by
 * _image_. GL_UNPACK_ROW_LENGTH / _ALIGNMENT apply as set by the caller.
 * Desktop OpenGL: glTexImage2D (GL_RGBA8, ..., GL_BGRA, GL_UNSIGNED_BYTE,
 * px) and glTexSubImage2D (..., GL_BGRA, ...) - the same calls as written
 * out. OpenGL ES: a GL_BGRA_EXT texture where the driver has
 * GL_EXT_texture_format_BGRA8888; else the bytes are reordered on the CPU
 * into an RGBA8 texture (not with a pixel unpack buffer bound). Either way
 * the texture samples, blits and mipmaps like the desktop one. */
WALPE_API void walpe_gl_tex_image_bgra (int w, int h, const void *px);
WALPE_API void walpe_gl_tex_sub_image_bgra (int x, int y, int w, int h, const void *px);

/* EGL display of the GTK GL context, set by the daemon at realize time
 * (GDK may use GLX while EGL images still come from the paired display) */
void        walpe_gl_set_egl_display (void *egl_display);
void       *walpe_gl_get_egl_display (void);

/* draw a textured quad; mvp maps unit quad (0..1) to clip space */
WALPE_API void walpe_glutil_draw_quad (GLuint tex, gboolean premult, float opacity,
                             const float *mvp /* 9 floats, column-major */,
                             int blend /* WalpeBlendMode */);
/* tint variant: multiplies rgb (premultiplied space), tint = {1,1,1} = neutral */
void walpe_glutil_draw_quad_tint (GLuint tex, gboolean premult, float opacity,
                                  const float *mvp, int blend, const float tint[3]);
void walpe_glutil_draw_mesh_tint (GLuint tex, gboolean premult, float opacity,
                                  const float *mvp, int blend, const float tint[3],
                                  GLuint vao, int index_count);
/* render thread: free the copy of the frame the blend-mode draws work on
 * (made again on demand) */
void walpe_glutil_blend_release (void);

/* draw planar I420 (three GL_R8 planes) as opaque RGB, BT.601 limited */
void walpe_glutil_draw_yuv (GLuint ty, GLuint tu, GLuint tv, const float *mvp);
/* mode: WALPE_YUV_* bits (yuvconv.h): range and matrix of the planes;
 * 0 = BT.601 limited, 1 = BT.601 full range (yuvj420p) */
WALPE_API void walpe_glutil_draw_yuv_range (GLuint ty, GLuint tu, GLuint tv, const float *mvp,
                                  guint mode);

GLuint walpe_gl_tex_from_pixbuf (GdkPixbuf *pb);      /* premultiplied */
GLuint walpe_gl_tex_from_walpetex (WalpeTex *t, gboolean *premult);
/* new GL texture from any WalpeTex (BGRA, native R8/RG88, DXT); BGRA data is
 * premultiplied in place when premultiply; *premult = texture holds
 * premultiplied color (FALSE for DXT: straight alpha) */
GLuint walpe_gl_upload_tex (WalpeTex *t, GLint wrap, gboolean premultiply,
                            gboolean *premult);
gboolean walpe_gl_has_s3tc (void);   /* GL context current */
/* minification (glutil.c "mipmaps"). On the texture bound to GL_TEXTURE_2D:
 * _sampling sets the min/mag filters for a texture that has (or has not) a
 * mip chain, nearest = point sampling; _gen_mipmaps builds the chain from
 * level 0 (w x h) and selects trilinear sampling - for textures made here
 * that are drawn minified and rarely change (text, album art), not for
 * render targets. Both honour WALPE_TEX_MIPMAPS=0. */
gboolean walpe_gl_mipmaps_enabled (void);
void walpe_gl_tex_sampling (gboolean mipmapped, gboolean nearest);
void walpe_gl_tex_gen_mipmaps (int w, int h);
/* For a texture that walpe_gl_tex_gen_mipmaps + walpe_gl_tex_lod_bias (-1)
 * would sample from level 0 alone while it is drawn at up to 2:1 (that
 * bias): sets the bound texture to sample level 0 the same way (same
 * anisotropy) WITHOUT building the chain, and returns TRUE. The caller
 * builds the chain (the two calls above) once the texture is drawn
 * smaller than 2:1. FALSE = the chain is always sampled here (GLES: no
 * LOD bias; WALPE_TEX_LOD_BIAS set): nothing changed, build it now. */
gboolean walpe_gl_tex_defer_mipmaps (void);
gboolean walpe_gl_tex_is_mipmapped (GLuint tex);   /* binds tex */
/* build the mip chain of a texture that has none yet (WalpeTexInfo
 * .want_mips; binds tex; no-op when it has one, is compressed or mipmaps
 * are off): call when the texture is drawn beyond 2:1 */
void walpe_gl_tex_ensure_mipmaps (GLuint tex);
/* another LOD bias than the default -0.5 for the bound mipmapped texture
 * (-1 = level 0 alone up to 2:1; no-op on GLES and when WALPE_TEX_LOD_BIAS
 * sets one for everything) */
void walpe_gl_tex_lod_bias (float bias);

/* file textures shared across monitor contexts (one EGL share group):
 * load + upload once, refcounted. Without walpe_gl_set_shared (TRUE) every
 * call loads its own texture. Release with walpe_gl_tex_release, which also
 * deletes textures that never came from the cache. */
typedef struct {
    /* the image's size at level 0 of the file, whatever is stored on the
     * GPU (phys_w/phys_h): UVs and resolution uniforms use these */
    int width, height, full_width, full_height;
    int format, channels, we_format;   /* WalpeTexFormat, WalpeTex.channels/we_format */
    gboolean premult;
    /* the file asks for a mip chain (WE samples it trilinear) and it is
     * not built yet: walpe_gl_tex_ensure_mipmaps when drawn beyond 2:1 */
    gboolean want_mips;
    int phys_w, phys_h;     /* texel size of the GL texture's level 0 */
    int lod;                /* halvings between the file's level 0 and it */
    int mips;               /* mip levels stored in the file (0: a plain picture) */
    gboolean compressed;    /* block compressed on the GPU (cannot be halved there) */
    gint64 bytes;           /* GL memory it takes (estimate, chain included) */
    int nframes, frame_w, frame_h;   /* sprite sheet (WalpeTex) */
} WalpeTexInfo;

/* upload options in the high bits of load_flags (the low 16 bits are the
 * loader's WALPE_TEX_LOAD_*): part of what identifies a cached texture.
 * LEVEL (n): the texture is stored n halvings below the file's level 0 -
 *   from the file's own mip level n when it has one (level 0 is then never
 *   decoded or uploaded; WALPE_TEX_STORED_MIPS=0: never), else from the
 *   nearest larger stored level halved on the GPU. A block compressed
 *   texture one level short stays compressed at that level: info.lod says
 *   what was reached, info.phys_w/h the size.
 * NO_CHAIN: block compressed textures: the base level without the stored
 *   levels below it (a texture never drawn beyond 2:1).
 * LAYER_SWIZZLE: R8/RG88 kept native are read as WE's genericimage shader
 *   reads a colour texture (white + alpha / luminance + alpha, straight).
 * NEAR_OK: with LEVEL: a block compressed texture whose file is one stored
 *   level short may stay compressed at that larger level (the caller
 *   samples it within 2:1 anyway); without it the picture is decoded and
 *   halved on the GPU to the level asked for. */
#define WALPE_GL_TEX_LEVEL(n)       ((guint) ((n) & 15) << 16)
#define WALPE_GL_TEX_LEVEL_OF(f)    (((f) >> 16) & 15u)
#define WALPE_GL_TEX_LEVEL_MASK     (15u << 16)
#define WALPE_GL_TEX_NO_CHAIN       (1u << 20)
#define WALPE_GL_TEX_LAYER_SWIZZLE  (1u << 21)
#define WALPE_GL_TEX_NEAR_OK        (1u << 22)
/* CEIL: halving on the GPU rounds odd sizes up (2045 -> 1023 -> 512), so the
 * copy is exactly the size of an effect target at 1/2, 1/4, ... of the
 * texture (ceil (w * k)) and holds what a pass would read from the full
 * texture at those positions. Default: down (w / 2), as the file's own
 * levels are. */
#define WALPE_GL_TEX_CEIL           (1u << 23)

void   walpe_gl_set_shared (gboolean shared);
/* the monitor contexts form one EGL share group (textures made in one are
 * valid in the others) */
gboolean walpe_gl_get_shared (void);
GLuint walpe_gl_tex_cached (const char *path, guint load_flags, GLint wrap, gboolean premultiply,
                            WalpeTexInfo *info);
/* the same, plus the sprite sheet's frame table (g_free; NULL without one) */
GLuint walpe_gl_tex_cached_frames (const char *path, guint load_flags, GLint wrap,
                                   gboolean premultiply, WalpeTexInfo *info,
                                   WalpeTexFrame **frames);
void   walpe_gl_tex_release (GLuint tex);
/* walpe_gl_tex_cached for a reduced level (WALPE_GL_TEX_LEVEL > 0) of a
 * file whose full-size texture the caller holds (own_tex, as own_info
 * describes it: uncompressed, lod 0). Without a share group there is no
 * cache that knows that texture: the copy is then halved from it instead
 * of decoding the file again (WALPE_TEX_OWN_FULL=0: decoded again). With a
 * share group the cache finds it by itself and own_* are not used. */
GLuint walpe_gl_tex_cached_own (const char *path, guint load_flags, GLint wrap, gboolean premultiply,
                                WalpeTexInfo *info, GLuint own_tex, const WalpeTexInfo *own_info);
/* Level 0 of a file is decoded on a worker thread, to be picked up by the
 * walpe_gl_tex_cached* call that loads the file with the same loader flags
 * (the low 16 bits of load_flags) - at whatever level: the decoded picture
 * is reduced on the CPU before it is uploaded. est_bytes = what the decoded
 * picture takes (the workers' byte bound, WALPE_TEX_DECODE_MB). Calls for
 * one file share one decode. TRUE: a decode is reserved for the caller,
 * who answers with walpe_gl_tex_predecode_cancel once it has its texture
 * (from that decode or any other way) or no longer wants one. FALSE:
 * nothing was started (WALPE_TEX_ASYNC=0, or a texture of the file is in
 * the cache already) and there is nothing to cancel. Any thread; needs no
 * GL context. */
gboolean walpe_gl_tex_predecode (const char *path, guint load_flags, gint64 est_bytes);
void   walpe_gl_tex_predecode_cancel (const char *path, guint load_flags);
/* Says which load will pick the decoded picture up (the load_flags, with
 * their WALPE_GL_TEX_LEVEL, and the premultiply argument of the
 * walpe_gl_tex_cached* call to come): a worker that has not finished the
 * decode then also reduces the picture to that level, so the load only
 * uploads. Optional; the first request for a file counts; a load that asks
 * for something else decodes the file again. */
void   walpe_gl_tex_predecode_request (const char *path, guint load_flags, gboolean premultiply);
gboolean walpe_gl_tex_async (void);   /* WALPE_TEX_ASYNC is not 0 */
typedef struct {
    guint started, taken, stolen, dropped;   /* decodes begun by a worker; results used by a
                                              * load; decodes the load did itself because no
                                              * worker had begun; results nobody used */
    guint pending;                           /* jobs not answered yet */
    guint cpu_reduced, gl_halved;            /* reduced copies made before the upload / by GL
                                              * blits of a full-size texture (odd sizes) */
    gint64 bytes, peak_bytes;                /* decoded pictures held or in work: now, most */
} WalpeTexDecodeStats;
void   walpe_gl_tex_predecode_stats (WalpeTexDecodeStats *st);
/* one halving of w x h texels (both even) of bpp = 4, 2 or 1 bytes, in
 * place: the 2x2 box with the rounding of a GL_LINEAR blit to half size,
 * out = avg (avg (a, b), avg (c, d)), avg (x, y) = (x + y + 1) >> 1 */
void   walpe_gl_tex_halve_cpu (guint8 *d, int w, int h, int bpp);
/* GL bytes of the file textures made by this file's loaders (uploads and
 * reduced copies): now, and the most at any moment since the last reset -
 * the video memory peak of a load. One share group / context. */
void   walpe_gl_tex_live_stats (gint64 *live, gint64 *peak, gboolean reset_peak);
/* reduced textures come from the file's own mip levels (WALPE_TEX_STORED_MIPS) */
gboolean walpe_gl_tex_stored_levels (void);
/* GL bytes of every texture walpe_gl_tex_cached* returns on this thread
 * between _begin and _end (a scene sums what it loads). budget > 0: a
 * texture that would take the sum beyond that many bytes is stored
 * smaller (halved until it fits, not below 64 texels; info.lod says by
 * how much): a scene with oversized or hostile textures degrades instead
 * of taking gigabytes. */
void   walpe_gl_tex_scope_begin (gint64 budget);
gint64 walpe_gl_tex_scope_end (void);
/* GL_RENDERER of the first initialised context is a CPU rasterizer
 * (llvmpipe/softpipe/swrast/SwiftShader); FALSE before walpe_glutil_init */
WALPE_API gboolean walpe_gl_is_software (void);
/* scene load summary: bytes of one texture with its levels / of every 2D
 * texture the context can see (0 on GLES, which cannot be asked) */
gint64 walpe_gl_tex_bytes (GLuint tex, gboolean *compressed);
void walpe_gl_tex_totals (int *count, gint64 *bytes, gint64 *compressed_bytes);
/* log texture count/bytes of the current context (WALPE_GPU_MEM diagnostics) */
void walpe_gl_mem_report (const char *tag);

/* load a wallpaper's picture file (.tex, or png / jpeg / bmp / gif / tiff /
 * webp / tga / avif / jxl by its content) -> GL texture, outside the cache.
 * width / height are the picture's; inside a walpe_gl_tex_scope with a
 * budget the texture itself may be stored smaller. */
WalpeGlTex walpe_gl_tex_from_file (const char *path);

typedef enum {
    WALPE_BLEND_NONE = 0,
    WALPE_BLEND_NORMAL = 1,
    WALPE_BLEND_ADDITIVE = 2,
    WALPE_BLEND_SUBTRACTIVE = 3,
    WALPE_BLEND_MULTIPLY = 4,
    WALPE_BLEND_SCREEN = 5,
    /* WE layer blend modes that need the destination ("Emulated (slow)" in
     * the WE editor): WALPE_BLEND_WE + n, n = WE colorBlendMode 1..32.
     * Drawn by copying the framebuffer under the layer and blending in a
     * shader (darken, overlay, hue, ...). */
    WALPE_BLEND_WE = 64,
} WalpeBlendMode;

/* WE scene-object "colorBlendMode" -> WalpeBlendMode. WE numbering (the
 * editor's blend list / BLENDMODE shader combo): 0 normal, 1 darken,
 * 2 multiply, 3 color burn, 4 linear burn, 5 darker color, 6 lighten,
 * 7 screen, 8 color dodge, 9 linear dodge, 10 lighter color, 11 overlay,
 * 12 soft light, 13 hard light, 14 vivid light, 15 linear light,
 * 16 pin light, 17 hard mix, 18 difference, 19 exclusion, 20 subtract,
 * 21 reflect, 22 glow, 23 phoenix, 24 average, 25 negation, 26 hue,
 * 27 saturation, 28 color, 29 luminosity, 30 tint, 31 add (native),
 * 32 diffuse light. Exact GL blend states are used where they exist. */
int walpe_blend_from_we (int color_blend_mode);

/* 3x3 column-major matrix helpers for layer transforms */
typedef struct { float m[9]; } WalpeMat3;
void walpe_mat3_identity (WalpeMat3 *m);
void walpe_mat3_mul (WalpeMat3 *out, const WalpeMat3 *a, const WalpeMat3 *b);
void walpe_mat3_translate (WalpeMat3 *m, float x, float y);
void walpe_mat3_scale (WalpeMat3 *m, float x, float y);
void walpe_mat3_rotate (WalpeMat3 *m, float radians);
/* unit quad -> clip space, aspect-preserving cover of a vw x vh viewport */
WALPE_API void walpe_mat3_cover (WalpeMat3 *m, float iw, float ih, float vw, float vh);

G_BEGIN_DECLS
G_END_DECLS
