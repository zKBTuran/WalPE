/* fakegpu_egl_vendor.c - a libglvnd EGL vendor library that stands in for
 * a GPU driver in tests/swsim/run.sh: like NVIDIA's on a machine that has that driver
 * it sorts before Mesa (10_fakegpu.json) and claims every X11 display, so
 * libglvnd hands the display to it and never asks Mesa. It cannot render:
 * its eglInitialize fails (and says so on stderr). That is enough to show
 * which vendor libglvnd picked - without "Render on CPU" the daemon ends
 * up here, with it this library is never loaded.
 *
 * Not part of WalPE; built by the script only. */
#include <EGL/egl.h>
#include <glvnd/libeglabi.h>
#include <stdio.h>
#include <string.h>

static int fake_display;   /* the handle returned as EGLDisplay */

static EGLBoolean fake_initialize (EGLDisplay d, EGLint *maj, EGLint *min)
{
    (void) d; (void) maj; (void) min;
    fprintf (stderr, "FAKEGPU: eglInitialize called - libglvnd gave the display to the GPU vendor\n");
    return EGL_FALSE;
}

static EGLint fake_get_error (void)
{
    return EGL_NOT_INITIALIZED;
}

static const char *fake_query_string (EGLDisplay d, EGLint name)
{
    (void) d;
    return name == EGL_VENDOR ? "FAKEGPU" : name == EGL_EXTENSIONS ? "" : "1.5 FAKEGPU";
}

/* every other entry point: fails with 0 / EGL_FALSE / NULL */
static long fake_zero (void)
{
    return 0;
}

static EGLDisplay fake_get_platform_display (EGLenum platform, void *native, const EGLAttrib *attrs)
{
    (void) attrs;
    fprintf (stderr, "FAKEGPU: asked for a display (platform 0x%x, native %p)\n", platform, native);
    return (EGLDisplay) &fake_display;
}

static EGLBoolean fake_supports_api (EGLenum api)
{
    (void) api;
    return EGL_TRUE;
}

static const char *fake_vendor_string (int name)
{
    return name == __EGL_VENDOR_STRING_PLATFORM_EXTENSIONS
               ? "EGL_KHR_platform_x11 EGL_EXT_platform_x11" : NULL;
}

static void *fake_get_proc_address (const char *name)
{
    if (!strcmp (name, "eglInitialize"))
        return (void *) fake_initialize;
    if (!strcmp (name, "eglGetError"))
        return (void *) fake_get_error;
    if (!strcmp (name, "eglQueryString"))
        return (void *) fake_query_string;
    if (!strncmp (name, "egl", 3))
        return (void *) fake_zero;
    return NULL;
}

static void *fake_get_dispatch_address (const char *name)
{
    (void) name;
    return NULL;
}

static void fake_set_dispatch_index (const char *name, int index)
{
    (void) name; (void) index;
}

EGLBoolean __egl_Main (uint32_t version, const __EGLapiExports *exports, __EGLvendorInfo *vendor,
                       __EGLapiImports *imports)
{
    (void) exports; (void) vendor;
    if (EGL_VENDOR_ABI_GET_MAJOR_VERSION (version) != EGL_VENDOR_ABI_MAJOR_VERSION)
        return EGL_FALSE;
    fprintf (stderr, "FAKEGPU: vendor library loaded by libglvnd\n");
    imports->getPlatformDisplay = fake_get_platform_display;
    imports->getSupportsAPI = fake_supports_api;
    imports->getVendorString = fake_vendor_string;
    imports->getProcAddress = fake_get_proc_address;
    imports->getDispatchAddress = fake_get_dispatch_address;
    imports->setDispatchIndex = fake_set_dispatch_index;
    return EGL_TRUE;
}
