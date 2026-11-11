/* puppet - Wallpaper Engine puppet-warp image layers (model json "puppet":
 * "models/<name>_puppet.mdl"): a skinned 2D triangle mesh over the layer's
 * texture, bones and keyframed bone animations (MDLV/MDLS/MDLA). Without it
 * the texture is drawn as a flat quad, which for part sheets (head, hair,
 * arms laid out apart and assembled by the animation) shows the raw sheet. */
#pragma once
#include <glib.h>
#include <json-glib/json-glib.h>
#include <epoxy/gl.h>

typedef struct WalpePuppet WalpePuppet;

/* NULL when the file has no usable mesh (the layer stays a flat quad) */
WalpePuppet *walpe_puppet_load (const char *path);
void walpe_puppet_free (WalpePuppet *p);   /* GL objects: walpe_puppet_gl_free first */
void walpe_puppet_gl_free (WalpePuppet *p);

/* the scene object's "animationlayers" ({animation, rate, blend, visible,
 * additive}); user_props resolves {"user": ...} bound visibility */
void walpe_puppet_set_layers (WalpePuppet *p, JsonArray *layers, JsonObject *user_props);

/* skin the mesh for time t (s) and draw it with the quad shader: mvp maps
 * the layer quad ([-1,1]^2 = the object size) to clip space; half_w/half_h
 * = size / 2 in the model's pixel units */
void walpe_puppet_draw (WalpePuppet *p, double t, float half_w, float half_h,
                        GLuint tex, gboolean premult, float opacity, const float *mvp,
                        int blend, const float tint[3]);

/* the part of the layer's texture the mesh can sample, whatever the pose:
 * up to max (at most 8) rectangles u0, v0, u1, v1 in texture coordinates
 * that contain every triangle; 0 = no statement (treat as everything) */
int walpe_puppet_uv_cover (WalpePuppet *p, float (*rects)[4], int max);

int walpe_puppet_bone_count (const WalpePuppet *p);

/* Attachment points (the puppet editor's "attachments": a named frame
 * fixed to a bone). A scene layer whose parent is a puppet and that names
 * one ("attachment": name) lives in that frame: its origin, angle and
 * scale are relative to the point and it follows the bone's animation.
 * m = the point's frame at time t (s) in the puppet layer's own units
 * (pixels around the layer's centre, y up): x' = m0 x + m2 y + m4,
 * y' = m1 x + m3 y + m5. FALSE when the model has no point of that name. */
gboolean walpe_puppet_attachment (WalpePuppet *p, const char *name, double t, float m[6]);
int walpe_puppet_attachment_count (const WalpePuppet *p);
/* WALPE_PROP_PROBE digest of the animation layers (propprobe.h) */
void walpe_puppet_probe (const WalpePuppet *p, JsonBuilder *b);
