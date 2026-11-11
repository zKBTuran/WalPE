/* glfeat.h - which OpenGL API the contexts of this process are, and what
 * the OpenGL ES path does about the things ES lacks (common/glapi.h has
 * the table; this file asks the running context once and keeps the
 * answers).
 *
 * Every context of a process comes from one driver and one API, so there
 * is one record per process. walpe_glutil_init () fills it with the first
 * context that is current; after that it is only read, from any thread.
 *
 * On desktop OpenGL nothing is asked of the context here (no GL call is
 * added to that path): the record only says "desktop OpenGL", and every
 * walpe_gl_feat_how () answer is WALPE_GLF_NATIVE - the code then makes
 * exactly the calls it always made, behind the checks it always had.
 *
 * A build that has a general capability structure for the driver can
 * carry these answers there (API, version, GLSL ES version, the plan);
 * this is the small part the OpenGL ES path needs by itself. */
#pragma once
#include <glib.h>
#include "common/glapi.h"

G_BEGIN_DECLS

/* with a GL context current; idempotent */
void walpe_gl_feat_init (void);
/* FALSE before the first walpe_gl_feat_init () */
gboolean walpe_gl_feat_known (void);
/* the contexts are OpenGL ES (FALSE: desktop OpenGL, or not known yet) */
gboolean walpe_gl_feat_es (void);
/* OpenGL ES version, major * 10 + minor (31, 32); 0 on desktop OpenGL */
int walpe_gl_feat_es_version (void);
/* GLSL ES version shaders are written in: 300 / 310 / 320; 0 = GLSL 3.30
 * (desktop OpenGL, or no context yet) */
int walpe_gl_feat_essl (void);
/* how one item of the table is done here */
WalpeGlfHow walpe_gl_feat_how (WalpeGlFeature f);
/* the whole table (WALPE_GLF_N items, indexed by WalpeGlFeature); on
 * desktop OpenGL every item reads native */
const WalpeGlfItem *walpe_gl_feat_plan (void);

/* Tests: make the record say this instead of asking a context
 * (extensions: names separated by spaces, NULL = none); es = -1 forgets
 * it, the next walpe_gl_feat_init () asks again. Not for use while render
 * threads run. */
void walpe_gl_feat_override (int es, int version, const char *extensions);

G_END_DECLS
