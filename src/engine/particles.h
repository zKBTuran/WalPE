/* particles.h - pragmatic subset of the WE particle system */
#pragma once
#include <json-glib/json-glib.h>

typedef struct WalpeParticles WalpeParticles;

/* systems = scene.json "particles" array. NULL if nothing usable parsed. */
WalpeParticles *walpe_particles_new (JsonArray *systems, const char *project_dir);
gboolean walpe_particles_usable (WalpeParticles *p);
void walpe_particles_gl_init (WalpeParticles *p);
/* draws with the same scene->clip matrix math; px/py = parallax -1..1 */
void walpe_particles_render (WalpeParticles *p, float scene_w, float scene_h,
                             int vw, int vh, float px, float py);
void walpe_particles_free (WalpeParticles *p);
