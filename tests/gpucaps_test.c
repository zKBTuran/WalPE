/* gpucaps_test.c - the capability structure (src/common/gpucaps.c)
 * without a GL context: the driver's vendor / renderer / version / GLSL
 * strings and extension lists of several drivers go in; which driver,
 * API and version, integrated or discrete, the optional features, the
 * required-capability sentences and the format fallback table come out.
 *
 * The strings below are ILLUSTRATIONS written from the publicly known
 * format of each driver's strings. None was captured from a machine, and
 * the extension lists are short selections, not what a real driver
 * lists. A check here says the parser reads that format; it does not say
 * a real driver of that name behaves so.
 *
 * Run: meson test -C build gpucaps -v */
#include "common/gpucaps.h"
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; g_printerr ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* extension selections (illustrative) */
#define EXT_DESKTOP_FULL \
    "GL_EXT_texture_compression_s3tc GL_EXT_texture_filter_anisotropic GL_KHR_debug " \
    "GL_ARB_buffer_storage GL_ARB_pipeline_statistics_query GL_ARB_timer_query " \
    "GL_ARB_invalidate_subdata GL_ARB_texture_swizzle GL_ARB_copy_image " \
    "GL_ARB_texture_compression_bptc GL_ARB_texture_compression_rgtc GL_ARB_ES3_compatibility"
#define EXT_NVIDIA EXT_DESKTOP_FULL " GL_NVX_gpu_memory_info GL_NV_texture_barrier"
#define EXT_MESA_HW EXT_DESKTOP_FULL " GL_NVX_gpu_memory_info GL_ATI_meminfo GL_OES_EGL_image " \
    "GL_KHR_texture_compression_astc_ldr"
#define EXT_MESA_SW EXT_DESKTOP_FULL " GL_OES_EGL_image"
#define EXT_ES_FULL \
    "GL_EXT_texture_format_BGRA8888 GL_EXT_color_buffer_float GL_EXT_color_buffer_half_float " \
    "GL_EXT_disjoint_timer_query GL_OES_EGL_image GL_OES_EGL_image_external GL_KHR_debug " \
    "GL_EXT_texture_norm16 GL_OES_texture_float_linear GL_EXT_texture_filter_anisotropic"
#define EXT_ES_BARE "GL_OES_EGL_image GL_KHR_debug"

static WalpeGlCaps mk (const char *vendor, const char *renderer, const char *version,
                       const char *glsl, const char *ext)
{
    WalpeGlCaps c;
    walpe_gl_caps_parse (&c, vendor, renderer, version, glsl, ext, NULL);
    return c;
}

static void test_nvidia (void)
{
    WalpeGlCaps c = mk ("NVIDIA Corporation", "NVIDIA GeForce RTX 4050 Laptop GPU/PCIe/SSE2",
                        "4.6.0 NVIDIA 550.120", "4.60 NVIDIA", EXT_NVIDIA);
    CHECK (c.driver == WALPE_GLDRV_NVIDIA && c.vendor_class == WALPE_GLVENDOR_NVIDIA);
    CHECK (!c.es && c.gl_major == 4 && c.gl_minor == 6 && c.gl_ver == 46 && c.glsl_ver == 460);
    CHECK (!c.mesa && !c.software && c.drv_major == 550 && c.drv_minor == 120);
    CHECK (c.gpu_class == WALPE_GPU_DISCRETE);
    /* everything the renderer has a fast path for */
    CHECK (c.s3tc && c.anisotropic && c.swizzle && c.lod_bias && c.bgra_upload && c.norm16 &&
           c.color_float && c.color_half_float && c.invalidate_fb && c.buffer_storage &&
           c.timer_query && c.samples_passed && c.pipeline_stats && c.khr_debug &&
           c.nvx_gpu_memory_info && c.get_tex_image && c.tex_level_query && c.float_linear);
    CHECK (!c.ati_meminfo && !c.bgra_ext && !c.disjoint_timer_query);
    CHECK (c.base_ok && c.scene_shaders);
    CHECK (walpe_gl_caps_missing (&c) == NULL && walpe_gl_caps_scene_missing (&c) == NULL);
    /* no format needs a fallback on this driver */
    for (int u = 0; u < WALPE_FMT_N; u++) {
        WalpeFmtChoice ch = walpe_gl_caps_format (&c, (WalpeFmtUse) u);
        CHECK (ch.how == WALPE_FMT_NATIVE && ch.as == (WalpeFmtUse) u && ch.reason == NULL);
        CHECK (walpe_fmt_choice_describe (&ch, &c) == NULL);
    }
    CHECK (walpe_gl_caps_has_ext (&c, "GL_NVX_gpu_memory_info"));
    CHECK (!walpe_gl_caps_has_ext (&c, "GL_NVX_gpu_memory"));     /* whole names only */
    CHECK (!walpe_gl_caps_has_ext (&c, "GL_ATI_meminfo") && !walpe_gl_caps_has_ext (NULL, "GL_KHR_debug"));
    CHECK (walpe_gl_caps_at_least (&c, FALSE, 3, 3) && walpe_gl_caps_at_least (&c, FALSE, 4, 6) &&
           !walpe_gl_caps_at_least (&c, FALSE, 4, 7) && !walpe_gl_caps_at_least (&c, TRUE, 3, 0));
    CHECK (c.n_extensions > 10);
    char *d = walpe_gl_caps_describe (&c);
    CHECK (d && strstr (d, "nvidia") && strstr (d, "550.120") && strstr (d, "OpenGL 4.6") &&
           strstr (d, "discrete") && strstr (d, "s3tc"));
    g_free (d);
    CHECK (!strcmp (walpe_gl_driver_name (c.driver), "nvidia"));
    walpe_gl_caps_clear (&c);

    /* a driver branch with three version numbers */
    c = mk ("NVIDIA Corporation", "NVIDIA GeForce GTX 1060 6GB/PCIe/SSE2", "4.6.0 NVIDIA 535.54.03",
            "4.60 NVIDIA", EXT_NVIDIA);
    CHECK (c.driver == WALPE_GLDRV_NVIDIA && c.drv_major == 535 && c.drv_minor == 54 && c.drv_patch == 3);
    walpe_gl_caps_clear (&c);

    /* the same driver giving an OpenGL ES context */
    c = mk ("NVIDIA Corporation", "NVIDIA GeForce RTX 4050 Laptop GPU/PCIe/SSE2",
            "OpenGL ES 3.2 NVIDIA 550.120", "OpenGL ES GLSL ES 3.20", EXT_ES_FULL);
    CHECK (c.driver == WALPE_GLDRV_NVIDIA && c.es && c.gl_ver == 32 && c.glsl_ver == 320);
    CHECK (c.base_ok && c.scene_shaders);      /* GLSL ES output from OpenGL ES 3.1 on */
    walpe_gl_caps_clear (&c);
}

static void test_amd (void)
{
    /* a card, Mesa's current renderer format */
    WalpeGlCaps c = mk ("AMD", "AMD Radeon RX 6600 XT (radeonsi, navi23, LLVM 17.0.6, DRM 3.57, "
                               "6.8.0-45-generic)",
                        "4.6 (Core Profile) Mesa 24.0.9-0ubuntu0.2", "4.60", EXT_MESA_HW);
    CHECK (c.driver == WALPE_GLDRV_RADEONSI && c.vendor_class == WALPE_GLVENDOR_AMD);
    CHECK (c.mesa && c.drv_major == 24 && c.drv_minor == 0 && c.drv_patch == 9);
    CHECK (c.gl_ver == 46 && c.glsl_ver == 460 && !c.software);
    CHECK (c.gpu_class == WALPE_GPU_DISCRETE);
    CHECK (c.s3tc && c.nvx_gpu_memory_info && c.ati_meminfo && c.oes_egl_image && c.astc_ldr &&
           c.buffer_storage && c.base_ok && c.scene_shaders);
    walpe_gl_caps_clear (&c);

    /* a processor's graphics, the same format */
    c = mk ("AMD", "AMD Radeon Graphics (radeonsi, renoir, LLVM 15.0.7, DRM 3.49, 6.2.0-39-generic)",
            "4.6 (Core Profile) Mesa 23.0.4", "4.60", EXT_MESA_HW);
    CHECK (c.driver == WALPE_GLDRV_RADEONSI && c.gpu_class == WALPE_GPU_INTEGRATED);
    walpe_gl_caps_clear (&c);
    /* "Vega" in a product name is not the Vega card */
    c = mk ("AMD", "AMD Radeon Vega 8 Graphics (raven, LLVM 12.0.0, DRM 3.40, 5.11.0-49-generic)",
            "4.6 (Core Profile) Mesa 21.2.6", "4.60", EXT_MESA_HW);
    CHECK (c.driver == WALPE_GLDRV_RADEONSI && c.gpu_class == WALPE_GPU_INTEGRATED);
    walpe_gl_caps_clear (&c);
    c = mk ("AMD", "AMD Radeon RX Vega (vega10, LLVM 15.0.7, DRM 3.49, 6.2.0)",
            "4.6 (Core Profile) Mesa 23.0.4", "4.60", EXT_MESA_HW);
    CHECK (c.gpu_class == WALPE_GPU_DISCRETE);
    walpe_gl_caps_clear (&c);
    /* the older format without a product name */
    c = mk ("AMD", "AMD RENOIR (DRM 3.40.0, 5.11.0-49-generic, LLVM 12.0.0)",
            "4.6 (Core Profile) Mesa 21.0.3", "4.60", EXT_MESA_HW);
    CHECK (c.driver == WALPE_GLDRV_RADEONSI && c.vendor_class == WALPE_GLVENDOR_AMD &&
           c.gpu_class == WALPE_GPU_INTEGRATED);
    walpe_gl_caps_clear (&c);
    /* a chip this file has no name for: AMD, class not known */
    c = mk ("AMD", "AMD Radeon Graphics (radeonsi, somethingnew, LLVM 19.1.0, DRM 3.61, 6.12.0)",
            "4.6 (Core Profile) Mesa 25.0.1", "4.60", EXT_MESA_HW);
    CHECK (c.driver == WALPE_GLDRV_RADEONSI && c.gpu_class == WALPE_GPU_UNKNOWN);
    walpe_gl_caps_clear (&c);

    /* Mesa's r600 driver: OpenGL 3.3 exactly */
    c = mk ("X.Org", "AMD CEDAR (DRM 2.50.0 / 5.15.0-91-generic, LLVM 13.0.1)",
            "3.3 (Core Profile) Mesa 22.0.5", "3.30",
            "GL_EXT_texture_compression_s3tc GL_EXT_texture_filter_anisotropic GL_ARB_timer_query "
            "GL_ARB_texture_swizzle");
    CHECK (c.driver == WALPE_GLDRV_R600 && c.vendor_class == WALPE_GLVENDOR_AMD);
    CHECK (c.gl_ver == 33 && c.glsl_ver == 330 && c.base_ok && c.scene_shaders);
    CHECK (c.gpu_class == WALPE_GPU_DISCRETE);
    /* what 3.3 does not have without an extension */
    CHECK (!c.buffer_storage && !c.invalidate_fb && !c.pipeline_stats && !c.khr_debug &&
           !c.copy_image && !c.bptc);
    CHECK (c.timer_query && c.swizzle && c.color_float && c.rgtc);
    walpe_gl_caps_clear (&c);
    c = mk ("X.Org", "AMD PALM (DRM 2.50.0 / 5.15.0, LLVM 13.0.1)", "3.3 (Core Profile) Mesa 22.0.5",
            "3.30", "GL_EXT_texture_compression_s3tc");
    CHECK (c.driver == WALPE_GLDRV_R600 && c.gpu_class == WALPE_GPU_INTEGRATED);
    walpe_gl_caps_clear (&c);

    /* AMD's own OpenGL driver */
    c = mk ("ATI Technologies Inc.", "AMD Radeon RX 6800 XT",
            "4.6.14800 Core Profile Context 22.20.3", "4.60", EXT_DESKTOP_FULL);
    CHECK (c.driver == WALPE_GLDRV_AMD_CLOSED && c.vendor_class == WALPE_GLVENDOR_AMD && !c.mesa);
    CHECK (c.gl_ver == 46 && c.base_ok && c.scene_shaders && c.gpu_class == WALPE_GPU_UNKNOWN);
    walpe_gl_caps_clear (&c);
}

static void test_intel (void)
{
    WalpeGlCaps c = mk ("Intel", "Mesa Intel(R) Xe Graphics (TGL GT2)",
                        "4.6 (Core Profile) Mesa 23.2.1-1ubuntu3.1~22.04.2", "4.60", EXT_MESA_HW);
    CHECK (c.driver == WALPE_GLDRV_IRIS && c.vendor_class == WALPE_GLVENDOR_INTEL && c.mesa);
    CHECK (c.drv_major == 23 && c.drv_minor == 2 && c.drv_patch == 1);
    CHECK (c.gpu_class == WALPE_GPU_INTEGRATED && c.base_ok && c.scene_shaders && c.s3tc);
    walpe_gl_caps_clear (&c);
    /* an Intel card */
    c = mk ("Intel", "Mesa Intel(R) Arc(tm) A770 Graphics (DG2)", "4.6 (Core Profile) Mesa 24.0.5",
            "4.60", EXT_MESA_HW);
    CHECK (c.driver == WALPE_GLDRV_IRIS && c.gpu_class == WALPE_GPU_DISCRETE);
    walpe_gl_caps_clear (&c);
    /* "Arc" is also the name of a processor's graphics */
    c = mk ("Intel", "Mesa Intel(R) Arc(tm) Graphics (MTL)", "4.6 (Core Profile) Mesa 24.0.5",
            "4.60", EXT_MESA_HW);
    CHECK (c.driver == WALPE_GLDRV_IRIS && c.gpu_class == WALPE_GPU_INTEGRATED);
    walpe_gl_caps_clear (&c);
    /* an older generation: Mesa's crocus driver, OpenGL 4.2 */
    c = mk ("Intel", "Mesa Intel(R) HD Graphics 4000 (IVB GT2)", "4.2 (Core Profile) Mesa 22.3.6",
            "4.20", "GL_EXT_texture_compression_s3tc GL_EXT_texture_filter_anisotropic "
                    "GL_ARB_timer_query GL_KHR_debug GL_ARB_invalidate_subdata");
    CHECK (c.driver == WALPE_GLDRV_CROCUS && c.gpu_class == WALPE_GPU_INTEGRATED);
    CHECK (c.gl_ver == 42 && c.glsl_ver == 420 && c.base_ok && c.scene_shaders);
    CHECK (c.bptc && c.invalidate_fb && !c.buffer_storage && !c.pipeline_stats && !c.copy_image);
    walpe_gl_caps_clear (&c);
    /* the driver before iris / crocus */
    c = mk ("Intel Open Source Technology Center", "Mesa DRI Intel(R) HD Graphics 620 (Kaby Lake GT2)",
            "4.6 (Core Profile) Mesa 20.0.8", "4.60", EXT_MESA_HW);
    CHECK (c.driver == WALPE_GLDRV_I965 && c.vendor_class == WALPE_GLVENDOR_INTEL &&
           c.gpu_class == WALPE_GPU_INTEGRATED);
    walpe_gl_caps_clear (&c);

    /* a generation without OpenGL 3: one sentence that names it */
    c = mk ("Intel Open Source Technology Center", "Mesa DRI Intel(R) Ironlake Mobile",
            "2.1 Mesa 20.0.8", "1.20", "GL_EXT_texture_compression_s3tc");
    CHECK (c.vendor_class == WALPE_GLVENDOR_INTEL && c.gl_ver == 21 && c.glsl_ver == 120);
    CHECK (!c.base_ok && !c.scene_shaders);
    char *m = walpe_gl_caps_missing (&c);
    CHECK (m && strstr (m, "OpenGL 3.3") && strstr (m, "Ironlake") && strstr (m, "2.1"));
    g_free (m);
    m = walpe_gl_caps_scene_missing (&c);
    CHECK (m && strstr (m, "Ironlake"));
    g_free (m);
    walpe_gl_caps_clear (&c);
}

static void test_others (void)
{
    /* the free driver for NVIDIA cards is a Mesa driver */
    WalpeGlCaps c = mk ("nouveau", "NV137", "4.3 (Core Profile) Mesa 23.0.4", "4.30", EXT_MESA_HW);
    CHECK (c.driver == WALPE_GLDRV_NOUVEAU && c.vendor_class == WALPE_GLVENDOR_NVIDIA && c.mesa);
    CHECK (c.gpu_class == WALPE_GPU_DISCRETE && c.gl_ver == 43 && c.scene_shaders);
    walpe_gl_caps_clear (&c);

    /* CPU rasterisers */
    c = mk ("Mesa", "llvmpipe (LLVM 17.0.6, 256 bits)", "4.5 (Core Profile) Mesa 24.0.9", "4.50",
            EXT_MESA_SW);
    CHECK (c.driver == WALPE_GLDRV_LLVMPIPE && c.software && c.vendor_class == WALPE_GLVENDOR_SOFTWARE);
    CHECK (c.gpu_class == WALPE_GPU_UNKNOWN && c.gl_ver == 45 && c.base_ok && c.scene_shaders);
    CHECK (c.s3tc && !c.nvx_gpu_memory_info);
    walpe_gl_caps_clear (&c);
    c = mk ("VMware, Inc.", "llvmpipe (LLVM 12.0.0, 256 bits)", "4.5 (Core Profile) Mesa 21.2.6",
            "4.50", EXT_MESA_SW);
    CHECK (c.driver == WALPE_GLDRV_LLVMPIPE && c.software);
    walpe_gl_caps_clear (&c);
    c = mk ("Mesa", "softpipe", "3.3 (Core Profile) Mesa 24.0.9", "3.30", "");
    CHECK (c.driver == WALPE_GLDRV_SOFTPIPE && c.software && c.base_ok);
    walpe_gl_caps_clear (&c);
    c = mk ("Google Inc.", "Google SwiftShader", "OpenGL ES 3.0 SwiftShader 4.1.0.7",
            "OpenGL ES GLSL ES 3.00", EXT_ES_BARE);
    CHECK (c.driver == WALPE_GLDRV_SWIFTSHADER && c.software && c.es && c.gl_ver == 30);
    walpe_gl_caps_clear (&c);

    /* OpenGL on a Vulkan driver: whose chip it is comes from the inner name */
    c = mk ("Mesa", "zink Vulkan 1.3(AMD Radeon RX 6600 XT (RADV NAVI23) (MESA_RADV))",
            "4.6 (Core Profile) Mesa 24.0.9", "4.60", EXT_MESA_SW);
    CHECK (c.driver == WALPE_GLDRV_ZINK && c.vendor_class == WALPE_GLVENDOR_AMD && !c.software);
    walpe_gl_caps_clear (&c);
    c = mk ("Mesa", "zink Vulkan 1.3(llvmpipe (LLVM 17.0.6, 256 bits) (MESA_LLVMPIPE))",
            "4.6 (Core Profile) Mesa 24.0.9", "4.60", EXT_MESA_SW);
    CHECK (c.software);     /* still the CPU */
    walpe_gl_caps_clear (&c);

    /* virtual machines */
    c = mk ("Mesa", "virgl (AMD Radeon RX 6600 XT (radeonsi, navi23, LLVM 17.0.6))",
            "4.3 (Core Profile) Mesa 24.0.9", "4.30", EXT_MESA_SW);
    CHECK (c.driver == WALPE_GLDRV_VIRGL && c.vendor_class == WALPE_GLVENDOR_VIRTUAL &&
           c.gpu_class == WALPE_GPU_VIRTUAL);
    walpe_gl_caps_clear (&c);
    c = mk ("VMware, Inc.", "SVGA3D; build: RELEASE;  LLVM;", "4.1 (Core Profile) Mesa 24.0.9", "4.10",
            EXT_MESA_SW);
    CHECK (c.driver == WALPE_GLDRV_SVGA && c.gpu_class == WALPE_GPU_VIRTUAL);
    walpe_gl_caps_clear (&c);

    /* a driver nobody here has heard of: nothing is assumed, the version decides */
    c = mk ("Some Vendor", "Some GPU 9000", "4.5.0 build 1.2.3", "4.50", "GL_EXT_texture_compression_s3tc");
    CHECK (c.driver == WALPE_GLDRV_UNKNOWN && c.vendor_class == WALPE_GLVENDOR_OTHER && !c.software);
    CHECK (c.gl_ver == 45 && c.base_ok && c.scene_shaders && c.s3tc && c.gpu_class == WALPE_GPU_UNKNOWN);
    CHECK (!strcmp (walpe_gl_driver_name (c.driver), "unknown"));
    walpe_gl_caps_clear (&c);

    /* no strings at all (a context that answers nothing) */
    c = mk (NULL, NULL, NULL, NULL, NULL);
    CHECK (c.driver == WALPE_GLDRV_UNKNOWN && !c.base_ok && c.gl_ver == 0 && c.n_extensions == 0);
    CHECK (c.vendor && c.renderer && c.version && c.glsl && !*c.renderer);
    char *m = walpe_gl_caps_missing (&c);
    CHECK (m != NULL);
    g_free (m);
    walpe_gl_caps_clear (&c);
    walpe_gl_caps_clear (&c);       /* twice is fine */
}

static void test_gles (void)
{
    /* an ARM board's GPU with everything optional */
    WalpeGlCaps c = mk ("Mesa", "Mali-G52 r1 (Panfrost)", "OpenGL ES 3.1 Mesa 24.0.9",
                        "OpenGL ES GLSL ES 3.10", EXT_ES_FULL);
    CHECK (c.driver == WALPE_GLDRV_PANFROST && c.vendor_class == WALPE_GLVENDOR_ARM && c.mesa);
    CHECK (c.es && c.gl_major == 3 && c.gl_minor == 1 && c.gl_ver == 31 && c.glsl_ver == 310);
    CHECK (c.gpu_class == WALPE_GPU_INTEGRATED);
    CHECK (c.base_ok && c.scene_shaders);      /* OpenGL ES 3.1: scenes through the GLSL ES output */
    CHECK (walpe_gl_caps_scene_missing (&c) == NULL);
    CHECK (walpe_gl_caps_missing (&c) == NULL);
    CHECK (!c.lod_bias && !c.bgra_upload && c.bgra_ext && !c.get_tex_image && c.tex_level_query &&
           !c.samples_passed && !c.timer_query && c.disjoint_timer_query && !c.buffer_storage &&
           !c.pipeline_stats && c.swizzle && c.invalidate_fb && c.etc2 && !c.s3tc && c.norm16 &&
           c.color_float && c.color_half_float && c.float_linear && c.oes_egl_image &&
           c.oes_egl_image_external && c.khr_debug && !c.copy_image);
    CHECK (walpe_gl_caps_at_least (&c, TRUE, 3, 1) && !walpe_gl_caps_at_least (&c, TRUE, 3, 2) &&
           !walpe_gl_caps_at_least (&c, FALSE, 3, 0));
    WalpeFmtChoice ch = walpe_gl_caps_format (&c, WALPE_FMT_TEX_BGRA8);
    CHECK (ch.how == WALPE_FMT_BGRA_EXT);
    ch = walpe_gl_caps_format (&c, WALPE_FMT_TEX_DXT1);
    CHECK (ch.how == WALPE_FMT_CPU_DECODE && ch.as == WALPE_FMT_TEX_BGRA8);
    ch = walpe_gl_caps_format (&c, WALPE_FMT_RT_R16F);
    CHECK (ch.how == WALPE_FMT_NATIVE);
    ch = walpe_gl_caps_format (&c, WALPE_FMT_VIDEO_R16);
    CHECK (ch.how == WALPE_FMT_NATIVE);
    walpe_gl_caps_clear (&c);

    /* OpenGL ES 3.0 with next to nothing */
    c = mk ("Broadcom", "V3D 4.2", "OpenGL ES 3.0 Mesa 23.2.1", "OpenGL ES GLSL ES 3.00", EXT_ES_BARE);
    CHECK (c.driver == WALPE_GLDRV_V3D && c.vendor_class == WALPE_GLVENDOR_BROADCOM && c.es &&
           c.gl_ver == 30 && c.glsl_ver == 300);
    /* below the minimum of the tree (OpenGL ES 3.1): both sentences name it */
    CHECK (!c.base_ok && !c.scene_shaders && !c.tex_level_query && !c.bgra_ext && !c.norm16 &&
           !c.color_float && !c.color_half_float && !c.disjoint_timer_query && !c.float_linear);
    char *m = walpe_gl_caps_missing (&c);
    CHECK (m && strstr (m, "OpenGL ES 3.1") && strstr (m, "V3D") && strstr (m, "3.0"));
    g_free (m);
    m = walpe_gl_caps_scene_missing (&c);
    CHECK (m && strstr (m, "OpenGL ES 3.1") && strstr (m, "V3D"));
    g_free (m);
    ch = walpe_gl_caps_format (&c, WALPE_FMT_TEX_BGRA8);
    CHECK (ch.how == WALPE_FMT_SWIZZLED && ch.reason);
    ch = walpe_gl_caps_format (&c, WALPE_FMT_RT_RG16F);
    CHECK (ch.how == WALPE_FMT_SUBSTITUTE && ch.as == WALPE_FMT_RT_RGBA8 && ch.reason && ch.loss);
    ch = walpe_gl_caps_format (&c, WALPE_FMT_RT_RGBA16F);
    CHECK (ch.how == WALPE_FMT_SUBSTITUTE && ch.as == WALPE_FMT_RT_RGBA8);
    ch = walpe_gl_caps_format (&c, WALPE_FMT_VIDEO_RG16);
    CHECK (ch.how == WALPE_FMT_SUBSTITUTE && ch.as == WALPE_FMT_VIDEO_RG8);
    ch = walpe_gl_caps_format (&c, WALPE_FMT_VIDEO_R16);
    CHECK (ch.how == WALPE_FMT_SUBSTITUTE && ch.as == WALPE_FMT_VIDEO_R8);
    /* part of OpenGL ES 3.0: never a fallback */
    static const WalpeFmtUse core[] = { WALPE_FMT_RT_RGBA8, WALPE_FMT_RT_R8, WALPE_FMT_RT_RG8,
                                        WALPE_FMT_RT_RGB8, WALPE_FMT_RT_RGB10_A2,
                                        WALPE_FMT_DATA_RGBA32F, WALPE_FMT_VIDEO_R8,
                                        WALPE_FMT_VIDEO_RG8, WALPE_FMT_TEX_R8, WALPE_FMT_TEX_RG8 };
    for (guint i = 0; i < G_N_ELEMENTS (core); i++)
        CHECK (walpe_gl_caps_format (&c, core[i]).how == WALPE_FMT_NATIVE);
    walpe_gl_caps_clear (&c);

    /* half-float targets only: enough for the 16F formats */
    c = mk ("Qualcomm", "FD630", "OpenGL ES 3.1 Mesa 24.0.9", "OpenGL ES GLSL ES 3.10",
            "GL_EXT_color_buffer_half_float GL_EXT_texture_format_BGRA8888");
    CHECK (c.driver == WALPE_GLDRV_FREEDRENO && c.vendor_class == WALPE_GLVENDOR_QUALCOMM);
    CHECK (!c.color_float && c.color_half_float && !c.copy_image && c.gl_ver == 31);
    CHECK (walpe_gl_caps_format (&c, WALPE_FMT_RT_R16F).how == WALPE_FMT_NATIVE);
    walpe_gl_caps_clear (&c);
    /* OpenGL ES 3.2 renders to float formats by itself: no extension has to be listed */
    c = mk ("Qualcomm", "FD630", "OpenGL ES 3.2 Mesa 24.0.9", "OpenGL ES GLSL ES 3.20",
            "GL_EXT_texture_format_BGRA8888");
    CHECK (c.color_float && c.color_half_float && c.copy_image && c.gl_ver == 32);
    CHECK (walpe_gl_caps_format (&c, WALPE_FMT_RT_RGBA16F).how == WALPE_FMT_NATIVE);
    walpe_gl_caps_clear (&c);
}

/* the fallbacks a desktop driver can need */
static void test_desktop_fallbacks (void)
{
    /* no S3TC (a Mesa built without it, or WALPE_GL_LACKS): DXT is decoded */
    WalpeGlCaps c = mk ("Mesa", "llvmpipe (LLVM 17.0.6, 256 bits)", "4.5 (Core Profile) Mesa 24.0.9",
                        "4.50", "GL_EXT_texture_filter_anisotropic GL_KHR_debug");
    CHECK (!c.s3tc);
    static const WalpeFmtUse dxt[] = { WALPE_FMT_TEX_DXT1, WALPE_FMT_TEX_DXT3, WALPE_FMT_TEX_DXT5 };
    for (guint i = 0; i < G_N_ELEMENTS (dxt); i++) {
        WalpeFmtChoice ch = walpe_gl_caps_format (&c, dxt[i]);
        CHECK (ch.how == WALPE_FMT_CPU_DECODE && ch.as == WALPE_FMT_TEX_BGRA8);
        CHECK (ch.reason && strstr (ch.reason, "s3tc") && ch.loss);
        char *s = walpe_fmt_choice_describe (&ch, &c);
        CHECK (s && strstr (s, "DXT") && strstr (s, "llvmpipe") && strstr (s, "decoded on the CPU") &&
               strstr (s, "video memory"));
        g_free (s);
    }
    /* everything else of the desktop table is part of OpenGL 3.3 */
    for (int u = WALPE_FMT_TEX_R8; u < WALPE_FMT_N; u++)
        CHECK (walpe_gl_caps_format (&c, (WalpeFmtUse) u).how == WALPE_FMT_NATIVE);
    walpe_gl_caps_clear (&c);

    /* features by version alone, no extension listed */
    c = mk ("Some Vendor", "Some GPU", "4.6.0", "4.60", "");
    CHECK (c.buffer_storage && c.invalidate_fb && c.pipeline_stats && c.khr_debug && c.copy_image &&
           c.anisotropic && c.bptc && c.etc2 && c.timer_query && c.swizzle);
    CHECK (!c.s3tc && !c.astc_ldr && !c.nvx_gpu_memory_info);    /* never part of a version */
    walpe_gl_caps_clear (&c);
    c = mk ("Some Vendor", "Some GPU", "4.3.0", "4.30", "");
    CHECK (!c.buffer_storage && c.invalidate_fb && !c.pipeline_stats && c.khr_debug && c.copy_image &&
           !c.anisotropic);
    walpe_gl_caps_clear (&c);
    c = mk ("Some Vendor", "Some GPU", "3.3.0", "3.30", "GL_ARB_buffer_storage GL_ARB_invalidate_subdata");
    CHECK (c.buffer_storage && c.invalidate_fb && c.base_ok && c.scene_shaders);
    walpe_gl_caps_clear (&c);
    /* OpenGL 3.2: below what is needed */
    c = mk ("Some Vendor", "Some GPU", "3.2.0", "1.50", "");
    CHECK (!c.base_ok && !c.scene_shaders);
    char *m = walpe_gl_caps_missing (&c);
    CHECK (m && strstr (m, "Some GPU") && strstr (m, "3.2"));
    g_free (m);
    walpe_gl_caps_clear (&c);
    /* a 3.3 context whose GLSL string says less: scenes cannot compile */
    c = mk ("Some Vendor", "Some GPU", "3.3.0", "1.50", "");
    CHECK (c.base_ok && !c.scene_shaders);
    walpe_gl_caps_clear (&c);

    /* before a context exists nothing is known to be missing */
    CHECK (walpe_gl_caps_format (NULL, WALPE_FMT_TEX_DXT5).how == WALPE_FMT_NATIVE);
    CHECK (walpe_gl_caps_missing (NULL) == NULL && walpe_gl_caps_scene_missing (NULL) == NULL);
    for (int u = 0; u < WALPE_FMT_N; u++)
        CHECK (walpe_fmt_use_name ((WalpeFmtUse) u) && strcmp (walpe_fmt_use_name ((WalpeFmtUse) u), "format"));
    for (int d = 0; d < WALPE_GLDRV_N; d++)
        CHECK (walpe_gl_driver_name ((WalpeGlDriver) d) != NULL);
}

static void test_version_strings (void)
{
    static const struct { const char *ver, *glsl; gboolean es; int gl, sl; } v[] = {
        { "4.6.0 NVIDIA 550.120", "4.60 NVIDIA", FALSE, 46, 460 },
        { "4.6 (Core Profile) Mesa 24.0.9", "4.60", FALSE, 46, 460 },
        { "4.6 (Compatibility Profile) Mesa 24.0.9", "4.60", FALSE, 46, 460 },
        { "3.3 (Core Profile) Mesa 22.0.5", "3.30", FALSE, 33, 330 },
        { "3.3.0", "3.3", FALSE, 33, 330 },
        { "3.1 Mesa 20.0.8", "1.40", FALSE, 31, 140 },
        { "OpenGL ES 3.2 Mesa 24.0.9", "OpenGL ES GLSL ES 3.20", TRUE, 32, 320 },
        { "OpenGL ES 3.0 Mesa 23.2.1", "OpenGL ES GLSL ES 3.00", TRUE, 30, 300 },
        { "OpenGL ES 3.2 NVIDIA 550.120", "OpenGL ES GLSL ES 3.20", TRUE, 32, 320 },
        { "OpenGL ES 2.0 Mesa 23.2.1", "OpenGL ES GLSL ES 1.00", TRUE, 20, 100 },
        { "", "", FALSE, 0, 0 },
    };
    for (guint i = 0; i < G_N_ELEMENTS (v); i++) {
        WalpeGlCaps c = mk ("v", "r", v[i].ver, v[i].glsl, "");
        if (c.es != v[i].es || c.gl_ver != v[i].gl || c.glsl_ver != v[i].sl) {
            fails++;
            g_printerr ("FAIL version \"%s\" / \"%s\": es %d gl %d glsl %d\n", v[i].ver, v[i].glsl,
                        c.es, c.gl_ver, c.glsl_ver);
        }
        walpe_gl_caps_clear (&c);
    }
}

int main (void)
{
    test_nvidia ();
    test_amd ();
    test_intel ();
    test_others ();
    test_gles ();
    test_desktop_fallbacks ();
    test_version_strings ();
    if (fails)
        g_printerr ("gpucaps-test: %d failure(s)\n", fails);
    else
        g_print ("gpucaps-test: all checks passed\n");
    return fails ? 1 : 0;
}
