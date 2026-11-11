/* glcaps.h - the capability structure of the GL driver this process
 * renders with (common/gpucaps.h says what is in it and why).
 *
 * Every context of the daemon comes from one driver, so there is one
 * structure per process. The first render thread fills it when its
 * context is current (walpe_glutil_init calls walpe_gl_caps_init); after
 * that it is only read. Code with an optional fast path asks it:
 *
 *     const WalpeGlCaps *c = walpe_gl_caps ();
 *     if (c && c->buffer_storage) ...          a named feature
 *     walpe_gl_caps_has_ext (c, "GL_...")      any extension
 *     walpe_gl_format (WALPE_FMT_TEX_DXT5)     a format, with its fallback
 *
 * Diagnostics: WALPE_GL_LACKS=<extension>[,<extension>...] treats the
 * named extensions as absent, so a fallback can be exercised on a driver
 * that has everything (WALPE_GL_LACKS=GL_EXT_texture_compression_s3tc
 * decodes DXT textures on the CPU). Unset: no effect. It cannot take away
 * what is part of the OpenGL version in use.
 * WALPE_GL_MAX_TEXTURE=<n> (1024 or more) lowers the largest texture side
 * below the driver's, so the handling of oversized pictures can be seen
 * on a driver with large textures. */
#pragma once
#include "common/api.h"
#include "common/gpucaps.h"

G_BEGIN_DECLS

/* The process's structure; NULL until a context has been initialised.
 * Any thread; the pointer stays valid for the life of the process. */
WALPE_API const WalpeGlCaps *walpe_gl_caps (void);

/* With a GL context current: fills the structure on the first call of
 * the process (later calls return it), logs one line that describes the
 * driver, and - when the driver lacks what WalPE needs to draw at all -
 * one error that names what is missing and the driver. Reads strings and
 * limits only: no GL state is changed and no GL error is left behind. */
const WalpeGlCaps *walpe_gl_caps_init (void);

/* NULL, or the sentence of that error (owned here, stays valid): for the
 * status the user sees */
const char *walpe_gl_caps_error (void);
/* NULL, or why scene wallpapers cannot be drawn with this driver */
const char *walpe_gl_caps_scene_error (void);

/* walpe_gl_caps_format () for the process's driver. A choice that is not
 * the native format is logged once per use. Before a context exists every
 * format reads as native. */
WALPE_API WalpeFmtChoice walpe_gl_format (WalpeFmtUse use);

/* Tests: replace the process's structure by one built from these strings
 * (walpe_gl_caps_parse); all NULL = forget it, the next
 * walpe_gl_caps_init reads the context again. Not thread-safe against
 * render threads. */
void walpe_gl_caps_override (const char *vendor, const char *renderer, const char *version,
                             const char *glsl, const char *extensions);

G_END_DECLS
