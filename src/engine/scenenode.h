/* scenenode.h - renderer for the modern WE scene format ("objects" array) */
#pragma once
#include "content.h"

/* takes ownership of the parsed scene json root (unrefs it on free);
 * project_dir = the dir the scene json was found in (pkg cache or plain) */
WalpeContent *walpe_scenenode_new_from_root (JsonNode *root, const char *project_dir,
                                             const WalpeLoadOpts *opts, GError **err);

#define SCENENODE_MAX_DEPTH 8
#define SCENENODE_MAX_PARENTS 256   /* longest "parent" chain kept (scenenode.c) */

/* WE install assets locator (walpe_we_assets_dir / _hint) */
#include "common/weassets.h"

/* ---- pure parts of the renderer, exposed for tests (no GL, no files) ---- */
#include "common/tex.h"

/* Camera parallax: the pointer followed with `delay` seconds of lag.
 * Positions are 0..1 across the monitor, from the left and from the
 * BOTTOM; the lag starts at rest in the middle. walpe_parallax_follow
 * returns TRUE while pos has not reached the target. */
typedef struct {
    float pos[2], target[2];
    float time, since;
} WalpeParallaxLag;
void walpe_parallax_lag_init (WalpeParallaxLag *l, float delay);
gboolean walpe_parallax_follow (WalpeParallaxLag *l, float tx, float ty, float dt, float delay);
/* the move of a layer, scene units per axis:
 * depth * amount * ((layer - camera, if offset) + (0.5 - pos) * scene size * influence);
 * a result that is no number is no move */
void walpe_parallax_shift (const float depth[2], float amount, float influence,
                           const float pos[2], const float layer[2], const float camera[2],
                           const float scene[2], gboolean offset, float out[2]);

/* Animated (sprite sheet) texture of an image layer: the frame table as it
 * is played (scenenode.c). The table is the wallpaper's and is checked
 * against the sheet: see walpe_sprite_anim_new. */
typedef struct WalpeSpriteAnim WalpeSpriteAnim;
WalpeSpriteAnim *walpe_sprite_anim_new (const WalpeTexFrame *frames, int nframes, int sheet_w,
                                        int sheet_h, char **why);
void walpe_sprite_anim_free (WalpeSpriteAnim *sp);
int walpe_sprite_anim_step (WalpeSpriteAnim *sp, double dt);   /* -> frame on show */
gboolean walpe_sprite_anim_control (WalpeSpriteAnim *sp, const char *what, double v, double now);
double walpe_sprite_anim_query (const WalpeSpriteAnim *sp, const char *what);
double walpe_sprite_anim_next_change (const WalpeSpriteAnim *sp);
void walpe_sprite_anim_frame_size (const WalpeSpriteAnim *sp, int *w, int *h);
void walpe_sprite_anim_uv (const WalpeSpriteAnim *sp, int frame, float uv[6]);
