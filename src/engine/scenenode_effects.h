/* scenenode_effects.h - Tier 2 effect pass-graph executor (node graphs) */
#pragma once
#include <glib.h>
#include <json-glib/json-glib.h>
#include "common/audio.h"

/* build an executor from an object's "effects" array (project dir for
 * asset resolution; user_props = {propid: value} for constants bound to
 * user properties, may be NULL). NULL when nothing usable parsed. */
gpointer fx_build_for_object (const char *dir, JsonArray *effects, const char *dbgname,
                              JsonObject *user_props);
void fx_free_for_object (gpointer fxset);
/* run the chains over `input` (w/h its image size, fullw/fullh the input's
 * padded texture size); returns the texture to draw. k (0..8) = working
 * resolution factor of the render targets (1 = full size); shaders still
 * see the full sizes */
guint fx_run_for_object (gpointer fxset, guint input, int w, int h,
                         int fullw, int fullh, double time, float k, gboolean input_static);
/* the layer's colour (rgb, straight; NULL / white = none), applied to the
 * input before the first effect by the next fx_run_for_object calls - the
 * layer's own material pass in WE. When the returned texture is not the
 * input, the caller must not tint again. */
void fx_set_input_tint (gpointer fxset, const float *rgb);
/* the layer's opacity, after fx_set_input_tint and like it applied to the
 * input before the first effect (Wallpaper Engine's order). TRUE: the set
 * takes it, draw the effects' result with opacity 1; FALSE
 * (WALPE_FX_ALPHA=after): the opacity stays with the draw of the result */
gboolean fx_set_input_alpha (gpointer fxset, float alpha);
/* The input is a sprite sheet and the layer shows this part of it: uv =
 * the frame's origin, x axis and y axis in texture coordinates of the
 * input (a point (u, v) of the frame, v = 0 its first row, is origin + u *
 * x axis + v * y axis). The layer's own material pass in front of the
 * first effect - the one that applies fx_set_input_tint / _alpha - then
 * reads that part and always runs when an effect runs, so the effects see
 * a picture of the frame's size (pass that size as w, h, fullw, fullh).
 * Kept until changed; NULL = the whole input (the default). When the
 * frame changes under the same texture the caller says so with
 * fx_invalidate (), as for any change of the input's picture. A run in
 * which no effect draws hands the input back: the caller then shows the
 * frame itself. */
void fx_set_input_frame (gpointer fxset, const float uv[6]);
/* static chain cache: input_static = the input texture's pixels only change
 * with its name (no video, album art, framebuffer copy). A set whose every
 * chain is proven time/audio/pointer/feedback-free then reuses its last
 * output while input, sizes, k, screen size and its constants/visibility
 * (fx_set_constant / fx_set_enabled bump a generation) are unchanged.
 * fx_invalidate: the input's pixels changed under the same name. */
void fx_invalidate (gpointer fxset);
/* every chain of the set is proven static (see fx_run_for_object) */
gboolean fx_is_static (gpointer fxset);
/* Effect render targets (render thread). Targets are lent from a pool per
 * run; a chain owns only what must outlive the frame (feedback targets,
 * the output a static cache keeps).
 * fx_targets_release: free the pool of this thread (scene freed or paused,
 * a scene that will not draw again). The textures returned by earlier
 * fx_run_for_object calls are gone after it.
 * fx_free_targets_for_object: free what a set owns and can render again
 * (cached outputs; not feedback targets) - the scene is paused.
 * fx_targets_stats: bytes / number of effect targets alive in this thread. */
void fx_targets_release (void);
void fx_free_targets_for_object (gpointer fxset);
void fx_targets_stats (gint64 *bytes, int *count);
/* Compose captures (render thread): a w x h target to copy the frame into,
 * lent from the pool until fx_capture_put() (at the latest the next
 * fx_set_frame_info / fx_targets_release); returns the handle for
 * fx_capture_put. *opaque = the target has no alpha channel: what is
 * sampled from it has alpha 1, the caller need not write alpha itself. */
int fx_capture_get (int w, int h, guint *fbo, guint *tex, gboolean *opaque);
void fx_capture_put (int handle);
/* render thread: the framebuffer the scene is drawn into (0 = the window,
 * the default); fx_run_for_object leaves it bound */
void fx_set_scene_target (guint fbo);
/* skipping the set's run while its layer is off screen changes nothing
 * later: no pass keeps a picture from the frame before (feedback targets,
 * copy passes) and no transform effect moves the layer on screen */
gboolean fx_cullable (gpointer fxset);
/* The share (0..1) of the layer over which the first effect that runs
 * reads the layer's own texture at displaced coordinates: 0 = it reads it
 * texel for texel (tint, opacity, blend, colour key ...), 1 = everywhere
 * (chromatic aberration, a blur, a shake without a mask), in between = as
 * far as the masks painted for it reach (worked out once per set: one
 * small read-back per mask; render thread). Such lookups land between the
 * texels: a texture stored at half size then gives a softer picture than
 * the full one there, where a straight read gives the same. */
float fx_input_displaced (gpointer fxset);
/* ... and what that costs: the share (0..1) of the layer's picture that
 * the first effect reads at displaced coordinates and that would come out
 * more than 16 / 255 different if `tex` - the layer's texture at full size,
 * res = (stored w, stored h, image w, image h) - were stored `levels`
 * halvings smaller. Measured on the GL thread from a 251 x 251 sampling
 * (a temporary copy of the texture at the reduced size, a few small
 * read-backs); 1 = cannot tell. */
float fx_input_soft_cost (gpointer fxset, guint tex, const int res[4], int levels);
/* config fx_cache; WALPE_FX_CACHE=0|1 overrides (default on) */
void fx_set_cache_enabled (gboolean on);
/* Pass programs (fx_specialize.h). By default one program per pass, built
 * exactly as in version 0.12. With WALPE_FX_SHARE=1, and with either setting
 * below, passes whose shader text is the same share one GL program per
 * render thread; the text and the picture are those of one program per
 * pass, the price is more uniform calls per frame.
 * fx_set_specialize: config fx_specialize, WALPE_FX_SPEC=0|1 overrides
 * (default off): material constants that cannot change are compiled into
 * the pass program. A specialised program is only used once it has drawn
 * what the generic one draws (fx_verify.h; WALPE_FX_SPEC_VERIFY=0 skips
 * the comparison).
 * fx_set_shader_quality: config fx_quality, WALPE_FX_QUALITY overrides: "exact"
 * (default) keeps the picture of the generic programs up to float
 * rounding, "fast" also uses cheaper shader variants that may differ by
 * more (it specialises as well, whatever fx_specialize says). Both apply
 * to wallpapers loaded afterwards. */
void fx_set_specialize (gboolean on);
void fx_set_shader_quality (const char *quality);
/* The language effect shaders are built in. By default it follows the GL
 * context: GLSL 3.30 on desktop OpenGL, GLSL ES 3.00 / 3.10 / 3.20 on
 * OpenGL ES (fx_essl.h). Tests and tools: 0, 300, 310 or 320 forces that
 * language whatever the context is (the sources can be built and inspected
 * without one); any other value follows the context again. */
void fx_set_glsl_es (int version);
/* programs compiled / passes that reused a program / relinks after a
 * script changed a baked constant / specialised builds that fell back to
 * the generic program / constants baked / identity passes, since the
 * last reset (diagnostics; any pointer may be NULL) */
void fx_program_stats (int *compiled, int *shared, int *respec, int *fallback, int *baked,
                       int *identity, gboolean reset);
/* specialised programs that were compared with the generic one behind a
 * pass's first draw: drew the same (in use since) / did not (the pass
 * keeps the generic program). Diagnostics; cleared by fx_program_stats'
 * reset. */
void fx_verify_stats (int *verified, int *rejected);
/* Bumped whenever something the layer state does not show changed what
 * the effects draw or whether a chain is static: fx_set_specialize /
 * fx_set_shader_quality with a new value, a pass that changed programs (the
 * specialised one took over behind the first draw; a script wrote a baked
 * constant), a run of effects that is fused or no longer is. A renderer
 * that skips unchanged frames or keeps static layers baked must make this
 * part of its frame signature. Any thread. */
guint fx_picture_serial (void);
/* runs of displacement effects fused into one pass / the effects in them
 * (fx_quality = fast; diagnostics) */
void fx_fuse_stats (int *groups, int *members);
/* render thread: effect passes drawn / served from the cache since the
 * last reset */
void fx_pass_stats (guint *run, guint *cached, gboolean reset);
gboolean fx_has_effects (gpointer fxset);
/* What the picture of a set depends on besides its input texture, its
 * constants / visibility and the sizes: the union over its enabled
 * effects. TIME: g_Time / g_Frametime; DAYTIME: g_Daytime; SYSTEX: media
 * album art; FEEDBACK: its own previous frame (all four: a new picture
 * every frame, as far as anyone can tell); AUDIO / POINTER: values that
 * may stand still; FRAME: the scene below (_rt_FullFrameBuffer, compose
 * layers). 0 = static. */
enum { FX_DYN_TIME = 1, FX_DYN_AUDIO = 2, FX_DYN_POINTER = 4, FX_DYN_DAYTIME = 8,
       FX_DYN_SYSTEX = 16, FX_DYN_FRAME = 32, FX_DYN_FEEDBACK = 64 };
guint fx_dynamic_inputs (gpointer fxset);
/* render thread, after fx_set_frame_info / fx_set_audio_frame: a value
 * that is the same in two frames exactly when the set would turn the same
 * input into the same picture in both (its generation and this frame's
 * audio / pointer values, as far as it reads them). Sets *vol when that
 * cannot be told (TIME, DAYTIME, SYSTEX, FEEDBACK): the picture must be
 * taken as new every frame. 0 for no effects. */
guint64 fx_frame_signature (gpointer fxset, gboolean *vol);
/* some pass reads g_PointerPosition / g_ParallaxPosition */
gboolean fx_uses_pointer (gpointer fxset);
/* render thread: texture holding the scene drawn so far, bound where an
 * effect samples _rt_FullFrameBuffer (0 = none: the effect input) */
void fx_set_fullframe (guint tex, int w, int h);
/* render thread, right before fx_run_for_object(): the layer's own quad
 * in layer units (scene pixels, y up): centre and size. Passes whose
 * vertex shader works on a_Position ("Vertex" mode of transform / skew /
 * foliage sway) are drawn with it, as in WE. fold = TRUE: the caller
 * draws the result with fx_layer_affine() applied; trailing transform
 * effects in Vertex mode are then not run as passes (WE draws the last
 * effect straight into the scene, where it moves the layer unclipped).
 * Applies to the next run only. */
void fx_set_layer_geometry (float cx, float cy, float w, float h, gboolean fold);
/* render thread, right before fx_run_for_object(): the part of that run's
 * result its caller is going to sample - up to 6 rectangles (u0, v0, u1,
 * v1) in the result's texture coordinates (u, v = 0, 0 is the first texel
 * of the first row). The run may then leave every other pixel of the
 * result undefined (fx_region.h: passes are restricted to what is read
 * later; same values where it is read). Consumed by that one run; without
 * it the whole result is produced. WALPE_FX_REGION=0 /
 * fx_set_region_enabled (FALSE): never restricted (A/B, tests). */
void fx_set_need (const float (*rects)[4], int n);
void fx_set_region_enabled (gboolean on);
/* this thread's effect passes: pixels of their targets / pixels shaded,
 * passes drawn restricted, rectangles drawn for them */
void fx_region_stats (guint64 *px_full, guint64 *px_drawn, guint *passes, guint *rects,
                      gboolean reset);
/* diagnostics: target pixels of the passes of the runs whose caller named
 * rectangles, and the part of them the rectangles are (the least those
 * passes could shade); reset with fx_region_stats */
void fx_region_potential (double *asked, double *floor_px);
/* WALPE_FX_REGION_CHECK=n / fx_set_region_check (n): every nth run with
 * restricted passes is drawn again whole and compared where it is read
 * (0 = off, the default); runs compared and runs that differed so far */
void fx_set_region_check (int every);
void fx_region_check_stats (guint *runs, guint *bad);
/* kept passes (see the .c): a pass that reads nothing that changes from
 * frame to frame is drawn once and its target used again. On by default;
 * WALPE_FX_STATIC_PASS=0 / FALSE = drawn every frame (A/B, tests). Such a
 * pass counts as "cached" in fx_pass_stats (). */
void fx_set_static_pass_enabled (gboolean on);

/* "Skip hidden areas": right before fx_run_for_object(), the parts of its
 * result the caller will show: n rectangles u0, v0, u1, v1 of the returned
 * texture (v = 0 = its row 0); n = 0 = none of it. The run then fills
 * only what those parts need (fx_clip.h): outside them the returned
 * texture holds anything. Not called = all of it. Applies to the next run
 * only. */
void fx_set_clip_uv (const float *uv, int n);
/* the affine map the last run left to the caller (see the .c); FALSE =
 * none. Goes between the layer's world transform and its quad. */
gboolean fx_layer_affine (gpointer fxset, float m[6]);
/* enabled effects of the set whose chain could not be built */
int fx_failed_count (gpointer fxset);
/* workshop id of the wallpaper whose effects are built next (WE zcompat
 * shader replacements apply up to a per-shader maximum project id) */
void fx_set_project_id (guint64 id);
/* Between these two calls (the load of one scene, on the thread that
 * builds its effects) the work a pass does on its shader text alone is
 * done once per text: most passes of a scene are a text an earlier pass
 * of it had. Calls nest; nothing is kept past the last _end. stats: texts
 * worked on / results taken over since the process started. */
void fx_text_memo_begin (void);
void fx_text_memo_end (void);
void fx_text_memo_stats (guint *computed, guint *reused);
/* effect chains built / failed since the last reset (diagnostics) */
void fx_stats (int *ok, int *failed, gboolean reset);
/* `walpe-ctl mem` (memreport.h), render thread, when asked: what the effects
 * of this thread hold. fx_mem_stats fills the thread's own tables (targets,
 * the target pool, the program and source caches of fx_specialize.h);
 * fx_set_mem_stats ADDS one layer's set to the chain / pass fields. */
typedef struct {
    int targets;              /* effect render targets alive in this thread ... */
    gint64 target_bytes;
    int pool, pool_lent;      /* ... of them in the pool: entries, lent right now */
    gint64 pool_bytes;
    int pool_peak;            /* the most the pool ever held */
    gint64 pool_peak_bytes;
    int programs, sources;    /* shared pass programs / generic source texts kept */
    gint64 source_bytes;
    int prog_users, rt_formats; /* entries of the two per-thread lookup tables */
    /* per set (fx_set_mem_stats) */
    int sets, chains, passes;
    int pass_programs;        /* passes that hold a GL program */
    int generic_kept;         /* specialised passes that also keep their generic program */
    int candidates;           /* specialised programs waiting to be compared */
    gint64 pass_text_bytes;   /* shader texts the passes keep in main memory */
    int own_targets;          /* targets a chain owns (feedback, kept passes, cached outputs) */
    int sims;                 /* chains that carry a simulation */
} FxMemStats;
void fx_mem_stats (FxMemStats *out);
void fx_set_mem_stats (gpointer fxset, FxMemStats *out);
/* any pass reads g_AudioSpectrum* (the scene then captures audio) */
gboolean fx_uses_audio (gpointer fxset);
/* render thread: spectrum for the passes of the current frame (NULL = 0) */
void fx_set_audio_frame (const WalpeAudioSpectrum *spec);
/* render thread: screen size (g_Screen), pointer in UV (g_PointerPosition,
 * g_ParallaxPosition) and frame delta (g_Frametime) for this frame */
void fx_set_frame_info (int screen_w, int screen_h, float pointer_x, float pointer_y,
                        float frametime);
/* render thread: media integration album art ($mediaThumbnail /
 * $mediaPreviousThumbnail usertextures) for this frame; 0 = none */
void fx_set_system_textures (guint thumb, int tw, int th, guint prev, int pw, int ph);
gboolean fx_uses_system_textures (gpointer fxset);
/* SceneScript access by effect index (position in the object's "effects"
 * array) / custom name; constants by constantshadervalues key; pass -1 =
 * every pass (set) / the first pass that has the key (get) */
int fx_find_effect (gpointer fxset, const char *name);
int fx_effect_count (gpointer fxset);
const char *fx_effect_name (gpointer fxset, int inst);
int fx_get_enabled (gpointer fxset, int inst);          /* -1 = no such effect */
void fx_set_enabled (gpointer fxset, int inst, gboolean on);
int fx_get_constant (gpointer fxset, int inst, int pass, const char *key, float v[4]);
gboolean fx_set_constant (gpointer fxset, int inst, int pass, const char *key,
                          const float *v, int n);

/* ---- quality knobs (common/quality.h), render thread -----------------
 * fx_set_quality: before a frame's layers. rate_div = animated effect sets
 * over a static input are redrawn every Nth frame (1 = every frame, the
 * default); rate_all = every such set, else only the ones classified slow
 * (fxclass.h); post = WALPE_POST_*: "reduced" runs the inner buffers of
 * blur and bloom chains at half size, "off" also leaves purely cosmetic
 * chains (bloom, film grain, vignette, chromatic aberration) out of
 * compose layers; fps = the scene's frame rate (what "slow" is measured
 * against); frame = the scene's frame counter. Changing post needs
 * fx_invalidate () on every set. */
void fx_set_quality (int rate_div, gboolean rate_all, int post, double fps, guint frame);
/* the runs until the next call are a compose layer's (TRUE) / not (FALSE) */
void fx_set_run_compose (gboolean on);
/* For a frame signature (a renderer that does not draw a frame equal to
 * the last one). After fx_set_quality () of the frame: TRUE = the update
 * divisor governs this set, it has a held output, and *epoch names the
 * redraw the frame about to be drawn will show - the same number in two
 * frames exactly when the set shows the same picture in both, whatever
 * time, audio or pointer say (provided its input, size and constants are
 * what they were, which the caller's signature has to cover anyway).
 * FALSE = no statement: treat the set as without the divisor (driven by
 * time = a new picture every frame). A set with a held prefix only is
 * never governed: its tail is drawn every frame. Always FALSE with the
 * divisor at 1. */
gboolean fx_set_hold_epoch (gpointer fxset, guint *epoch);
/* quality knob fx_res, for the next fx_run_for_object only: k_full = the
 * working scale the run has with the knob at "full" (what its cost at
 * High is counted with), keep[l] = the share of those pixels the levels
 * "screen", "reduced", "half" leave */
void fx_set_run_res (float k_full, const float keep[3]);
/* post-processing "off" leaves no chain of this compose layer's set: the
 * layer need not be captured or drawn */
gboolean fx_post_dropped (gpointer fxset);
/* accounting since the last fx_acct_take: effect pass pixels / passes
 * drawn (now), what the same frames cost with every knob at its default
 * (nom), and of that: in sets an update divisor may hold ([0] slow /2,
 * [1] slow /3, [2] all /2, [3] all /3), taken off by post-processing
 * "reduced" / "off" and by effect resolution "screen" / "reduced" / "half"
 * (px_fxres) */
typedef struct {
    guint64 px_now, px_nom, px_hold[4], px_post_red, px_post_off, px_fxres[3];
    guint passes_now, passes_nom, passes_hold[4], passes_post_off;
} FxAcct;
void fx_acct_take (FxAcct *out);
/* a compose layer's own draw of draw_px pixels (after its run), or the
 * whole layer left out (dropped: fx_post_dropped; vw, vh, k = what the
 * run would have got) */
void fx_acct_compose (gpointer fxset, int vw, int vh, float k, guint64 draw_px, gboolean dropped);
/* passes not drawn because their set was held (update divisor) */
void fx_pass_stats_held (guint *held, gboolean reset);
/* one line per chain: effect, static / animated, time rate, family; and
 * the divisors the set is granted (diagnostics, `walpe-ctl quality`) */
void fx_quality_describe (gpointer fxset, const char *layer, GString *out);

/* WALPE_PROP_PROBE digest of a layer's effects (propprobe.h) */
void fx_probe_dump (gpointer fxset, JsonBuilder *b);

/* ---- simulation effects (fx_sim.h) --------------------------------------
 * An effect whose pass list has a "swap" command carries fields from frame
 * to frame (Wallpaper Engine's Fluid Simulation: velocity, pressure, dye).
 * Such a chain owns its targets, advances in steps of a fixed length by
 * scene time - the same fluid at 30, 60 and 144 frames per second - and is
 * never cached, held, kept or drawn in part.
 * fx_sim_set_enabled: config fluid_sim (default on; WALPE_FLUID=0|1
 * overrides). Off: such chains are left out like an effect switched
 * invisible, their targets freed. Any thread; running wallpapers follow
 * with their next frame.
 * fx_sim_set_quality: config fluid_quality, "full" (default) | "reduced" |
 * "low" (common/fluidsim.h; WALPE_FLUID_QUALITY overrides). FALSE = not a
 * level's name, nothing changed. Any thread; live. */
void fx_sim_set_enabled (gboolean on);
gboolean fx_sim_enabled (void);
gboolean fx_sim_set_quality (const char *quality);
const char *fx_sim_quality (void);
/* render thread, once per frame (any time before the frame's runs; the
 * values stay until the next call): is there a pointer for this frame's
 * picture at all - FALSE while it is on another monitor or, on Wayland,
 * not over the wallpaper - and is its left button held. A simulation gets
 * no push from a pointer that is not there and none along the way from
 * where it left to where it comes back. Not called: present, not held. */
void fx_set_pointer_state (gboolean present, gboolean down);
/* render thread, right before fx_run_for_object () of a set with a
 * simulation (fx_has_simulation): the 3x3 column-major transform the
 * run's result is drawn with into the frame (the layer's +-1 quad to clip
 * space). The pointer is taken through its inverse, so the fluid is pushed
 * under the pointer wherever the layer sits. Applies to the next run only;
 * without it the frame stands in for the layer. */
void fx_set_pointer_map (const float *m3);
gboolean fx_has_simulation (gpointer fxset);
/* render thread: the fields of the set's simulations start again from
 * their clear values (as after a load) */
void fx_sim_reset (gpointer fxset);
/* render thread: run a "functions" entry of a simulation's effect file
 * (the stock effect has clearVelocity and clearDye: named clears of some
 * targets); inst = position in the effects array, -1 = any. FALSE = no
 * such function. */
gboolean fx_sim_call (gpointer fxset, int inst, const char *function);
/* a set's simulations: how many (off = left out by the switch or for lack
 * of float targets), steps run and resets so far, their targets and bytes,
 * the grid and the picture-sized fields, the step rate in force */
typedef struct {
    int chains, off, targets;
    guint64 steps;
    guint resets;
    gint64 bytes;
    int grid_w, grid_h, field_w, field_h;
    double step_hz;
} FxSimStats;
void fx_sim_stats (gpointer fxset, FxSimStats *out);
/* render thread: simulation steps run / runs that drew only the picture /
 * runs that drew nothing (the last picture still held), since the reset */
void fx_sim_thread_stats (guint64 *steps, guint *presented, guint *reused, gboolean reset);
