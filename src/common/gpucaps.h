/* gpucaps.h - what the GL driver in use is and can do, read once.
 *
 * WalPE was written against one driver. This is the one place that says
 * which driver runs now and which optional things it offers; the code
 * that has a fast path or a format it would like asks here instead of
 * assuming. Nothing in here removes a path: on the NVIDIA driver and on
 * llvmpipe every answer is the one the code assumed before.
 *
 * This file is the part without GL: the driver's strings and extension
 * list go in, the structure comes out (walpe_gl_caps_parse), so it is
 * tested with strings of drivers that are not installed. engine/glcaps.h
 * reads the strings from the context and keeps the process's structure.
 *
 * REQUIRED to run at all: OpenGL 3.3 (GLSL 3.30) or OpenGL ES 3.1 (the
 * same minimum as the context ladder of common/glapi.h). Scene wallpapers
 * run on both: their effect shaders are written out as GLSL 3.30 or as
 * GLSL ES 3.10 / 3.20. Everything else is optional and has a fallback:
 * walpe_gl_caps_format () is the table of them. */
#pragma once
#include <glib.h>
#include "common/gpusel.h"

G_BEGIN_DECLS

typedef enum {
    WALPE_GLDRV_UNKNOWN = 0,
    WALPE_GLDRV_NVIDIA,         /* NVIDIA's own driver */
    WALPE_GLDRV_RADEONSI,       /* Mesa: AMD Radeon HD 7000 and later, all APUs since Kaveri */
    WALPE_GLDRV_R600,           /* Mesa: AMD Radeon HD 2000 .. HD 6000 */
    WALPE_GLDRV_R300,           /* Mesa: ATI Radeon 9500 .. X1000 */
    WALPE_GLDRV_IRIS,           /* Mesa: Intel, Broadwell and later */
    WALPE_GLDRV_CROCUS,         /* Mesa: Intel, up to Haswell */
    WALPE_GLDRV_I965,           /* Mesa (before 22): Intel, the earlier driver */
    WALPE_GLDRV_NOUVEAU,        /* Mesa: NVIDIA cards, the free driver */
    WALPE_GLDRV_ZINK,           /* Mesa: OpenGL on top of a Vulkan driver */
    WALPE_GLDRV_LLVMPIPE,       /* Mesa: on the CPU */
    WALPE_GLDRV_SOFTPIPE,       /* Mesa: on the CPU, the reference rasteriser */
    WALPE_GLDRV_SWIFTSHADER,    /* on the CPU */
    WALPE_GLDRV_VIRGL,          /* Mesa: a virtual machine's GPU pass-through */
    WALPE_GLDRV_SVGA,           /* Mesa: VMware's virtual GPU */
    WALPE_GLDRV_D3D12,          /* Mesa: on Direct3D 12 (WSL) */
    WALPE_GLDRV_PANFROST,       /* Mesa: ARM Mali */
    WALPE_GLDRV_FREEDRENO,      /* Mesa: Qualcomm Adreno */
    WALPE_GLDRV_V3D,            /* Mesa: Broadcom VideoCore (Raspberry Pi 4 / 5) */
    WALPE_GLDRV_ETNAVIV,        /* Mesa: Vivante */
    WALPE_GLDRV_ASAHI,          /* Mesa: Apple silicon */
    WALPE_GLDRV_AMD_CLOSED,     /* AMD's own OpenGL driver */
    WALPE_GLDRV_N
} WalpeGlDriver;

typedef enum {
    WALPE_GLVENDOR_OTHER = 0,
    WALPE_GLVENDOR_NVIDIA,
    WALPE_GLVENDOR_AMD,
    WALPE_GLVENDOR_INTEL,
    WALPE_GLVENDOR_ARM,
    WALPE_GLVENDOR_QUALCOMM,
    WALPE_GLVENDOR_BROADCOM,
    WALPE_GLVENDOR_APPLE,
    WALPE_GLVENDOR_VIRTUAL,     /* a virtual machine's adapter */
    WALPE_GLVENDOR_SOFTWARE,    /* no GPU: a CPU rasteriser */
} WalpeGlVendorClass;

/* numbers a context is asked for (engine/glcaps.c); all 0 = not known */
typedef struct {
    int max_texture_size;
    int max_renderbuffer_size;
    int max_texture_units;      /* combined texture image units */
    int max_samples;
    float max_anisotropy;       /* 0 without the extension */
} WalpeGlLimits;

typedef struct {
    /* ---- identity */
    char *vendor, *renderer, *version, *glsl;   /* the driver's own strings ("" = none) */
    gboolean es;                /* OpenGL ES */
    int gl_major, gl_minor;
    int gl_ver;                 /* major * 10 + minor: 33, 46; OpenGL ES: 30, 31, 32 */
    int glsl_ver;               /* 330, 460; OpenGL ES: 300, 310, 320; 0 = not given */
    WalpeGlDriver driver;
    WalpeGlVendorClass vendor_class;
    gboolean mesa;
    int drv_major, drv_minor, drv_patch;        /* Mesa 24.0.9 / NVIDIA 550.120 (patch 0) */
    gboolean software;          /* a CPU rasteriser */
    /* every extension name the context lists (GL_ARB_..., GL_EXT_..., ...):
     * walpe_gl_caps_has_ext (). The named fields below are the answers the
     * code needs most; anything else is asked by name. */
    GHashTable *extensions;
    guint n_extensions;
    /* integrated / discrete as far as the renderer string tells (Intel's
     * and AMD's Mesa strings carry the chip's code name); UNKNOWN else */
    WalpeGpuClass gpu_class;
    WalpeGlLimits limits;

    /* ---- textures */
    gboolean s3tc;              /* DXT1 / DXT3 / DXT5 stay compressed on the GPU */
    gboolean rgtc, bptc, etc2, astc_ldr;        /* known, not used by any loader yet */
    gboolean anisotropic;
    gboolean swizzle;           /* texture swizzle: GL 3.3, ES 3.0 */
    gboolean lod_bias;          /* GL_TEXTURE_LOD_BIAS as a texture parameter: desktop GL */
    gboolean bgra_upload;       /* BGRA bytes into an RGBA8 texture: desktop GL */
    gboolean bgra_ext;          /* OpenGL ES: GL_EXT_texture_format_BGRA8888 */
    gboolean norm16;            /* R16 / RG16: desktop GL, ES with GL_EXT_texture_norm16 */
    gboolean float_linear;      /* linear filtering of 32-bit float textures */
    gboolean get_tex_image;     /* reading a texture back: desktop GL */
    gboolean tex_level_query;   /* glGetTexLevelParameter: desktop GL, ES 3.1 */
    gboolean copy_image;        /* GL 4.3 / ARB_copy_image, ES 3.2 */
    /* ---- render targets */
    gboolean color_float;       /* 16- and 32-bit float targets (OpenGL ES: 3.2 or the extension) */
    gboolean color_half_float;  /* 16-bit float targets only (ES extension) */
    gboolean invalidate_fb;     /* glInvalidateFramebuffer */
    /* ---- buffers, queries, diagnostics */
    gboolean buffer_storage;    /* persistent mapping: GL 4.4 / ARB_buffer_storage */
    gboolean timer_query;       /* GL_TIME_ELAPSED: GL 3.3 / ARB_timer_query */
    gboolean disjoint_timer_query;              /* its OpenGL ES form */
    gboolean samples_passed;    /* exact sample counts: desktop GL */
    gboolean pipeline_stats;    /* GL 4.6 / ARB_pipeline_statistics_query */
    gboolean khr_debug;
    gboolean nvx_gpu_memory_info;
    gboolean ati_meminfo;
    /* ---- images from other APIs (video frames, web pages) */
    gboolean oes_egl_image;     /* an EGLImage as a GL_TEXTURE_2D */
    gboolean oes_egl_image_external;
    gboolean egl_image_dmabuf;  /* EGL: DMA-BUF import (set by engine/glcaps.c) */
    /* ---- what whole features need */
    gboolean base_ok;           /* pictures, GIFs, videos, web pages can be drawn */
    gboolean scene_shaders;     /* scenes: the effect shaders compile here (GLSL
                                 * 3.30, or GLSL ES from OpenGL ES 3.1 on) */
} WalpeGlCaps;

/* Fills *c (zeroed first; walpe_gl_caps_clear frees its strings).
 * extensions: the names separated by spaces (as GL_EXTENSIONS was, or
 * joined from glGetStringi); NULL = none. limits may be NULL. */
void walpe_gl_caps_parse (WalpeGlCaps *c, const char *vendor, const char *renderer,
                          const char *version, const char *glsl, const char *extensions,
                          const WalpeGlLimits *limits);
void walpe_gl_caps_clear (WalpeGlCaps *c);
/* the context lists the extension `name` (the full name, "GL_EXT_...").
 * FALSE for c == NULL. A feature that became part of the core version in
 * use is often no longer listed: ask the named field where one exists. */
WALPE_API gboolean walpe_gl_caps_has_ext (const WalpeGlCaps *c, const char *name);
/* at least this version of the API in use: (c, FALSE, 4, 3) = desktop
 * OpenGL 4.3 or later; (c, TRUE, 3, 1) = OpenGL ES 3.1 or later. FALSE
 * when the context is of the other API. */
WALPE_API gboolean walpe_gl_caps_at_least (const WalpeGlCaps *c, gboolean es, int major,
                                           int minor);

const char *walpe_gl_driver_name (WalpeGlDriver d);            /* "radeonsi", "nvidia", ... */
const char *walpe_gl_vendor_class_name (WalpeGlVendorClass v); /* "AMD", "Intel", ... */

/* NULL when the driver has what WalPE needs to draw anything; else one
 * sentence that names what is missing and the driver (g_free) */
char *walpe_gl_caps_missing (const WalpeGlCaps *c);
/* the same for scene wallpapers (NULL = scenes can be drawn) */
char *walpe_gl_caps_scene_missing (const WalpeGlCaps *c);
/* one line for the log: driver, version, class, the optional features */
char *walpe_gl_caps_describe (const WalpeGlCaps *c);

/* ---- formats: what is used when the driver lacks the one wanted */
typedef enum {
    /* pictures of wallpapers (.tex containers, image files) */
    WALPE_FMT_TEX_DXT1 = 0,
    WALPE_FMT_TEX_DXT3,
    WALPE_FMT_TEX_DXT5,
    WALPE_FMT_TEX_R8,           /* one channel, read through a swizzle */
    WALPE_FMT_TEX_RG8,
    WALPE_FMT_TEX_BGRA8,        /* 8 bits per channel, BGRA in memory */
    /* render targets of scene effects */
    WALPE_FMT_RT_RGBA8,
    WALPE_FMT_RT_R8,
    WALPE_FMT_RT_RG8,
    WALPE_FMT_RT_R16F,
    WALPE_FMT_RT_RG16F,
    WALPE_FMT_RT_RGBA16F,
    WALPE_FMT_RT_RGB8,          /* the copy of the frame under a compose layer */
    WALPE_FMT_RT_RGB10_A2,
    /* data */
    WALPE_FMT_DATA_RGBA32F,     /* particle tables: sampled unfiltered, never drawn to */
    /* video planes */
    WALPE_FMT_VIDEO_R8,
    WALPE_FMT_VIDEO_RG8,
    WALPE_FMT_VIDEO_R16,
    WALPE_FMT_VIDEO_RG16,
    WALPE_FMT_N
} WalpeFmtUse;

typedef enum {
    WALPE_FMT_NATIVE = 0,       /* the format itself */
    WALPE_FMT_CPU_DECODE,       /* decoded / expanded on the CPU, uploaded as `as` */
    WALPE_FMT_SWIZZLED,         /* uploaded as `as`, channels reordered by a texture swizzle */
    WALPE_FMT_CPU_SWAP,         /* bytes reordered on the CPU, uploaded as `as` */
    WALPE_FMT_SUBSTITUTE,       /* the next best format `as` (see `loss`) */
    WALPE_FMT_BGRA_EXT,         /* OpenGL ES: the unsized BGRA format of the extension */
} WalpeFmtHow;

typedef struct {
    WalpeFmtUse use;
    WalpeFmtHow how;
    WalpeFmtUse as;             /* == use when native */
    const char *reason;         /* what the driver lacks; NULL when native */
    const char *loss;           /* what that costs (memory, CPU time, range); NULL = nothing */
} WalpeFmtChoice;

WALPE_API WalpeFmtChoice walpe_gl_caps_format (const WalpeGlCaps *c, WalpeFmtUse use);
const char *walpe_fmt_use_name (WalpeFmtUse use);       /* "DXT5 texture", ... */
/* one sentence for the log (g_free); NULL when the format is native */
char *walpe_fmt_choice_describe (const WalpeFmtChoice *ch, const WalpeGlCaps *c);

G_END_DECLS
