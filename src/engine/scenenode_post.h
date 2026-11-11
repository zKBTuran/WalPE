/* scenenode_post.h - a scene's bloom and camera shake inside the scene
 * renderer. Included by scenenode.c behind SceneNodeContent; everything
 * here is static and belongs to that file. The work itself is in bloom.c
 * (the chain on the finished frame) and camshake.c (the offset of the view
 * as a function of time); this header is the glue: what the scene file
 * asks for, what a frame does about it, and what it means for the parts of
 * the renderer that keep pictures between frames.
 *
 * A scene that asks for neither (the great majority) has s->post == NULL
 * and s->cam_sway == 0 0: every hook below returns at its first line and
 * the frame is drawn exactly as without this file.
 *
 * general.bloom, bloomstrength, bloomthreshold, bloomtint
 *   Bloom on the finished frame (bloom.h), after the last layer and before
 *   a reduced frame is scaled up to the window. Values as the scene file
 *   gives them (plain or bound to an option of the wallpaper), in the
 *   frame's own display-referred values.
 *   general.hdr: the scene was made for Wallpaper Engine's HDR pipeline,
 *   whose bloom (bloomhdr* values) is another chain that is not built
 *   here. Such a scene gets the standard chain with its standard values -
 *   an estimate of what Wallpaper Engine shows when its HDR bloom is not
 *   in use - and intermediates that do not clamp where the driver has
 *   them.
 *   The quality knob post_processing applies: full = the chain as it is,
 *   reduced = its inner targets at half size, off = no bloom.
 *
 * general.camerashake, camerashakeamplitude, camerashakeroughness,
 * camerashakespeed
 *   The view centre sways (camshake.h). The offset is written to
 *   s->cam_sway once per frame, before the frame's view is set up, and
 *   view_centre () adds it for view_map (): every layer the camera shows
 *   moves as one rigid picture. A fullscreen layer (it follows the frame,
 *   not the camera) stays where it is and works on the swayed picture
 *   below it. Camera parallax is measured from the camera's place of
 *   rest, without the sway (view_centre (s, FALSE, ..)).
 *   WALPE_SHAKE_AMOUNT=<factor> and WALPE_SHAKE_RATE=<factor> (default 1)
 *   scale how far and how fast, WALPE_SHAKE_COVER=1 zooms the view in by
 *   the reach of the sway so that no edge of the canvas is uncovered: the
 *   look of the shake is an estimate, these are for comparing it with the
 *   original. Read when a scene loads.
 *
 * What the two do to pictures kept between frames:
 *   - Frames that equal the last one are not drawn. Bloom is a function of
 *     the frame and of its own settings: its state is part of the frame's
 *     signature (scene_post_signature), so a switch or a quality level
 *     that changes it draws a frame, and an unchanged frame keeps the
 *     bloom it was drawn with. A swaying view changes the view matrix,
 *     which the signature holds already: every frame is drawn while the
 *     shake runs, and none is once it is switched off.
 *   - The static bake is a copy of the layers that stand still, in the
 *     view they were drawn with. While the view sways no copy survives its
 *     own frame: the bake is not taken (scene_post_view_moves), instead of
 *     being made anew and thrown away every frame. Bloom comes after all
 *     layers and never is in the bake.
 *   - Effect outputs kept per layer live in the layer's own space and are
 *     not touched by either.
 *   - "Skip hidden areas": bloom reads the whole frame, so in a frame that
 *     gets bloom the layers are drawn unclipped and only the final add is
 *     limited to what can be seen (scene_post_reads_frame). */
#pragma once
#include "engine/bloom.h"
#include "engine/camshake.h"
#include "engine/features.h"   /* the switches "bloom" and "camera_shake" */

struct ScenePost {
    /* ---- bloom */
    gboolean bloom_on;        /* the scene asks for it */
    WalpeBloomParams bloom;   /* its values; .level is set per frame */
    WalpeBloom *bloom_gl;     /* programs and targets, made by the first frame that needs them */
    gboolean bloom_now;       /* this frame gets bloom */
    /* ---- camera shake */
    gboolean shake_on;        /* the scene asks for it (and it can move) */
    float shake_amp, shake_rough, shake_speed;
    gboolean shake_now;       /* the view sways in this frame */
    /* the scene's frame clock is on for the sway alone (scene_post_frame):
     * what s->animates / s->only_video_anim were without it */
    gboolean clock_taken, clock_animates, clock_video_only;
    /* ---- quality accounting of this frame (FxAcct units) */
    guint64 px_now, px_nom, px_red;
    guint passes_now, passes_nom;
};

/* helpers of scenenode.c used here (defined further down in that file) */
static void parse_vec (const char *s, float *v, int n, const float *dflt);
static const char *prop_str (JsonObject *o, const char *k);
static double prop_num (JsonObject *o, const char *k, double d);
static void probe_vec (JsonBuilder *b, const char *k, const float *v, int n);

static double scene_post_env_factor (const char *name)
{
    const char *e = g_getenv (name);
    double v = e && *e ? g_ascii_strtod (e, NULL) : 1.0;
    return isfinite (v) ? CLAMP (v, 0.0, 10.0) : 1.0;
}

/* load: general.bloom* / general.camerashake* (while the wallpaper's
 * options are known to prop_num). Leaves s->post NULL when the scene asks
 * for neither. */
static void scene_post_parse (SceneNodeContent *s, JsonObject *gen0)
{
    gboolean bloom = prop_num (gen0, "bloom", 0.0) != 0;
    gboolean shake = prop_num (gen0, "camerashake", 0.0) != 0;
    float amp = 0.0f, speed = 0.0f, rough = 0.0f;
    if (shake) {
        /* (only for a scene that shakes: a value bound to an option of
         * the wallpaper counts as consumed when it is read - propprobe.h -
         * and behind a shake that is off nothing consumes it) */
        amp = (float) (prop_num (gen0, "camerashakeamplitude", 0.5) *
                       scene_post_env_factor ("WALPE_SHAKE_AMOUNT"));
        speed = (float) (prop_num (gen0, "camerashakespeed", 3.0) *
                         scene_post_env_factor ("WALPE_SHAKE_RATE"));
        rough = (float) prop_num (gen0, "camerashakeroughness", 1.0);
    }
    /* a shake that cannot move anything is none (no frame clock for it) */
    if (!isfinite (amp) || !isfinite (speed) || !isfinite (rough) || amp == 0 || !(speed > 0))
        shake = FALSE;
    if (!bloom && !shake)
        return;
    struct ScenePost *p = g_new0 (struct ScenePost, 1);
    s->post = p;
    if (bloom) {
        static const float white[3] = { 1.0f, 1.0f, 1.0f };
        p->bloom_on = TRUE;
        p->bloom.strength = (float) prop_num (gen0, "bloomstrength", 2.0);
        p->bloom.threshold = (float) prop_num (gen0, "bloomthreshold", 0.65);
        parse_vec (prop_str (gen0, "bloomtint"), p->bloom.tint, 3, white);
        for (int i = 0; i < 3; i++)
            if (!isfinite (p->bloom.tint[i]))
                p->bloom.tint[i] = 1.0f;
        p->bloom.hdr = prop_num (gen0, "hdr", 0.0) != 0;
        p->bloom.level = WALPE_BLOOM_FULL;
    }
    if (shake) {
        p->shake_on = TRUE;
        p->shake_amp = amp;
        p->shake_rough = rough;
        p->shake_speed = speed;
        const char *cover = g_getenv ("WALPE_SHAKE_COVER");
        if (cover && *cover == '1')   /* no edge of the canvas comes into view */
            s->gen_zoom *= 1.0f + 2.0f * walpe_camera_shake_reach (amp);
    }
}

/* gl_init: what the scene asks for, in the log. (The frame clock a swaying
 * view needs is taken per frame, while the shake is in effect:
 * scene_post_frame.) */
static void scene_post_init (SceneNodeContent *s)
{
    const struct ScenePost *p = s->post;
    if (!p)
        return;
    if (p->shake_on) {
        g_message ("scenenode: camera shake (amplitude %.2f, roughness %.2f, speed %.2f)%s",
                   (double) p->shake_amp, (double) p->shake_rough, (double) p->shake_speed,
                   walpe_feature (WALPE_FEAT_CAMERA_SHAKE) ? "" : " - switched off");
    }
    if (p->bloom_on)
        g_message ("scenenode: bloom (strength %.2f, threshold %.2f%s)%s%s",
                   (double) p->bloom.strength, (double) p->bloom.threshold,
                   p->bloom.hdr ? "; the scene is made for HDR bloom, which is not built: "
                                  "standard bloom with these values" : "",
                   walpe_bloom_visible (&p->bloom) ? "" : " - these values change no pixel",
                   walpe_feature (WALPE_FEAT_BLOOM) ? "" : " - switched off");
}

/* the scene's clock for the frame about to be drawn (seconds), before the
 * frame has set its own up: a fixed instant (WALPE_FX_TIME, the digest's
 * virtual clock) or the time since the scene started, which stands still
 * while the scene is paused */
static double scene_post_clock (const SceneNodeContent *s)
{
    if (s->pprobe)
        return (s->frame_no + 1) / 30.0;
    if (s->fx_time >= 0)
        return s->fx_time;
    return ((s->paused_at ? s->paused_at : g_get_monotonic_time ()) - s->t0) / 1e6;
}

/* top of a frame, before its view is set up: this frame's sway, and
 * whether it gets bloom */
static void scene_post_frame (SceneNodeContent *s)
{
    struct ScenePost *p = s->post;
    if (!p)
        return;
    p->px_now = p->px_nom = p->px_red = 0;
    p->passes_now = p->passes_nom = 0;
    p->shake_now = p->shake_on && walpe_feature (WALPE_FEAT_CAMERA_SHAKE);
    /* The frame clock. A swaying view moves all the time, so the scene
     * animates - but only while the shake is in effect: with the switch
     * off the scene has the clock gl_init gave it for what else moves in
     * it (none for a still scene: it is drawn once, keeps no bake and asks
     * for no frames, as without this file). Whoever flips the switch asks
     * for a frame, which comes by here. */
    if (p->shake_now && !p->clock_taken && (!s->animates || s->only_video_anim)) {
        p->clock_taken = TRUE;
        p->clock_animates = s->animates;
        p->clock_video_only = s->only_video_anim;
        s->animates = TRUE;
        s->only_video_anim = FALSE;   /* (a video's frame rate does not pace the sway) */
    } else if (!p->shake_now && p->clock_taken) {
        p->clock_taken = FALSE;
        s->animates = p->clock_animates;
        s->only_video_anim = p->clock_video_only;
    }
    if (p->shake_now)
        walpe_camera_shake (scene_post_clock (s), p->shake_amp, p->shake_rough, p->shake_speed,
                            s->scene_w, s->scene_h, s->cam_sway);
    else
        s->cam_sway[0] = s->cam_sway[1] = 0.0f;
    p->bloom.level = s->q.post == WALPE_POST_REDUCED ? WALPE_BLOOM_REDUCED : WALPE_BLOOM_FULL;
    p->bloom_now = p->bloom_on && s->q.post != WALPE_POST_OFF &&
                   walpe_feature (WALPE_FEAT_BLOOM) && walpe_bloom_visible (&p->bloom);
    if (!p->bloom_now && p->bloom_gl)
        walpe_bloom_drop_targets (p->bloom_gl);   /* switched off while the scene runs */
}

/* the view of this frame is not the view of the last one: nothing that is
 * a copy of drawn layers can be used again */
static inline gboolean scene_post_view_moves (const SceneNodeContent *s)
{
    return s->post && s->post->shake_now;
}

/* the whole finished frame is read after the layers */
static inline gboolean scene_post_reads_frame (const SceneNodeContent *s)
{
    return s->post && s->post->bloom_now;
}

/* what bloom adds to the frame, as a number for the frame's signature:
 * the same in two frames exactly when the same finished frame gets the
 * same bloom (its values are fixed at load; what changes while the scene
 * runs is whether it is drawn and at which level) */
static inline guint scene_post_signature (const SceneNodeContent *s)
{
    return s->post && s->post->bloom_now ? 1u + (guint) s->post->bloom.level : 0u;
}

/* after the last layer: the bloom of the frame in scene_fb (0 = the
 * window; else the scene's own target, whose texture is frame_tex).
 * w x h = the frame, ref_w x ref_h = the size the picture is made for
 * (what the effects are told the screen is). */
static void scene_post_bloom (SceneNodeContent *s, GLuint scene_fb, GLuint frame_tex, int w, int h,
                              int ref_w, int ref_h)
{
    struct ScenePost *p = s->post;
    if (!p || !p->bloom_on)
        return;
    /* accounting: what the chain costs at the default level, whether it
     * is drawn or not (a level that drops it is told what that saves) */
    WalpeBloomPlan full, red;
    const WalpeBloomSource src = scene_fb && frame_tex ? WALPE_BLOOM_SRC_TEXTURE
                                                       : WALPE_BLOOM_SRC_HALF_COPY;
    if (walpe_feature (WALPE_FEAT_BLOOM) && walpe_bloom_visible (&p->bloom) &&
        walpe_bloom_plan (w, h, ref_w, ref_h, src, WALPE_BLOOM_FULL, &full) &&
        walpe_bloom_plan (w, h, ref_w, ref_h, src, WALPE_BLOOM_REDUCED, &red)) {
        p->px_nom = full.px_small + full.px_add;
        p->px_red = full.px_small - red.px_small;
        p->passes_nom = 4;
    }
    if (!p->bloom_now)
        return;
    WalpeBox seen[WALPE_CLIP_MAX];
    if (walpe_clip_boxes (seen) == 0)
        return;   /* "Skip hidden areas": nothing of this frame can be seen */
    if (!p->bloom_gl)
        p->bloom_gl = walpe_bloom_new ();
    const WalpeBloomFrame frame = { .fbo = scene_fb, .tex = scene_fb ? frame_tex : 0, .w = w,
                                    .h = h, .ref_w = ref_w, .ref_h = ref_h };
    WalpeBloomStats st;
    if (!walpe_bloom_apply (p->bloom_gl, &frame, &p->bloom, &st)) {
        /* this GL cannot (logged by bloom.c): the scene goes on without */
        p->bloom_on = p->bloom_now = FALSE;
        p->px_nom = p->px_red = 0;
        p->passes_nom = 0;
        return;
    }
    if (st.drawn) {
        p->px_now = st.px;
        p->passes_now = st.passes;
    }
}

/* quality accounting: the frame's bloom next to its effect passes (a:
 * what fx_acct_take () gave for this frame) */
static void scene_post_acct (SceneNodeContent *s, FxAcct *a)
{
    const struct ScenePost *p = s->post;
    if (!p || !p->px_nom)
        return;
    a->px_now += p->px_now;
    a->px_nom += p->px_nom;
    a->px_post_red += p->px_red;
    a->px_post_off += p->px_nom;
    a->passes_now += p->passes_now;
    a->passes_nom += p->passes_nom;
    a->passes_post_off += p->passes_nom;
}

/* the scene is paused or covered: its targets go like every other one */
static void scene_post_drop_targets (SceneNodeContent *s)
{
    if (s->post && s->post->bloom_gl)
        walpe_bloom_drop_targets (s->post->bloom_gl);
}

static void scene_post_gl_free (SceneNodeContent *s)
{
    if (s->post)
        g_clear_pointer (&s->post->bloom_gl, walpe_bloom_free);
}

static void scene_post_free (SceneNodeContent *s)
{
    g_clear_pointer (&s->post, g_free);
}

/* WALPE_PROP_PROBE digest: only for scenes that ask for one of the two,
 * and only what is in effect - with a feature switched off the state
 * digest of a scene is the one it had before the feature existed (and an
 * option of the wallpaper that only feeds a feature that is off does not
 * read as effective) */
static void scene_post_probe (SceneNodeContent *s, JsonBuilder *b)
{
    const struct ScenePost *p = s->post;
    if (!p)
        return;
    if (p->shake_on && walpe_feature (WALPE_FEAT_CAMERA_SHAKE))
        probe_vec (b, "camerashake", s->cam_sway, 2);   /* the offset of the last frame */
    if (p->bloom_on && walpe_feature (WALPE_FEAT_BLOOM)) {
        const float bl[2] = { p->bloom.strength, p->bloom.threshold };
        probe_vec (b, "bloom", bl, 2);
    }
}
