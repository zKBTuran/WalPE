/* weparticles.h - Wallpaper Engine particle systems (node-graph scenes).
 *
 * Implements the WE particle JSON (particles/<name>.json referenced by a scene
 * object's "particle"): emitters (boxrandom, sphererandom), the common
 * initializers and operators, sprite / spritetrail renderers, the
 * particle material (texture + blending) incl. animated TEXS sprite
 * sheets (animationmode sequence / randomframe / once), child systems
 * (static, eventfollow, eventspawn, eventdeath) and the scene object's
 * "instanceoverride" multipliers. Simulation runs on the render thread in
 * the system's local space (WE scene space is Y-up); the caller passes the
 * object's scene->clip matrix. GL state is per render thread. */
#pragma once
#include <glib.h>
#include <json-glib/json-glib.h>

typedef struct WalpeWeParticles WalpeWeParticles;

/* scene object "instanceoverride" factors (1 = unchanged). Wallpaper Engine
 * defines them for the instance (lib.sceneScript.d.ts,
 * IParticleSystemInstance): count = "multiplier of particle being emitted",
 * rate = "playback rate simulation and emission" (the system's clock:
 * emission, movement and ageing), size / lifetime / alpha / speed / colour
 * = what a particle is given when it is emitted. */
typedef struct {
    float rate, count, size, lifetime, alpha, speed;
    float color[3];
    float cp[8][3];   /* "controlpointN": control point N's position */
    guint cp_set;     /* bit N: cp[N] given */
} WalpeWeParticleOverride;

void walpe_we_particle_override_default (WalpeWeParticleOverride *ov);
/* What "count" and "rate" of the scene file do. 1: what Wallpaper Engine
 * defines (above); a 0 is a value (nothing emitted / the system stands
 * still). 0: version 0.12's reading - count moves only the cap of live
 * particles (maxcount * count), rate multiplies the emission, a 0 means
 * "not set". Values a script drives have Wallpaper Engine's meaning in
 * both cases. */
#define WALPE_WE_STATIC_OVERRIDES_AS_WE 1
/* The cap of live particles under a count override. 0: the particle file's
 * maxcount, whatever count is - Wallpaper Engine documents count as the
 * "factor to control emission rate" and Max Count as what "controls the
 * maximum number of particles allowed to be spawned" and "directly controls
 * how much memory is allocated by the particles". 1: maxcount x count
 * (version 0.12 and early 0.12.1 builds). Version 0.12's reading of count
 * (WALPE_WE_STATIC_OVERRIDES_AS_WE 0) always scales the cap: count is
 * nothing else there. WALPE_PT_CAP=count in the environment selects 1 at
 * run time (weparticles.c, pt_cap_count): a switch for comparing the two
 * readings on a real screen, like WALPE_PT_SIZE / _CAMERA / _WORLD.
 * Child systems that follow events (eventfollow / eventspawn / eventdeath)
 * are a different matter: Wallpaper Engine runs one instance of the child
 * per parent particle, each capped by the child file's maxcount, and the
 * child reference's "maxcount" limits the instances. Here the instances
 * share one system whose pool is file maxcount x instances. */
#define WALPE_WE_CAP_FOLLOWS_COUNT 0

/* SceneScript (IParticleSystemInstance, scripts bound to an
 * "instanceoverride" value): the object's instance overrides changed while
 * the system runs. scripted = the WALPE_WE_OV_* bits of the values a script
 * drives. Taken up by the next simulation step: count and rate at once,
 * size / lifetime / alpha / speed / colour by the particles emitted from
 * then on, control points with their cp_set bit at once. Live particles
 * end on their own. Child systems follow as at creation. */
#define WALPE_WE_OV_COUNT 1u
#define WALPE_WE_OV_RATE 2u
void walpe_weparticles_set_override (WalpeWeParticles *p, const WalpeWeParticleOverride *ov, guint scripted);
/* no GL (the unit test's entries): one simulation step of dt seconds of the
 * frame clock (walpe_weparticles_step, declared with the render calls
 * below: without GL objects it steps the bare system); position, velocity
 * and age of the i-th live particle of the root system (FALSE: there is
 * none) */
gboolean walpe_weparticles_peek (WalpeWeParticles *p, int i, float pos[3], float vel[3], float *age);
/* the same for system number `sys` of the object, counted root first, then
 * each child with its own children (0 = the root: walpe_weparticles_peek);
 * walpe_weparticles_sys_count: its live particles, -1 = no such system */
gboolean walpe_weparticles_sys_peek (WalpeWeParticles *p, int sys, int i, float pos[3], float vel[3],
                                     float *age);
int walpe_weparticles_sys_count (WalpeWeParticles *p, int sys);
/* alpha the i-th live particle of that system is drawn with after its
 * operators (fades, oscillation); -1 = none */
float walpe_weparticles_sys_alpha (WalpeWeParticles *p, int sys, int i);

/* sys = parsed particle json object; dir = project dir (textures, child
 * systems); we_assets = WE install assets dir or NULL. No GL. NULL when
 * the json is not a WE particle system. */
WalpeWeParticles *walpe_weparticles_new (JsonObject *sys, const char *dir, const char *we_assets,
                                         const WalpeWeParticleOverride *ov);
/* render thread: textures + buffers; pre-simulates "starttime" */
void walpe_weparticles_gl_init (WalpeWeParticles *p);
/* render thread: advance by dt seconds (clamped) and draw with mvp
 * (column-major 3x3, system local space -> clip); alpha/tint = object's */
void walpe_weparticles_render (WalpeWeParticles *p, const float *mvp, double dt,
                               float alpha, const float tint[3]);
/* render thread: the simulation half of render alone (advance by dt, after
 * the set_* calls of the frame). A render call that follows only draws;
 * the caller may also leave it out when walpe_weparticles_count() is 0
 * (nothing would be drawn). */
void walpe_weparticles_step (WalpeWeParticles *p, double dt);
/* audio-reactive emission (emitters with audioprocessingmode): 0..1 level */
void walpe_weparticles_set_audio_level (WalpeWeParticles *p, float level);
/* the object's world transform (column-major mat3): refraction tangents
 * and the projection of perspective systems */
void walpe_weparticles_set_world (WalpeWeParticles *p, const float *m);
/* per frame, for systems with the "perspective rendering" flag: scene ->
 * clip matrix (column-major mat3), the view centre in scene units, the
 * object's origin z / scale z (depth of its particles) and the distance
 * of the perspective camera from the z = 0 plane in scene units:
 * (scene height / 2) / tan (fov / 2), fov = the scene's general.fov
 * (WE default 50 degrees), which makes z = 0 match the orthographic
 * scene. <= 1 keeps the last distance. */
void walpe_weparticles_set_view (WalpeWeParticles *p, const float *scene_to_clip, float cx,
                                 float cy, float origin_z, float scale_z, float persp_dist);
/* some control point is linked to the mouse (forces at the cursor): the
 * owner passes the cursor with set_cursor when it has one */
gboolean walpe_weparticles_uses_cursor (WalpeWeParticles *p);
/* some system of it refracts: its sprites sample the frame drawn so far
 * at displaced places (what lies under them must be complete: clip.h) */
gboolean walpe_weparticles_reads_frame (WalpeWeParticles *p);
/* SceneScript play()/pause()/stop(): mode 1 / 0 / -1 */
void walpe_weparticles_set_playing (WalpeWeParticles *p, int mode);
gboolean walpe_weparticles_is_playing (WalpeWeParticles *p);
/* control point 0 linked to the mouse: emit at the cursor, given in the
 * system's local space (before the first call: at the system origin) */
gboolean walpe_weparticles_follows_cursor (WalpeWeParticles *p);
void walpe_weparticles_set_cursor (WalpeWeParticles *p, float x, float y);
gboolean walpe_weparticles_uses_audio (WalpeWeParticles *p);
int walpe_weparticles_count (WalpeWeParticles *p); /* live particles (diagnostics) */
/* systems of the object, child systems included (at most 128) */
int walpe_weparticles_systems (WalpeWeParticles *p);
/* sprite sheets (no GL; also the unit test's entry): which two frames of
 * an n-frame sheet a particle shows at life fraction `life` and the blend
 * between them. mode 0 = sequence, 1 = randomframe (random_frame is the
 * particle's frame), 2 = once; blend FALSE = "no frame blending". */
float walpe_weparticles_sheet_frames (int n, int mode, float seqmult, float life,
                                      int random_frame, gboolean blend, int *f0, int *f1);
/* how smooth a sprite picture is (no GL; the unit test's entry): the second
 * differences of a w x h block of premultiplied RGBA (`stride` bytes between
 * rows), counted by size into hist[0] (along x, 1 texel apart), hist[1]
 * (x, 2 texels), hist[2] (y, 1) and hist[3] (y, 2), added to what is there */
void walpe_weparticles_soft_hist (const guint8 *px, gsize stride, int w, int h, guint (*hist)[511]);
/* ... and how often that was done since the process started: sprite files
 * decoded and measured / systems that took the measurement an earlier
 * system made of the same, unchanged file (walpe_weparticles_gl_init) */
void walpe_weparticles_soft_stats (guint *measured, guint *kept);
/* The reduced-resolution layer (render thread). Large soft sprites - fog,
 * smoke, glows, light shafts - can be drawn into a buffer of 1/2 or 1/4 of
 * the view and composited once: a fraction of the blending, the same
 * picture to within a step of an 8-bit channel (weparticles.c, lr_decide).
 * The caller tells where it draws and promises to flush:
 *   begin_frame (framebuffer, width, height)  once per frame, before the
 *       first walpe_weparticles_render;
 *   flush ()      before it draws anything else to that framebuffer
 *       (several particle objects in a row share one composite);
 *   end_frame ()  after the last object.
 * Without begin_frame every system is drawn directly, as before.
 * drop_targets () frees the buffer (a paused wallpaper). */
void walpe_weparticles_begin_frame (guint target_fbo, int w, int h);
void walpe_weparticles_flush (void);
void walpe_weparticles_end_frame (void);
void walpe_weparticles_drop_targets (void);
/* user settings, any thread, effective from the next frame:
 * "particle_lowres" - which systems may use the layer: OFF none, AUTO
 * (default) those whose picture stays the same, ALL also everything drawn
 * large (visibly softer); "particle_density" - the percentage of its
 * particles every system emits (100 = as authored; 5..100). The
 * environment overrides both: WALPE_PT_LOWRES=off|auto|all,
 * WALPE_PT_DENSITY=<percent>. */
enum { WALPE_PT_LOWRES_OFF = 0, WALPE_PT_LOWRES_AUTO = 1, WALPE_PT_LOWRES_ALL = 2 };
void walpe_weparticles_set_lowres (int mode);
void walpe_weparticles_set_density (int percent);
/* sprites as instanced quads (default) or, FALSE, as six full vertices
 * each like before version 0.12's GPU work: the same picture from 9x the
 * vertex data. A reference for tests and a way around a driver that
 * mishandles instancing (config "particle_instancing"); WALPE_PT_INSTANCED=0
 * / =1 in the environment overrides it. */
void walpe_weparticles_set_instanced (gboolean on);
/* what is in force (the environment's overrides applied) */
void walpe_weparticles_settings_get (int *lowres, int *density, gboolean *instanced);
/* A refracting system reads a copy of the part of the frame it can reach
 * (its quads' bounds grown by how far each one's normal map can move a
 * sample); within a frame only what the copy does not hold yet, or what was
 * drawn on since, is copied. TRUE: every such system copies the whole
 * frame, the same picture from more pixels copied - a reference for tests;
 * WALPE_PT_REFRACT_COPY=whole in the environment does the same (=each: the
 * part it can reach, every time). */
void walpe_weparticles_set_refract_whole (gboolean on);
/* Counts the changes of the three settings above. They change what a
 * scene's particle objects draw while nothing in the scene changed: a
 * renderer that skips unchanged frames puts this number into what it
 * compares (frame signature / picture serial), read on its own thread. */
guint walpe_weparticles_settings_serial (void);

/* diagnostics: what the particle systems of this render thread cost since
 * the last reset. Collected once WALPE_PT_STATS is set in the environment
 * (scenes then log a summary every 150 frames; WALPE_PT_STATS=2 adds one
 * line per system when its wallpaper is unloaded) or after the first call
 * here. "area" is in screen areas: 1.0 = every pixel of the view once. */
typedef struct {
    guint renders;         /* walpe_weparticles_render calls */
    guint systems;         /* systems (children included) the simulation visited */
    guint sim_skipped;     /* ... of which had nothing to do (no step) */
    guint live;            /* live particles after the step */
    guint drawn;           /* particles / ribbon segments submitted */
    guint culled;          /* sprites left out: wholly outside the view (and
                            * trails without area: particles that never moved) */
#define WALPE_PT_STATS_CULLED 1
    guint draws;           /* draw calls */
    guint uploads;         /* vertex buffer uploads */
    guint state_changes;   /* blend function / texture / program state set */
    guint lowres_systems;  /* systems drawn into the reduced-resolution layer */
    guint composites;      /* ... and how often the layer went onto the frame */
    guint copies;          /* copies of the frame for refracting systems */
    double copied;         /* ... their area, screen areas */
    guint64 verts;         /* vertices the GPU runs the vertex shader for */
    guint64 bytes;         /* bytes uploaded */
    double area;           /* the quads' on-screen area clipped to the view */
    double shaded;         /* what is rasterized for it, in full-resolution
                            * screen areas (reduced buffer + its composite) */
    guint64 samples;       /* GL_SAMPLES_PASSED of the draws (desktop GL) */
    /* thread CPU time: simulation; building the geometry; the whole draw
     * step (geometry + GL calls: on a software GL that includes the
     * vertex shader and the rasterizer's setup) */
    double sim_ms, build_ms, draw_ms;
    guint trig;            /* sprites whose cos / sin of the rotation had to be
                            * computed (in a system whose sprites never turn:
                            * once in a sprite's life, not once a frame) */
} WalpePtStats;
void walpe_weparticles_stats (WalpePtStats *out, gboolean reset);
/* render thread (GL objects) */
void walpe_weparticles_free (WalpeWeParticles *p);
