/* scenenode.c - renderer for the modern WE scene format ("node graph").
 *
 * scene.json "version":1 files use an "objects" array with string-vector
 * transforms (origin/angles/scale/size), alpha 0-1, per-object color +
 * colorBlendMode, and content references: "image" -> models/X.json ->
 * materials/X.json -> materials/X.tex, "particle" -> particles/X.json
 * (same system format as legacy), "text" -> { script/value } (JS-computed
 * text: rendered with its static "value"; scripting unsupported).
 * Effects are parsed + counted (tier-2 will execute the pass graphs).
 *
 * The legacy "object"/"layers" format stays in scene.c; scene.c sniffs
 * "objects" and delegates here with the parsed root transferred. */
#include "scenenode.h"
#include "scenescript.h"
#include "videotex.h"
#include "video.h"      /* walpe_video_frames_noted */
#include "scenenode_effects.h"
#include "glutil.h"
#include "clip.h"
#include "particles.h"
#include "weparticles.h"
#include "common/util.h"
#include "common/tex.h"
#include "common/config.h"
#include "common/premult.h"
#include "common/media.h"
#ifdef __GLIBC__
#include <malloc.h>
#endif
#include <json-glib/json-glib.h>
#include <pango/pangocairo.h>
#include "scenetext.h"
#include "puppet.h"
#include <string.h>
#include <math.h>
#include <locale.h>
#include "propprobe.h"
#include "cpuphase.h"
#include "gpuprof.h"   /* GPUPROF hooks: WALPE_PROF_* in scenenode_gl_render */

typedef enum { NK_EMPTY, NK_IMAGE, NK_PARTICLE, NK_TEXT } NodeKind;

typedef struct {
    gboolean visible;
    float alpha;
    float origin[2];
    float angle_z;       /* radians; x/y tilts ignored in the 2D path */
    float scale[2];
    float origin_z, scale_z; /* depth of perspective-rendered particle systems */
    float bright;        /* "brightness": multiplies the layer colour (g_Color4.rgb) */
    float size[2];
    float tint[3];
    int blend;
    NodeKind kind;
    GLuint tex;
    gboolean tex_ready, premult;
    int tex_w, tex_h;
    int tex_full_w, tex_full_h;  /* padded texture size (fx Resolution) */
    WalpeParticles *parts;      /* legacy "objectconfig" systems */
    WalpeWeParticles *weparts;  /* WE particle json (emitter/initializer/...) */
    gpointer fxset;      /* Tier 2 effect chains (may be NULL) */
    int compose;         /* WE compose layer (models/util/composelayer|projectlayer:
                          * 1 = the region under the layer, fullscreenlayer: 2 =
                          * the whole frame) run through the layer's effects */
    GLuint cfbo, ctex;   /* compose: captured background */
    int cw, ch;
    float fx_k;          /* effect working-resolution factor in use (0 = unset) */
    float fx_k2;         /* the same with the quality knob fx_res below "full" */
    char *text_value;
    WalpeTextStyle *tstyle; /* text layers: font/size/box/alignment */
    WalpePuppet *puppet;    /* puppet-warp image layer (skinned mesh), or NULL */
    float tquad[4];      /* text texture rect in local space {cx, cy, w, h} */
    float anchor[2];     /* image "alignment": where the origin sits on the
                          * quad, -1..1 per axis (0 0 = center, -1 0 = left
                          * edge, 0 1 = top edge; scene space is Y-up) */
    float text_tint[3];  /* text with effects: color baked into the glyphs */
    gboolean text_baked;
    WalpeTextInk text_ink; /* what the text texture holds (scenetext.h); w = 0: unknown */
    gboolean tex_ready_text; /* ... and it was made by make_text_texture with this string's
                          * predecessor (refresh_text clears tex_ready before a render) */
    int video;           /* index into SceneNodeContent.videos, -1 = none
                          * (tex is then borrowed from the shared decoder) */
    int id, parent_id;   /* WE object id / "parent" reference (-1 = root) */
    char *name;          /* object name (thisScene.getLayer) */
    char *attach;        /* "attachment": the attachment point of the parent's
                          * puppet this layer lives in (node_world), or NULL */
    float pov[9];        /* WALPE_PROP_PROBE: particle instance overrides */
    WalpeWeParticleOverride *inst; /* WE particle system: its instance overrides,
                          * which scripts read and change ("inst.*") */
    guint inst_scripted; /* WALPE_WE_OV_*: count / rate are driven by a script */
    gboolean text_dirty; /* a script changed text_value: re-render */
    int parent;          /* resolved node index (-1 = root) */
    /* per frame: world transform (parent chain applied, size excluded)
     * and visibility inherited from the parents */
    WalpeMat3 world;
    gboolean world_visible;
    int world_state;     /* 0 = stale, 1 = computing, 2 = done */
    gboolean solid;      /* "solid" (default true): takes cursor events */
    /* oversized-texture downscale (tex_downscale): the file texture's
     * cache key, and the state of the decision taken at the first frame */
    char *tex_src;       /* file path the layer texture came from */
    guint tex_lf;        /* its load flags */
    gboolean tex_pmflag; /* uploaded premultiplied */
    gboolean tex_rgba;   /* plain RGBA8 (not DXT / R8 / RG88) */
    gboolean scripted;   /* a script or property animation may touch it */
    int ds_state;        /* 0 = undecided, 1 = downscaled (watching), 2 = final,
                          * 3 = full size, watching for > 2:1 (mip chain then) */
    gboolean tex_want_mips; /* its file has a mip chain that is not built yet */
    int phys_w, phys_h;  /* texel size of the texture as stored */
    /* the file texture is loaded `tex_lod` halvings below the file's size,
     * from the file's own mip levels where it has them (node_tex_load) */
    gboolean tex_lazy;   /* not loaded yet: its level is chosen at the first
                          * frame, when the on-screen size is known, so the
                          * levels above it are never decoded or uploaded */
    int tex_lod;
    gboolean tex_pic;    /* ... and its file stores a PNG / JPEG: decoded on a
                          * worker thread since the scene was parsed, and
                          * loaded with the first frame also when hidden */
    gboolean tex_job;    /* walpe_gl_tex_predecode was called for it and is
                          * not answered yet */
    WalpeTexInfo tex_ti; /* the texture it holds, as the cache described it */
    gboolean tex_dxt;    /* the file is DXT and is kept block compressed */
    gboolean tex_comp;   /* the GL texture is block compressed right now */
    gboolean tex_chain;  /* ... and has the file's levels below its base */
    int tex_mips;        /* mip levels stored in the file (1 = none, 0 = picture) */
    gint64 tex_bytes;    /* GL bytes of the texture as stored */
    int tex_min_lod;     /* texture budget: never stored above this level */
    int tex_plan;        /* ... the level planned for it at the first frame */
    int tex_bpt8;        /* bytes per texel x 8 as stored on the GPU (estimates) */
    int systex;          /* material/instance "usertextures" system texture
                          * replacing the layer image: 1 = $mediaThumbnail,
                          * 2 = $mediaPreviousThumbnail (media integration) */
    gboolean canvas;     /* WE solid layer: a flat colour, no picture of its own */
    gboolean fx_skip;    /* never drawn: a solid / compose layer one of whose
                          * effects could not be built (see parse_node) */
    int fxin_state;      /* layer with effects, its texture as the passes'
                          * input: 0 = undecided, 1 = stored at half size
                          * (watching for growth), 2 = final (node_fx_input_fit) */
    gboolean fx_soft_said; /* node_fx_input_soft: its message is out */
    gboolean fx_soft_meas, fx_soft_pending; /* fx_soft_cost is measured / is
                          * waited for: the texture stays at full size
                          * until it is there to be looked at */
    float fx_soft_cost;  /* the share of the layer's picture a reduced input
                          * of its effects would change (fx_input_soft_cost) */
    gboolean dbg_skip;   /* WALPE_SKIP_IDS names this object (read at load) */
    gboolean text_mips;  /* text texture: its mip chain is built (it was drawn
                          * smaller than 2:1); FALSE = level 0 only so far */
    /* frame signature (node_signature): what this layer would draw in the
     * current frame, and for how many frames in a row that was the same */
    guint pix_serial;    /* the layer's own pixels changed under the same
                          * texture name (text rendered again) */
    guint64 sig;
    gboolean sig_vol;    /* a new picture every frame whatever sig says */
    int stable;          /* frames since sig last changed (capped) */
    /* "Adjust layers" (scenenode_layers.h) */
    int adj;             /* index + 1 into SceneNodeContent.ladj, 0 = not adjusted */
    float ua[3];         /* per frame, while the scene has adjustments: the user
                          * transform accumulated over the parent chain,
                          * x' = ua[0] * x + (ua[1], ua[2]) in scene space */
    float cost_px;       /* pixels its draw wrote when it was last drawn (its
                          * bounds within the view; a compose layer with
                          * effects counts its capture and a pass as well):
                          * what a bake saves by standing in for it */
} NodeObj;

/* one per unique video texture in the scene (WE .tex with an embedded
 * MP4): every object using it draws the same per-monitor GL texture */
typedef struct {
    char *path;
    WalpeVideoTex *v;
    GLuint tex;
    int w, h;                /* size of the frames in tex: the video's, or half of
                              * it while every user shows it that small */
    int full_w, full_h;      /* the video's own size */
    guint serial;            /* counts the frames this scene's texture received: it
                              * changes when, and only when, the layer shows another
                              * picture (a paused video, or one whose next frame is
                              * not due yet, keeps it) - what "did this layer
                              * change?" is to be asked of a video layer */
    int view_w, view_h;      /* this frame: largest on-screen size of a user */
    gboolean view_seen, view_full;
} SceneVideo;
/* the frame skip asks this counter whether a video layer shows another
 * picture */
#define SCENE_VIDEO_SERIAL(sv) ((sv)->serial)

typedef struct {
    WalpeContent base;
    JsonNode *root;
    float scene_w, scene_h;
    GArray *nodes;       /* NodeObj */
    char *dir;           /* project dir (cache dir for pkgs) */
    int fps_cap;
    gboolean paused;
    gint64 t0;           /* content-local clock for g_Time (float precision) */
    gint64 paused_at;    /* monotonic us the scene clock stopped (0 = running) */
    gboolean text_held;  /* walpe_text_hold (text layers) */
    gboolean fx_stats;   /* WALPE_FX_STATS / WALPE_GPU_MEM: passes per frame */
    gboolean pt_stats;   /* WALPE_PT_STATS: particle cost per frame (weparticles.h) */
    guint pt_serial;     /* walpe_weparticles_settings_serial () this scene last drew with */
    gboolean trim_heap;  /* large textures were freed in this frame */
    int fx_ok, fx_failed, fx_static; /* effect chains built / failed, static sets */
    gboolean summary_done;
    gboolean tex_deferred; /* the first frame's texture pass is to come (scene_load_deferred_textures) */
    gint64 tex_budget;   /* bytes of file textures this scene may hold, 0 = no limit
                          * (WALPE_TEX_BUDGET_MB, default 1024) */
    gint64 tex_eager_bytes; /* effect, sprite and not deferred layer textures */
    float slack_pct;     /* magnification tolerance (tex_slack), per cent */
    guint tex_serial;    /* counts every change of a layer texture after its
                          * first load (another level, a mip chain added):
                          * SCENE_PICTURE_CHANGED */
    int downscale;       /* tex_downscale: 0 off, 1 strict (2:1 only), 2 aggressive
                          * (opts + WALPE_TEX_DOWNSCALE=0|1|2) */
    gint64 ds_saved;     /* bytes saved by downscaled layer textures */
    gboolean animates;   /* particles or effects: needs the frame clock */
    gint64 due_us;       /* frame clock phase */
    gboolean warned_script, warned_tilt, warned_particle_xform;
    guint fx_count;
    JsonObject *user_props; /* Tier 4: {propid: value} from project.json */
    JsonObject *user_types; /* {propid: declared type}, for the script runtime */
    char *origin;        /* wallpaper path as opened (override key) */
    char *preset_dir;    /* WE preset item dir ("files/..." values), or NULL */
    GArray *videos;      /* SceneVideo: shared video textures */
    gint64 last_frame_us; /* particle simulation clock */
    double frame_time;   /* this frame's scene time (s): where a puppet's
                          * attachment points are (node_world) */
    guint frame_no;
    gboolean probe;      /* WALPE_PROBE diagnostics */
    gboolean nofx;       /* WALPE_NOFX: draw layers without their effects */
    double fx_time;      /* WALPE_FX_TIME=s: fixed effect time (A/B screenshots), else -1 */
    float persp_dist;    /* perspective particle systems: camera distance from z = 0 */
    GHashTable *fx_miss_logged; /* WALPE_PROBE: "node/effect/pass/key" already reported */
    gboolean needs_audio;  /* g_AudioSpectrum effects / audio emitters */
    gboolean audio_ref;    /* holds a capture reference (visible + unpaused) */
    WalpeAudioSpectrum audio;
    GPtrArray *pending_scripts; /* PendingScript, consumed at gl_init */
    WalpeSceneScript *ss;       /* SceneScript runtime (NULL = none) */
    gint64 ss_us;               /* WALPE_PROBE: tick time accumulator */
    int ss_ticks;
    gboolean only_video_anim; /* frame clock needed only for video */
    /* media integration (media.h): scripts with media* events and/or
     * $mediaThumbnail system textures */
    gboolean needs_media, media_ref;
    guint64 media_serial;
    guint media_seen[5];        /* status/props/thumb/playback/timeline serials */
    gboolean media_first;       /* next dispatch is the initial one */
    GLuint media_tex[3];        /* [1] thumbnail, [2] previous thumbnail */
    int media_w[3], media_h[3];
    float media_aspect;         /* width / height of the layer the album art
                                 * fills (0 = not looked up yet) */
    /* SceneScript cursor events: layers with cursor* handlers, pointer
     * events queued by the host (pointer()), handled at the next frame */
    int *cur_layers, n_cur_layers;
    GArray *ptr_q;              /* PtrEv */
    float cur_x, cur_y;         /* last pointer position, scene coordinates */
    gboolean cur_have, cur_down;
    int cur_hover, cur_press;   /* node index or -1 */
    int view_w, view_h;         /* last rendered size (pointer mapping) */
    float ptr_nx, ptr_ny;       /* last on-desktop pointer, monitor [0,1] */
    gboolean ptr_have;          /* (mouse-linked particle control points) */
    /* WE camera layer ({"camera": "default", "zoom": z, "origin"}): the view
     * shows canvas / zoom scene units around the canvas centre + the
     * layer's origin (x, y). zoom < 1 zooms out (3042492564: 0.73) */
    gboolean cam_found;
    int cam_id, cam_node;       /* object id / node index (-1 = none) */
    float cam_zoom;
    /* general.clearcolor (plain or bound to a user colour): what shows
     * where no layer draws; general.zoom: the view's zoom without a camera
     * layer (2929592935 binds both to options) */
    float clear[3];
    float gen_zoom;
    /* WALPE_PROP_PROBE (propprobe.h): virtual clock + one state digest */
    gboolean pprobe, pprobe_done;
    /* Frame skipping: the signature of the frame the surface shows (every
     * input of the picture: view, layer states, effect generations and
     * the audio / pointer values the effects read). A frame with the same
     * signature and no layer that changes by itself (time-driven effect,
     * live particles, video, puppet) is not drawn at all. */
    guint64 frame_sig;
    gboolean frame_valid;    /* frame_sig describes a frame that was drawn */
    gboolean no_skip;        /* WALPE_FRAME_SKIP=0 */
    guint n_skipped, n_drawn, n_baked; /* WALPE_FX_STATS, per 150 frames */
    /* Static bake: when the leading run of layers (in draw order) has not
     * changed for a few frames, the frame as it stands once that run is
     * drawn (clear colour included) is copied into a texture; while the
     * run stays as it is, a frame starts with one copy of that texture
     * instead of the clear and one draw per layer. Any change of one of
     * those layers (option, script, resize) shows in the run's signature
     * and the run is drawn and copied again. */
    struct {
        GLuint fbo, tex;
        int w, h;
        guint64 sig;         /* signature of the run it holds */
        guint end;           /* first node index behind the run */
        gboolean valid;
        int unused;          /* frames since it was last used */
        int age;             /* frames it has been used for */
        gboolean lean;       /* the cached effect outputs of its layers were
                              * given back (bake_release_fx) */
    } bake;
    gboolean no_bake;        /* WALPE_BAKE=0 */
    /* render scale (config render_scale, opt-in): the scene is drawn into
     * a target of scale % of the monitor's size and scaled up to it */
    int render_scale;        /* 25..100 */
    gboolean rs_fixed;       /* set by WALPE_RENDER_SCALE: the setting is ignored */
    struct { GLuint fbo, tex; int w, h; } rs;
    guint media_gen;         /* media_upload count (album art pixels) */
    int profile_frame;       /* WALPE_SCENE_PROFILE=<frame>: one line per layer
                              * of that frame (what it costs, why it is redrawn) */
    /* "Adjust layers" (scenenode_layers.h, common/layeradj.h) */
    guint lay_gen;              /* layeradj generation last synced with */
    GArray *ladj;               /* SceneLayerAdj: this wallpaper's adjustments that
                                 * matched a node */
    guint n_adj;                /* ladj->len; 0 = nothing below runs */
    GArray *hl;                 /* int: node indices to outline (NULL / empty = none) */
    GLuint hl_tex;              /* 1x1 white, made on first use */
    GHashTable *lay_missing;    /* saved keys already reported as matching no layer */
    guint64 lay_list_seen;      /* newest layer-list request seen */
    gboolean lay_publish;       /* publish the layer list after this frame */
    /* quality knobs (common/quality.h): the settings in use; accounting
     * sums since the last report (qa) and the last report (qstats); the
     * GPU's own count of shaded pixels, sampled now and then while
     * somebody looks at the numbers (px_*: state 0 idle, 1 counting this
     * frame, 2 waiting for the result) */
    WalpeQuality q;
    gboolean texres_applied;   /* q.tex_res was applied (scenenode_texres_apply) */
    struct { FxAcct fx; guint frames; double base; } qa;
    WalpeQualityStats qstats;
    gboolean qstats_ok;
    GLuint px_query;
    int px_state, px_can;
    guint64 px_fx;
    double px_base;
    gboolean px_ok;
} SceneNodeContent;

typedef struct { int kind; float x, y; } PtrEv;

#include "scenenode_layers.h"

/* same resolution rules as scene.c legacy: direct, dir/assets/. The name
 * comes from the wallpaper's json: it stays below dir (no absolute path, no
 * "..") and has to be a regular file (walpe_asset_path). */
static char *resolve_asset (const char *dir, const char *file)
{
    if (!file || !*file)
        return NULL;
    char *a = walpe_asset_path (dir, file);
    if (!a) {
        char *b = g_build_filename ("assets", file, NULL);
        a = walpe_asset_path (dir, b);
        g_free (b);
    }
    return a;
}

/* resolve a model/material ref: project dir first, then the WE install */
static char *resolve_model_ref (const char *dir, const char *ref)
{
    char *p = resolve_asset (dir, ref);
    if (!p) {
        const char *we = walpe_we_assets_dir ();
        if (we)
            p = resolve_asset (we, ref);
    }
    return p;
}

/* ------------------------------------------------------------- helpers */

/* Numbers in scene files, effect constants, particle files and scripts are
 * written with '.', whatever the user's locale. The daemon runs in the "C"
 * locale, but a library can switch the process locale later (GTK did, at
 * the first web wallpaper: from then on sscanf ("%f") stopped at the '.'
 * of "0.5 0.75 1" in a comma-decimal locale and QuickJS read 0.25 as 0).
 * A thread that loads or renders a scene therefore gets a locale of its
 * own with "C" numbers: it covers every strtod / sscanf / printf under
 * it, in the engine and in the libraries it calls, and nothing else in
 * the process. */
static void numeric_locale_free (gpointer p)
{
    uselocale (LC_GLOBAL_LOCALE);
    freelocale ((locale_t) p);
}
static GPrivate numeric_locale_key = G_PRIVATE_INIT (numeric_locale_free);

static void scene_thread_numeric_c (void)
{
    if (g_private_get (&numeric_locale_key))
        return;
    locale_t base = duplocale (LC_GLOBAL_LOCALE);
    locale_t c = base ? newlocale (LC_NUMERIC_MASK, "C", base) : (locale_t) 0;
    if (!c) {
        if (base)
            freelocale (base);
        return;
    }
    uselocale (c);
    g_private_set (&numeric_locale_key, c);
}

static void parse_vec (const char *s, float *v, int n, const float *dflt)
{
    float t[3] = { 0, 0, 0 };
    for (int i = 0; i < n; i++)
        v[i] = t[i] = dflt[i];
    if (!s)
        return;
    /* parse into t: a 2-element v (NodeObj.size) must never get a 3rd write */
    int got = sscanf (s, "%f %f %f", &t[0], &t[1], &t[2]);
    if (got < n && n == 2)
        sscanf (s, "%f %f", &t[0], &t[1]);
    for (int i = 0; i < n; i++)
        v[i] = t[i];
}

/* video texture: attach the object to the scene's shared entry for this
 * file (one decoder per unique texture, shared across monitors too) */
static gboolean attach_video (SceneNodeContent *s, NodeObj *nv, const char *path,
                              guint64 off, guint64 len, int w, int h)
{
    for (guint i = 0; i < s->videos->len; i++) {
        SceneVideo *sv = &g_array_index (s->videos, SceneVideo, i);
        if (!g_strcmp0 (sv->path, path)) {
            nv->video = (int) i;
            goto ok;
        }
    }
    GError *err = NULL;
    WalpeVideoTex *v = walpe_video_tex_new (path, off, len, &err);
    if (!v) {
        g_warning ("scenenode: video texture %s: %s", path, err ? err->message : "?");
        g_clear_error (&err);
        return FALSE;
    }
    SceneVideo sv = { .path = g_strdup (path), .v = v, .w = w, .h = h, .full_w = w, .full_h = h };
    g_array_append_val (s->videos, sv);
    nv->video = (int) s->videos->len - 1;
    g_message ("scenenode: video texture %s (%dx%d)", path, w, h);
ok:
    nv->tex_w = nv->tex_full_w = w > 0 ? w : 16;
    nv->tex_h = nv->tex_full_h = h > 0 ? h : 16;
    nv->premult = TRUE;      /* decoded frames are opaque */
    nv->tex_ready = FALSE;   /* until the first decoded frame */
    return TRUE;
}

/* "usertextures" entry -> system texture kind (0 = none / user property) */
static int systex_kind (JsonNode *e)
{
    if (!e || json_node_get_node_type (e) != JSON_NODE_OBJECT)
        return 0;
    const char *n = wpj_str (json_node_get_object (e), "name", NULL);
    return !g_strcmp0 (n, "$mediaThumbnail") ? 1 : !g_strcmp0 (n, "$mediaPreviousThumbnail") ? 2 : 0;
}

/* model json -> material json -> texture file; returns texture + blending.
 * Handles the WE builtin models: solidlayer (white quad tinted by the
 * object color) and passthrough/composelayer (draws nothing). */
/* the file a "scenetexture" user option points at: the user's absolute
 * path, or a path inside the preset / wallpaper (a WE preset's
 * "files/x.jpg"). NULL when unset or missing. g_free. */
static char *user_texture_file (SceneNodeContent *s, const char *upid)
{
    JsonNode *uv = upid && s->user_props ? json_object_get_member (s->user_props, upid) : NULL;
    const char *ufile = uv && json_node_get_node_type (uv) == JSON_NODE_VALUE &&
                        json_node_get_value_type (uv) == G_TYPE_STRING
        ? json_node_get_string (uv) : NULL;
    if (!ufile || !*ufile)
        return NULL;
    walpe_probe_use (upid, "usertexture");
    char *own = NULL;
    if (g_str_has_prefix (ufile, "file://")) {
        own = g_filename_from_uri (ufile, NULL, NULL);
        ufile = own;
    }
    char *up = NULL;
    if (ufile && g_path_is_absolute (ufile)) {
        /* the user's own choice (GUI file picker, walpe-ctl setprop): an
         * absolute path counts only when it is the saved override, not when
         * it is the default the item (or a preset) ships */
        char *ov = walpe_config_get_prop (s->origin, upid);
        if (ov && g_file_test (ufile, G_FILE_TEST_IS_REGULAR))
            up = g_strdup (ufile);
        g_free (ov);
    } else if (ufile && !strstr (ufile, "..")) {
        /* a default the item or a preset ships: like every other file a
         * wallpaper names, a regular file below that folder, also once
         * links are resolved (walpe_asset_path) */
        const char *roots[3] = { s->preset_dir, s->origin, s->dir };
        for (int r = 0; r < 3 && !up; r++)
            if (roots[r])
                up = walpe_asset_path (roots[r], ufile);
    }
    if (up)
        g_message ("scenenode: user texture %s = %s", upid, up);
    else
        g_warning ("scenenode: user texture %s = %s not found", upid, ufile ? ufile : "?");
    g_free (own);
    return up;
}

/* WALPE_TEX_DXT_FX=0: the DXT texture of a layer with effects is decoded
 * to RGBA8 as before (3-8 times the memory) instead of staying compressed */
static gboolean node_dxt_fx (void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = g_getenv ("WALPE_TEX_DXT_FX");
        on = !(e && *e == '0');
    }
    return on;
}

/* scene_load_deferred_textures, first pass: node_tex_load only tells the
 * decode workers which level it is going to ask for and loads nothing */
static __thread gboolean tex_announce;

/* the decode asked for ahead (node_tex_predict) is answered: used by the
 * load, or no longer wanted */
static void node_tex_job_done (NodeObj *nv)
{
    if (nv->tex_job) {
        nv->tex_job = FALSE;
        walpe_gl_tex_predecode_cancel (nv->tex_src, nv->tex_lf);
    }
}

/* (re)load the layer's file texture `level` halvings below the file's size
 * (glutil.h WALPE_GL_TEX_LEVEL). chain: a compressed texture brings the
 * file's levels below that one along; near_ok: a compressed texture may
 * stay one level larger (it is sampled within 2:1 either way). */
static gboolean node_tex_load (NodeObj *nv, int level, gboolean chain, gboolean near_ok)
{
    guint lf = nv->tex_lf | WALPE_GL_TEX_LEVEL (level);
    if (fx_has_effects (nv->fxset))
        lf |= WALPE_GL_TEX_CEIL;   /* read by effect passes: sized like their targets */
    if (nv->tex_dxt && !chain)
        lf |= WALPE_GL_TEX_NO_CHAIN;
    if (nv->tex_dxt && near_ok && level > 0)
        lf |= WALPE_GL_TEX_NEAR_OK;
    if (tex_announce) {
        if (nv->tex_job)
            walpe_gl_tex_predecode_request (nv->tex_src, lf, nv->tex_pmflag);
        return FALSE;
    }
    WalpeTexInfo ti;
    /* the full-size texture this layer holds (decoded while the scene was
     * parsed) is what a reduced copy is made from when no cache knows it */
    GLuint t = walpe_gl_tex_cached_own (nv->tex_src, lf, GL_CLAMP_TO_EDGE, nv->tex_pmflag, &ti,
                                        nv->tex_lazy ? 0 : nv->tex, &nv->tex_ti);
    node_tex_job_done (nv);
    if (!t)
        return FALSE;
    if (nv->tex)
        walpe_gl_tex_release (nv->tex);
    nv->tex = t;
    nv->tex_ti = ti;
    nv->tex_lazy = FALSE;
    nv->premult = ti.premult;
    nv->phys_w = ti.phys_w;
    nv->phys_h = ti.phys_h;
    nv->tex_lod = ti.lod;
    nv->tex_want_mips = ti.want_mips;
    nv->tex_comp = ti.compressed;
    nv->tex_chain = ti.compressed && chain;
    nv->tex_mips = ti.mips;
    nv->tex_bytes = ti.bytes;
    return TRUE;
}

/* What loading nv->tex_src will give, from the file's headers alone
 * (walpe_tex_probe): sizes, premultiplication, whether it stays block
 * compressed. TRUE when the load itself can wait for the first frame: a
 * container whose level 0 is LZ4 (it cannot be an embedded PNG/JPEG, whose
 * real size and format only decoding tells), no sprite sheet. */
static gboolean node_tex_predict (NodeObj *nv)
{
    static int lazy = -1;
    if (lazy < 0) {
        const char *e = g_getenv ("WALPE_TEX_LAZY");
        lazy = !(e && *e == '0');
    }
    WalpeTexProbe pr;
    if (!walpe_tex_probe (nv->tex_src, &pr) || pr.levels < 1)
        return FALSE;
    int wf = pr.we_format;
    int w = MIN (pr.width, pr.tex_width), h = MIN (pr.height, pr.tex_height);
    if (w < 1 || h < 1 || (wf != 0 && wf != 4 && wf != 6 && wf != 7 && wf != 8 && wf != 9))
        return FALSE;
    gboolean dxt = wf == 4 || wf == 6 || wf == 7;
    nv->tex_dxt = dxt && (nv->tex_lf & WALPE_TEX_LOAD_NATIVE_DXT) &&
                  ((w == pr.tex_width && h == pr.tex_height) || (w % 4 == 0 && h % 4 == 0));
    nv->tex_mips = pr.mips;
    if (!lazy || (pr.tex_flags & (WALPE_TEXF_IS_GIF | WALPE_TEXF_VIDEO)))
        return FALSE;
    /* A PNG / JPEG in the container (WALPE_TEX_ASYNC=0: decoded and
     * uploaded at full size right here, as before): the picture's own
     * header says how large the result is, a worker thread starts decoding
     * it now, and the first frame uploads it at the size the layer is drawn
     * at. The decoded picture is RGBA8 whatever the container's format
     * field says, and the loader takes it only from a container it would
     * also read pixels from (tex.c). */
    gboolean pic = FALSE;
    if (pr.picture) {
        int pw = 0, ph = 0;
        if (!walpe_gl_tex_async () || pr.width > 16384 || pr.height > 16384 ||
            (gint64) pr.tex_width * pr.tex_height > (gint64) 8192 * 8192 ||
            !walpe_tex_picture_size (nv->tex_src, &pw, &ph))
            return FALSE;
        pic = TRUE;
        w = pw;
        h = ph;
        wf = 0;
        dxt = nv->tex_dxt = FALSE;
    } else if (!pr.lz4) {
        return FALSE;
    }
    nv->tex_w = nv->tex_full_w = w;
    nv->tex_h = nv->tex_full_h = h;
    nv->tex_rgba = wf == 0 || (dxt && !nv->tex_dxt);
    nv->tex_bpt8 = nv->tex_dxt ? (wf == 7 ? 4 : 8) : wf == 8 ? 16 : wf == 9 ? 8 : 32;
    /* R8/RG88 are read as WE's genericimage reads them: straight alpha */
    nv->premult = nv->tex_dxt ? FALSE
                : wf == 8 || wf == 9 ? !(nv->tex_lf & WALPE_GL_TEX_LAYER_SWIZZLE) : nv->tex_pmflag;
    nv->tex_lazy = TRUE;
    nv->tex_pic = pic;
    /* decoded ahead: what will be needed at level 0 whatever size the
     * layer is drawn at - a picture, or pixels of which the file stores no
     * smaller level that could be read instead */
    if (walpe_gl_tex_async () && !nv->tex_dxt && (pic || (pr.levels < 2 && nv->visible))) {
        nv->tex_job = walpe_gl_tex_predecode (nv->tex_src, nv->tex_lf,
                                              ((gint64) w * h * nv->tex_bpt8) / 8);
    }
    return TRUE;
}

/* the layer's file texture: now at full size, or at the first frame at the
 * size it is drawn at (node_tex_fit / node_fx_input_fit) */
static gboolean node_tex_attach (NodeObj *nv, const char *path, guint lf, gboolean pmflag,
                                 gboolean chain)
{
    node_tex_job_done (nv);
    g_free (nv->tex_src);
    nv->tex_src = g_strdup (path);
    nv->tex_lf = lf;
    nv->tex_pmflag = pmflag;
    nv->tex_dxt = FALSE;
    nv->tex_pic = FALSE;
    if (!node_tex_predict (nv)) {
        WalpeTexInfo ti;
        guint f = lf | (nv->tex_dxt && !chain ? WALPE_GL_TEX_NO_CHAIN : 0);
        GLuint tx = walpe_gl_tex_cached (path, f, GL_CLAMP_TO_EDGE, pmflag, &ti);
        if (!tx) {
            g_clear_pointer (&nv->tex_src, g_free);
            return FALSE;
        }
        nv->tex_ti = ti;
        nv->tex_rgba = ti.format == WALPE_TEX_RGBA8 && ti.channels == 0;
        nv->tex_want_mips = ti.want_mips;
        nv->tex_w = ti.width;
        nv->tex_h = ti.height;
        nv->tex_full_w = ti.full_width;
        nv->tex_full_h = ti.full_height;
        nv->phys_w = ti.phys_w;
        nv->phys_h = ti.phys_h;
        nv->tex = tx;
        nv->premult = ti.premult;
        nv->tex_dxt = nv->tex_comp = ti.compressed;
        nv->tex_chain = ti.compressed && chain;
        nv->tex_mips = ti.mips;
        nv->tex_bytes = ti.bytes;
        nv->tex_lod = nv->tex_min_lod = ti.lod;   /* > 0: the texture budget reduced it */
    }
    nv->tex_ready = TRUE;
    return TRUE;
}

static gboolean load_image_object (SceneNodeContent *s, NodeObj *nv, const char *model_ref,
                                   const char *inst_utex)
{
    static gboolean warned_missing;
    char *mpath = resolve_model_ref (s->dir, model_ref);
    if (!mpath) {
        if (!warned_missing) {
            warned_missing = TRUE;
            g_warning ("scenenode: model %s not found (further misses silent)", model_ref);
        }
        return FALSE;
    }
    JsonNode *mj = wpj_parse_file (mpath);
    if (!mj) {
        g_warning ("scenenode: model %s unparseable", model_ref);
        g_free (mpath);
        return FALSE;
        return FALSE;
    }
    JsonObject *mo = json_node_get_object (mj);
    /* WE builtin: solid color layer = white quad tinted by the object color */
    if (wpj_bool (mo, "solidlayer", FALSE)) {
        json_node_unref (mj);
        g_free (mpath);
        /* a solid layer whose instance names a user texture option shows
         * the user's picture once one is chosen (stretched over the layer) */
        char *utex = user_texture_file (s, inst_utex);
        if (utex) {
            gboolean pmflag = !fx_has_effects (nv->fxset);
            if (node_tex_attach (nv, utex, WALPE_TEX_LOAD_NATIVE_RG, pmflag, TRUE)) {
                g_free (utex);
                return TRUE;
            }
            g_warning ("scenenode: user texture %s cannot be decoded", utex);
            g_free (utex);
        }
        glGenTextures (1, &nv->tex);
        glBindTexture (GL_TEXTURE_2D, nv->tex);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        const guint8 white[4] = { 0xff, 0xff, 0xff, 0xff };
        glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_BGRA, GL_UNSIGNED_BYTE, white);
        nv->premult = TRUE;
        /* effects on a solid layer (audio bars, gradients, ...) render at
         * the layer's size, not at the 1x1 texture's (capped: the chain
         * output is scaled to the quad anyway) */
        float big = MAX (nv->size[0], nv->size[1]);
        float k = big > 2048.0f ? 2048.0f / big : 1.0f;
        nv->tex_w = nv->tex_full_w = MAX (1, walpe_d2i (nv->size[0] * k));
        nv->tex_h = nv->tex_full_h = MAX (1, walpe_d2i (nv->size[1] * k));
        nv->tex_ready = TRUE;
        nv->canvas = TRUE;
        if (nv->size[0] < 1 || nv->size[1] < 1)
            nv->visible = FALSE; /* no size to fill */
        return TRUE;
    }
    /* WE builtin: compose passthrough. Without effects nothing to draw
     * (children render on their own); with effects the layer captures the
     * frame drawn so far (_rt_FullFrameBuffer) under its rect - or the whole
     * frame for fullscreenlayer - and draws it back through the effects */
    if (wpj_bool (mo, "passthrough", FALSE)) {
        gboolean full = wpj_bool (mo, "fullscreen", FALSE);
        json_node_unref (mj);
        g_free (mpath);
        if (!fx_has_effects (nv->fxset))
            return FALSE;
        nv->compose = full ? 2 : 1;
        nv->premult = FALSE;
        nv->tex_ready = TRUE;
        nv->tex_w = nv->tex_full_w = MAX (1, walpe_d2i (nv->size[0]));
        nv->tex_h = nv->tex_full_h = MAX (1, walpe_d2i (nv->size[1]));
        return TRUE;
    }
    char *matref = g_strdup (wpj_str (mo, "material", NULL));
    /* puppet warp: the texture is drawn through a skinned mesh */
    const char *pup = wpj_str (mo, "puppet", NULL);
    if (pup) {
        char *ppath = resolve_model_ref (s->dir, pup);
        if (ppath)
            nv->puppet = walpe_puppet_load (ppath);
        else
            g_message ("scenenode: puppet %s not found (flat layer)", pup);
        g_free (ppath);
    }
    json_node_unref (mj);
    g_free (mpath);
    if (!matref) {
        g_warning ("scenenode: model has no material");
        return FALSE;
    }
    char *matpath = resolve_model_ref (s->dir, matref);
    if (!matpath) {
        g_warning ("scenenode: material %s not found", matref);
        g_free (matref);
        return FALSE;
    }
    JsonNode *matj = wpj_parse_file (matpath);
    char *matdir = g_path_get_dirname (matpath);
    g_free (matpath);
    if (!matj) {
        g_warning ("scenenode: material %s unparseable", matref);
        g_free (matref);
        g_free (matdir);
        return FALSE;
    }
    g_free (matref);
    JsonObject *mo2 = json_node_get_object (matj);
    JsonArray *passes = wpj_arr (mo2, "passes");
    gboolean ok = FALSE;
    if (wpj_arr_len (passes) > 0) {
        JsonObject *pass = wpj_arr_obj (passes, 0);
        if (pass) {
            /* blending string -> blend mode */
            const char *bl = wpj_str (pass, "blending", "translucent");
            if (!g_ascii_strcasecmp (bl, "additive"))
                nv->blend = WALPE_BLEND_ADDITIVE;
            else if (!g_ascii_strcasecmp (bl, "subtractive"))
                nv->blend = WALPE_BLEND_SUBTRACTIVE;
            else if (!g_ascii_strcasecmp (bl, "multiply"))
                nv->blend = WALPE_BLEND_MULTIPLY;
            else if (!g_ascii_strcasecmp (bl, "screen"))
                nv->blend = WALPE_BLEND_SCREEN;
            else if (!g_ascii_strcasecmp (bl, "opaque"))
                nv->blend = WALPE_BLEND_NONE;
            else
                nv->blend = WALPE_BLEND_NORMAL;

            {
                JsonArray *ua = wpj_arr (pass, "usertextures");
                if (!nv->systex && ua && json_array_get_length (ua) > 0)
                    nv->systex = systex_kind (json_array_get_element (ua, 0));
            }
            /* texture: array of names or object with named slots */
            JsonNode *tn = json_object_get_member (pass, "textures");
            char *texname = NULL;
            if (tn && json_node_get_node_type (tn) == JSON_NODE_ARRAY) {
                JsonArray *ta = json_node_get_array (tn);
                if (json_array_get_length (ta) > 0) {
                    JsonNode *e0 = json_array_get_element (ta, 0);
                    if (e0 && json_node_get_node_type (e0) == JSON_NODE_VALUE)
                        texname = g_strdup (json_node_get_string (e0));
                }
            } else if (tn && json_node_get_node_type (tn) == JSON_NODE_OBJECT) {
                JsonObject *to = json_node_get_object (tn);
                texname = g_strdup (wpj_str (to, "texture", wpj_str (to, "diffuse", NULL)));
            }
            if (texname) {
                /* name -> file: as-is, or materials/<name>.tex */
                char *texpath = NULL;
                if (strchr (texname, '/')) {
                    texpath = resolve_asset (s->dir, texname);
                    /* a workshop asset's texture is named with its folder:
                     * "workshop/2652493753/bar" is materials/workshop/
                     * 2652493753/bar.tex (the bars of the script visualizers
                     * 3671993849, 2411409959 had no picture) */
                    if (!texpath) {
                        char *rel = g_strconcat ("materials/", texname, ".tex", NULL);
                        texpath = resolve_asset (s->dir, rel);
                        g_free (rel);
                    }
                } else {
                    char *rel = g_strconcat ("materials/", texname, ".tex", NULL);
                    texpath = resolve_asset (s->dir, rel);
                    g_free (rel);
                    if (!texpath)
                        texpath = resolve_asset (s->dir, texname);
                }
                g_free (texname);
                /* "usertextures": [propid]: a "scenetexture" user property
                 * (e.g. set by a preset to "files/x.jpg", or the user's own
                 * file) replaces slot 0; the object instance's entry wins */
                JsonArray *uta = wpj_arr (pass, "usertextures");
                JsonNode *ut0 = uta && json_array_get_length (uta) > 0
                    ? json_array_get_element (uta, 0) : NULL;
                const char *upid = inst_utex ? inst_utex
                    : ut0 && json_node_get_node_type (ut0) == JSON_NODE_VALUE &&
                      json_node_get_value_type (ut0) == G_TYPE_STRING
                    ? json_node_get_string (ut0) : NULL;
                char *up = user_texture_file (s, upid);
                if (up) {
                    g_free (texpath);
                    texpath = up;
                }
                guint64 voff = 0, vlen = 0;
                int vw = 0, vh = 0;
                if (texpath && walpe_tex_video_range (texpath, &voff, &vlen, &vw, &vh)) {
                    ok = attach_video (s, nv, texpath, voff, vlen, vw, vh);
                    g_clear_pointer (&texpath, g_free);
                }
                if (texpath) {
                    /* GPU-native formats: R8/RG88 always (identical samples;
                     * read as WE's genericimage ConvertTexture0Format reads
                     * them: R8 = white with alpha R, RG88 = luminance R with
                     * alpha G, straight alpha - WALPE_GL_TEX_LAYER_SWIZZLE);
                     * DXT stays block compressed, also under effects: their
                     * passes read straight alpha, which DXT is (it used to
                     * be decoded for them: 4-8 times the memory;
                     * WALPE_TEX_DXT_FX=0 decodes again). */
                    gboolean fxl = fx_has_effects (nv->fxset);
                    guint lf = WALPE_TEX_LOAD_NATIVE_RG | WALPE_GL_TEX_LAYER_SWIZZLE;
                    if (walpe_gl_has_s3tc () && !g_getenv ("WALPE_NO_DXT") &&
                        (!fxl || node_dxt_fx ()))
                        lf |= WALPE_TEX_LOAD_NATIVE_DXT;
                    /* WE effect shaders read and write STRAIGHT alpha:
                     * a layer with effects keeps its texture straight.
                     * Shared with the other monitors' contexts. */
                    ok = node_tex_attach (nv, texpath, lf, !fxl, !fxl);
                    g_free (texpath);
                }
            }
        }
    }
    g_free (matdir);
    json_node_unref (matj);
    return ok;
}

/* A text layer's string is bounded: a script (or a text option, or the
 * scene file) can hand over tens of megabytes, which were copied and laid
 * out by Pango on the render thread on every change (20 M characters:
 * 20 s per frame, 1.5 GB) although nothing wider than 8192 px is drawn.
 * TEXT_MAX_BYTES of UTF-8 is far more than such a texture can show. */
#define TEXT_MAX_BYTES (16 * 1024)

/* length of s up to TEXT_MAX_BYTES, cut at a character boundary */
static gsize text_clamp_len (const char *s)
{
    gsize n = strnlen (s, TEXT_MAX_BYTES + 1);
    if (n <= TEXT_MAX_BYTES)
        return n;
    n = TEXT_MAX_BYTES;
    while (n > 0 && ((guchar) s[n] & 0xC0) == 0x80)
        n--;
    return n;
}

/* text -> premultiplied BGRA texture at the layer's box size (WE renders
 * text into a texture the size of the object, see scenetext.c) */
static void make_text_texture (NodeObj *nv)
{
    if (!nv->text_value || !*nv->text_value || !nv->tstyle)
        return;
    float q[4];
    WALPE_CPH_COUNT (text_renders, 1);
    /* A layer that has its texture gets only the part that can differ from
     * it recoloured and uploaded: where the old or the new string has ink
     * (scenetext.h). A clock in a 2500 x 1900 box used to recolour and
     * upload 19 MB for a few changed digits. `part` is the whole texture
     * for the first string, another size, or a texture that is gone. */
    WalpeTextInk ink;
    int part[4];
    const gboolean have_tex = nv->tex && nv->tex_ready_text && nv->text_ink.w > 0 &&
                              nv->text_ink.w == nv->tex_w && nv->text_ink.h == nv->tex_h;
    cairo_surface_t *sf = walpe_text_render_part (nv->tstyle, nv->text_value, q,
                                                  have_tex ? &nv->text_ink : NULL, &ink, part);
    if (!sf)
        return;
    const int tw = cairo_image_surface_get_width (sf), th = cairo_image_surface_get_height (sf);
    const int pw = part[2], ph = part[3];
    const gboolean whole = pw == tw && ph == th;
    const int sstride = cairo_image_surface_get_stride (sf);
    guint8 *const part_px = cairo_image_surface_get_data (sf) + (gsize) part[1] * sstride +
                            (gsize) part[0] * 4;
    memcpy (nv->tquad, q, sizeof q);
    gboolean straight = fx_has_effects (nv->fxset);
    if (straight) {
        /* effects take straight alpha, and WE renders the glyphs in the
         * layer color BEFORE its effects (outline/recolor effects such as
         * 3163060610's read the glyph color or only its alpha; tinting the
         * effect output afterwards turned their colors black):
         * premultiplied white -> (color, a) */
        guint8 cb[3];
        for (int c = 0; c < 3; c++) {
            nv->text_tint[c] = nv->tint[c];
            cb[c] = (guint8) CLAMP (nv->tint[c] * nv->bright * 255.0f + 0.5f, 0.0f, 255.0f);
        }
        nv->text_baked = TRUE;
        guint8 *px = part_px;   /* (all the ink is in the part) */
        int stride = sstride;
        /* a pixel is one word (ARGB32 in memory order B G R A is native
         * endian 0xAARRGGBB): where alpha is not 0, the colour under the
         * pixel's own alpha. Whole words without a branch per byte, so
         * the compiler can do several pixels at once (it was a third of
         * a text update's own time). */
        const guint32 cw = (guint32) cb[0] << 16 | (guint32) cb[1] << 8 | (guint32) cb[2];
        for (int y = 0; y < ph; y++) {
            guint32 *row = (guint32 *) (void *) (px + (gsize) y * stride);
            for (int x = 0; x < pw; x++) {
                const guint32 v = row[x];
                row[x] = (v & 0xff000000u) ? ((v & 0xff000000u) | cw) : v;
            }
        }
    } else {
        nv->text_baked = FALSE;
    }
    /* a changed string of the same size goes into the texture the layer
     * has (a clock or counter updates every second or every frame: the
     * texture used to be deleted and made anew, mip chain included) */
    if (nv->tex && (nv->tex_w != tw || nv->tex_h != th || (!whole && !have_tex))) {
        glDeleteTextures (1, &nv->tex);
        nv->tex = 0;
    }
    gboolean fresh = !nv->tex;
    if (fresh && !whole) {
        /* (cannot happen: a part is only rendered for a texture that is there) */
        cairo_surface_destroy (sf);
        nv->text_ink.w = 0;
        nv->tex_ready_text = FALSE;
        return;
    }
    if (fresh) {
        glGenTextures (1, &nv->tex);
        glBindTexture (GL_TEXTURE_2D, nv->tex);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glBindTexture (GL_TEXTURE_2D, nv->tex);
    }
    glPixelStorei (GL_UNPACK_ROW_LENGTH, cairo_image_surface_get_stride (sf) / 4);
    glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
    if (fresh)
        glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, tw, th, 0, GL_BGRA, GL_UNSIGNED_BYTE,
                      cairo_image_surface_get_data (sf));
    else
        glTexSubImage2D (GL_TEXTURE_2D, 0, part[0], part[1], pw, ph, GL_BGRA, GL_UNSIGNED_BYTE,
                         part_px);
    glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei (GL_UNPACK_ALIGNMENT, 4);
    /* text is rendered at the scene's size and drawn at the monitor's:
     * beyond 2:1 (a zoomed-out camera, a 4K scene on a small monitor)
     * glyph strokes broke up without mipmaps. Up to 2:1 it keeps level 0
     * (bias -1): bilinear is clean there, and pixel fonts stay as crisp
     * as they were (3122339805's calendar). The chain is therefore only
     * built once the layer is drawn smaller than 2:1 (the render loop
     * does that, text_mips); until then level 0 is sampled exactly as the
     * biased chain would be. With a chain, every new string rebuilds it. */
    if (fresh)
        nv->text_mips = !walpe_gl_tex_defer_mipmaps ();
    if (nv->text_mips) {
        walpe_gl_tex_gen_mipmaps (tw, th);
        walpe_gl_tex_lod_bias (-1.0f);
    }
    cairo_surface_destroy (sf);
    /* a new texture may be refused (size); an update of one that exists
     * cannot be, and needs no round trip to the driver */
    if (!fresh || glGetError () == GL_NO_ERROR) {
        nv->premult = !straight;
        nv->tex_w = tw;
        nv->tex_h = th;
        nv->tex_ready = TRUE;
        nv->text_ink = ink;
        nv->tex_ready_text = TRUE;
    } else {
        nv->text_ink.w = 0;
        nv->tex_ready_text = FALSE;
    }
}

/* ---------------------------------------------------------------- parse */

/* Tier 4: user property VALUES ({propid: value}) from project.json
 * "general"."properties", falling back to the scene root, overlaid with
 * the user's persisted overrides from config.json. Copied out so the
 * temp project doc can be dropped. */
/* how a stored override (always a string) of option `o` is read when
 * project.json gives it no scalar default to copy the type from: by its
 * declared type. 'b' bool, 'n' number, 's' string (text, colour, file,
 * directory, scenetexture; a combo by the type of its option values). */
static char user_prop_kind (JsonObject *o)
{
    const char *type = wpj_str (o, "type", "");
    if (!g_ascii_strcasecmp (type, "bool"))
        return 'b';
    if (!g_ascii_strcasecmp (type, "slider"))
        return 'n';
    if (!g_ascii_strcasecmp (type, "combo")) {
        JsonArray *opts = wpj_arr (o, "options");
        JsonNode *e = opts && json_array_get_length (opts) > 0 ? json_array_get_element (opts, 0) : NULL;
        JsonNode *v = e && json_node_get_node_type (e) == JSON_NODE_OBJECT
            ? json_object_get_member (json_node_get_object (e), "value") : NULL;
        if (v && json_node_get_node_type (v) == JSON_NODE_VALUE) {
            GType vt = json_node_get_value_type (v);
            return vt == G_TYPE_BOOLEAN ? 'b' : vt == G_TYPE_STRING ? 's' : 'n';
        }
    }
    return 's';
}

static JsonObject *collect_user_props (const char *dir, const char *origin, JsonObject *ro,
                                       JsonObject *preset, JsonObject **types_out)
{
    JsonObject *out = json_object_new ();
    JsonObject *types = json_object_new ();
    *types_out = types;
    JsonObject *src = wpj_obj (wpj_obj (ro, "general"), "properties");
    JsonNode *pj = NULL;
    if (!src) {
        char *pp = g_build_filename (dir, "project.json", NULL);
        pj = wpj_parse_file (pp);
        g_free (pp);
        if (pj && json_node_get_node_type (pj) == JSON_NODE_OBJECT)
            src = wpj_obj (wpj_obj (json_node_get_object (pj), "general"),
                           "properties");
    }
    if (src) {
        GList *ks = json_object_get_members (src);
        for (GList *k = ks; k; k = k->next) {
            /* an option that is no object ("properties": {"x": 5}) has no
             * default: json-glib was asked for a member of NULL (a CRITICAL
             * in the log for every such entry of a malformed project.json) */
            JsonObject *po = wpj_obj (src, k->data);
            JsonNode *vn = po ? json_object_get_member (po, "value") : NULL;
            if (vn)
                json_object_set_member (out, (char *) k->data,
                                        json_node_copy (vn));
            char *ty = g_ascii_strdown (wpj_str (wpj_obj (src, k->data), "type", ""), -1);
            json_object_set_string_member (types, (char *) k->data, ty);
            g_free (ty);
        }
        g_list_free (ks);
    }
    /* a WE preset's values replace the base item's defaults - of every
     * option the item declares, with or without a default of its own */
    if (preset) {
        GList *ks = json_object_get_members (preset);
        for (GList *k = ks; k; k = k->next) {
            JsonNode *pv = json_object_get_member (preset, k->data);
            if (pv && json_node_get_node_type (pv) == JSON_NODE_VALUE &&
                (json_object_has_member (out, k->data) ||
                 (src && json_object_has_member (src, k->data))))
                json_object_set_member (out, (char *) k->data, json_node_copy (pv));
        }
        g_list_free (ks);
    }
    /* persisted user overrides win (bools/numbers parsed back by the
     * consumers; values stored as strings), keyed by the wallpaper path
     * the daemon opened (workshop/project dir for plain and pkg loads) */
    const char *wp = origin;
    if (wp) {
        /* every declared option, also one without a default (an option
         * with "value" missing or null used to drop the user's value) */
        GList *copy = src ? json_object_get_members (src) : json_object_get_members (out);
        for (GList *k = copy; k; k = k->next) {
            char *ov = walpe_config_get_prop (wp, k->data);
            if (!ov)
                continue;
            /* coerce by the existing default's type, else by the declared one */
            JsonNode *cur = json_object_get_member (out, k->data);
            char kind;
            if (cur && json_node_get_node_type (cur) == JSON_NODE_VALUE) {
                GType vt = json_node_get_value_type (cur);
                kind = vt == G_TYPE_BOOLEAN ? 'b' : vt == G_TYPE_STRING ? 's' : 'n';
            } else if (cur && json_node_get_node_type (cur) != JSON_NODE_NULL) {
                kind = 0;   /* an object / array default: left alone, as before */
            } else {
                kind = src ? user_prop_kind (wpj_obj (src, k->data)) : 0;
            }
            if (kind) {
                JsonNode *nn = json_node_new (JSON_NODE_VALUE);
                if (kind == 'b')
                    json_node_set_boolean (nn, !g_ascii_strcasecmp (ov, "true") ||
                                               !g_strcmp0 (ov, "1"));
                else if (kind == 'n')
                    json_node_set_double (nn, g_ascii_strtod (ov, NULL));
                else
                    json_node_set_string (nn, ov);
                json_object_set_member (out, k->data, nn);
            }
            g_free (ov);
        }
        g_list_free (copy);
    }
    if (pj)
        json_node_unref (pj);
    return out;
}

/* "visible" may be a boolean or a user-property binding
 * {"user": "propid", "value": fallback} (Tier 4) */
static gboolean node_visible (JsonObject *o, JsonObject *user_props)
{
    JsonNode *visn = json_object_get_member (o, "visible");
    if (!visn || json_node_get_node_type (visn) != JSON_NODE_OBJECT)
        return wpj_bool (o, "visible", TRUE);
    JsonObject *vo = json_node_get_object (visn);
    JsonObject *cond = wpj_obj (vo, "user");
    if (cond) {
        /* combo-bound visibility: {"user": {"name": prop, "condition":
         * "1"}} = visible while the property's value equals condition */
        const char *name = wpj_str (cond, "name", NULL);
        const char *want = wpj_str (cond, "condition", NULL);
        JsonNode *pv = name ? json_object_get_member (user_props, name) : NULL;
        if (!want || !pv || json_node_get_node_type (pv) != JSON_NODE_VALUE)
            return wpj_bool (vo, "value", TRUE);
        walpe_probe_use (name, "visible");
        char buf[G_ASCII_DTOSTR_BUF_SIZE];
        const char *have;
        GType vt = json_node_get_value_type (pv);
        if (vt == G_TYPE_STRING)
            have = json_node_get_string (pv);
        else if (vt == G_TYPE_BOOLEAN)
            have = json_node_get_boolean (pv) ? "1" : "0";
        else if (vt == G_TYPE_INT64)
            have = (g_snprintf (buf, sizeof (buf), "%" G_GINT64_FORMAT, json_node_get_int (pv)), buf);
        else
            have = (g_snprintf (buf, sizeof (buf), "%g", json_node_get_double (pv)), buf);
        if (!g_strcmp0 (have, want))
            return TRUE;
        /* numeric compare ("1" vs 1.0) */
        char *e1 = NULL, *e2 = NULL;
        double a = g_ascii_strtod (have, &e1), b = g_ascii_strtod (want, &e2);
        return e1 && !*e1 && e2 && !*e2 && e1 != have && a == b;
    }
    const char *upid = wpj_str (vo, "user", NULL);
    if (!upid)
        return wpj_bool (vo, "value", TRUE);
    JsonNode *pv = json_object_get_member (user_props, upid);
    if (pv && json_node_get_node_type (pv) == JSON_NODE_VALUE &&
        json_node_get_value_type (pv) == G_TYPE_BOOLEAN) {
        walpe_probe_use (upid, "visible");
        return json_node_get_boolean (pv);
    }
    return wpj_bool (vo, "value", TRUE);
}

static void parse_nodes (JsonArray *arr, GArray *out, const char *dir, SceneNodeContent *s);

/* scripted property, compiled once GL/context exist (gl_init) */
typedef struct {
    int idx;
    int effect, pass;    /* -1 = a layer property; else effect index (and
                          * material constant pass, -1 = effect visible) */
    char *prop;
    char *src;
    char *init;          /* effect constant: static value as JSON */
    JsonObject *sp;      /* scriptproperties (ref) or NULL */
    gboolean anim;       /* property animation: src = {"animation"} JSON,
                          * init = the static value (JSON) */
    gboolean inert;      /* a script on a property this renderer does not drive:
                          * it runs (init, events, shared state) with its static
                          * value (init, JSON); what it returns is kept in JS only */
} PendingScript;

static void pending_script_free (gpointer p)
{
    PendingScript *ps = p;
    g_free (ps->prop);
    g_free (ps->src);
    g_free (ps->init);
    if (ps->sp)
        json_object_unref (ps->sp);
    g_free (ps);
}

/* a property that may be wrapped as {"script": ..., "value": v} (or a
 * user binding {"user": ..., "value": v}): the plain/static value */
/* user property values of the scene being parsed (parse runs on one
 * thread, synchronously, inside walpe_scenenode_new_from_root) */
static __thread JsonObject *parse_user_props;

static JsonNode *prop_node (JsonObject *o, const char *k)
{
    JsonNode *n = o ? json_object_get_member (o, k) : NULL;
    if (n && json_node_get_node_type (n) == JSON_NODE_OBJECT) {
        JsonObject *b = json_node_get_object (n);
        /* {"user": propid, "value": v}: the user's (or preset's) value */
        const char *up = wpj_str (b, "user", NULL);
        JsonNode *pv = up && parse_user_props ? json_object_get_member (parse_user_props, up) : NULL;
        if (pv && json_node_get_node_type (pv) == JSON_NODE_VALUE)
            walpe_probe_use (up, k);
        n = pv && json_node_get_node_type (pv) == JSON_NODE_VALUE
            ? pv : json_object_get_member (b, "value");
    }
    return n && json_node_get_node_type (n) == JSON_NODE_VALUE ? n : NULL;
}

static const char *prop_str (JsonObject *o, const char *k)
{
    JsonNode *n = prop_node (o, k);
    return n && json_node_get_value_type (n) == G_TYPE_STRING ? json_node_get_string (n) : NULL;
}

/* a vector property ("x y z"): a user slider bound to it holds ONE number,
 * which WE applies to every component (3465215190's clock size, 3042492564's
 * constellation size: "scale": {"user": slider, "value": "1 1 1"}); the
 * string form was required and the option did nothing */
static void prop_vec (JsonObject *o, const char *k, float *v, int n, const float *dflt)
{
    JsonNode *nd = prop_node (o, k);
    if (nd && json_node_get_value_type (nd) != G_TYPE_STRING &&
        json_node_get_value_type (nd) != G_TYPE_BOOLEAN) {
        double d = json_node_get_double (nd);
        for (int i = 0; i < n; i++)
            v[i] = isfinite (d) ? (float) d : dflt[i];
        return;
    }
    const char *str = nd && json_node_get_value_type (nd) == G_TYPE_STRING ? json_node_get_string (nd) : NULL;
    /* "0.5" alone (a text-typed override of a slider): uniform as well */
    if (str && n > 1) {
        char *end = NULL;
        double d = g_ascii_strtod (str, &end);
        while (end && g_ascii_isspace (*end))
            end++;
        if (end && end != str && !*end && isfinite (d)) {
            for (int i = 0; i < n; i++)
                v[i] = (float) d;
            return;
        }
    }
    parse_vec (str, v, n, dflt);
}

/* a text layer's string: plain, {"value": s} or bound to a user text
 * option {"user": prop, "value": s} (2929592935's console lines,
 * 3122339805's window titles). Numbers/booleans print as text. g_free. */
static char *prop_text (JsonObject *o, const char *k)
{
    JsonNode *n = prop_node (o, k);
    if (!n)
        return NULL;
    GType t = json_node_get_value_type (n);
    if (t == G_TYPE_STRING)
        return g_strdup (json_node_get_string (n));
    if (t == G_TYPE_BOOLEAN)
        return g_strdup ("");   /* a "text" option without a string value */
    if (t == G_TYPE_INT64)
        return g_strdup_printf ("%" G_GINT64_FORMAT, json_node_get_int (n));
    char buf[G_ASCII_DTOSTR_BUF_SIZE];
    return g_strdup (g_ascii_formatd (buf, sizeof (buf), "%.6g", json_node_get_double (n)));
}

static double prop_num (JsonObject *o, const char *k, double d)
{
    JsonNode *n = prop_node (o, k);
    if (!n)
        return d;
    GType t = json_node_get_value_type (n);
    if (t == G_TYPE_STRING)
        return g_ascii_strtod (json_node_get_string (n), NULL);
    if (t == G_TYPE_BOOLEAN)
        return json_node_get_boolean (n);
    return json_node_get_double (n);
}

/* remember {"script": ...} on a supported layer property of node idx */
static void collect_script (SceneNodeContent *s, int idx, JsonObject *o, const char *k,
                            const char *as_prop)
{
    JsonNode *n = o ? json_object_get_member (o, k) : NULL;
    if (!n || json_node_get_node_type (n) != JSON_NODE_OBJECT)
        return;
    JsonObject *so = json_node_get_object (n);
    const char *src = wpj_str (so, "script", NULL);
    if (!src || !walpe_ss_supported_prop (as_prop))
        return;
    PendingScript *ps = g_new0 (PendingScript, 1);
    ps->idx = idx;
    ps->effect = ps->pass = -1;
    ps->prop = g_strdup (as_prop);
    ps->src = g_strdup (src);
    JsonObject *sp = wpj_obj (so, "scriptproperties");
    ps->sp = sp ? json_object_ref (sp) : NULL;
    if (!s->pending_scripts)
        s->pending_scripts = g_ptr_array_new_with_free_func (pending_script_free);
    g_ptr_array_add (s->pending_scripts, ps);
}

/* {"script": ...} on any other property of node idx (-1 = the scene's
 * "general" section): parallaxDepth, size, volume, particle instance
 * overrides, the camera zoom ... The renderer has no such property to
 * drive, but WE authors park general-purpose code on whatever property is
 * at hand: its init(), the shared state it sets up and its cursor / media
 * handlers must run, or the scripts that depend on them fail. */
static void collect_inert_script (SceneNodeContent *s, int idx, JsonObject *o, const char *k)
{
    JsonNode *n = o ? json_object_get_member (o, k) : NULL;
    if (!n || json_node_get_node_type (n) != JSON_NODE_OBJECT)
        return;
    JsonObject *so = json_node_get_object (n);
    const char *src = wpj_str (so, "script", NULL);
    if (!src)
        return;
    PendingScript *ps = g_new0 (PendingScript, 1);
    ps->inert = TRUE;
    ps->idx = idx;
    ps->effect = ps->pass = -1;
    ps->prop = g_strdup (k);
    ps->src = g_strdup (src);
    JsonNode *vn = json_object_get_member (so, "value");
    if (vn && json_node_get_node_type (vn) == JSON_NODE_VALUE)
        ps->init = json_to_string (vn, FALSE);
    JsonObject *sp = wpj_obj (so, "scriptproperties");
    ps->sp = sp ? json_object_ref (sp) : NULL;
    if (!s->pending_scripts)
        s->pending_scripts = g_ptr_array_new_with_free_func (pending_script_free);
    g_ptr_array_add (s->pending_scripts, ps);
}

static void collect_inert_scripts (SceneNodeContent *s, int idx, JsonObject *o,
                                   const char *const *driven)
{
    GList *ks = o ? json_object_get_members (o) : NULL;
    for (GList *k = ks; k; k = k->next) {
        gboolean skip = !strcmp (k->data, "text") || !strcmp (k->data, "effects");
        for (int d = 0; driven && driven[d] && !skip; d++)
            skip = !strcmp (k->data, driven[d]);
        if (!skip)
            collect_inert_script (s, idx, o, k->data);
    }
    g_list_free (ks);
}

/* {"animation": {...}, "value": v} on a layer property / effect constant */
static void collect_anim (SceneNodeContent *s, int idx, int effect, int pass, JsonObject *o,
                          const char *k)
{
    JsonObject *po = o ? wpj_obj (o, k) : NULL;
    JsonNode *an = po ? json_object_get_member (po, "animation") : NULL;
    if (!an || json_node_get_node_type (an) != JSON_NODE_OBJECT)
        return;
    PendingScript *ps = g_new0 (PendingScript, 1);
    ps->anim = TRUE;
    ps->idx = idx;
    ps->effect = effect;
    ps->pass = pass;
    ps->prop = g_strdup (k);
    ps->src = json_to_string (an, FALSE);
    JsonNode *iv = json_object_get_member (po, "value");
    ps->init = iv && json_node_get_node_type (iv) == JSON_NODE_VALUE ? json_to_string (iv, FALSE) : NULL;
    if (!s->pending_scripts)
        s->pending_scripts = g_ptr_array_new_with_free_func (pending_script_free);
    g_ptr_array_add (s->pending_scripts, ps);
}

/* instanceoverride value: number, numeric string, or a user-property
 * binding {"user": prop, "value": fallback} */
static JsonNode *override_node (JsonObject *io, const char *k, JsonObject *user_props)
{
    JsonNode *n = json_object_get_member (io, k);
    if (n && json_node_get_node_type (n) == JSON_NODE_OBJECT) {
        JsonObject *b = json_node_get_object (n);
        const char *up = wpj_str (b, "user", NULL);
        JsonNode *pv = up && user_props ? json_object_get_member (user_props, up) : NULL;
        if (pv)
            walpe_probe_use (up, "particle");
        n = pv ? pv : json_object_get_member (b, "value");
    }
    return n && json_node_get_node_type (n) == JSON_NODE_VALUE ? n : NULL;
}

static float override_num (JsonObject *io, const char *k, JsonObject *user_props, float d)
{
    JsonNode *n = override_node (io, k, user_props);
    if (!n)
        return d;
    GType t = json_node_get_value_type (n);
    double v;
    if (t == G_TYPE_STRING) {
        /* text that is no number (an option of another type bound here)
         * is "not set": as 0 it switched the system off (count), stopped
         * it (rate) or made it invisible (size, alpha) */
        const char *str = json_node_get_string (n);
        char *end = NULL;
        v = g_ascii_strtod (str, &end);
        if (end == str)
            return d;
    } else if (t == G_TYPE_BOOLEAN) {
        return json_node_get_boolean (n) ? 1.0f : 0.0f;
    } else {
        v = json_node_get_double (n);
    }
    /* 1e999 in a file is infinity: as a count it ran the emitter's loop to
     * its limit every frame, as a size it drew quads without bounds */
    if (!isfinite (v))
        return d;
    return (float) CLAMP (v, -1e6, 1e6);
}

static void override_vec3 (JsonObject *io, const char *k, JsonObject *user_props, float *v)
{
    JsonNode *n = override_node (io, k, user_props);
    if (n && json_node_get_value_type (n) == G_TYPE_STRING) {
        float t[3];
        if (sscanf (json_node_get_string (n), "%f %f %f", &t[0], &t[1], &t[2]) == 3)
            for (int i = 0; i < 3; i++)
                v[i] = t[i];
    }
}

static void parse_node (NodeObj *nv, JsonObject *o, const char *dir, SceneNodeContent *s, GArray *out, int depth)
{
    static const float one[2] = { 1.0f, 1.0f };
    static const float zero3[3] = { 0.0f, 0.0f, 0.0f };
    memset (nv, 0, sizeof (*nv));
    nv->video = -1;
    nv->id = walpe_d2i (wpj_num (o, "id", -1));
    nv->parent_id = walpe_d2i (wpj_num (o, "parent", -1));
    nv->parent = -1;
    nv->visible = node_visible (o, s->user_props);
    nv->solid = wpj_bool (o, "solid", TRUE);
    nv->alpha = (float) prop_num (o, "alpha", 1.0);
    nv->name = g_strdup (wpj_str (o, "name", NULL));
    {
        const char *at = wpj_str (o, "attachment", NULL);
        if (at && *at)
            nv->attach = g_strdup (at);
    }
    if (!s->cam_found && wpj_str (o, "camera", NULL)) {
        double z = prop_num (o, "zoom", 1.0);
        s->cam_found = TRUE;
        s->cam_id = nv->id;
        s->cam_zoom = isfinite (z) && z > 0.01 ? (float) z : 1.0f;
        g_debug ("scenenode: camera layer %d, zoom %.3f", nv->id, s->cam_zoom);
    }
    {
        int idx = (int) out->len;  /* flat objects: appended right after */
        static const char *const sprops[] = { "visible", "alpha", "origin", "scale",
                                              "angles", "color", "pointsize", "maxwidth",
                                              "padding", NULL };
        for (int k = 0; sprops[k]; k++)
            collect_script (s, idx, o, sprops[k], sprops[k]);
        collect_inert_scripts (s, idx, o, sprops);
        static const char *const aprops[] = { "origin", "scale", "angles", "alpha", "color", NULL };
        for (int k = 0; aprops[k]; k++)
            collect_anim (s, idx, -1, -1, o, aprops[k]);
        JsonObject *to = wpj_obj (o, "text");
        if (to && wpj_str (to, "script", NULL)) {
            /* text: {"script", "scriptproperties", "value"} is the object itself */
            PendingScript *ps = g_new0 (PendingScript, 1);
            ps->idx = idx;
            ps->effect = ps->pass = -1;
            ps->prop = g_strdup ("text");
            ps->src = g_strdup (wpj_str (to, "script", NULL));
            JsonObject *sp = wpj_obj (to, "scriptproperties");
            ps->sp = sp ? json_object_ref (sp) : NULL;
            if (!s->pending_scripts)
                s->pending_scripts = g_ptr_array_new_with_free_func (pending_script_free);
            g_ptr_array_add (s->pending_scripts, ps);
        }
    }
    nv->blend = WALPE_BLEND_NORMAL;

    float v3[3];
    parse_vec (prop_str (o, "origin"), v3, 2, (const float[2]) { 0.0f, 0.0f });
    nv->origin[0] = v3[0];
    nv->origin[1] = v3[1];
    parse_vec (prop_str (o, "angles"), v3, 3, zero3);
    nv->angle_z = v3[2];
    if (fabsf (v3[0]) > 0.01f || fabsf (v3[1]) > 0.01f) {
        if (!s->warned_tilt) {
            s->warned_tilt = TRUE;
            g_debug ("scenenode: 3D tilt angles present (2D approximation ignores x/y)");
        }
    }
    prop_vec (o, "scale", v3, 2, one);
    /* negative = mirrored (WE flips sprites/emitters this way); only an
     * exact 0 falls back to 1 */
    nv->scale[0] = v3[0] != 0 ? v3[0] : 1.0f;
    nv->scale[1] = v3[1] != 0 ? v3[1] : 1.0f;
    {
        float z3[3];
        /* prop_vec: a slider bound to "scale" is one number for x, y and z */
        prop_vec (o, "scale", z3, 3, (const float[3]) { 1.0f, 1.0f, 1.0f });
        nv->scale_z = z3[2] != 0 ? z3[2] : 1.0f;
        parse_vec (prop_str (o, "origin"), z3, 3, zero3);
        nv->origin_z = z3[2];
    }
    parse_vec (wpj_str (o, "size", NULL), nv->size, 2, (const float[2]) { 0.0f, 0.0f });

    float c3[3];
    parse_vec (prop_str (o, "color"), c3, 3, (const float[3]) { 1.0f, 1.0f, 1.0f });
    /* "0 0 0" is a valid color (black window backgrounds): only missing
     * or malformed components fall back to white */
    for (int i = 0; i < 3; i++)
        nv->tint[i] = isfinite (c3[i]) ? CLAMP (c3[i], 0.0f, 16.0f) : 1.0f;
    {
        double br = prop_num (o, "brightness", 1.0);
        nv->bright = isfinite (br) ? (float) CLAMP (br, 0.0, 16.0) : 1.0f;
    }

    /* WE layer blend mode (the editor's blend list, see glutil.h); applied
     * after the material is loaded: a non-normal layer mode overrides the
     * material's "blending" (solid layers set to Add were drawn as opaque
     * black: 3219908811) */
    int cbm = walpe_d2i (wpj_num (o, "colorBlendMode", 0.0));

    JsonArray *fx = wpj_arr (o, "effects");
    if (fx) {
        /* effect "visible" can be bound to a user property ({"user":
         * "blush", "value": false}): only enabled instances are built */
        int idx = (int) out->len;
        JsonArray *on = json_array_new ();
        for (guint i = 0; i < json_array_get_length (fx); i++) {
            JsonObject *ei = wpj_arr_obj (fx, i);
            if (!ei)
                continue;
            /* SceneScript on the effect's visibility / its material
             * constants (run even when the effect is not built: their
             * init() often sets up shared state) */
            JsonObject *vo = wpj_obj (ei, "visible");
            const char *vsrc = vo ? wpj_str (vo, "script", NULL) : NULL;
            if (vsrc) {
                PendingScript *ps = g_new0 (PendingScript, 1);
                ps->idx = idx;
                ps->effect = (int) i;
                ps->pass = -1;
                ps->prop = g_strdup ("visible");
                ps->src = g_strdup (vsrc);
                JsonObject *sp = wpj_obj (vo, "scriptproperties");
                ps->sp = sp ? json_object_ref (sp) : NULL;
                if (!s->pending_scripts)
                    s->pending_scripts = g_ptr_array_new_with_free_func (pending_script_free);
                g_ptr_array_add (s->pending_scripts, ps);
            }
            JsonArray *ipasses = wpj_arr (ei, "passes");
            for (guint pi = 0; pi < wpj_arr_len (ipasses); pi++) {
                JsonObject *csv = wpj_obj (wpj_arr_obj (ipasses, pi), "constantshadervalues");
                GList *ks = csv ? json_object_get_members (csv) : NULL;
                for (GList *k = ks; k; k = k->next) {
                    collect_anim (s, idx, (int) i, (int) pi, csv, k->data);
                    JsonObject *co = wpj_obj (csv, k->data);
                    const char *csrc = co ? wpj_str (co, "script", NULL) : NULL;
                    if (!csrc)
                        continue;
                    PendingScript *ps = g_new0 (PendingScript, 1);
                    ps->idx = idx;
                    ps->effect = (int) i;
                    ps->pass = (int) pi;
                    ps->prop = g_strdup (k->data);
                    ps->src = g_strdup (csrc);
                    JsonNode *iv = json_object_get_member (co, "value");
                    ps->init = iv && json_node_get_node_type (iv) == JSON_NODE_VALUE
                        ? json_to_string (iv, FALSE) : NULL;
                    JsonObject *sp = wpj_obj (co, "scriptproperties");
                    ps->sp = sp ? json_object_ref (sp) : NULL;
                    if (!s->pending_scripts)
                        s->pending_scripts = g_ptr_array_new_with_free_func (pending_script_free);
                    g_ptr_array_add (s->pending_scripts, ps);
                }
                g_list_free (ks);
            }
            gboolean vis = node_visible (ei, s->user_props);
            if (!vis && !vsrc)
                continue;
            /* shallow copy carrying the effect's index and, for a
             * scripted visibility, its initial state */
            JsonObject *cp = json_object_new ();
            GList *ms = json_object_get_members (ei);
            for (GList *m = ms; m; m = m->next)
                if (strcmp (m->data, "visible"))
                    json_object_set_member (cp, m->data,
                                            json_node_copy (json_object_get_member (ei, m->data)));
            g_list_free (ms);
            json_object_set_boolean_member (cp, "visible", TRUE);
            json_object_set_int_member (cp, "__walpe_index", (gint64) i);
            json_object_set_boolean_member (cp, "__walpe_enabled", vis);
            json_array_add_object_element (on, cp);
        }
        nv->fxset = fx_build_for_object (dir, on, wpj_str (o, "name", "?"), s->user_props);
        s->fx_count += json_array_get_length (on);
        json_array_unref (on);
    }

    const char *image = wpj_str (o, "image", NULL);
    const char *particle = wpj_str (o, "particle", NULL);
    gboolean inst_scripts = FALSE;   /* instance override scripts were collected */
    {
        /* WE object anchor ("alignment": topleft, left, bottomright, ...):
         * the origin is that point of the layer, not its center
         * (3122339805's window bars / borders / video panes) */
        const char *al = wpj_str (o, "alignment", NULL);
        if (al && image) {
            if (strstr (al, "left"))
                nv->anchor[0] = -1.0f;
            else if (strstr (al, "right"))
                nv->anchor[0] = 1.0f;
            if (strstr (al, "top"))
                nv->anchor[1] = 1.0f;
            else if (strstr (al, "bottom"))
                nv->anchor[1] = -1.0f;
        }
    }
    JsonObject *text = wpj_obj (o, "text");
    /* older text layers store the string itself: "text": "Host:" */
    const char *text_plain = !text ? wpj_str (o, "text", NULL) : NULL;
    if (image && depth <= SCENENODE_MAX_DEPTH) {
        nv->kind = NK_IMAGE;
        JsonArray *iua = wpj_arr (wpj_obj (o, "instance"), "usertextures");
        const char *inst_utex = NULL;
        if (iua && json_array_get_length (iua) > 0) {
            JsonNode *u0 = json_array_get_element (iua, 0);
            nv->systex = systex_kind (u0);
            /* a string names a "scenetexture" user option (3161790118's
             * "Own Background", 3163060610's external picture) */
            if (u0 && json_node_get_node_type (u0) == JSON_NODE_VALUE &&
                json_node_get_value_type (u0) == G_TYPE_STRING)
                inst_utex = json_node_get_string (u0);
        }
        if (!load_image_object (s, nv, image, inst_utex) && nv->size[0] < 1)
            nv->visible = FALSE;
        if (cbm != 0)
            nv->blend = walpe_blend_from_we (cbm);
        /* A solid layer (a flat colour) and a compose layer (a copy of
         * the scene behind it) have no picture of their own: what they
         * show is what their effects make of them (a glow, audio bars, a
         * blur of the background). If one of those effects cannot be
         * built, the bare layer would be an opaque coloured rectangle or
         * a second copy of the scene, so the layer is not drawn at all.
         * Layers with a picture, text or video keep showing it without
         * the broken effect. Effects that are switched off do not count. */
        if ((nv->canvas || nv->compose) && fx_failed_count (nv->fxset) > 0) {
            nv->fx_skip = TRUE;
            g_message ("scenenode: layer \"%s\" (id %d) not drawn: %d of its effects could not "
                       "be built and a %s layer has no picture of its own",
                       nv->name ? nv->name : "?", nv->id, fx_failed_count (nv->fxset),
                       nv->canvas ? "solid" : "compose");
        }
        if (nv->puppet)
            walpe_puppet_set_layers (nv->puppet, wpj_arr (o, "animationlayers"), s->user_props);
    } else if (particle && depth <= SCENENODE_MAX_DEPTH) {
        nv->kind = NK_PARTICLE;
        char *ppath = resolve_asset (dir, particle);
        if (!ppath && walpe_we_assets_dir ())
            ppath = resolve_asset (walpe_we_assets_dir (), particle);
        if (ppath) {
            JsonNode *pj = wpj_parse_file (ppath);
            g_free (ppath);
            if (pj && json_node_get_node_type (pj) == JSON_NODE_OBJECT &&
                json_object_has_member (json_node_get_object (pj), "emitter")) {
                /* WE particle system + the object's instance overrides */
                WalpeWeParticleOverride ov;
                walpe_we_particle_override_default (&ov);
                JsonObject *io = wpj_obj (o, "instanceoverride");
                if (io) {
                    ov.rate = override_num (io, "rate", s->user_props, 1);
                    ov.count = override_num (io, "count", s->user_props, 1);
                    ov.size = override_num (io, "size", s->user_props, 1);
                    ov.lifetime = override_num (io, "lifetime", s->user_props, 1);
                    ov.alpha = override_num (io, "alpha", s->user_props, 1);
                    ov.speed = override_num (io, "speed", s->user_props, 1);
                    override_vec3 (io, "colorn", s->user_props, ov.color);
                    /* scenes made before Wallpaper Engine had "colorn"
                     * (normalized) store the instance colour as "color"
                     * in 0..255, like a particle file's colorrandom:
                     * "241 64 30" (1378563229's orange fireflies were
                     * drawn in the file's own colour). "colorn" wins
                     * when a scene has both. */
                    if (!json_object_has_member (io, "colorn") &&
                        override_node (io, "color", s->user_props)) {
                        float c255[3] = { 255.0f, 255.0f, 255.0f };
                        override_vec3 (io, "color", s->user_props, c255);
                        for (int ci = 0; ci < 3; ci++)
                            ov.color[ci] = isfinite (c255[ci]) ? CLAMP (c255[ci] / 255.0f, 0.0f, 16.0f)
                                                               : 1.0f;
                    }
                    /* "controlpointN": where the editor placed control
                     * point N (system-local) */
                    for (int ci = 0; ci < 8; ci++) {
                        char key[20];
                        snprintf (key, sizeof (key), "controlpoint%d", ci);
                        if (override_node (io, key, s->user_props)) {
                            override_vec3 (io, key, s->user_props, ov.cp[ci]);
                            ov.cp_set |= 1u << ci;
                        }
                    }
                }
                memcpy (nv->pov, &ov.rate, sizeof (float) * 6);
                memcpy (nv->pov + 6, ov.color, sizeof (float) * 3);
                nv->inst = g_memdup2 (&ov, sizeof (ov));
                nv->weparts = walpe_weparticles_new (json_node_get_object (pj), dir,
                                                     walpe_we_assets_dir (), &ov);
                if (nv->weparts && io) {
                    /* scripts bound to an instance override drive it */
                    guint before = s->pending_scripts ? s->pending_scripts->len : 0;
                    GList *ks = json_object_get_members (io);
                    for (GList *k = ks; k; k = k->next) {
                        char *as = g_strconcat ("inst.", (const char *) k->data, NULL);
                        guint n0 = s->pending_scripts ? s->pending_scripts->len : 0;
                        if (walpe_ss_supported_prop (as))
                            collect_script (s, (int) out->len, io, k->data, as);
                        else
                            collect_inert_script (s, (int) out->len, io, k->data);
                        if (s->pending_scripts && s->pending_scripts->len > n0)
                            nv->inst_scripted |= !strcmp (k->data, "count") ? WALPE_WE_OV_COUNT
                                               : !strcmp (k->data, "rate") ? WALPE_WE_OV_RATE : 0;
                        g_free (as);
                    }
                    g_list_free (ks);
                    inst_scripts = TRUE;
                    /* a scripted override starts from the value in the
                     * scene, where 0 is a value too (count 0: none) */
                    if (s->pending_scripts && s->pending_scripts->len > before)
                        walpe_weparticles_set_override (nv->weparts, nv->inst, nv->inst_scripted);
                }
                json_node_unref (pj);
                pj = NULL;
            }
            if (pj) {
                if (json_node_get_node_type (pj) == JSON_NODE_OBJECT) {
                    JsonArray *one = json_array_new ();
                    json_array_add_object_element (one, json_object_ref (json_node_get_object (pj)));
                    nv->parts = walpe_particles_new (one, dir);
                    json_array_unref (one);
                } else if (json_node_get_node_type (pj) == JSON_NODE_ARRAY) {
                    nv->parts = walpe_particles_new (json_node_get_array (pj), dir);
                }
                json_node_unref (pj);
            }
            if (nv->parts && !walpe_particles_usable (nv->parts))
                g_clear_pointer (&nv->parts, walpe_particles_free);
        }
    } else if (text || text_plain) {
        nv->kind = NK_TEXT;
        /* "text": {"user": option, "value": s}: the user's text */
        nv->text_value = text ? prop_text (o, "text") : g_strdup (text_plain);
        if (nv->text_value) {
            gsize tl = text_clamp_len (nv->text_value);
            if (nv->text_value[tl]) {
                g_message ("scenenode: text of layer %s cut to %" G_GSIZE_FORMAT " bytes",
                           nv->name ? nv->name : "?", tl);
                nv->text_value[tl] = 0;
            }
        }
        WalpeTextStyle *ts = g_new0 (WalpeTextStyle, 1);
        const char *font = prop_str (o, "font");
        ts->font_ref = g_strdup (font);
        if (font && !g_str_has_prefix (font, "systemfont_")) {
            ts->font_path = resolve_asset (dir, font);
            if (!ts->font_path && walpe_we_assets_dir ())
                ts->font_path = resolve_asset (walpe_we_assets_dir (), font);
            if (!ts->font_path)
                g_message ("scenenode: text font %s not found (system font used)", font);
        }
        ts->pointsize = (float) prop_num (o, "pointsize", 32.0);
        ts->box_w = nv->size[0];
        ts->box_h = nv->size[1];
        ts->padding = (float) prop_num (o, "padding", 0.0);
        const char *ha = prop_str (o, "horizontalalign"), *va = prop_str (o, "verticalalign");
        ts->halign = !g_strcmp0 (ha, "left") ? 0 : !g_strcmp0 (ha, "right") ? 2 : 1;
        ts->valign = !g_strcmp0 (va, "top") ? 0 : !g_strcmp0 (va, "bottom") ? 2 : 1;
        ts->maxwidth = (float) prop_num (o, "maxwidth", 0.0);
        ts->limitwidth = prop_num (o, "limitwidth", 0.0) != 0;
        ts->maxrows = walpe_d2i (prop_num (o, "maxrows", 0.0));
        ts->limitrows = prop_num (o, "limitrows", 0.0) != 0;
        ts->ellipsis = prop_num (o, "limituseellipsis", 0.0) != 0;
        nv->tstyle = ts;
    } else if (wpj_str (o, "sound", NULL) || wpj_obj (o, "light")) {
        nv->visible = FALSE; /* silent renderer */
    }
    if (!inst_scripts)   /* no WE particle system here: such scripts run without effect */
        collect_inert_scripts (s, (int) out->len, wpj_obj (o, "instanceoverride"), NULL);
}

/* Objects in file order; an object's "children" (nested objects) follow
 * it: they are drawn after their parent. The parent is appended first -
 * its scripts and animations were collected for the index it gets (it
 * used to be appended behind its children: they then drew under it, and
 * its scripts drove the first child). Nesting is followed
 * SCENENODE_MAX_DEPTH levels deep (it was not counted at all). */
static void parse_nodes_at (JsonArray *arr, GArray *out, const char *dir, SceneNodeContent *s,
                            int depth)
{
    guint n = wpj_arr_len (arr);
    for (guint i = 0; i < n; i++) {
        JsonObject *o = wpj_arr_obj (arr, i);
        if (!o)
            continue;
        NodeObj nv;
        parse_node (&nv, o, dir, s, out, depth);
        g_array_append_val (out, nv);
        JsonArray *ch = wpj_arr (o, "children");
        if (wpj_arr_len (ch) > 0 && depth < SCENENODE_MAX_DEPTH)
            parse_nodes_at (ch, out, dir, s, depth + 1);
    }
}

static void parse_nodes (JsonArray *arr, GArray *out, const char *dir, SceneNodeContent *s)
{
    parse_nodes_at (arr, out, dir, s, 1);
}

/* ------------------------------------------------------------ gl + render */

/* ------------------------------------------------- SceneScript host */

static int ss_find (gpointer u, const char *name)
{
    SceneNodeContent *s = u;
    for (guint i = 0; i < s->nodes->len; i++)
        if (!g_strcmp0 (g_array_index (s->nodes, NodeObj, i).name, name))
            return (int) i;
    return -1;
}

static const char *ss_name (gpointer u, int idx)
{
    SceneNodeContent *s = u;
    if (idx < 0 || (guint) idx >= s->nodes->len)
        return NULL;
    const char *n = g_array_index (s->nodes, NodeObj, idx).name;
    return n ? n : "";
}

static WalpeSsType ss_get (gpointer u, int idx, const char *prop, double *v, int *n,
                           const char **str)
{
    SceneNodeContent *s = u;
    if (idx < 0 || (guint) idx >= s->nodes->len)
        return WSS_NONE;
    NodeObj *nv = &g_array_index (s->nodes, NodeObj, idx);
    *n = 3;
    if (!strcmp (prop, "origin")) {
        v[0] = nv->origin[0]; v[1] = nv->origin[1]; v[2] = 0;
        return WSS_VEC;
    } else if (!strcmp (prop, "scale")) {
        v[0] = nv->scale[0]; v[1] = nv->scale[1]; v[2] = 1;
        return WSS_VEC;
    } else if (!strcmp (prop, "angles")) {
        v[0] = 0; v[1] = 0; v[2] = nv->angle_z;
        return WSS_VEC;
    } else if (!strcmp (prop, "color")) {
        v[0] = nv->tint[0]; v[1] = nv->tint[1]; v[2] = nv->tint[2];
        return WSS_VEC;
    } else if (!strcmp (prop, "alpha")) {
        v[0] = nv->alpha;
        return WSS_NUM;
    } else if (!strcmp (prop, "visible")) {
        v[0] = nv->visible;
        return WSS_BOOL;
    } else if (!strcmp (prop, "text")) {
        *str = nv->text_value ? nv->text_value : "";
        return WSS_STR;
    } else if (nv->tstyle && !strcmp (prop, "pointsize")) {
        v[0] = nv->tstyle->pointsize;
        return WSS_NUM;
    } else if (nv->tstyle && !strcmp (prop, "maxwidth")) {
        v[0] = nv->tstyle->maxwidth;
        return WSS_NUM;
    } else if (nv->tstyle && !strcmp (prop, "padding")) {
        v[0] = nv->tstyle->padding;
        return WSS_NUM;
    } else if (!strcmp (prop, "playing")) {
        v[0] = nv->weparts ? walpe_weparticles_is_playing (nv->weparts) : 0;
        return WSS_BOOL;
    } else if (nv->inst && !strncmp (prop, "inst.", 5)) {
        /* IParticleSystemInstance: the object's instance overrides */
        const char *k = prop + 5;
        const float *f = !strcmp (k, "rate") ? &nv->inst->rate : !strcmp (k, "count") ? &nv->inst->count
                       : !strcmp (k, "size") ? &nv->inst->size : !strcmp (k, "lifetime") ? &nv->inst->lifetime
                       : !strcmp (k, "alpha") ? &nv->inst->alpha : !strcmp (k, "speed") ? &nv->inst->speed : NULL;
        if (f) {
            v[0] = *f;
            return WSS_NUM;
        }
        const float *c3 = !strcmp (k, "colorn") ? nv->inst->color
                        : !strncmp (k, "controlpoint", 12) && k[12] >= '0' && k[12] <= '7' && !k[13]
                          ? nv->inst->cp[k[12] - '0'] : NULL;
        if (!c3)
            return WSS_NONE;
        v[0] = c3[0]; v[1] = c3[1]; v[2] = c3[2];
        return WSS_VEC;
    } else if (!strcmp (prop, "parent")) {
        v[0] = nv->parent;
        return WSS_NUM;
    } else if (!strcmp (prop, "id")) {
        v[0] = nv->id;
        return WSS_NUM;
    } else if (!strcmp (prop, "size")) {
        v[0] = nv->size[0]; v[1] = nv->size[1];
        *n = 2;
        return WSS_VEC;
    } else if (!strcmp (prop, "solid")) {
        v[0] = nv->solid;
        return WSS_BOOL;
    } else if (nv->tstyle && !strcmp (prop, "font")) {
        *str = nv->tstyle->font_ref ? nv->tstyle->font_ref : "";
        return WSS_STR;
    } else if (nv->tstyle && !strcmp (prop, "horizontalalign")) {
        *str = nv->tstyle->halign == 0 ? "left" : nv->tstyle->halign == 2 ? "right" : "center";
        return WSS_STR;
    } else if (nv->tstyle && !strcmp (prop, "verticalalign")) {
        *str = nv->tstyle->valign == 0 ? "top" : nv->tstyle->valign == 2 ? "bottom" : "center";
        return WSS_STR;
    } else if (nv->tstyle && !strcmp (prop, "limitwidth")) {
        v[0] = nv->tstyle->limitwidth;
        return WSS_BOOL;
    } else if (nv->tstyle && !strcmp (prop, "limitrows")) {
        v[0] = nv->tstyle->limitrows;
        return WSS_BOOL;
    } else if (nv->tstyle && !strcmp (prop, "maxrows")) {
        v[0] = nv->tstyle->maxrows;
        return WSS_NUM;
    }
    return WSS_NONE;
}

static void ss_set (gpointer u, int idx, const char *prop, WalpeSsType t, const double *v,
                    int n, const char *str)
{
    SceneNodeContent *s = u;
    if (idx < 0 || (guint) idx >= s->nodes->len)
        return;
    NodeObj *nv = &g_array_index (s->nodes, NodeObj, idx);
    if (nv->inst && nv->weparts && !strncmp (prop, "inst.", 5)) {
        /* multipliers stay in the range the static overrides have; the
         * particle system takes them up at its next step */
        const char *k = prop + 5;
        float *f = !strcmp (k, "rate") ? &nv->inst->rate : !strcmp (k, "count") ? &nv->inst->count
                 : !strcmp (k, "size") ? &nv->inst->size : !strcmp (k, "lifetime") ? &nv->inst->lifetime
                 : !strcmp (k, "alpha") ? &nv->inst->alpha : !strcmp (k, "speed") ? &nv->inst->speed : NULL;
        if (f && (t == WSS_NUM || t == WSS_BOOL)) {
            /* count is n / frametime in scripts that emit n particles in
             * one frame (3042492564's click sparks) */
            *f = (float) CLAMP (v[0], 0.0, f == &nv->inst->count ? 1e5 : 1000.0);
            nv->inst_scripted |= f == &nv->inst->count ? WALPE_WE_OV_COUNT
                               : f == &nv->inst->rate ? WALPE_WE_OV_RATE : 0;
        } else if (!f && t == WSS_VEC && n >= 3 && !strcmp (k, "colorn")) {
            for (int i = 0; i < 3; i++)
                nv->inst->color[i] = (float) CLAMP (v[i], 0.0, 16.0);
        } else if (!f && t == WSS_VEC && n >= 3 && !strncmp (k, "controlpoint", 12) && k[12] >= '0' &&
                   k[12] <= '7' && !k[13]) {
            int ci = k[12] - '0';
            for (int i = 0; i < 3; i++)
                nv->inst->cp[ci][i] = (float) CLAMP (v[i], -1e6, 1e6);
            nv->inst->cp_set |= 1u << ci;
        } else {
            return;
        }
        walpe_weparticles_set_override (nv->weparts, nv->inst, nv->inst_scripted);
        return;
    }
    if (t == WSS_VEC && n >= 2) {
        if (!strcmp (prop, "origin")) {
            nv->origin[0] = (float) v[0];
            nv->origin[1] = (float) v[1];
        } else if (!strcmp (prop, "scale")) {
            nv->scale[0] = (float) v[0];
            nv->scale[1] = (float) v[1];
        } else if (!strcmp (prop, "angles") && n >= 3) {
            nv->angle_z = (float) v[2];
        } else if (!strcmp (prop, "color") && n >= 3) {
            for (int i = 0; i < 3; i++) {
                nv->tint[i] = (float) CLAMP (v[i], 0.0, 4.0);
                if (nv->text_baked && fabsf (nv->tint[i] - nv->text_tint[i]) > 0.5f / 255.0f)
                    nv->text_dirty = TRUE;
            }
        }
    } else if (t == WSS_NUM && nv->tstyle &&
               (!strcmp (prop, "pointsize") || !strcmp (prop, "maxwidth") ||
                !strcmp (prop, "padding"))) {
        float *f = prop[0] == 'p' && prop[1] == 'o' ? &nv->tstyle->pointsize
                 : prop[0] == 'm' ? &nv->tstyle->maxwidth : &nv->tstyle->padding;
        float nvv = (float) CLAMP (v[0], 0.0, 4096.0);
        if (fabsf (*f - nvv) > 1e-3f) {
            *f = nvv;
            nv->text_dirty = TRUE;
        }
    } else if (t == WSS_STR && nv->tstyle &&
               (!strcmp (prop, "horizontalalign") || !strcmp (prop, "verticalalign"))) {
        /* ITextLayer alignment: left / center / right, top / center / bottom */
        gboolean hz = prop[0] == 'h';
        int a = !g_ascii_strcasecmp (str, hz ? "left" : "top") ? 0
              : !g_ascii_strcasecmp (str, hz ? "right" : "bottom") ? 2
              : !g_ascii_strcasecmp (str, "center") ? 1 : -1;
        int *cur = hz ? &nv->tstyle->halign : &nv->tstyle->valign;
        if (a >= 0 && a != *cur) {
            *cur = a;
            nv->text_dirty = TRUE;
        }
    } else if ((t == WSS_BOOL || t == WSS_NUM) && nv->tstyle &&
               (!strcmp (prop, "limitwidth") || !strcmp (prop, "limitrows"))) {
        gboolean *cur = prop[5] == 'w' ? &nv->tstyle->limitwidth : &nv->tstyle->limitrows;
        if (*cur != (v[0] != 0)) {
            *cur = v[0] != 0;
            nv->text_dirty = TRUE;
        }
    } else if (t == WSS_NUM && nv->tstyle && !strcmp (prop, "maxrows")) {
        int rows = (int) CLAMP (v[0], 0.0, 4096.0);
        if (rows != nv->tstyle->maxrows) {
            nv->tstyle->maxrows = rows;
            nv->text_dirty = TRUE;
        }
    } else if (t == WSS_NUM && !strcmp (prop, "parent")) {
        /* ILayer.setParent: a layer of this scene or none; a cycle is cut
         * by node_world */
        int pi = (int) CLAMP (v[0], -1.0, (double) s->nodes->len);
        nv->parent = pi >= 0 && (guint) pi < s->nodes->len && pi != idx ? pi : -1;
        /* the load's rule for the chain above the new parent
         * (SCENENODE_MAX_PARENTS; node_world bounds the walk itself) */
        int hops = 0;
        for (int k = nv->parent; k >= 0 && hops <= SCENENODE_MAX_PARENTS; hops++)
            k = g_array_index (s->nodes, NodeObj, k).parent;
        if (hops > SCENENODE_MAX_PARENTS)
            nv->parent = -1;
    } else if ((t == WSS_BOOL || t == WSS_NUM) && !strcmp (prop, "solid")) {
        nv->solid = v[0] != 0;
    } else if (t == WSS_NUM && !strcmp (prop, "playing")) {
        if (nv->weparts)
            walpe_weparticles_set_playing (nv->weparts, v[0] > 0 ? 1 : v[0] < 0 ? -1 : 0);
    } else if (t == WSS_NUM && !strcmp (prop, "alpha")) {
        nv->alpha = (float) CLAMP (v[0], 0.0, 1.0);
    } else if ((t == WSS_BOOL || t == WSS_NUM) && !strcmp (prop, "visible")) {
        nv->visible = v[0] != 0;
    } else if (t == WSS_STR && !strcmp (prop, "text") && nv->kind == NK_TEXT) {
        gsize tl = text_clamp_len (str);
        if (!nv->text_value || strncmp (nv->text_value, str, tl) || nv->text_value[tl]) {
            g_free (nv->text_value);
            nv->text_value = g_strndup (str, tl);
            nv->text_dirty = TRUE;
        }
    }
}

static void ss_audio (gpointer u, int res, float *l, float *r)
{
    SceneNodeContent *s = u;
    const float *sl = res == 16 ? s->audio.left16 : res == 32 ? s->audio.left32 : s->audio.left;
    const float *sr = res == 16 ? s->audio.right16 : res == 32 ? s->audio.right32 : s->audio.right;
    memcpy (l, sl, sizeof (float) * (gsize) res);
    memcpy (r, sr, sizeof (float) * (gsize) res);
}

/* effects of node idx for SceneScript (IEffect / IMaterial) */
static WalpeSsType ss_fx_get (gpointer u, int idx, int e, int p, const char *key, double *v,
                              int *n)
{
    SceneNodeContent *s = u;
    if (idx < 0 || (guint) idx >= s->nodes->len)
        return WSS_NONE;
    gpointer fx = g_array_index (s->nodes, NodeObj, idx).fxset;
    if (p < 0 && !strcmp (key, "visible")) {
        int en = fx_get_enabled (fx, e);
        if (en < 0)
            return WSS_NONE;
        v[0] = en;
        *n = 1;
        return WSS_BOOL;
    }
    float f[4];
    int k = fx_get_constant (fx, e, p, key, f);
    if (k <= 0)
        return WSS_NONE;
    for (int i = 0; i < k; i++)
        v[i] = f[i];
    *n = k;
    return WSS_VEC;
}

static void ss_fx_set (gpointer u, int idx, int e, int p, const char *key, WalpeSsType t,
                       const double *v, int n)
{
    SceneNodeContent *s = u;
    if (idx < 0 || (guint) idx >= s->nodes->len || n < 1)
        return;
    gpointer fx = g_array_index (s->nodes, NodeObj, idx).fxset;
    if (p < 0 && !strcmp (key, "visible")) {
        fx_set_enabled (fx, e, v[0] != 0);
        return;
    }
    (void) t;
    float f[4] = { 0, 0, 0, 1 };
    for (int i = 0; i < n && i < 4; i++)
        f[i] = (float) v[i];
    if (!fx_set_constant (fx, e, p, key, f, MIN (n, 4)) && s->probe) {
        /* scripts set constants every frame: report each miss once */
        if (!s->fx_miss_logged)
            s->fx_miss_logged = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
        char *mk = g_strdup_printf ("%d/%d/%d/%s", idx, e, p, key);
        if (g_hash_table_add (s->fx_miss_logged, mk))
            g_debug ("scenescript: no effect constant %s (node %d effect %d pass %d)", key, idx,
                     e, p);
    }
}

static int ss_fx_find (gpointer u, int idx, const char *name)
{
    SceneNodeContent *s = u;
    if (idx < 0 || (guint) idx >= s->nodes->len)
        return -1;
    return fx_find_effect (g_array_index (s->nodes, NodeObj, idx).fxset, name);
}

static const char *ss_fx_name (gpointer u, int idx, int e)
{
    SceneNodeContent *s = u;
    if (idx < 0 || (guint) idx >= s->nodes->len)
        return NULL;
    return fx_effect_name (g_array_index (s->nodes, NodeObj, idx).fxset, e);
}

static int ss_fx_count (gpointer u, int idx)
{
    SceneNodeContent *s = u;
    if (idx < 0 || (guint) idx >= s->nodes->len)
        return 0;
    return fx_effect_count (g_array_index (s->nodes, NodeObj, idx).fxset);
}

static void scenenode_scripts_start (SceneNodeContent *s)
{
    if (!s->pending_scripts || g_getenv ("WALPE_NO_SCRIPTS"))
        return;
    WalpeSsHost host = { s, ss_find, ss_name, ss_get, ss_set, ss_audio,
                         ss_fx_get, ss_fx_set, ss_fx_find, ss_fx_name, ss_fx_count };
    s->ss = walpe_ss_new (&host, s->scene_w, s->scene_h, s->user_props);
    if (!s->ss) {
        g_debug ("scenenode: %u scripts left static (built without QuickJS)",
                 s->pending_scripts->len);
        return;
    }
    guint ok = 0, n_inert = 0;
    GString *inert_props = g_string_new (NULL);
    /* scripts get colour options as Vec3, everything else as it is stored */
    if (s->user_types && json_object_get_size (s->user_types) > 0)
        walpe_ss_set_prop_types (s->ss, s->user_types);
    for (guint i = 0; i < s->pending_scripts->len; i++) {
        PendingScript *ps = g_ptr_array_index (s->pending_scripts, i);
        if (ps->anim)
            ok += walpe_ss_add_anim (s->ss, ps->idx, ps->effect, ps->pass, ps->prop, ps->src,
                                     ps->init);
        else if (ps->effect >= 0)
            ok += walpe_ss_add_fx (s->ss, ps->idx, ps->effect, ps->pass, ps->prop, ps->src, ps->sp,
                                   s->user_props, ps->init);
        else if (ps->inert) {
            if (walpe_ss_add_inert (s->ss, ps->idx, ps->prop, ps->src, ps->sp, s->user_props,
                                    ps->init)) {
                ok++;
                n_inert++;
                /* each property name once */
                char *pat = g_strdup_printf (" %s,", ps->prop);
                if (!strstr (inert_props->str, pat))
                    g_string_append (inert_props, pat);
                g_free (pat);
            }
        } else
            ok += walpe_ss_add (s->ss, ps->idx, ps->prop, ps->src, ps->sp, s->user_props);
    }
    walpe_ss_start (s->ss, s->user_props);
    g_message ("scenenode: SceneScript %u/%u property scripts + animations running", ok,
               s->pending_scripts->len);
    if (n_inert > 0) {
        g_string_truncate (inert_props, inert_props->len - 1);
        g_message ("scenenode: %u of them are bound to properties this renderer does not drive "
                   "(they run, their value has no effect):%s", n_inert, inert_props->str);
    }
    g_string_free (inert_props, TRUE);
    g_clear_pointer (&s->pending_scripts, g_ptr_array_unref);
    if (ok == 0)
        g_clear_pointer (&s->ss, walpe_ss_free);
    s->n_cur_layers = s->ss ? walpe_ss_cursor_layers (s->ss, &s->cur_layers) : 0;
    s->cur_hover = s->cur_press = -1;
    if (s->n_cur_layers > 0) {
        s->base.wants_pointer = TRUE;
        g_message ("scenenode: %d layer(s) with cursor events (clicks on the desktop reach them)",
                   s->n_cur_layers);
    }
}

/* effect render targets at the layer's on-screen size instead of its
 * texture size (a 4700x3098 layer of a 3840x2160 scene on a 1920x1080
 * monitor is drawn at ~2350x1550): k = on-screen / texture size, in 1/8
 * steps (powers of two below 1/8) with hysteresis so animated scales do
 * not reallocate every frame. WALPE_FX_FULLRES=1: always 1.
 * kmin: the smallest factor. It used to be 0.5 for every layer ("bilinear
 * minification of the input stays alias-free enough"): a 4096 px layer
 * drawn 128 px wide ran its passes over 2048x2048 targets, 250 times the
 * pixels shown, and its 2048 px output was then drawn at 16:1 with plain
 * bilinear sampling (shimmer). Now only a layer whose size a script or
 * animation may change keeps that floor (no reallocation while it moves);
 * every other one works at the size it is drawn at, at least 16 px, and
 * reads its texture from a level stored that small (node_fx_input_fit).
 * WALPE_FX_MIN_SCALE=0.5 restores the floor for all. */
/* How much a reduced texture / effect output may be magnified when drawn
 * before the next larger size is used: 2 %. Scenes with a camera "zoom" of
 * 1.01 are common (room for shake and parallax): a 3840 px layer is then
 * 1939 px on a 1920 px monitor, and with no tolerance its effects worked
 * at 5/8 of the texture (2400x1350 targets, the full 4K input) for a
 * picture 19 px wider than the half-size one. Plain layers already had
 * 1 % (which 1.01 x 1920 = 1939.2 only met by rounding luck).
 * Config "texture_slack": 2 or 0 (walpe-ctl set texture_slack=0, applies
 * to the next wallpaper load); WALPE_TEX_SLACK=<percent> overrides it.
 * 0 = as before (effects exact, plain 1 %). */
static __thread float tex_slack_pct = 2.0f;   /* of the scene being rendered */

static float tex_slack (gboolean plain)
{
    float sl = 1.0f + tex_slack_pct / 100.0f;
    return plain ? fmaxf (sl, 1.01f) : sl;
}

static float fx_min_scale (void)
{
    static float v = -1.0f;
    if (v < 0.0f) {
        const char *e = g_getenv ("WALPE_FX_MIN_SCALE");
        float f = e && *e ? (float) g_ascii_strtod (e, NULL) : 1.0f / 64.0f;
        v = isfinite (f) ? CLAMP (f, 1.0f / 64.0f, 1.0f) : 1.0f / 64.0f;
    }
    return v;
}

static float fx_working_scale (NodeObj *nv, const WalpeMat3 *m, int w, int h, int tw, int th,
                               float kmin)
{
    static int fullres = -1;
    if (fullres < 0)
        fullres = g_getenv ("WALPE_FX_FULLRES") && *g_getenv ("WALPE_FX_FULLRES") == '1';
    if (fullres || tw < 1 || th < 1)
        return 1.0f;
    float pw = hypotf (m->m[0] * w, m->m[1] * h), ph = hypotf (m->m[3] * w, m->m[4] * h);
    float k = fmaxf (pw / tw, ph / th) / tex_slack (FALSE);
    if (!(k < 0.125f)) {
        k = ceilf (k * 8.0f) / 8.0f;
    } else {
        float q = 0.125f;
        while (q * 0.5f >= k && q > 1.0f / 64.0f)
            q *= 0.5f;
        k = q;
    }
    /* never less than 16 texels of working size */
    kmin = fmaxf (kmin, fminf (0.5f, fmaxf (16.0f / tw, 16.0f / th)));
    k = fminf (1.0f, fmaxf (kmin, k));
    if (nv->fx_k > 0 && k < nv->fx_k && k > nv->fx_k - 0.2f)
        k = nv->fx_k;  /* shrink only by more than a step */
    nv->fx_k = k;
    return k;
}

/* Quality knob fx_res: the working scale below the default one (k_full,
 * from fx_working_scale). "screen": the on-screen size itself in 1/64
 * steps instead of the next 1/8 (a 2293 px layer shown 1158 px wide: 0.516
 * instead of 0.625, a third fewer pixels in every pass), still never below
 * half the texture and never above the default. "reduced" / "half": 3/4 /
 * 1/2 of that; the result is drawn enlarged (softer). keep[l] = the share
 * of the default's pixels level l + 1 leaves (for the estimates). Returns
 * the scale of the level in use (k_full at "full"); it shrinks only by
 * more than 0.1, so a layer that pulses in size keeps its targets. */
static float fx_res_scale (int level, NodeObj *nv, const WalpeMat3 *m, int w, int h, int tw,
                           int th, float k_full, float kmin, float keep[3])
{
    keep[0] = keep[1] = keep[2] = 1.0f;
    if (tw < 1 || th < 1 || !(k_full > 0))
        return k_full;
    /* the floors of fx_working_scale: the layer's own, and 16 texels */
    kmin = fminf (k_full, fmaxf (kmin, fminf (0.5f, fmaxf (16.0f / tw, 16.0f / th))));
    float pw = hypotf (m->m[0] * w, m->m[1] * h), ph = hypotf (m->m[3] * w, m->m[4] * h);
    /* a hair of slack: 3840 px on 1920 px computes as 0.50000004 */
    float ks = ceilf (fmaxf (pw / tw, ph / th) * 64.0f - 0.02f) / 64.0f;
    ks = fminf (k_full, fmaxf (kmin, ks));
    const float kl[3] = { ks, fmaxf (kmin, 0.75f * ks), fmaxf (kmin, 0.5f * ks) };
    for (int l = 0; l < 3; l++)
        keep[l] = (kl[l] * kl[l]) / (k_full * k_full);
    if (level <= WALPE_FXRES_FULL || level > WALPE_FXRES_HALF) {
        nv->fx_k2 = 0;
        return k_full;
    }
    float k = kl[level - 1];
    if (nv->fx_k2 > 0 && k < nv->fx_k2 && k > nv->fx_k2 - 0.1f && nv->fx_k2 <= k_full)
        k = nv->fx_k2;
    nv->fx_k2 = k;
    return k;
}

/* (re)render a text layer whose string a script changed */
static void make_text_texture (NodeObj *nv);
static void refresh_text (NodeObj *nv)
{
    nv->text_dirty = FALSE;
    nv->tex_ready = FALSE;
    nv->pix_serial++;
    make_text_texture (nv);      /* into the same texture when the size is the same */
    if (!nv->tex_ready && nv->tex) {
        glDeleteTextures (1, &nv->tex);   /* empty string / not drawable */
        nv->tex = 0;
    }
    fx_invalidate (nv->fxset);   /* new pixels, maybe under the same name */
}

/* ------------------------------------------- media integration (media.h) */

/* width / height of the layer the album art is shown on. Every layer that
 * binds $mediaThumbnail / $mediaPreviousThumbnail as its image or in one
 * of its effects is a candidate, shown or not (cover layers start hidden
 * and are switched on by a media event). A square one wins: that is the
 * cover slot WE's editor asks for, and other shapes next to it are
 * backdrops made from the cover. Without a square one, the first
 * candidate's shape; without any, square. */
static float media_slot_aspect (SceneNodeContent *s)
{
    float first = 0;
    for (guint i = 0; i < s->nodes->len; i++) {
        NodeObj *nv = &g_array_index (s->nodes, NodeObj, i);
        if ((nv->systex || fx_uses_system_textures (nv->fxset)) && nv->size[0] >= 1 &&
            nv->size[1] >= 1) {
            float a = (nv->size[0] * fabsf (nv->scale[0])) / (nv->size[1] * fabsf (nv->scale[1]));
            if (!isfinite (a) || a < 0.05f || a > 20.0f)
                continue;
            if (fabsf (a - 1.0f) < 0.02f)
                return 1.0f;
            if (first <= 0)
                first = a;
        }
    }
    return first > 0 ? first : 1.0f;
}

/* WE hands scenes the album cover as a texture that fills the cover layer
 * (and the effect slots that blend the previous cover) edge to edge; its
 * editor asks for a square placeholder. MPRIS art is whatever the player
 * has - 16:9 video thumbnails included - so only the part that has the
 * layer's shape is used (centred, as large as possible): the cover is
 * cropped, never stretched. NULL = the art already has that shape. */
static GBytes *media_crop (SceneNodeContent *s, GBytes *px, int *w, int *h)
{
    if (s->media_aspect <= 0)
        s->media_aspect = media_slot_aspect (s);
    int cw = *w, ch = *h;
    if ((float) *w > (float) *h * s->media_aspect)
        cw = CLAMP ((int) lroundf ((float) *h * s->media_aspect), 1, *w);
    else
        ch = CLAMP ((int) lroundf ((float) *w / s->media_aspect), 1, *h);
    if (cw == *w && ch == *h)
        return NULL;
    const guint8 *src = g_bytes_get_data (px, NULL);
    int x0 = (*w - cw) / 2, y0 = (*h - ch) / 2;
    guint8 *crop = g_malloc ((gsize) cw * ch * 4);
    for (int y = 0; y < ch; y++)
        memcpy (crop + (gsize) y * cw * 4, src + ((gsize) (y + y0) * *w + x0) * 4, (gsize) cw * 4);
    g_debug ("scenenode: album art %dx%d shown as its centred %dx%d (layer aspect %.3f)", *w, *h,
             cw, ch, s->media_aspect);
    *w = cw;
    *h = ch;
    return g_bytes_new_take (crop, (gsize) cw * ch * 4);
}

static void media_upload (SceneNodeContent *s, int k, GBytes *px, int w, int h)
{
    s->media_gen++;
    if (!px || w < 1 || h < 1) {
        if (s->media_tex[k])
            glDeleteTextures (1, &s->media_tex[k]);
        s->media_tex[k] = 0;
        s->media_w[k] = s->media_h[k] = 0;
        return;
    }
    g_autoptr (GBytes) crop = media_crop (s, px, &w, &h);   /* w, h: the part used */
    if (crop)
        px = crop;
    if (!s->media_tex[k])
        glGenTextures (1, &s->media_tex[k]);
    glBindTexture (GL_TEXTURE_2D, s->media_tex[k]);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei (GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                  g_bytes_get_data (px, NULL));
    walpe_gl_tex_gen_mipmaps (w, h);   /* a 640 px cover in a small widget */
    s->media_w[k] = w;
    s->media_h[k] = h;
}

static void json_color (JsonBuilder *b, const char *name, const float c[3])
{
    json_builder_set_member_name (b, name);
    json_builder_begin_array (b);
    for (int k = 0; k < 3; k++)
        json_builder_add_double_value (b, c[k]);
    json_builder_end_array (b);
}

static void media_event (SceneNodeContent *s, const char *kind, JsonBuilder *b)
{
    json_builder_end_object (b);
    JsonNode *root = json_builder_get_root (b);
    char *js = json_to_string (root, FALSE);
    json_node_unref (root);
    g_object_unref (b);
    if (s->probe)
        g_message ("scenenode: %s %.200s", kind, js);
    walpe_ss_media_event (s->ss, kind, js);
    g_free (js);
}

static JsonBuilder *media_begin (void)
{
    JsonBuilder *b = json_builder_new ();
    json_builder_begin_object (b);
    return b;
}

/* once per frame: new snapshot -> album art textures + script events.
 * The first dispatch after taking the reference waits (<= 1 s) for the
 * media thread to connect, so scripts don't see a spurious "disabled". */
static void scene_media_update (SceneNodeContent *s)
{
    guint64 ser = walpe_media_serial ();
    if (ser == s->media_serial && !s->media_first)
        return;
    WalpeNowPlaying m;
    walpe_media_get (&m);
    if (s->media_first && !m.enabled && s->media_first++ < 30) {
        walpe_media_now_clear (&m);
        return;
    }
    gboolean first = s->media_first != 0;
    s->media_first = FALSE;
    s->media_serial = ser;
    guint cur[5] = { m.status_serial, m.props_serial, m.thumb_serial, m.playback_serial,
                     m.timeline_serial };
    gboolean ch[5];
    for (int k = 0; k < 5; k++)
        ch[k] = first || cur[k] != s->media_seen[k];
    memcpy (s->media_seen, cur, sizeof (cur));
    if (ch[2]) {
        media_upload (s, 1, m.has_thumbnail ? m.thumb : NULL, m.thumb_w, m.thumb_h);
        media_upload (s, 2, m.prev_thumb, m.prev_w, m.prev_h);
    }
    gboolean playing = m.player && *m.player;
    if (ch[0]) {
        JsonBuilder *b = media_begin ();
        json_builder_set_member_name (b, "enabled");
        json_builder_add_boolean_value (b, m.enabled);
        media_event (s, "mediaStatusChanged", b);
    }
    if (ch[3]) {
        JsonBuilder *b = media_begin ();
        json_builder_set_member_name (b, "state");
        json_builder_add_int_value (b, m.state);
        media_event (s, "mediaPlaybackChanged", b);
    }
    if (ch[1] && (!first || playing)) {
        JsonBuilder *b = media_begin ();
        const char *names[] = { "title", "artist", "subTitle", "albumTitle", "albumArtist",
                                "genres", "contentType" };
        const char *vals[] = { m.title, m.artist, m.subtitle, m.album_title, m.album_artist,
                               m.genres, m.content_type };
        for (int k = 0; k < 7; k++) {
            json_builder_set_member_name (b, names[k]);
            json_builder_add_string_value (b, vals[k]);
        }
        media_event (s, "mediaPropertiesChanged", b);
    }
    /* also when a player is there without art: scripts start their
     * startpaused "reveal" animations on this event (3151551777's cover
     * stayed at frame 0 = white, 2901582518's titles at alpha 0) */
    if (ch[2] && (!first || m.has_thumbnail || playing)) {
        JsonBuilder *b = media_begin ();
        json_builder_set_member_name (b, "hasThumbnail");
        json_builder_add_boolean_value (b, m.has_thumbnail);
        json_color (b, "primaryColor", m.primary);
        json_color (b, "secondaryColor", m.secondary);
        json_color (b, "tertiaryColor", m.tertiary);
        json_color (b, "textColor", m.text);
        json_color (b, "highContrastColor", m.high_contrast);
        media_event (s, "mediaThumbnailChanged", b);
    }
    if (ch[4] && (!first || playing)) {
        JsonBuilder *b = media_begin ();
        json_builder_set_member_name (b, "position");
        json_builder_add_double_value (b, m.position);
        json_builder_set_member_name (b, "duration");
        json_builder_add_double_value (b, m.duration);
        media_event (s, "mediaTimelineChanged", b);
    }
    walpe_media_now_clear (&m);
}

static void scene_media_hold (SceneNodeContent *s, gboolean on)
{
    if (!s->needs_media || on == s->media_ref)
        return;
    s->media_ref = on;
    if (on) {
        s->media_first = TRUE;
        walpe_media_ref ();
    } else {
        walpe_media_unref ();
    }
}

static gboolean scenenode_gl_init (WalpeContent *c, GError **err)
{
    SceneNodeContent *s = (SceneNodeContent *) c;
    scene_thread_numeric_c ();
    walpe_glutil_init ();
    for (guint i = 0; i < s->nodes->len && !s->text_held; i++)
        if (g_array_index (s->nodes, NodeObj, i).kind == NK_TEXT) {
            s->text_held = TRUE;
            walpe_text_hold ();
        }
    /* particle sprites: counted against the texture budget like the rest */
    walpe_gl_tex_scope_begin (s->tex_budget > 0 ? MAX ((gint64) 1, s->tex_budget - s->tex_eager_bytes)
                                                : 0);
    for (guint i = 0; i < s->nodes->len; i++) {
        NodeObj *nv = &g_array_index (s->nodes, NodeObj, i);
        if (nv->tex_ready && nv->tex_src)
            s->tex_deferred = TRUE;   /* file textures: the first frame loads the
                                       * deferred ones and drops the hidden layers' */
        /* text is rasterised in the first frame that shows the layer (the
         * render loop, text_dirty): a text layer that is hidden then - an
         * option that is off, a variant of the scene - used to be rendered
         * here all the same and kept a texture of its whole box for the
         * run (2842824609: a 6379 x 3736 box that is never shown, 91 MiB) */
        if (nv->kind == NK_TEXT)
            nv->text_dirty = TRUE;
        if (nv->weparts) {
            walpe_weparticles_gl_init (nv->weparts);
            if (walpe_weparticles_follows_cursor (nv->weparts))
                s->base.wants_pointer = TRUE;  /* cursor-linked emitter */
        }
        if (nv->parts) {
            walpe_particles_gl_init (nv->parts);
            if (!walpe_particles_usable (nv->parts))
                g_clear_pointer (&nv->parts, walpe_particles_free);
        }
    }
    s->tex_eager_bytes += walpe_gl_tex_scope_end ();
    if (s->fx_count > 0) {
        int fok = 0, ffail = 0, nfx = 0, nstatic = 0;
        for (guint i = 0; i < s->nodes->len; i++) {
            NodeObj *nv = &g_array_index (s->nodes, NodeObj, i);
            if (fx_has_effects (nv->fxset)) {
                nfx++;
                nstatic += fx_is_static (nv->fxset) && nv->video < 0 && !nv->systex &&
                           !nv->compose;
            }
        }
        g_message ("scenenode: %d of %d layers with effects are static (fx_cache)", nstatic, nfx);
        fx_stats (&fok, &ffail, TRUE);
        s->fx_ok = fok;
        s->fx_failed = ffail;
        s->fx_static = nstatic;
        g_message ("scenenode: %u effect instances; chains built %d, failed %d (WE assets: %s)",
                   s->fx_count, fok, ffail,
                   walpe_we_assets_dir () ? walpe_we_assets_dir () : "none");
    }

    /* diagnostics (WALPE_PROBE, desktop GL only - GLES has no
     * glGetTexImage and epoxy would abort): read back the first image
     * texture (glGetTexImage writes the WHOLE texture: full-size buffer) */
    if (g_getenv ("WALPE_PROBE") && epoxy_is_desktop_gl ()) {
        for (guint i = 0; i < s->nodes->len; i++) {
            NodeObj *nv = &g_array_index (s->nodes, NodeObj, i);
            if (nv->tex_ready && nv->tex && !nv->tex_lod && nv->tex_w > 4) {
                gsize tsz = (gsize) nv->tex_w * nv->tex_h * 4;
                GLubyte *tb = g_malloc (tsz);
                glBindTexture (GL_TEXTURE_2D, nv->tex);
                glPixelStorei (GL_PACK_ALIGNMENT, 4);
                glGetTexImage (GL_TEXTURE_2D, 0, GL_BGRA, GL_UNSIGNED_BYTE, tb);
                GLenum glerr = glGetError ();
                gsize ctr = ((gsize) nv->tex_h / 2 * nv->tex_w + nv->tex_w / 2) * 4;
                g_message ("scenenode: GL tex %u %dx%d center(bgra)=%d,%d,%d,%d glerr=0x%x",
                           nv->tex, nv->tex_w, nv->tex_h, tb[ctr], tb[ctr+1], tb[ctr+2],
                           tb[ctr+3], (unsigned) glerr);
                g_free (tb);
                break;
            }
        }
    }
    for (guint i = 0; i < s->nodes->len; i++) {
        NodeObj *nv = &g_array_index (s->nodes, NodeObj, i);
        /* effects animate through g_Time - they need the frame clock
         * even when the scene has no particle system. Not the ones proven
         * static (fx_is_static: no time, audio, pointer or feedback):
         * colour grading / blur / tint on a still picture rendered the
         * same frame again at the full frame rate. Puppets play their
         * bone animations by the clock. */
        if (nv->parts || nv->weparts || nv->puppet ||
            (fx_has_effects (nv->fxset) && !fx_is_static (nv->fxset)))
            s->animates = TRUE;
    }
    /* layers whose on-screen size a script may change keep their texture
     * as loaded (tex_downscale leaves them alone): a script or property
     * animation on their scale/origin/angles/size, or a script that names
     * the layer (thisScene.getLayer('name')) and touches a geometry
     * property. Scripts on visible/alpha/color/text or effect constants do
     * not change the size, and neither does one on parallaxDepth (how far
     * the layer follows the pointer: an offset. Scenes bind it to a user
     * option on every layer and group; counted here it kept all of
     * 3163060610's layers at file size, 270 MiB instead of 106).
     * (Parents are checked at use: node_scripted.) */
#define SCRIPT_SCAN_MAX (G_GUINT64_CONSTANT (10) * 1000 * 1000 * 1000)   /* seconds at most */
    guint64 scan_work = 0;
    gboolean scan_all = FALSE;
    for (guint p = 0; s->pending_scripts && p < s->pending_scripts->len; p++) {
        PendingScript *ps = g_ptr_array_index (s->pending_scripts, p);
        const char *pr = ps->prop ? ps->prop : "";
        gboolean geo_prop = ps->effect < 0 &&
            (!strcmp (pr, "scale") || !strcmp (pr, "origin") || !strcmp (pr, "angles") ||
             !strcmp (pr, "size"));
        if (geo_prop && ps->idx >= 0 && (guint) ps->idx < s->nodes->len)
            g_array_index (s->nodes, NodeObj, ps->idx).scripted = TRUE;
        if (ps->anim || !ps->src)
            continue;
        gboolean geo_code = strstr (ps->src, "scale") || strstr (ps->src, "origin") ||
                            strstr (ps->src, "angles") || strstr (ps->src, "size") ||
                            strstr (ps->src, "camera");
        /* every layer name is looked for in every such script: layers x
         * scripts x script length, all three the wallpaper's to choose
         * (100 000 layers and 2 000 scripts of 10 KiB: minutes of load).
         * Past SCRIPT_SCAN_MAX bytes of text searched the question is
         * dropped and every layer counts as scripted - the answer that
         * costs texture memory, never a wrong picture. */
        gsize src_len = geo_code ? strlen (ps->src) : 0;
        for (guint i = 0; geo_code && !scan_all && i < s->nodes->len; i++) {
            NodeObj *nv = &g_array_index (s->nodes, NodeObj, i);
            if (nv->scripted || !nv->name || strlen (nv->name) < 2)
                continue;
            scan_work += src_len;
            if (scan_work > SCRIPT_SCAN_MAX)
                scan_all = TRUE;
            else if (strstr (ps->src, nv->name))
                nv->scripted = TRUE;
        }
    }
    if (scan_all) {
        for (guint i = 0; i < s->nodes->len; i++)
            g_array_index (s->nodes, NodeObj, i).scripted = TRUE;
        g_message ("scenenode: %u layers and %u scripts: which layers the scripts name is not "
                   "worked out (every layer keeps its texture at file size)", s->nodes->len,
                   s->pending_scripts ? s->pending_scripts->len : 0);
    }
    scenenode_scripts_start (s);
    if (s->ss)
        s->animates = TRUE;   /* update() runs every frame */
    for (guint i = 0; i < s->nodes->len; i++) {
        NodeObj *nv = &g_array_index (s->nodes, NodeObj, i);
        if (fx_uses_audio (nv->fxset) || walpe_weparticles_uses_audio (nv->weparts))
            s->needs_audio = TRUE;
    }
    if (walpe_ss_uses_audio (s->ss))
        s->needs_audio = TRUE;
    for (guint i = 0; i < s->nodes->len; i++) {
        NodeObj *nv = &g_array_index (s->nodes, NodeObj, i);
        if (nv->systex || fx_uses_system_textures (nv->fxset))
            s->needs_media = TRUE;
    }
    if (walpe_ss_uses_media (s->ss))
        s->needs_media = TRUE;
    if (s->needs_media) {
        g_message ("scenenode: media integration (MPRIS while visible)");
        s->animates = TRUE;
        scene_media_hold (s, TRUE);   /* dropped by gl_free */
    }
    if (s->needs_audio && !g_getenv ("WALPE_NO_AUDIO")) {
        g_message ("scenenode: audio-reactive (capturing while visible)");
        s->audio_ref = TRUE;     /* dropped by set_paused(TRUE) / gl_free */
        walpe_audio_ref ();
    }
    if (!s->animates && s->videos->len > 0) {
        s->animates = TRUE;
        s->only_video_anim = TRUE;
    }
    for (guint i = 0; i < s->nodes->len; i++) {
        NodeObj *nv = &g_array_index (s->nodes, NodeObj, i);
        if (nv->parts || fx_uses_pointer (nv->fxset) ||
            (nv->weparts && walpe_weparticles_follows_cursor (nv->weparts)))
            s->base.uses_pointer_pos = TRUE;   /* incl. mouse-linked control points */
    }
    if (walpe_ss_uses_cursor (s->ss))
        s->base.uses_pointer_pos = TRUE;
    g_message ("scenenode: pointer position %s, cursor events %s",
               s->base.uses_pointer_pos ? "used every frame" : "unused",
               s->base.wants_pointer ? "yes" : "no");
    (void) err;
    return TRUE;
}

/* world transform of a node: parent chain first (WE "parent" ids: a
 * child's origin/angle/scale live in its parent's space), invisibility
 * inherited. Memoized per frame via world_state; cycles are cut. */
/* pointer -> SceneScript cursor events -------------------------------- */

static void scenenode_pointer (WalpeContent *c, int kind, float x, float y)
{
    SceneNodeContent *s = (SceneNodeContent *) c;
    if (x >= 0 && y >= 0) {
        s->ptr_nx = x;
        s->ptr_ny = y;
        s->ptr_have = TRUE;
    }
    if (!s->n_cur_layers)
        return;
    if (!s->ptr_q)
        s->ptr_q = g_array_new (FALSE, FALSE, sizeof (PtrEv));
    if (s->ptr_q->len > 64)
        return;
    PtrEv e = { kind, x, y };
    g_array_append_val (s->ptr_q, e);
}

/* layer quad (world * size, the draw transform without the view) */
static gboolean node_quad_matrix (NodeObj *nv, WalpeMat3 *out)
{
    WalpeMat3 quad;
    walpe_mat3_identity (&quad);
    if (nv->kind == NK_TEXT && nv->tquad[2] > 0) {
        walpe_mat3_scale (&quad, nv->tquad[2] / 2.0f, nv->tquad[3] / 2.0f);
        walpe_mat3_translate (&quad, nv->tquad[0], nv->tquad[1]);
    } else {
        if (nv->size[0] <= 0 || nv->size[1] <= 0)
            return FALSE;
        walpe_mat3_scale (&quad, nv->size[0] / 2.0f, nv->size[1] / 2.0f);
        if (nv->anchor[0] != 0 || nv->anchor[1] != 0)
            walpe_mat3_translate (&quad, -nv->anchor[0] * nv->size[0] / 2.0f,
                                  -nv->anchor[1] * nv->size[1] / 2.0f);
    }
    *out = nv->world;
    walpe_mat3_mul (out, out, &quad);
    return TRUE;
}

/* topmost cursor layer under scene point (x, y): WE hit-tests the layer's
 * rectangle (its alpha does not matter: 3163060610 clicks an invisible
 * alpha-0 "switch" layer); "solid": false layers let clicks through.
 * Only layers whose scripts handle cursor events take part (assumption:
 * an unscripted layer on top does not swallow the click). */
static int cursor_hit (SceneNodeContent *s, float x, float y, float *lx, float *ly)
{
    int best = -1;
    for (int k = 0; k < s->n_cur_layers; k++) {
        int i = s->cur_layers[k];
        if (i < 0 || (guint) i >= s->nodes->len || i < best)
            continue;
        NodeObj *nv = &g_array_index (s->nodes, NodeObj, i);
        if (!nv->world_visible || !nv->solid)
            continue;
        WalpeMat3 m;
        if (!node_quad_matrix (nv, &m))
            continue;
        /* inverse of the affine 2D part: [a c tx; b d ty] (column-major) */
        float a = m.m[0], b = m.m[1], cc = m.m[3], d = m.m[4], tx = m.m[6], ty = m.m[7];
        float det = a * d - b * cc;
        if (fabsf (det) < 1e-12f)
            continue;
        float px = x - tx, py = y - ty;
        float u = (d * px - cc * py) / det, v = (-b * px + a * py) / det;
        if (u < -1.0f || u > 1.0f || v < -1.0f || v > 1.0f)
            continue;
        best = i;
        float hw = nv->kind == NK_TEXT ? nv->tquad[2] / 2.0f : nv->size[0] / 2.0f;
        float hh = nv->kind == NK_TEXT ? nv->tquad[3] / 2.0f : nv->size[1] / 2.0f;
        *lx = u * hw;
        *ly = v * hh;
    }
    return best;
}

/* scene -> monitor mapping: pixels per scene unit (cover of the canvas,
 * times the camera zoom) and the scene point at the monitor centre (the
 * canvas centre moved by the camera layer's origin) */
static void view_map (SceneNodeContent *s, float w, float h, float *k, float *cx, float *cy)
{
    *k = fmaxf (w / s->scene_w, h / s->scene_h) * s->cam_zoom * s->gen_zoom;
    *cx = s->scene_w / 2.0f;
    *cy = s->scene_h / 2.0f;
    if (s->cam_node >= 0 && (guint) s->cam_node < s->nodes->len) {
        const NodeObj *cn = &g_array_index (s->nodes, NodeObj, s->cam_node);
        *cx += cn->origin[0];
        *cy += cn->origin[1];
    }
}

static void cursor_dispatch (SceneNodeContent *s)
{
    if (!s->ptr_q || !s->ptr_q->len || !s->ss || s->view_w < 1 || s->view_h < 1)
        return;
    float w = (float) s->view_w, h = (float) s->view_h;
    float k, vcx, vcy;
    view_map (s, w, h, &k, &vcx, &vcy);
    for (guint q = 0; q < s->ptr_q->len; q++) {
        PtrEv e = g_array_index (s->ptr_q, PtrEv, q);
        gboolean out = e.x < 0 || e.y < 0;
        /* monitor [0,1] (top-left) -> scene (Y-up), the cover/crop of the
         * view transform inverted */
        float sx = vcx + (e.x - 0.5f) * w / k;
        float sy = vcy + (0.5f - e.y) * h / k;
        float lx = 0, ly = 0;
        int hit = out ? -1 : cursor_hit (s, sx, sy, &lx, &ly);
        if (!out) {
            s->cur_x = sx;
            s->cur_y = sy;
            s->cur_have = TRUE;
        }
        if (s->probe || g_getenv ("WALPE_TRACE_CURSOR"))
            g_message ("cursor: %s at scene %.0f,%.0f -> layer %s",
                       e.kind == WALPE_PTR_DOWN ? "down" : e.kind == WALPE_PTR_UP ? "up" : "move",
                       sx, sy, hit >= 0 ? g_array_index (s->nodes, NodeObj, hit).name : "-");
        if (hit != s->cur_hover) {
            if (s->cur_hover >= 0)
                walpe_ss_cursor_event (s->ss, "cursorLeave", s->cur_hover, sx, sy, 0, 0);
            if (hit >= 0)
                walpe_ss_cursor_event (s->ss, "cursorEnter", hit, sx, sy, lx, ly);
            s->cur_hover = hit;
        }
        if (e.kind == WALPE_PTR_MOVE) {
            if (hit >= 0)
                walpe_ss_cursor_event (s->ss, "cursorMove", hit, sx, sy, lx, ly);
        } else if (e.kind == WALPE_PTR_DOWN) {
            s->cur_down = TRUE;
            s->cur_press = hit;
            if (hit >= 0)
                walpe_ss_cursor_event (s->ss, "cursorDown", hit, sx, sy, lx, ly);
        } else if (e.kind == WALPE_PTR_UP) {
            s->cur_down = FALSE;
            if (hit >= 0) {
                walpe_ss_cursor_event (s->ss, "cursorUp", hit, sx, sy, lx, ly);
                if (hit == s->cur_press)
                    walpe_ss_cursor_event (s->ss, "cursorClick", hit, sx, sy, lx, ly);
            }
            s->cur_press = -1;
        }
    }
    g_array_set_size (s->ptr_q, 0);
}

/* how deep node_world () is in its ancestors on this render thread. The
 * load keeps "parent" chains short (SCENENODE_MAX_PARENTS), but a script
 * re-parents layers while the scene runs (ILayer.setParent): 100 000
 * layers chained one to the next were a stack overflow in the first frame
 * after. A layer that far below the layer being placed becomes a root,
 * as the members of a parent cycle do. */
static __thread int node_world_depth;

static void node_world (SceneNodeContent *s, guint i)
{
    NodeObj *nv = &g_array_index (s->nodes, NodeObj, i);
    if (nv->world_state == 2)
        return;
    if (nv->world_state == 1) {          /* parent cycle: treat as root */
        nv->parent = -1;
        return;
    }
    if (nv->parent >= 0 && node_world_depth >= SCENENODE_MAX_PARENTS)
        nv->parent = -1;
    nv->world_state = 1;
    WalpeMat3 local;
    walpe_mat3_identity (&local);
    /* helpers premultiply: T * R * S */
    walpe_mat3_scale (&local, nv->scale[0], nv->scale[1]);
    /* WE angles are radians (values like 3.1416 / -1.5708 in scenes),
     * counter-clockwise in the Y-up scene space */
    walpe_mat3_rotate (&local, nv->angle_z);
    walpe_mat3_translate (&local, nv->origin[0], nv->origin[1]);
    nv->world_visible = nv->visible;
    if (nv->parent >= 0) {
        node_world_depth++;
        node_world (s, (guint) nv->parent);
        node_world_depth--;
        NodeObj *pn = &g_array_index (s->nodes, NodeObj, nv->parent);
        if (nv->parent >= 0) {
            /* A child of a puppet that names one of its attachment points
             * ("attachment") lives in that point's frame, not in the
             * layer's: its origin is relative to the point and it follows
             * the bone the point is fixed to. (It used to sit relative to
             * the puppet layer's centre and stand still: 3465215190's
             * droplet emitters were 322 x 188 scene pixels off the
             * umbrella they are attached to, three star emitters of
             * 3436033033 with origins of a few pixels all lay on their
             * character's centre.) */
            float af[6];
            if (nv->attach && pn->puppet &&
                walpe_puppet_attachment (pn->puppet, nv->attach, s->frame_time, af)) {
                const WalpeMat3 am = { { af[0], af[1], 0, af[2], af[3], 0, af[4], af[5], 1 } };
                walpe_mat3_mul (&local, &am, &local);
            }
            walpe_mat3_mul (&nv->world, &pn->world, &local);
            nv->world_visible = nv->visible && pn->world_visible;
        } else {
            nv->world = local;
        }
    } else {
        nv->world = local;
    }
    /* the user's "Adjust layers" transform: last, after everything the
     * scene, its options, animations and scripts put into the node */
    if (s->n_adj)
        scene_layers_apply (s, nv, nv->parent >= 0
                                ? &g_array_index (s->nodes, NodeObj, nv->parent) : NULL);
    nv->world_state = 2;
}

static gboolean node_scripted (SceneNodeContent *s, guint i)
{
    for (int k = (int) i, hops = 0; k >= 0 && (guint) k < s->nodes->len && hops < 64; hops++) {
        NodeObj *nv = &g_array_index (s->nodes, NodeObj, k);
        if (nv->scripted)
            return TRUE;
        k = nv->parent;
    }
    return FALSE;
}

/* A layer's texture was replaced by another level of its file, or got its
 * mip chain: the same scene state now draws (slightly) other pixels. Code
 * that skips or reuses frames while "nothing changed" (the
 * frame signature and static bake, `WalpeContent.picture_serial`) has to
 * see it: the picture serial is bumped here, so the next frame is drawn
 * and the bake rebuilt. */
#ifndef SCENE_PICTURE_CHANGED
#define SCENE_PICTURE_CHANGED(s) ((s)->tex_serial++, (s)->base.picture_serial++)
#endif

/* halvings that keep a W x H texture at >= 1 texel per pixel when drawn
 * pw x ph (1 % slack: a 3840 px layer on 1920 px computes as 1920.0001) */
static int node_fit_level (int W, int H, float pw, float ph)
{
    int L = 0;
    float sl = tex_slack (TRUE);
    while (L < 4 && (W >> (L + 1)) * sl >= pw && (H >> (L + 1)) * sl >= ph)
        L++;
    return L;
}

/* the layer's texture has a mip chain to come that is not on the GPU yet */
static gboolean node_chain_pending (const NodeObj *nv)
{
    if (nv->tex_comp)
        return !nv->tex_chain && nv->tex_mips > 1 && walpe_gl_mipmaps_enabled ();
    return nv->tex_want_mips;
}

/* ... and now it is sampled beyond 2:1: an uncompressed texture gets it
 * from glGenerateMipmap, a compressed one is loaded again with the levels
 * the file stores below its base */
static void node_chain_build (NodeObj *nv)
{
    if (nv->tex_comp)
        node_tex_load (nv, nv->tex_lod, TRUE, TRUE);
    else
        walpe_gl_tex_ensure_mipmaps (nv->tex);
    nv->tex_want_mips = FALSE;
}

static int node_plain_level (SceneNodeContent *s, NodeObj *nv, guint i, float pw, float ph,
                             gboolean *fits_out);

static void node_tex_failed (NodeObj *nv)
{
    if (tex_announce)
        return;
    g_warning ("scenenode: layer %s: texture %s cannot be loaded", nv->name ? nv->name : "?",
               nv->tex_src ? nv->tex_src : "?");
    node_tex_job_done (nv);
    nv->tex_lazy = FALSE;
    nv->tex_ready = FALSE;
}

/* tex_downscale: a plain layer texture drawn at half its size (or
 * smaller) is stored reduced (see the ratio rule below). Conservative:
 * only layers with no effects (those: node_fx_input_fit), no script or
 * animation on them or their parents, no puppet/video/album art/compose/
 * text. If the layer later grows on screen (camera zoom, other monitor
 * size), the full texture comes back for good.
 * The reduced texture is the file's own mip level where it stores one -
 * what WE itself samples when it minifies, and nothing above that level is
 * decoded or uploaded - else the full texture box-halved on the GPU
 * (glutil.h WALPE_GL_TEX_LEVEL; WALPE_TEX_STORED_MIPS=0: always halved).
 * A DXT texture stays compressed either way.
 * Version 0.12, mip chains: bilinear sampling is clean up to 2:1 and skips
 * texels beyond (jagged lines, shimmer in motion). No layer texture is
 * sampled beyond 2:1 any more: it is stored reduced until it is within 2:1
 * (above), or, where that is not possible (scripts, puppets, tex_downscale
 * off), the mip chain its file calls for is added the first time the layer
 * is drawn that small (ds_state 3 watches for it). Layers with effects need
 * neither: their passes read the texture at 2:1 at most (fx_working_scale).
 * So a chain's extra third is spent only where it is sampled. */
static void node_tex_fit (SceneNodeContent *s, NodeObj *nv, guint i, const WalpeMat3 *m,
                          int w, int h)
{
    float pw = hypotf (m->m[0] * w, m->m[1] * h), ph = hypotf (m->m[3] * w, m->m[4] * h);
    if (nv->ds_state == 3) {
        if (pw >= 1 && ph >= 1 && (nv->phys_w > pw * 2.02f || nv->phys_h > ph * 2.02f)) {
            node_chain_build (nv);
            SCENE_PICTURE_CHANGED (s);
            g_debug ("scenenode: layer %s texture %dx%d drawn at %.0fx%.0f: mip chain built",
                     nv->name ? nv->name : "?", nv->phys_w, nv->phys_h, pw, ph);
            nv->ds_state = 2;
        }
        return;
    }
    if (nv->ds_state == 1) {
        float sl = tex_slack (TRUE) + 0.002f;
        if ((pw > nv->phys_w * sl || ph > nv->phys_h * sl) && nv->tex_lod > nv->tex_min_lod) {
            /* drawn larger now: the level that fits the new size (the
             * full texture when it takes that), never a smaller one again:
             * a camera that zooms in costs one reload per level crossed */
            gboolean fits = FALSE;
            int L = MAX (node_plain_level (s, nv, i, pw, ph, &fits), nv->tex_min_lod);
            L = MIN (L, nv->tex_lod - 1);
            if (node_tex_load (nv, L, FALSE, FALSE)) {
                fx_invalidate (nv->fxset);
                SCENE_PICTURE_CHANGED (s);
                g_message ("scenenode: layer %s drawn at %.0fx%.0f now: texture back at %dx%d",
                           nv->name ? nv->name : "?", pw, ph, nv->phys_w, nv->phys_h);
                s->trim_heap = TRUE;
            }
            if (nv->tex_lod <= nv->tex_min_lod)
                nv->ds_state = node_chain_pending (nv) ? 3 : 2;
        } else if (node_chain_pending (nv) && pw >= 1 && ph >= 1 &&
                   (nv->phys_w > pw * 2.02f || nv->phys_h > ph * 2.02f)) {
            /* the copy itself shrank beyond 2:1 (camera zoom): its chain */
            node_chain_build (nv);
            SCENE_PICTURE_CHANGED (s);
        }
        return;
    }
    nv->ds_state = 2;
    if ((gint64) nv->tex_w * nv->tex_h >= 1 << 20)    /* a compose layer's size is the scene file's */
        g_debug ("scenenode: tex_fit %s %dx%d on screen %.0fx%.0f: src %d rgba %d dxt %d video %d "
                 "systex %d compose %d puppet %d kind %d fx %d scripted %d",
                 nv->name ? nv->name : "?", nv->tex_w, nv->tex_h, pw, ph, nv->tex_src != NULL,
                 nv->tex_rgba, nv->tex_dxt, nv->video >= 0, nv->systex, nv->compose,
                 nv->puppet != NULL, nv->kind, fx_has_effects (nv->fxset), node_scripted (s, i));
    /* drawn straight from its file texture (no effect chain in between) */
    gboolean fxl = fx_has_effects (nv->fxset) && !s->nofx;
    gboolean plain = nv->tex_src && nv->video < 0 && !nv->systex && !nv->compose &&
                     !fx_has_effects (nv->fxset);
    if (!plain) {
        /* a layer with effects: node_fx_input_fit picks the level */
        if (nv->tex_lazy && !fxl && !node_tex_load (nv, nv->tex_min_lod, TRUE, FALSE))
            node_tex_failed (nv);
        return;
    }
    gboolean fits = FALSE;
    int W = nv->tex_w, H = nv->tex_h, L = node_plain_level (s, nv, i, pw, ph, &fits);
    L = MAX (L, nv->tex_min_lod);
    /* a compressed texture brings its stored chain when it will be sampled
     * beyond 2:1, or when nobody can say (scripts, puppets: as before) */
    gboolean beyond = pw >= 1 && ph >= 1 && ((W >> L) > pw * 2.02f || (H >> L) > ph * 2.02f);
    gboolean chain = nv->tex_dxt && (!fits || beyond || !walpe_gl_tex_stored_levels ());
    gboolean near_ok = L > 0 && (W >> (L - 1)) <= pw * 2.1f && (H >> (L - 1)) <= ph * 2.1f;
    if (nv->tex_lazy || L != nv->tex_lod || (nv->tex_comp && chain != nv->tex_chain)) {
        if (!node_tex_load (nv, L, chain, near_ok)) {
            if (nv->tex_lazy)
                node_tex_failed (nv);
            return;
        }
        SCENE_PICTURE_CHANGED (s);
        if (nv->tex_lod > 0)
            s->trim_heap = TRUE;
    }
    if (nv->tex_lod <= nv->tex_min_lod) {
        /* stays at its size: watch for the day it is drawn beyond 2:1 */
        if (node_chain_pending (nv)) {
            nv->ds_state = 3;
            node_tex_fit (s, nv, i, m, w, h);
        }
        return;
    }
    gint64 full = nv->tex_comp ? nv->tex_bytes << (2 * nv->tex_lod) : (gint64) W * H * 4;
    gint64 saved = full - nv->tex_bytes;
    s->ds_saved += saved;
    g_message ("scenenode: layer %s texture %dx%d drawn at %.0fx%.0f -> stored %dx%d%s "
               "(%.1f MiB less; %.1f MiB in this scene)", nv->name ? nv->name : "?", W, H, pw,
               ph, nv->phys_w, nv->phys_h, nv->tex_comp ? " compressed" : "",
               saved / 1048576.0, s->ds_saved / 1048576.0);
    nv->ds_state = 1;
}

/* the level a plain layer's texture is stored at when the layer is drawn
 * pw x ph pixels; *fits: its size on screen is settled (no script, no
 * puppet), so it may be reduced */
static int node_plain_level (SceneNodeContent *s, NodeObj *nv, guint i, float pw, float ph,
                             gboolean *fits_out)
{
    int W = nv->tex_w, H = nv->tex_h;
    /* without mipmaps, bilinear sampling at exactly 2:1 averages each 2x2
     * texel block, i.e. what a box-halved copy drawn 1:1 shows: identical
     * up to rounding (4K-authored layers on a 1080p monitor). At other
     * ratios the original sampling skips texels (aliasing: thin lines,
     * star fields) and a prefiltered copy looks smoother. A texture whose
     * file has a mip chain is sampled through it by WE: that IS the
     * prefiltered look, so those are reduced while the copy keeps >= 1
     * texel per pixel (a 2.7:1 layer: a quarter of the memory). A texture
     * imported without mipmaps aliases in WE too: strict mode halves it
     * only at 2:1 (+5 %), where nothing changes.
     * WALPE_TEX_DOWNSCALE=2: every texture by the first rule.
     * A DXT texture is reduced through the levels its file stores (it
     * cannot be filtered on the GPU without decoding it); one without
     * stored levels stays as it is, sampled at 2:1 like before. */
    int L = 0;
    gboolean fits = s->downscale && !nv->puppet && nv->kind == NK_IMAGE &&
                    !node_scripted (s, i) && pw >= 1 && ph >= 1 && W > 0 && H > 0;
    gboolean file_mips = walpe_gl_mipmaps_enabled () && nv->tex_mips != 1;
    if (fits && nv->tex_dxt && !(walpe_gl_tex_stored_levels () && nv->tex_mips > 1))
        fits = FALSE;
    if (fits) {
        float rx = W / pw, ry = H / ph;
        if (s->downscale == 2 || file_mips)
            L = node_fit_level (W, H, pw, ph);
        else {
            /* "at 2:1": down to 2 / (1 + tolerance), see tex_slack */
            float lo = tex_slack (FALSE) > 1.0f ? 2.0f / tex_slack (FALSE) - 0.001f : 1.99f;
            if (rx >= lo && rx <= 2.1f && ry >= lo && ry <= 2.1f)
                L = 1;
        }
    }
    *fits_out = fits;
    return L;
}

/* the level a texture read by effect passes working at fk x its size is
 * stored at: the smallest one that still has every texel the first pass
 * reads (fk <= 1/2: half size, <= 1/4: a quarter, ...) */
static int fx_input_level (float fk)
{
    int L = 0;
    while (L < 6 && fk <= 0.5f / (float) (1 << L))
        L++;
    return L;
}

static gint64 node_tex_estimate (const NodeObj *nv, int level);

/* the share of the frame the layer's quad covers within the view (its
 * bounds: a rotated layer counts for more than it shows) */
static float node_view_share (const WalpeMat3 *m)
{
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (int k = 0; k < 4; k++) {
        float qx = (k & 1) ? 1.0f : -1.0f, qy = (k & 2) ? 1.0f : -1.0f;
        float cx = m->m[0] * qx + m->m[3] * qy + m->m[6];
        float cy = m->m[1] * qx + m->m[4] * qy + m->m[7];
        x0 = fminf (x0, cx); x1 = fmaxf (x1, cx);
        y0 = fminf (y0, cy); y1 = fmaxf (y1, cy);
    }
    x0 = fmaxf (x0, -1.0f); y0 = fmaxf (y0, -1.0f);
    x1 = fminf (x1, 1.0f); y1 = fminf (y1, 1.0f);
    /* (a matrix with NaN in it compares false throughout: the whole frame) */
    if (x1 <= x0 || y1 <= y0)
        return 0.0f;
    float a = (x1 - x0) * (y1 - y0) * 0.25f;
    return a >= 0.0f && a <= 1.0f ? a : 1.0f;
}

/* Storing the input of a layer's effects at half size (node_fx_input_fit)
 * gives the same picture only where the first effect reads the texture
 * texel for texel: its first pass then samples the 2 x 2 average either
 * way. Where it displaces its lookups - a shake or water wave under its
 * mask, chromatic aberration over the whole picture - it used to
 * interpolate between full-size texels and now does between the averaged
 * ones: softer, small bright detail loses its core (3306713517: the stars
 * under chromatic aberration; 3258032485: a character whose first shake
 * mask covers 39 % of the picture; 1.2 - 1.4 % of the frame's pixels off
 * by more than 16 / 255 against version 0.12, beyond the 1 % the reduced
 * textures are allowed). Most scenes have such effects on small painted
 * areas (hair, a skirt, water), where it stays far below that. So: a
 * layer whose first effect displaces its lookups over more than a quarter
 * of the frame (the mask's share of the layer x the layer's share of the
 * frame) keeps its texture at full size as the effects' input, as version
 * 0.12 had it; every other layer is stored reduced as before. (Measured:
 * the share of the frame's pixels that end up off by more than 16 / 255
 * is 1.4 - 3.2 % of the displaced share; a quarter keeps it below 1 %.)
 * WALPE_TEX_FX_SOFT=1: reduced whatever the first effect does (the
 * earlier build's behaviour, for comparing).
 *
 * That rule does not look at the picture: 3151551777 kept a blurred
 * backdrop under a water flow and a scrolling strip at full size (21 MiB)
 * for a difference of 4 / 255 at most. So a layer that passes it is
 * measured once its full-size texture is there (fx_input_soft_cost: where
 * the first effect displaces its lookups, how much of the picture would a
 * lookup in the reduced texture get more than 16 / 255 wrong?), and it
 * keeps the full size only when that is more than FX_INPUT_SOFT_P of the
 * frame; else it is reduced like every other layer. (3306713517: 100 %
 * displaced, stars; 3258032485: line art under a shake - both stay.) */
#define FX_INPUT_SOFT_MAX 0.25f
#define FX_INPUT_SOFT_P 0.002f
/* WALPE_TEX_FX_SOFT: 1 = always reduced (the earlier build), keep =
 * never measured, every layer the rule names stays at full size; else 0 */
static int node_fx_soft_mode (void)
{
    static gint mode = -1;
    if (g_atomic_int_get (&mode) < 0) {
        const char *e = g_getenv ("WALPE_TEX_FX_SOFT");
        g_atomic_int_set (&mode, e && *e == '1' ? 1 : e && !strcmp (e, "keep") ? 2 : 0);
    }
    return g_atomic_int_get (&mode);
}
static gboolean node_fx_input_soft (NodeObj *nv, float share)
{
    if (node_fx_soft_mode () == 1 || !(share > FX_INPUT_SOFT_MAX))
        return FALSE;
    /* a layer colour other than white: its own material pass reads the
     * texture first, texel for texel (fx_set_input_tint) */
    for (int c = 0; c < 3; c++)
        if (nv->tint[c] * nv->bright != 1.0f)
            return FALSE;
    return fx_input_displaced (nv->fxset) * share > FX_INPUT_SOFT_MAX;
}

/* ... and it is not known yet, or known to cost more than FX_INPUT_SOFT_P */
static gboolean node_fx_input_keep (NodeObj *nv, float share)
{
    return node_fx_input_soft (nv, share) &&
           (!nv->fx_soft_meas || nv->fx_soft_cost * share > FX_INPUT_SOFT_P);
}

/* the level the texture of a layer with effects working at fk is stored
 * at; share = node_view_share () of the layer */
static int node_fx_level (SceneNodeContent *s, NodeObj *nv, guint i, float fk, float share)
{
    int L = 0;
    if (s->downscale && nv->tex_src && nv->video < 0 && nv->kind == NK_IMAGE &&
        nv->tex_w * nv->tex_h >= 256 * 256 && !node_scripted (s, i) &&
        (nv->tex_rgba || nv->tex_dxt)) {
        L = fx_input_level (fk);
        /* A size that does not divide by 2^L (a 2293 px wide character):
         * the copy is halved rounding up (WALPE_GL_TEX_CEIL, node_tex_load),
         * which makes it exactly the size of the chain's targets at
         * fk = 1/2, 1/4 (ceil (w * fk)): the first pass reads it 1:1, the
         * texels the blit put there are the ones the pass used to read from
         * the full texture. WALPE_TEX_FX_ODD=0: such a texture stays at
         * full size, as before. */
        static int odd = -1;
        if (odd < 0) {
            const char *e = g_getenv ("WALPE_TEX_FX_ODD");
            odd = !(e && *e == '0');
        }
        while (!odd && L > 0 && ((nv->tex_w & ((1 << L) - 1)) || (nv->tex_h & ((1 << L) - 1))))
            L--;
        if (L > 0 && node_fx_input_soft (nv, share)) {
            if (node_fx_soft_mode () == 2) {
                nv->fx_soft_cost = 1.0f;   /* not looked at: as if all of it were lost */
                nv->fx_soft_meas = TRUE;
            }
            if (!nv->fx_soft_meas && nv->tex && nv->tex_ready && !nv->tex_lazy &&
                nv->tex_lod == 0) {
                /* the full-size texture is there: what would it lose? */
                const int res[4] = { nv->tex_full_w ? nv->tex_full_w : nv->tex_w,
                                     nv->tex_full_h ? nv->tex_full_h : nv->tex_h,
                                     nv->tex_w, nv->tex_h };
                nv->fx_soft_cost = fx_input_soft_cost (nv->fxset, nv->tex, res, L);
                nv->fx_soft_meas = TRUE;
            }
            if (!nv->fx_soft_meas) {
                nv->fx_soft_pending = TRUE;   /* full size until it can be looked at */
                L = 0;
            } else if (nv->fx_soft_cost * share > FX_INPUT_SOFT_P) {
                if (!nv->fx_soft_said) {
                    nv->fx_soft_said = TRUE;
                    g_debug ("scenenode: layer %s: its first effect reads the texture at displaced "
                             "coordinates over %.0f %% of the frame (reduced, %.2f %% of the frame "
                             "would be off): the %dx%d texture stays at full "
                             "size as the effects' input (%.1f MiB instead of %.1f)",
                             nv->name ? nv->name : "?",
                             (double) (fx_input_displaced (nv->fxset) * share * 100.0f),
                             (double) (nv->fx_soft_cost * share * 100.0f), nv->tex_w,
                             nv->tex_h, node_tex_estimate (nv, 0) / 1048576.0,
                             node_tex_estimate (nv, L) / 1048576.0);
                }
                L = 0;
            } else if (!nv->fx_soft_said) {
                nv->fx_soft_said = TRUE;
                g_debug ("scenenode: layer %s: its first effect reads the texture at displaced "
                         "coordinates over %.0f %% of the frame, but reduced only %.2f %% of the "
                         "frame would be off: the %dx%d texture is stored reduced",
                         nv->name ? nv->name : "?",
                         (double) (fx_input_displaced (nv->fxset) * share * 100.0f),
                         (double) (nv->fx_soft_cost * share * 100.0f), nv->tex_w, nv->tex_h);
            }
        }
    }
    return L;
}

/* A layer with effects is never drawn from its texture: the first effect
 * pass reads it into a target of the working size (fk x the texture, see
 * fx_working_scale). At fk = 0.5 - a 4K layer on a 1080p monitor, the
 * usual case - the pass reads the texture at exactly 2:1, where bilinear
 * sampling averages 2x2 texels. The texture is then stored at that size:
 * a quarter of the memory (3840x2160 RGBA8: 31.6 -> 7.9 MiB; DXT5: 7.9 ->
 * 2.0 MiB, it stays compressed), from the file's own mip level when it
 * stores one, else the GPU box-filtered half-size copy; at fk <= 0.25 a
 * sixteenth, and so on. Sizes and resolution uniforms stay the full ones.
 * A pass that reads its input straight (tint, colour, masks) gets the
 * same values (+-1/255 for the box copy); one that displaces its lookups
 * interpolates between the reduced texels instead of between full-size
 * ones (slightly softer at hard edges).
 * Same rules as tex_downscale: file textures whose size divides evenly,
 * not video / album art / text, not when a script or animation may change
 * the layer's size. If the layer is drawn larger later, the full texture
 * is loaded again and kept. Off with tex_downscale. */
static void node_fx_input_fit (SceneNodeContent *s, NodeObj *nv, guint i, float fk, float share)
{
    if (nv->fxin_state == 1) {
        if ((fx_input_level (fk) >= nv->tex_lod && !node_fx_input_keep (nv, share)) ||
            nv->tex_lod <= nv->tex_min_lod)
            return;
        /* drawn larger now (or over more of the frame, with a first effect
         * that displaces its lookups): the level its passes read at the
         * new working size (the full texture when it takes that), never a
         * smaller one again */
        int need = MAX (node_fx_level (s, nv, i, fk, share), nv->tex_min_lod);
        need = MIN (need, nv->tex_lod - 1);
        gboolean near = need > 0 && (nv->tex_w >> (need - 1)) <= ceilf (nv->tex_w * fk) * 2.02f &&
                        (nv->tex_h >> (need - 1)) <= ceilf (nv->tex_h * fk) * 2.02f;
        if (node_tex_load (nv, need, FALSE, near)) {
            fx_invalidate (nv->fxset);
            SCENE_PICTURE_CHANGED (s);
            g_message ("scenenode: layer %s is drawn larger now (effects at %.3f of its "
                       "texture): texture back at %dx%d", nv->name ? nv->name : "?", (double) fk,
                       nv->phys_w, nv->phys_h);
            s->trim_heap = TRUE;
        }
        if (nv->tex_lod <= nv->tex_min_lod)
            nv->fxin_state = 2;
        return;
    }
    nv->fxin_state = 2;
    /* (a second round for a layer that node_fx_level () keeps at full size
     * until it has seen the texture: it is loaded in the first round and
     * looked at in the second, which may reduce it after all) */
    for (int round = 0; round < 2; round++) {
        int L = MAX (node_fx_level (s, nv, i, fk, share), nv->tex_min_lod);
        /* one level larger is still read within 2:1 by the first pass */
        gboolean near_ok = L > 0 && (nv->tex_w >> (L - 1)) <= ceilf (nv->tex_w * fk) * 2.02f &&
                           (nv->tex_h >> (L - 1)) <= ceilf (nv->tex_h * fk) * 2.02f;
        if (nv->tex_lazy || L != nv->tex_lod) {
            if (!node_tex_load (nv, L, FALSE, near_ok)) {
                if (nv->tex_lazy)
                    node_tex_failed (nv);
                return;
            }
            fx_invalidate (nv->fxset);
            SCENE_PICTURE_CHANGED (s);
        }
        if (!nv->fx_soft_pending)
            break;
        nv->fx_soft_pending = FALSE;
    }
    if (nv->tex_lod <= nv->tex_min_lod)
        return;
    gint64 full = nv->tex_comp ? nv->tex_bytes << (2 * nv->tex_lod)
                               : (gint64) nv->tex_w * nv->tex_h * 4;
    gint64 saved = full - nv->tex_bytes;
    s->ds_saved += saved;
    g_debug ("scenenode: layer %s texture %dx%d is the input of effects run at %.3f of it -> "
             "stored %dx%d%s (%.1f MiB less; %.1f MiB in this scene)", nv->name ? nv->name : "?",
             nv->tex_w, nv->tex_h, (double) fk, nv->phys_w, nv->phys_h,
             nv->tex_comp ? " compressed" : "", saved / 1048576.0, s->ds_saved / 1048576.0);
    nv->fxin_state = 1;
    s->trim_heap = TRUE;
}

/* the smallest working factor of a layer's effects (fx_working_scale):
 * half the texture for a layer a script or animation may resize, a compose
 * layer (its size is the capture's) and text; else the global floor */
static float node_fx_kmin (SceneNodeContent *s, NodeObj *nv, guint i)
{
    if (nv->compose || nv->kind != NK_IMAGE || nv->video >= 0 || nv->puppet || node_scripted (s, i))
        return fmaxf (0.5f, fx_min_scale ());
    /* tex_downscale off (preset Maximum, the exact picture): the texture
     * stays at file size, so its effects must not work below half of it
     * either - a first pass that reads it beyond 2:1 skips texels (plain
     * bilinear, no mip chain). 3306713517's 5760 px layer shown 1920 px
     * wide worked at 3/8 and Maximum was not version 0.12's picture. */
    if (!s->downscale)
        return fmaxf (0.5f, fx_min_scale ());
    return fx_min_scale ();
}

/* fx_region.h: the part of a layer's unit quad (corners +-1, drawn
 * with matrix m) that can reach a pixel of the view, as the rectangle of
 * texture coordinates the draw samples there (the quad shader: u = (x + 1)
 * / 2, v = (1 - y) / 2). The quad is cut, in its own coordinates, by the
 * four edges of the view pushed out a little (pixel centres stop half a
 * pixel inside the view; the 1e-4 on top covers the arithmetic here).
 * Returns 1 and uv = u0, v0, u1, v1, or 0 when nothing is said about the
 * draw: no number in the matrix, nothing of the quad in view, or less than
 * a sixteenth of it outside. */
static int node_visible_uv (const WalpeMat3 *m, float uv[4])
{
    double px[16], py[16];
    int n = 4;
    for (int i = 0; i < 9; i++)
        if (!isfinite (m->m[i]))
            return 0;
    px[0] = -1, py[0] = -1;
    px[1] = 1, py[1] = -1;
    px[2] = 1, py[2] = 1;
    px[3] = -1, py[3] = 1;
    const double lim = 1.0 + 1e-4;
    for (int e = 0; e < 4 && n >= 3; e++) {
        /* inside: sg * (a x + b y + c) <= lim */
        const double a = e < 2 ? m->m[0] : m->m[1], b = e < 2 ? m->m[3] : m->m[4];
        const double c = e < 2 ? m->m[6] : m->m[7], sg = (e & 1) ? 1.0 : -1.0;
        double qx[16], qy[16];
        int k = 0;
        for (int i = 0; i < n; i++) {
            const int j = (i + 1) % n;
            const double di = lim - sg * (a * px[i] + b * py[i] + c);
            const double dj = lim - sg * (a * px[j] + b * py[j] + c);
            if (di >= 0)
                qx[k] = px[i], qy[k] = py[i], k++;
            if ((di >= 0) != (dj >= 0)) {
                const double t = di / (di - dj);
                qx[k] = px[i] + t * (px[j] - px[i]);
                qy[k] = py[i] + t * (py[j] - py[i]);
                k++;
            }
        }
        n = MIN (k, 12);
        memcpy (px, qx, sizeof (double) * (gsize) n);
        memcpy (py, qy, sizeof (double) * (gsize) n);
    }
    if (n < 3)
        return 0;
    double x0 = 2, y0 = 2, x1 = -2, y1 = -2;
    for (int i = 0; i < n; i++) {
        if (!isfinite (px[i]) || !isfinite (py[i]))
            return 0;
        x0 = fmin (x0, px[i]); x1 = fmax (x1, px[i]);
        y0 = fmin (y0, py[i]); y1 = fmax (y1, py[i]);
    }
    x0 = CLAMP (x0, -1.0, 1.0); x1 = CLAMP (x1, -1.0, 1.0);
    y0 = CLAMP (y0, -1.0, 1.0); y1 = CLAMP (y1, -1.0, 1.0);
    if (!(x1 > x0 && y1 > y0) || (x1 - x0) * (y1 - y0) * 16.0 > 4.0 * 15.0)
        return 0;
    uv[0] = (float) ((x0 + 1.0) / 2.0);
    uv[2] = (float) ((x1 + 1.0) / 2.0);
    uv[1] = (float) ((1.0 - y1) / 2.0);
    uv[3] = (float) ((1.0 - y0) / 2.0);
    /* outwards after the conversion to float */
    uv[0] = nextafterf (uv[0], -1.0f);
    uv[1] = nextafterf (uv[1], -1.0f);
    uv[2] = nextafterf (uv[2], 2.0f);
    uv[3] = nextafterf (uv[3], 2.0f);
    return 1;
}

/* unit quad -> clip space for a node: tail * world (T-R-S, parents
 * applied) * quad size; the quad's +Y edge holds v=0 (art top) and scene
 * space is Y-up: no flip needed */
static void node_clip_matrix (const NodeObj *nv, const WalpeMat3 *tail, WalpeMat3 *m,
                              WalpeMat3 *quad)
{
    *m = *tail;
    walpe_mat3_identity (quad);
    if (nv->kind == NK_TEXT && nv->tquad[2] > 0) {
        /* text texture: the box, or grown from its aligned edge */
        /* helpers premultiply: scale first, then translate */
        walpe_mat3_scale (quad, nv->tquad[2] / 2.0f, nv->tquad[3] / 2.0f);
        walpe_mat3_translate (quad, nv->tquad[0], nv->tquad[1]);
    } else {
        walpe_mat3_scale (quad, nv->size[0] / 2.0f, nv->size[1] / 2.0f);
        /* anchor: move the quad so the origin lands on that point */
        if (nv->anchor[0] != 0 || nv->anchor[1] != 0)
            walpe_mat3_translate (quad, -nv->anchor[0] * nv->size[0] / 2.0f,
                                  -nv->anchor[1] * nv->size[1] / 2.0f);
    }
    walpe_mat3_mul (m, m, &nv->world);
    walpe_mat3_mul (m, m, quad);
}

/* First frame: every file texture whose load was put off (node_tex_predict)
 * is loaded now, at the size its layer is drawn at on this monitor -
 * hidden and off-screen layers too, from the transform they have, so no
 * decode is left for the moment a layer is first shown. */
static gint64 node_tex_estimate (const NodeObj *nv, int level)
{
    /* (a texture that was not probed, an embedded PNG / JPEG: RGBA8) */
    return ((gint64) MAX (1, nv->tex_w >> level) * MAX (1, nv->tex_h >> level) *
            (nv->tex_bpt8 ? nv->tex_bpt8 : 32)) / 8;
}

/* Texture budget: when the scene's file textures (effect textures and
 * sprites already loaded + the layer textures at the sizes planned for
 * them) need more than the budget, the largest layer textures are stored
 * one more level down, largest first, until the sum fits or everything is
 * down to 64 texels. A normal scene is far below it; one with dozens of
 * 8K layers, or made to exhaust memory, gets soft instead of taking
 * gigabytes. */
static void scene_plan_texture_budget (SceneNodeContent *s, const WalpeMat3 *tail, int w, int h)
{
    gint64 total = s->tex_eager_bytes;
    for (guint i = 0; i < s->nodes->len; i++) {
        NodeObj *nv = &g_array_index (s->nodes, NodeObj, i);
        if (!nv->tex_lazy)
            continue;
        WalpeMat3 m, quad;
        node_clip_matrix (nv, tail, &m, &quad);
        gboolean fits = FALSE;
        float pw = hypotf (m.m[0] * w, m.m[1] * h), ph = hypotf (m.m[3] * w, m.m[4] * h);
        if (fx_has_effects (nv->fxset) && !s->nofx) {
            float fk = fx_working_scale (nv, &m, w, h, nv->tex_w ? nv->tex_w : 256,
                                         nv->tex_h ? nv->tex_h : 256, node_fx_kmin (s, nv, i));
            nv->tex_plan = node_fx_level (s, nv, i, fk, node_view_share (&m));
        } else if (!nv->systex && !nv->compose) {
            nv->tex_plan = node_plain_level (s, nv, i, pw, ph, &fits);
        }
        total += node_tex_estimate (nv, nv->tex_plan);
    }
    gint64 need = total;
    int reduced = 0;
    while (total > s->tex_budget) {
        NodeObj *big = NULL;
        gint64 bb = 0;
        for (guint i = 0; i < s->nodes->len; i++) {
            NodeObj *nv = &g_array_index (s->nodes, NodeObj, i);
            int lod = MAX (nv->tex_plan, nv->tex_min_lod);
            if (!nv->tex_lazy || (nv->tex_w >> lod) <= 64 || (nv->tex_h >> lod) <= 64 || lod >= 8)
                continue;
            gint64 e = node_tex_estimate (nv, lod);
            if (e > bb) {
                bb = e;
                big = nv;
            }
        }
        if (!big)
            break;
        int lod = MAX (big->tex_plan, big->tex_min_lod);
        big->tex_min_lod = lod + 1;
        total -= bb - node_tex_estimate (big, lod + 1);
        reduced++;
    }
    if (reduced)
        g_warning ("scenenode: the textures of this scene need %.0f MiB, the budget is %.0f MiB: "
                   "the largest were stored smaller (%d halvings, now %.0f MiB); "
                   "WALPE_TEX_BUDGET_MB=<MiB> changes it, 0 = no limit", need / 1048576.0,
                   s->tex_budget / 1048576.0, reduced, total / 1048576.0);
}

/* a deferred layer texture is loaded now, at the level the layer's size on
 * screen asks for (a texture that is loaded already is fitted to it) */
static void node_load_deferred (SceneNodeContent *s, NodeObj *nv, guint i, const WalpeMat3 *tail,
                                int w, int h, int win_w, int win_h)
{
    WalpeMat3 m, quad;
    node_clip_matrix (nv, tail, &m, &quad);
    if (nv->ds_state != 2)
        node_tex_fit (s, nv, i, &m, w, h);
    if ((nv->tex_lazy || nv->fxin_state != 2) && fx_has_effects (nv->fxset) && !s->nofx) {
        int fw = nv->tex_w ? nv->tex_w : 256, fh = nv->tex_h ? nv->tex_h : 256;
        node_fx_input_fit (s, nv, i, fx_working_scale (nv, &m, win_w, win_h, fw, fh,
                                                       node_fx_kmin (s, nv, i)),
                           node_view_share (&m));
    }
    if (nv->tex_lazy && !node_tex_load (nv, nv->tex_min_lod, TRUE, FALSE))
        node_tex_failed (nv);
}

/* WALPE_TEX_LOAD_HIDDEN=1: the textures of layers that are not shown are
 * loaded with the first frame too, as before */
static gboolean tex_load_hidden (void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = g_getenv ("WALPE_TEX_LOAD_HIDDEN");
        on = e && *e == '1';
    }
    return on;
}

/* The first frame: every deferred texture of a layer that is shown, and
 * the hidden layers. A layer that is hidden (its own "visible" or a
 * parent's: a user option that is off, a variant of the scene, something a
 * script shows later) used to keep its file texture at file size for good,
 * since nothing is ever fitted to a layer that is not drawn; scenes carry
 * dozens of them (2908585704: two hidden variants of every picture). Now:
 * - a deferred one (stored levels, cheap to load: about 20 ms for a 4K
 *   picture read from its level 1) holds no texture until the layer is
 *   drawn for the first time (render loop);
 * - one that had to be decoded while the scene was parsed (an embedded PNG
 *   or JPEG: decoding a 4K one again takes several hundred ms, which would
 *   freeze the frame it appears on) stays loaded, but is fitted to the size
 *   the layer would be drawn at, like the layers that are shown; a variant
 *   of a shown layer then shares that layer's texture.
 * Once shown a layer keeps its texture, hidden again or not. A layer at
 * alpha 0 counts as shown: that is how fades start. */
/* w, h: the size the frame is rendered at; win_w, win_h: the window's (the
 * same unless render_scale is below 100). As in the render loop, what is
 * stored for an effect layer follows the window's size (the working factor
 * shrinks with the render scale afterwards), the plain layer policy the
 * rendered size. */
static void scene_load_deferred_textures (SceneNodeContent *s, const WalpeMat3 *tail, int w, int h,
                                          int win_w, int win_h)
{
    if (s->tex_budget > 0)
        scene_plan_texture_budget (s, tail, w, h);
    /* The pictures that worker threads are decoding since the scene was
     * parsed: every load below is worked out once without loading, only to
     * tell the workers the level it will ask for (the layer's state is put
     * back afterwards). A worker that has not finished a picture then
     * reduces it as well, and this thread only uploads the result. */
    for (guint i = 0; i < s->nodes->len; i++) {
        NodeObj *nv = &g_array_index (s->nodes, NodeObj, i);
        if (!nv->tex_lazy || !nv->tex_job ||
            (!nv->world_visible && !tex_load_hidden () && !nv->tex_pic))
            continue;
        NodeObj keep = *nv;
        tex_announce = TRUE;
        node_load_deferred (s, nv, i, tail, w, h, win_w, win_h);
        tex_announce = FALSE;
        *nv = keep;
    }
    int waiting = 0, fitted = 0;
    for (guint i = 0; i < s->nodes->len; i++) {
        NodeObj *nv = &g_array_index (s->nodes, NodeObj, i);
        gboolean hidden = !nv->world_visible && !tex_load_hidden ();
        if (!nv->tex_lazy) {
            if (hidden && nv->tex && nv->tex_src && nv->video < 0 && !nv->systex &&
                !nv->compose && nv->kind == NK_IMAGE) {
                node_load_deferred (s, nv, i, tail, w, h, win_w, win_h);   /* fits it, nothing to load */
                fitted++;
            }
            continue;
        }
        if (hidden && nv->tex_pic) {
            /* a PNG / JPEG: decoded already (worker thread), and decoding
             * it when the layer appears would freeze that frame: loaded
             * now, at the size the layer would be drawn at */
            node_load_deferred (s, nv, i, tail, w, h, win_w, win_h);
            fitted++;
            continue;
        }
        if (hidden) {
            waiting++;
            node_tex_job_done (nv);   /* cheap to read when it is shown */
            continue;
        }
        node_load_deferred (s, nv, i, tail, w, h, win_w, win_h);
    }
    if (waiting || fitted)
        g_debug ("scenenode: hidden layers: %d hold no texture until they are shown, "
                 "%d keep theirs at the size they would be drawn at", waiting, fitted);
    SCENE_PICTURE_CHANGED (s);
    s->tex_deferred = FALSE;
}

/* WALPE_PROP_PROBE: the resolved scene state as JSON (propprobe.h) */
static void probe_vec (JsonBuilder *b, const char *k, const float *v, int n);

static void scenenode_probe_general (SceneNodeContent *s, JsonBuilder *b)
{
    probe_vec (b, "camzoom", &s->cam_zoom, 1);
    probe_vec (b, "genzoom", &s->gen_zoom, 1);
    probe_vec (b, "clearcolor", s->clear, 3);
}

static void probe_vec (JsonBuilder *b, const char *k, const float *v, int n)
{
    json_builder_set_member_name (b, k);
    json_builder_begin_array (b);
    for (int i = 0; i < n; i++)
        json_builder_add_double_value (b, isfinite (v[i]) ? round ((double) v[i] * 1e4) / 1e4 : 0);
    json_builder_end_array (b);
}

static void scenenode_probe_dump (SceneNodeContent *s)
{
    JsonBuilder *b = json_builder_new ();
    json_builder_begin_object (b);
    json_builder_set_member_name (b, "origin");
    json_builder_add_string_value (b, s->origin);
    json_builder_set_member_name (b, "frame");
    json_builder_add_int_value (b, s->frame_no);
    scenenode_probe_general (s, b);
    json_builder_set_member_name (b, "nodes");
    json_builder_begin_array (b);
    for (guint i = 0; i < s->nodes->len; i++) {
        NodeObj *nv = &g_array_index (s->nodes, NodeObj, i);
        json_builder_begin_object (b);
        json_builder_set_member_name (b, "id");
        json_builder_add_int_value (b, nv->id);
        json_builder_set_member_name (b, "name");
        json_builder_add_string_value (b, nv->name ? nv->name : "");
        json_builder_set_member_name (b, "kind");
        json_builder_add_int_value (b, nv->kind);
        json_builder_set_member_name (b, "visible");
        json_builder_add_boolean_value (b, nv->visible);
        probe_vec (b, "alpha", &nv->alpha, 1);
        probe_vec (b, "origin", nv->origin, 2);
        probe_vec (b, "scale", nv->scale, 2);
        probe_vec (b, "angle", &nv->angle_z, 1);
        probe_vec (b, "color", nv->tint, 3);
        probe_vec (b, "size", nv->size, 2);
        json_builder_set_member_name (b, "blend");
        json_builder_add_int_value (b, nv->blend);
        json_builder_set_member_name (b, "tex");
        {
            char *bn = nv->tex_src ? g_path_get_basename (nv->tex_src) : NULL;
            char *t = g_strdup_printf ("%s %dx%d%s%s", bn ? bn : "-", nv->tex_w, nv->tex_h,
                                       nv->video >= 0 ? " video" : "", nv->systex ? " sys" : "");
            json_builder_add_string_value (b, t);
            g_free (t);
            g_free (bn);
        }
        if (nv->kind == NK_TEXT) {
            json_builder_set_member_name (b, "text");
            json_builder_add_string_value (b, nv->text_value ? nv->text_value : "");
            if (nv->tstyle) {
                float ts[3] = { nv->tstyle->pointsize, nv->tstyle->maxwidth, nv->tstyle->padding };
                probe_vec (b, "textstyle", ts, 3);
                json_builder_set_member_name (b, "font");
                json_builder_add_string_value (b, nv->tstyle->font_ref ? nv->tstyle->font_ref : "");
            }
        }
        if (nv->weparts) {
            if (nv->inst) {   /* as scripts left them */
                memcpy (nv->pov, &nv->inst->rate, sizeof (float) * 6);
                memcpy (nv->pov + 6, nv->inst->color, sizeof (float) * 3);
            }
            probe_vec (b, "particle", nv->pov, 9);
        }
        if (nv->puppet) {
            json_builder_set_member_name (b, "puppet");
            walpe_puppet_probe (nv->puppet, b);
        }
        if (nv->fxset) {
            json_builder_set_member_name (b, "fx");
            fx_probe_dump (nv->fxset, b);
        }
        json_builder_end_object (b);
    }
    json_builder_end_array (b);
    walpe_probe_write (b, s->origin);
    g_object_unref (b);
}

/* Nothing is drawn while paused (a fullscreen window covers the monitor,
 * the display sleeps): the effect render targets, the cached effect outputs
 * and the compose captures are given back - the first frame after the pause
 * makes them again. File textures stay (decoding them again would take too
 * long), so do targets that carry a picture from frame to frame. */
static void bake_drop (SceneNodeContent *s);
static void scene_target_drop (GLuint *fbo, GLuint *tex);

/* ---- quality accounting (common/quality.h) -----------------------------
 * Effect pass pixels are counted exactly where the passes are drawn
 * (FxAcct). Everything else - layers, particles - is what the GPU reports
 * as fragment shader invocations for a frame minus that frame's effect
 * pixels: a pipeline statistics query (GL_ARB_pipeline_statistics_query,
 * core in GL 4.6) around one frame in 20, read back a few frames later
 * when the result is there, and only while somebody looks at the numbers
 * (WalpeContent.stats_wanted). Without the extension the layers' on-screen
 * areas stand in (particles are then not counted). */
#ifndef GL_FRAGMENT_SHADER_INVOCATIONS_ARB
#define GL_FRAGMENT_SHADER_INVOCATIONS_ARB 0x82F4
#endif

static void scenenode_px_begin (SceneNodeContent *s)
{
    s->qa.base = 0;
    if (!s->base.stats_wanted || s->px_state != 0 || s->frame_no % 20 != 3)
        return;
    if (s->px_can < 0)
        s->px_can = epoxy_is_desktop_gl () &&
                    (epoxy_gl_version () >= 46 ||
                     epoxy_has_gl_extension ("GL_ARB_pipeline_statistics_query"));
    if (!s->px_can)
        return;
    GLint cur = 0;   /* somebody else's query of the same kind is running */
    glGetQueryiv (GL_FRAGMENT_SHADER_INVOCATIONS_ARB, GL_CURRENT_QUERY, &cur);
    if (cur)
        return;
    if (!s->px_query)
        glGenQueries (1, &s->px_query);
    glBeginQuery (GL_FRAGMENT_SHADER_INVOCATIONS_ARB, s->px_query);
    s->px_state = 1;
}

static void scenenode_px_end (SceneNodeContent *s, int w, int h)
{
    FxAcct a;
    fx_acct_take (&a);
    if (s->px_state == 1) {
        glEndQuery (GL_FRAGMENT_SHADER_INVOCATIONS_ARB);
        s->px_fx = a.px_now;
        s->px_state = 2;
    } else if (s->px_state == 2) {
        GLint ready = 0;
        glGetQueryObjectiv (s->px_query, GL_QUERY_RESULT_AVAILABLE, &ready);
        if (ready) {
            GLuint64 total = 0;
            glGetQueryObjectui64v (s->px_query, GL_QUERY_RESULT, &total);
            double base = total > s->px_fx ? (double) (total - s->px_fx) : 0.0;
            s->px_base = s->px_ok ? s->px_base * 0.5 + base * 0.5 : base;
            s->px_ok = TRUE;
            s->px_state = 0;
        }
    }
    /* sums; a report is their average (scenenode_quality_stats) */
    guint64 *d = (guint64 *) &s->qa.fx, *o = (guint64 *) &a;
    for (gsize k = 0; k < G_STRUCT_OFFSET (FxAcct, passes_now) / sizeof (guint64); k++)
        d[k] += o[k];
    s->qa.fx.passes_now += a.passes_now;
    s->qa.fx.passes_nom += a.passes_nom;
    s->qa.fx.passes_post_off += a.passes_post_off;
    for (int k = 0; k < 4; k++)
        s->qa.fx.passes_hold[k] += a.passes_hold[k];
    s->qstats.w = w;
    s->qstats.h = h;
    /* qa.base holds this frame's layer areas: into the running mean */
    double n = ++s->qa.frames;
    s->qstats.px_base += (s->qa.base - s->qstats.px_base) / MIN (n, 30.0);
}

/* render thread: the averages since the last call (at least a few frames) */
static gboolean scenenode_quality_stats (WalpeContent *c, WalpeQualityStats *out)
{
    SceneNodeContent *s = (SceneNodeContent *) c;
    if (s->qa.frames >= 4 || !s->qstats_ok) {
        if (!s->qa.frames)
            return FALSE;
        double n = s->qa.frames;
        const FxAcct *a = &s->qa.fx;
        WalpeQualityStats *q = &s->qstats;
        q->px_fx = a->px_nom / n;
        q->passes = a->passes_nom / n;
        for (int k = 0; k < 4; k++) {
            q->px_hold[k] = a->px_hold[k] / n;
            q->passes_hold[k] = a->passes_hold[k] / n;
        }
        for (int k = 0; k < 3; k++)
            q->px_fxres[k] = a->px_fxres[k] / n;
        q->px_post_red = a->px_post_red / n;
        q->px_post_off = a->px_post_off / n;
        q->passes_post_off = a->passes_post_off / n;
        q->px_now = a->px_now / n;
        q->passes_now = a->passes_now / n;
        memset (&s->qa.fx, 0, sizeof (s->qa.fx));
        s->qa.frames = 0;
        s->qstats_ok = TRUE;
    }
    *out = s->qstats;
    out->fps = s->animates ? (s->fps_cap > 0 ? s->fps_cap : 30) : 0;
    out->measured = s->px_ok;
    if (s->px_ok)
        out->px_base = s->px_base;
    gint64 bytes = 0;
    fx_targets_stats (&bytes, NULL);
    out->target_bytes = (double) bytes;
    return TRUE;
}

/* render thread: new knob values for the running scene. The effect update
 * rate applies from the next frame; a different post-processing level
 * makes every effect set draw again (cached outputs were made with the old
 * one). Texture resolution is a load-time decision: the host reloads. */
/* A quality knob changed what this scene draws while it runs (effect
 * update rate or its mode, effect resolution, post-processing; a preset or
 * a wallpaper's own settings arrive as those). The picture is no longer
 * the one on screen: the serial is part of the frame's signature, so an
 * idle scene repaints and the static bake is taken anew. */
static void scenenode_picture_changed (SceneNodeContent *s)
{
    s->base.picture_serial++;
}

static void scenenode_set_quality (WalpeContent *c, const WalpeQuality *q)
{
    SceneNodeContent *s = (SceneNodeContent *) c;
    gboolean post = q->post != s->q.post;
    /* every knob that changes the picture of a running scene (the others:
     * tex_res and texture_slack reload it, render_scale has its own
     * setter, fps_idle changes no frame) */
    const gboolean picture = post || q->fx_rate != s->q.fx_rate ||
                             !q->fx_rate_all != !s->q.fx_rate_all || q->fx_res != s->q.fx_res;
    if (q->fx_res != s->q.fx_res)   /* its hysteresis starts anew */
        for (guint i = 0; i < s->nodes->len; i++)
            g_array_index (s->nodes, NodeObj, i).fx_k2 = 0;
    int tex_res = s->q.tex_res;
    s->q = *q;
    s->q.tex_res = tex_res;
    if (post)
        for (guint i = 0; i < s->nodes->len; i++)
            fx_invalidate (g_array_index (s->nodes, NodeObj, i).fxset);
    if (picture)
        scenenode_picture_changed (s);
    g_debug ("scenenode: quality: effect rate 1/%d (%s), effect resolution %d, post-processing %d",
             s->q.fx_rate, s->q.fx_rate_all ? "all" : "slow", s->q.fx_res, s->q.post);
}

/* render thread, once, when the first frames have been drawn (the layers'
 * textures are stored at the level their on-screen size calls for by
 * then): quality knob tex_res (half / quarter) - every image layer's
 * texture one or two halvings smaller than that (softer; a quarter / a
 * sixteenth of its memory), never below 64 pixels a side, and never
 * stored larger again. A block-compressed texture has only the levels its
 * file stores; one that is not loaded yet keeps its own level. */
static void scenenode_texres_apply (SceneNodeContent *s)
{
    if (s->texres_applied || s->frame_no < 3)
        return;
    s->texres_applied = TRUE;
    for (guint i = 0; s->q.tex_res > 0 && i < s->nodes->len; i++) {
        NodeObj *nv = &g_array_index (s->nodes, NodeObj, i);
        if (nv->kind != NK_IMAGE || !nv->tex_src || nv->video >= 0 || nv->systex ||
            nv->tex_lazy || !nv->tex)
            continue;
        const int have = nv->tex_lod, was = nv->tex_min_lod;
        int lod = have + s->q.tex_res;
        while (lod > have && ((nv->tex_w >> lod) < 64 || (nv->tex_h >> lod) < 64))
            lod--;
        if (lod <= have)
            continue;
        nv->tex_min_lod = MAX (was, lod);
        if (node_tex_load (nv, lod, nv->tex_chain, FALSE))
            fx_invalidate (nv->fxset);
        if (nv->tex_lod < nv->tex_min_lod)   /* the file has no such level */
            nv->tex_min_lod = MAX (was, nv->tex_lod);
    }
}

static char *scenenode_quality_describe (WalpeContent *c)
{
    SceneNodeContent *s = (SceneNodeContent *) c;
    GString *o = g_string_new (NULL);
    fx_set_quality (s->q.fx_rate, s->q.fx_rate_all, s->q.post,
                    s->fps_cap > 0 ? s->fps_cap : 30, s->frame_no);
    for (guint i = 0; i < s->nodes->len; i++) {
        NodeObj *nv = &g_array_index (s->nodes, NodeObj, i);
        if (!nv->fxset || !nv->world_visible || nv->fx_skip)
            continue;
        /* an update divisor holds a set only over an input that stands
         * still: not a compose layer's captured frame, not a video */
        char *lab = g_strdup_printf ("%s%s", nv->name ? nv->name : "?",
                                     nv->compose ? " [compose: redrawn every frame]"
                                     : nv->video >= 0 ? " [video: redrawn every frame]" : "");
        fx_quality_describe (nv->fxset, lab, o);
        g_free (lab);
    }
    return g_string_free (o, FALSE);
}

static void scenenode_drop_targets (SceneNodeContent *s)
{
    for (guint i = 0; i < s->nodes->len; i++) {
        NodeObj *nv = &g_array_index (s->nodes, NodeObj, i);
        fx_free_targets_for_object (nv->fxset);
        if (nv->ctex)
            glDeleteTextures (1, &nv->ctex);
        if (nv->cfbo)
            glDeleteFramebuffers (1, &nv->cfbo);
        nv->ctex = nv->cfbo = 0;
    }
    bake_drop (s);
    scene_target_drop (&s->rs.fbo, &s->rs.tex);
    walpe_weparticles_drop_targets ();
    fx_targets_release ();
    s->trim_heap = TRUE;
    if (g_getenv ("WALPE_GPU_MEM"))
        walpe_gl_mem_report ("paused");
}

/* One line per scene load, at the normal log level, after the first frame
 * (render targets, half-size copies and text exist by then): what the
 * scene is made of and what its textures cost, by kind. "other" is the
 * rest of what the context can see: effect masks and noise, particle
 * sprites, video frames, frames shared between monitors - and the scenes
 * of other monitors, whose textures live in the same share group. */
static void scenenode_log_summary (SceneNodeContent *s, int w, int h)
{
    int n_img = 0, n_text = 0, n_part = 0, n_vis = 0, n_fx = 0, n_cap = 0, n_imgtex = 0,
        n_texttex = 0, n_tot = 0, n_rt = 0;
    gint64 b_img = 0, b_imgc = 0, b_text = 0, b_cap = 0, b_tot = 0, b_totc = 0, b_rt = 0;
    GHashTable *seen = g_hash_table_new (NULL, NULL);
    for (guint i = 0; i < s->nodes->len; i++) {
        NodeObj *nv = &g_array_index (s->nodes, NodeObj, i);
        n_img += nv->kind == NK_IMAGE;
        n_text += nv->kind == NK_TEXT;
        n_part += nv->kind == NK_PARTICLE;
        n_vis += nv->visible != 0;
        n_fx += fx_has_effects (nv->fxset);
        if (nv->ctex) {
            n_cap++;
            b_cap += (gint64) nv->cw * nv->ch * 4;
        }
        if (!nv->tex || nv->video >= 0 || nv->systex ||
            !g_hash_table_add (seen, GUINT_TO_POINTER (nv->tex)))
            continue;
        gboolean c = FALSE;
        gint64 b = walpe_gl_tex_bytes (nv->tex, &c);
        if (nv->kind == NK_TEXT) {
            n_texttex++;
            b_text += b;
        } else {
            n_imgtex++;
            b_img += b;
            if (c)
                b_imgc += b;
        }
    }
    g_hash_table_destroy (seen);
    fx_targets_stats (&b_rt, &n_rt);
    walpe_gl_tex_totals (&n_tot, &b_tot, &b_totc);
    gint64 b_other = b_tot - b_img - b_text - b_cap - b_rt;
    int n_other = n_tot - n_imgtex - n_texttex - n_cap - n_rt;
#define MIB(x) ((double) (x) / 1048576.0)
    g_message ("scene summary: %ux%u scene on %dx%d; %u layers (%d image, %d text, %d particle, "
               "%d visible); effects on %d layers, %d chains built, %d failed, %d layers static; "
               "%d scripts; textures %d = %.1f MiB (%.1f compressed): layer images %d = %.1f "
               "(%.1f compressed), effect targets %d = %.1f, text %d = %.1f, captures %d = %.1f, "
               "other %d = %.1f%s",
               (guint) fminf (fmaxf (s->scene_w, 0.0f), 4e9f),
               (guint) fminf (fmaxf (s->scene_h, 0.0f), 4e9f), w, h, s->nodes->len, n_img, n_text, n_part,
               n_vis, n_fx, s->fx_ok, s->fx_failed, s->fx_static, walpe_ss_count (s->ss), n_tot,
               MIB (b_tot), MIB (b_totc), n_imgtex, MIB (b_img), MIB (b_imgc), n_rt, MIB (b_rt),
               n_text ? n_texttex : 0, MIB (b_text), n_cap, MIB (b_cap), MAX (n_other, 0),
               MIB (MAX (b_other, 0)), n_tot ? "" : " (sizes not available on this GL)");
    /* how the pictures got there: the most file-texture bytes on the GPU
     * at any moment since this load began (its video-memory peak; the
     * count starts in walpe_scenenode_new_from_root), and the process's
     * counts so far */
    gint64 ft_live = 0, ft_peak = 0;
    WalpeTexDecodeStats ds;
    walpe_gl_tex_live_stats (&ft_live, &ft_peak, FALSE);
    walpe_gl_tex_predecode_stats (&ds);
    g_message ("scene load: file textures on the GPU at most %.1f MiB while loading, %.1f MiB now; "
               "so far %u pictures decoded on worker threads (+ %u by the render thread, %u not "
               "used), reduced copies: %u made before the upload, %u by a GL blit of the full "
               "texture", MIB (ft_peak), MIB (ft_live), ds.started, ds.stolen, ds.dropped,
               ds.cpu_reduced, ds.gl_halved);
#undef MIB
}

/* glibc keeps freed heap pages resident: give them back after a frame that
 * freed large buffers (a software GL keeps its textures on the heap, and
 * drivers keep system copies of them) */
static void scenenode_trim_heap (SceneNodeContent *s)
{
    if (!s->trim_heap)
        return;
    s->trim_heap = FALSE;
#ifdef __GLIBC__
    glFinish ();   /* deletes are only queued until the driver has run them */
    malloc_trim (0);
#endif
}

/* ---- frame signature ---------------------------------------------------
 * FNV-1a over the values a draw depends on. Two frames with the same
 * signature and no layer flagged volatile put the same pixels on screen. */
static inline guint64 sig_mix (guint64 h, const void *p, gsize n)
{
    const guint8 *b = p;
    for (gsize i = 0; i < n; i++) {
        h ^= b[i];
        h *= 1099511628211ull;
    }
    return h;
}
#define SIG_ADD(h, v) ((h) = sig_mix ((h), &(v), sizeof (v)))
#define SIG_SEED 1469598103934665603ull

/* the layer takes part in this frame (the draw loop's own test) */
static inline gboolean node_drawn (const NodeObj *nv)
{
    return nv->world_visible && nv->alpha > 0 && !nv->fx_skip && !nv->dbg_skip;
}

/* Everything the draw loop reads for layer i, after the scripts, the
 * video upload and the particle step of this frame. *vol: the layer shows
 * a new picture every frame by itself (video, puppet animation, live or
 * legacy particles, effects driven by time / their own last frame / album
 * art): its signature says nothing.
 *
 * Video layers: a tree whose video side counts the frames it puts into a
 * layer's texture defines SCENE_VIDEO_SERIAL (sv) as that counter of a
 * SceneVideo (it must change when, and only when, the texture got another
 * frame, and before this function runs in the same tick). The layer is
 * then compared like any other: a paused clip costs no frame, a 30 fps
 * clip under a 60 fps limit every second one. Without the macro a video
 * layer counts as new every frame. */
static guint64 node_signature (SceneNodeContent *s, guint i, gboolean *vol)
{
    NodeObj *nv = &g_array_index (s->nodes, NodeObj, i);
    guint64 h = SIG_SEED;
    SIG_ADD (h, i);
    SIG_ADD (h, nv->kind);
    SIG_ADD (h, nv->tex);
    SIG_ADD (h, nv->tex_ready);
    SIG_ADD (h, nv->premult);
    SIG_ADD (h, nv->tex_w);
    SIG_ADD (h, nv->tex_h);
    SIG_ADD (h, nv->tex_full_w);
    SIG_ADD (h, nv->tex_full_h);
    SIG_ADD (h, nv->phys_w);
    SIG_ADD (h, nv->phys_h);
    SIG_ADD (h, nv->pix_serial);
    SIG_ADD (h, nv->text_baked);
    h = sig_mix (h, nv->world.m, sizeof (nv->world.m));
    h = sig_mix (h, nv->size, sizeof (nv->size));
    h = sig_mix (h, nv->anchor, sizeof (nv->anchor));
    h = sig_mix (h, nv->tquad, sizeof (nv->tquad));
    SIG_ADD (h, nv->alpha);
    h = sig_mix (h, nv->tint, sizeof (nv->tint));
    SIG_ADD (h, nv->bright);
    SIG_ADD (h, nv->blend);
    SIG_ADD (h, nv->compose);
    SIG_ADD (h, nv->canvas);
    SIG_ADD (h, nv->origin_z);
    SIG_ADD (h, nv->scale_z);
    if (nv->video >= 0) {
#ifdef SCENE_VIDEO_SERIAL
        /* a new picture exactly when the decoder put another frame into
         * the texture (the upload of this tick has happened by now) */
        guint vs = SCENE_VIDEO_SERIAL (&g_array_index (s->videos, SceneVideo, nv->video));
        SIG_ADD (h, vs);
#else
        *vol = TRUE;   /* nobody says when: a new picture every frame */
#endif
    }
    if (nv->puppet || nv->parts)
        *vol = TRUE;
    if (nv->systex) {
        SIG_ADD (h, nv->systex);
        SIG_ADD (h, s->media_tex[nv->systex]);
        SIG_ADD (h, s->media_gen);
    }
    if (nv->weparts) {
        /* a system with no particle alive draws nothing; any other one
         * moves */
        gboolean live = walpe_weparticles_count (nv->weparts) > 0;
        SIG_ADD (h, live);
        if (live)
            *vol = TRUE;
    }
    if (!s->nofx && nv->fxset) {
        guint64 fs = fx_frame_signature (nv->fxset, vol);
        SIG_ADD (h, fs);
    }
    return h;
}

/* copy the w x h pixels of framebuffer `from` to `to`, 1:1; `bind` is
 * bound when it returns */
static void scene_copy (GLuint from, GLuint to, int w, int h, GLuint bind)
{
    glBindFramebuffer (GL_READ_FRAMEBUFFER, from);
    glBindFramebuffer (GL_DRAW_FRAMEBUFFER, to);
    glBlitFramebuffer (0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer (GL_FRAMEBUFFER, bind);
}

/* a colour target of w x h (the scene's own: the static bake, the
 * reduced-size frame); TRUE when it is there */
static gboolean scene_target_ensure (GLuint *fbo, GLuint *tex, int *cw, int *ch, int w, int h,
                                     GLenum ifmt, gboolean linear)
{
    if (*tex && *cw == w && *ch == h)
        return TRUE;
    if (*tex)
        glDeleteTextures (1, tex);
    if (!*fbo)
        glGenFramebuffers (1, fbo);
    glGenTextures (1, tex);
    glBindTexture (GL_TEXTURE_2D, *tex);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (ifmt == GL_RGB10_A2)
        glTexImage2D (GL_TEXTURE_2D, 0, GL_RGB10_A2, w, h, 0, GL_RGBA,
                      GL_UNSIGNED_INT_2_10_10_10_REV, NULL);
    else
        glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindFramebuffer (GL_FRAMEBUFFER, *fbo);
    glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
    gboolean ok = glCheckFramebufferStatus (GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    *cw = w;
    *ch = h;
    if (!ok) {
        glDeleteTextures (1, tex);
        *tex = 0;
    }
    return ok;
}

static void scene_target_drop (GLuint *fbo, GLuint *tex)
{
    if (*tex)
        glDeleteTextures (1, tex);
    if (*fbo)
        glDeleteFramebuffers (1, fbo);
    *tex = *fbo = 0;
}

/* A texture format that holds the window's pixels exactly: the static
 * bake is a copy of what was drawn in the window (taken once, put back
 * every frame), so it must lose nothing. RGBA8 for the usual 8-bit window
 * (with or without alpha), RGB10_A2 for a 30-bit one, 0 = something else
 * (no bake). Looked up once per render thread. */
static GLenum scene_window_format (void)
{
    static __thread int fmt = -1;
    if (fmt < 0) {
        GLint r = 8, g = 8, b = 8, a = 0;
        GLint fb = 0;
        glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &fb);
        glBindFramebuffer (GL_FRAMEBUFFER, 0);
        while (glGetError () != GL_NO_ERROR)
            ;
        GLenum att = epoxy_is_desktop_gl () ? GL_BACK_LEFT : GL_BACK;
        glGetFramebufferAttachmentParameteriv (GL_FRAMEBUFFER, att,
                                               GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE, &r);
        glGetFramebufferAttachmentParameteriv (GL_FRAMEBUFFER, att,
                                               GL_FRAMEBUFFER_ATTACHMENT_GREEN_SIZE, &g);
        glGetFramebufferAttachmentParameteriv (GL_FRAMEBUFFER, att,
                                               GL_FRAMEBUFFER_ATTACHMENT_BLUE_SIZE, &b);
        glGetFramebufferAttachmentParameteriv (GL_FRAMEBUFFER, att,
                                               GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE, &a);
        if (glGetError () != GL_NO_ERROR)
            r = g = b = 8, a = 0;   /* no answer: the EGL config asked for 8 */
        while (glGetError () != GL_NO_ERROR)
            ;
        glBindFramebuffer (GL_FRAMEBUFFER, (GLuint) fb);
        fmt = r <= 8 && g <= 8 && b <= 8 && a <= 8 ? GL_RGBA8
            : r == 10 && g == 10 && b == 10 && a <= 2 ? GL_RGB10_A2 : 0;
        if (!fmt)
            g_message ("scenenode: window has %d/%d/%d/%d colour bits: static layers are not "
                       "baked (no exact copy format)", r, g, b, a);
    }
    return (GLenum) fmt;
}

/* the bake no longer matches or is not wanted: free it */
static void bake_drop (SceneNodeContent *s)
{
    scene_target_drop (&s->bake.fbo, &s->bake.tex);
    s->bake.valid = FALSE;
    s->bake.unused = s->bake.age = 0;
    s->bake.lean = FALSE;
}

#define BAKE_STABLE 2        /* frames a layer must have been unchanged */
#define BAKE_MIN_DRAWS 2     /* layers a run must replace */
#define BAKE_MIN_COVER 1.25f /* ... and what they write, in frames: putting
                              * the bake back is a copy of one whole frame,
                              * more than a few small layers cost */
#define BAKE_UNUSED_MAX 120  /* frames an unused bake is kept */
#define BAKE_LEAN_AGE 300    /* frames in use before its layers' cached effect outputs go */

/* A particle setting changed while the scene runs (walpe-ctl set
 * particle_lowres / particle_density / particle_instancing): what its
 * particle objects draw changed although nothing in the scene did. Every
 * frame is drawn in this tree, so there is nothing to do here; a renderer
 * that skips unchanged frames marks the picture as changed in this one
 * place (its picture serial). Render thread, before the frame is drawn. */
static void scenenode_particles_changed (SceneNodeContent *s)
{
    s->base.picture_serial++;   /* the frame skip and the bake see it */
}

static gboolean scenenode_gl_render (WalpeContent *c, int w, int h, float px, float py)
{
    SceneNodeContent *s = (SceneNodeContent *) c;
    WALPE_PROF_STAGE (WALPE_PROF_ST_UPDATE);   /* GPUPROF */
    walpe_cph_to (WALPE_CPH_OTHER);   /* CPU phases (cpuphase.h): off unless WALPE_CPU_PHASES */
    WALPE_CPH_COUNT (frames, 1);
    scene_thread_numeric_c ();
    tex_slack_pct = s->slack_pct;
    gboolean lost = c->surface_lost;
    gboolean exposed = c->surface_exposed;
    c->surface_lost = c->surface_exposed = FALSE;
    c->frame_unchanged = FALSE;
    /* render scale: everything below works at the reduced size; the frame
     * is scaled up to the window (win_w x win_h) at the end */
    const int win_w = w, win_h = h;
    GLuint scene_fb = 0;   /* where the frame is drawn: the window, or s->rs */
    if (s->render_scale < 100) {
        int rw = MIN (win_w, MAX (16, (win_w * s->render_scale + 50) / 100));
        int rh = MIN (win_h, MAX (16, (win_h * s->render_scale + 50) / 100));
        if (rw != win_w || rh != win_h) {
            if (scene_target_ensure (&s->rs.fbo, &s->rs.tex, &s->rs.w, &s->rs.h, rw, rh, GL_RGBA8,
                                     TRUE)) {
                scene_fb = s->rs.fbo;
                w = rw;
                h = rh;
            } else {
                g_warning ("scenenode: no %dx%d render target: render_scale stays at 100", rw, rh);
                s->render_scale = 100;
                s->rs_fixed = TRUE;
            }
        }
    }
    if (!scene_fb && s->rs.tex)
        scene_target_drop (&s->rs.fbo, &s->rs.tex);
    const gboolean scaled = scene_fb != 0;

    float k, vcx, vcy;
    view_map (s, (float) w, (float) h, &k, &vcx, &vcy);
    WalpeMat3 tail;
    walpe_mat3_identity (&tail);
    walpe_mat3_translate (&tail, -vcx, -vcy);
    walpe_mat3_scale (&tail, k, k);
    walpe_mat3_translate (&tail, (float) w / 2.0f, (float) h / 2.0f);
    /* WE scene space is Y-up (origin bottom-left): children of a window
     * frame sit at negative y offsets below their parent, particle
     * gravity/velocity "0 -N 0" falls */
    walpe_mat3_scale (&tail, 2.0f / (float) w, 2.0f / (float) h);
    walpe_mat3_translate (&tail, -1.0f, -1.0f);

    s->frame_no++;
    /* particle systems may draw large soft sprites at reduced resolution
     * and composite them when the next thing is drawn (weparticles.h) */
    walpe_weparticles_begin_frame (scene_fb, w, h);   /* the frame's target: the window or the reduced frame */
    guint pts = walpe_weparticles_settings_serial ();
    if (pts != s->pt_serial) {
        s->pt_serial = pts;
        scenenode_particles_changed (s);
    }
    gint64 now_us = g_get_monotonic_time ();
    /* "Skip hidden areas": a frame drawn while the scene is paused - a
     * window moved off a part of the wallpaper - shows the instant the
     * clock stopped at, the picture the rest of the window holds, not the
     * present (the clock goes on from that instant at the resume). Only
     * with a clip set: without one nothing here changes. */
    const gint64 held_us = s->paused_at && walpe_clip_serial () ? s->paused_at : 0;
    if (held_us)
        now_us = held_us;
    if (s->pprobe) {
        /* virtual 30 fps clock: the digest of two loads is comparable */
        now_us = s->t0 + (gint64) s->frame_no * 33333;
        s->fx_time = s->frame_no / 30.0;
    }
    double dt = s->last_frame_us ? (now_us - s->last_frame_us) / 1e6 : 0.0;
    if (held_us && dt < 0.0)    /* (a frame was drawn after the clock stopped) */
        dt = 0.0;
    s->last_frame_us = now_us;
    s->frame_time = s->fx_time >= 0 ? s->fx_time : (now_us - s->t0) / 1e6;
    /* quality knobs for this frame's effect runs (defaults: no change) */
    scenenode_texres_apply (s);
    fx_set_quality (s->q.fx_rate, s->q.fx_rate_all, s->q.post,
                    s->fps_cap > 0 ? s->fps_cap : 30, s->frame_no);
    scenenode_px_begin (s);

    /* one spectrum snapshot per frame for every pass and emitter */
    if (s->audio_ref) {
        walpe_audio_get (&s->audio);
        fx_set_audio_frame (&s->audio);
        if (s->probe && s->frame_no % 30 == 1) {
            int pk = 0;
            for (int b = 1; b < WALPE_AUDIO_BANDS; b++)
                if (s->audio.left[b] > s->audio.left[pk])
                    pk = b;
            g_debug ("scenenode: audio peak band %d = %.2f, bass level %.2f", pk,
                     s->audio.left[pk], walpe_audio_level (&s->audio));
        }
    } else {
        fx_set_audio_frame (NULL);
    }
    /* g_Screen / g_TexelSize are the monitor's whatever the render scale:
     * blur radii and the like are given in screen texels */
    fx_set_frame_info (win_w, win_h, px * 0.5f + 0.5f, py * -0.5f + 0.5f, (float) dt);
    if (s->media_ref)
        scene_media_update (s);
    fx_set_system_textures (s->media_tex[1], s->media_w[1], s->media_h[1],
                            s->media_tex[2], s->media_w[2], s->media_h[2]);

    if (s->ss) {
        walpe_cph_to (WALPE_CPH_SCRIPT);
        gint64 ss_t0 = s->probe ? g_get_monotonic_time () : 0;
        s->view_w = w;
        s->view_h = h;
        cursor_dispatch (s);
        walpe_ss_tick (s->ss, dt, (now_us - s->t0) / 1e6,
                       s->cur_have ? s->cur_x : (px * 0.5f + 0.5f) * s->scene_w,
                       s->cur_have ? s->cur_y : (py * -0.5f + 0.5f) * s->scene_h, s->cur_down);
        if (s->probe) {
            s->ss_us += g_get_monotonic_time () - ss_t0;
            if (++s->ss_ticks == 90) {
                g_message ("scenenode: SceneScript %d scripts, %.0f us/frame (avg of 90)",
                           walpe_ss_count (s->ss), s->ss_us / 90.0);
                s->ss_us = 0;
                s->ss_ticks = 0;
            }
        }
    }

    /* video textures: upload each unique decoder's newest frame once */
    walpe_cph_to (WALPE_CPH_VIDEO);
    gint64 video_iv = 0;
    for (guint i = 0; i < s->videos->len; i++) {
        SceneVideo *sv = &g_array_index (s->videos, SceneVideo, i);
        int vw = 0, vh = 0;
        guint noted = walpe_video_frames_noted ();
        GLuint t = walpe_video_tex_update (sv->v, &vw, &vh);
        if (walpe_video_frames_noted () != noted)
            sv->serial++;    /* a new video frame is in the texture */
        if (t) {
            sv->tex = t;
            sv->w = vw;
            sv->h = vh;
            if (vw > sv->full_w || vh > sv->full_h) {
                sv->full_w = vw;
                sv->full_h = vh;
            }
        }
        gint64 iv = walpe_video_tex_interval_us (sv->v);
        if (iv > 0 && (!video_iv || iv < video_iv))
            video_iv = iv;
    }

    /* "Adjust layers": look again only when something changed there */
    walpe_cph_to (WALPE_CPH_OTHER);
    {
        guint lg = walpe_layers_generation ();
        if (lg != s->lay_gen)
            scene_layers_sync (s, lg);
    }
    walpe_cph_to (WALPE_CPH_WORLD);

    for (guint i = 0; i < s->nodes->len; i++)
        g_array_index (s->nodes, NodeObj, i).world_state = 0;
    for (guint i = 0; i < s->nodes->len; i++)
        node_world (s, i);
    /* Text a script changed is rasterised and uploaded here, once it is known
     * which layers are shown: a hidden text layer keeps its mark and is
     * rendered in the frame that shows it again. (It used to be redone on
     * every change, shown or not: 2901582518 types its welcome line into
     * three variants of its date widget, two of them hidden, and runs a clock
     * with seconds in a media widget that is hidden while nothing plays.) */
    walpe_cph_to (WALPE_CPH_TEXT);
    for (guint i = 0; i < s->nodes->len; i++) {
        NodeObj *nv = &g_array_index (s->nodes, NodeObj, i);
        if (nv->text_dirty && nv->world_visible)
            refresh_text (nv);
    }
    walpe_cph_to (WALPE_CPH_OTHER);
    if (s->tex_deferred)
        scene_load_deferred_textures (s, &tail, w, h, win_w, win_h);
    walpe_cph_to (WALPE_CPH_PT_SIM);

    /* Particle systems are advanced here, before anything is drawn: with
     * the scripts, the video upload and the text of this frame done as
     * well, everything that can change the picture has happened and the
     * frame can be compared with the one on screen. (The draw loop used
     * to step each system right before drawing it; the order of the
     * systems and their inputs are the same.) */
    for (guint i = 0; i < s->nodes->len; i++) {
        NodeObj *nv = &g_array_index (s->nodes, NodeObj, i);
        if (!nv->weparts || !node_drawn (nv))
            continue;
        if (s->ptr_have && (walpe_weparticles_follows_cursor (nv->weparts) ||
                            walpe_weparticles_uses_cursor (nv->weparts))) {
            /* pointer (monitor [0,1], top-left) -> scene (Y-up, the
             * view_map inverse) -> system local (world inverse) */
            float sx = vcx + (s->ptr_nx - 0.5f) * (float) w / k;
            float sy = vcy + (0.5f - s->ptr_ny) * (float) h / k;
            const float *M = nv->world.m;
            float det = M[0] * M[4] - M[1] * M[3];
            if (fabsf (det) > 1e-12f) {
                float qx = sx - M[6], qy = sy - M[7];
                walpe_weparticles_set_cursor (nv->weparts, (M[4] * qx - M[3] * qy) / det,
                                              (-M[1] * qx + M[0] * qy) / det);
            }
        }
        if (s->audio_ref)
            walpe_weparticles_set_audio_level (nv->weparts, walpe_audio_level (&s->audio));
        if (scene_layers_touched (s, nv)) {
            /* moved / scaled by the user ("Adjust layers"): the system
             * keeps simulating and projecting in its own place, its
             * picture is moved */
            WalpeMat3 w0, clip_u;
            scene_layers_unadjust (nv, &tail, &w0, &clip_u);
            walpe_weparticles_set_world (nv->weparts, w0.m);
            walpe_weparticles_set_view (nv->weparts, clip_u.m, vcx, vcy, nv->origin_z,
                                        nv->scale_z, s->persp_dist);
        } else {
            walpe_weparticles_set_world (nv->weparts, nv->world.m);
            walpe_weparticles_set_view (nv->weparts, tail.m, vcx, vcy, nv->origin_z, nv->scale_z,
                                        s->persp_dist);
        }
        walpe_weparticles_step (nv->weparts, dt);
    }

    /* ---- what this frame shows: one signature per layer, one for the
     * frame (view, clear colour and every layer that draws) */
    walpe_cph_to (WALPE_CPH_SIG);
    guint64 fsig = SIG_SEED;
    gboolean fvol = FALSE;
    SIG_ADD (fsig, w);
    SIG_ADD (fsig, h);
    SIG_ADD (fsig, win_w);
    SIG_ADD (fsig, win_h);
    fsig = sig_mix (fsig, tail.m, sizeof (tail.m));
    fsig = sig_mix (fsig, s->clear, sizeof (s->clear));
    SIG_ADD (fsig, s->nofx);
    SIG_ADD (fsig, c->picture_serial);
    /* effect pass programs that changed while the scene runs (opt-in
     * fx_specialize / fx_quality: fx_specialize.h) */
    const guint fx_serial = fx_picture_serial ();
    SIG_ADD (fsig, fx_serial);
    const guint64 view_sig = fsig;
    /* (view_sig also starts the signature of the static bake. An input
     * that changes where the frame is drawn but not what the layers look
     * like - a clip region - is added to fsig from here on, never above:
     * the bake holds the full picture and outlives such a change.) */
    {
        /* "Skip hidden areas" (clip.h): the frame on screen was drawn
         * into the boxes of that clip only (0 = no clip) */
        guint clip_serial = walpe_clip_serial ();
        SIG_ADD (fsig, clip_serial);
    }
    /* "Adjust layers": any change of the user's adjustments draws a frame
     * (their effect on a layer is in its world matrix / visibility already;
     * the generation makes it certain), and so does the outline of
     * highlighted layers appearing or going */
    SIG_ADD (fsig, s->lay_gen);
    if (s->hl && s->hl->len)
        fsig = sig_mix (fsig, s->hl->data, s->hl->len * sizeof (int));
    /* the leading run of layers that have stood still (static bake):
     * run_end = first node behind it, run_sig = its signature */
    guint run_end = 0, run_draws = 0;
    float run_px = 0;
    guint64 run_sig = view_sig;
    gboolean run_open = TRUE;
    for (guint i = 0; i < s->nodes->len; i++) {
        NodeObj *nv = &g_array_index (s->nodes, NodeObj, i);
        if (!node_drawn (nv)) {
            nv->stable = 0;
            nv->sig = 0;
            if (run_open)
                run_end = i + 1;
            continue;
        }
        gboolean vol = FALSE;
        guint64 ns = node_signature (s, i, &vol);
        if (vol || ns != nv->sig || nv->sig_vol)
            nv->stable = 0;
        else if (nv->stable < 1000000)
            nv->stable++;
        nv->sig = ns;
        nv->sig_vol = vol;
        fvol |= vol;
        SIG_ADD (fsig, ns);
        if (run_open && !vol && nv->stable >= BAKE_STABLE) {
            SIG_ADD (run_sig, ns);
            run_end = i + 1;
            run_draws += nv->tex_ready ? 1 : 0;
            run_px += nv->cost_px;
        } else {
            run_open = FALSE;
        }
    }

    /* ---- nothing changed since the frame on screen: draw nothing */
    if (!s->no_skip && !lost && s->frame_valid && !fvol && fsig == s->frame_sig) {
        c->frame_unchanged = TRUE;
        s->n_skipped++;
        if (scene_fb)   /* (a target made above leaves itself bound) */
            glBindFramebuffer (GL_FRAMEBUFFER, 0);
        walpe_weparticles_end_frame ();   /* paired with begin_frame on a skipped frame too */
        goto frame_done;
    }
    walpe_cph_to (WALPE_CPH_DRAW);
    s->frame_sig = fsig;
    s->frame_valid = !fvol;   /* a frame with a moving layer is never "the same" */
    s->n_drawn++;

    /* ---- where the frame goes: the window, or the scene's own target
     * when it renders at a reduced size */
    glBindFramebuffer (GL_FRAMEBUFFER, scene_fb);
    glViewport (0, 0, w, h);
    /* the effect passes come back to this framebuffer after a run
     * (fx_gls_end; 0 = the window) */
    fx_set_scene_target (scene_fb);
    if (scene_fb)   /* "Skip hidden areas": the boxes in the reduced frame's pixels */
        walpe_clip_retarget (scene_fb, w, h);

    /* ---- static bake (see struct): put it back, take it anew, or do
     * without. The bake is a copy of the frame as it stood when the run
     * had been drawn the normal way - into the window, with the very
     * draws every frame used to repeat - so putting it back gives the
     * same pixels by construction, on any driver. */
    guint first = 0;
    gboolean baking = FALSE;   /* TRUE while the run is drawn for a new bake:
                                * until the copy at i == run_end those layers
                                * must be drawn in full (no clip) */
    GLenum bake_fmt = scene_fb ? GL_RGBA8 : scene_window_format ();
    gboolean bake_ok = !s->no_bake && s->animates && !s->paused && run_draws >= BAKE_MIN_DRAWS &&
                       run_px >= BAKE_MIN_COVER * (float) w * (float) h && bake_fmt != 0;
    /* the window was covered in part and came back: the bake is taken
     * anew from this frame. A copy made while another window covered a
     * part of ours may hold anything there - a driver need not keep the
     * pixels of a window's buffer that are not on screen. (At a reduced
     * render scale the bake is a copy of the scene's own target.) */
    if (exposed && !scene_fb)
        s->bake.valid = FALSE;
    if (bake_ok && s->bake.valid && s->bake.w == w && s->bake.h == h &&
        s->bake.sig == run_sig && s->bake.end == run_end) {
        scene_copy (s->bake.fbo, scene_fb, w, h, scene_fb);
        first = run_end;
        s->bake.unused = 0;
        s->n_baked += run_draws;
        if (++s->bake.age == BAKE_LEAN_AGE && !s->bake.lean) {
            /* in use for a while: the cached effect outputs of its layers
             * are not read any more (they are made again if the run ever
             * has to be drawn anew) */
            for (guint i = 0; i < run_end; i++)
                fx_free_targets_for_object (g_array_index (s->nodes, NodeObj, i).fxset);
            s->bake.lean = TRUE;
        }
    } else {
        if (bake_ok) {
            baking = TRUE;
            s->bake.valid = FALSE;
        } else if (s->bake.tex && ++s->bake.unused > BAKE_UNUSED_MAX) {
            bake_drop (s);
        }
        glClearColor (s->clear[0], s->clear[1], s->clear[2], 1);
        glClear (GL_COLOR_BUFFER_BIT);
    }

    /* "Skip hidden areas" (clip.h): with a clip set, layers are drawn
     * into the visible boxes only. Not below a layer that reads the frame
     * under it at places of its own choosing - the effects of a compose
     * layer, refracting particles: what it may read is drawn in full, so
     * clipping starts at the last such layer (its own result, and every
     * layer after it, is clipped). Nor while the static bake is taken
     * (`baking`): the bake is the full picture under every clip. */
    guint clip_from = 0;
    gboolean clip_from_parts = FALSE;
    {
        WalpeBox clip_any[WALPE_CLIP_MAX];
        if (walpe_clip_boxes (clip_any) > 0)    /* (nothing visible: nothing is read either) */
            for (guint i = 0; i < s->nodes->len; i++) {
                NodeObj *nv = &g_array_index (s->nodes, NodeObj, i);
                if (!nv->world_visible || nv->alpha <= 0 || nv->fx_skip || nv->dbg_skip)
                    continue;
                gboolean parts = nv->weparts && walpe_weparticles_reads_frame (nv->weparts);
                if (parts || (nv->compose && nv->tex_ready && !s->nofx && fx_has_effects (nv->fxset))) {
                    clip_from = i;
                    clip_from_parts = parts;
                }
            }
    }

    WALPE_PROF_STAGE (WALPE_PROF_ST_LAYERS);   /* GPUPROF */
    /* WE draws the objects array in order: the first object is the
     * backmost (the old reverse walk put full-screen backgrounds on top
     * of every other layer) */
    for (guint i = first; i <= s->nodes->len; i++) {
        if (baking && i == run_end) {
            /* the run is on the frame: keep a copy of it (particles
             * still in the reduced-resolution layer go on first) */
            walpe_weparticles_flush ();
            baking = FALSE;
            if (scene_target_ensure (&s->bake.fbo, &s->bake.tex, &s->bake.w, &s->bake.h, w, h,
                                     bake_fmt, FALSE)) {
                scene_copy (scene_fb, s->bake.fbo, w, h, scene_fb);
                s->bake.valid = TRUE;
                s->bake.sig = run_sig;
                s->bake.end = run_end;
                s->bake.unused = s->bake.age = 0;
                s->bake.lean = FALSE;
            } else {
                glBindFramebuffer (GL_FRAMEBUFFER, scene_fb);
                s->no_bake = TRUE;   /* no such target here: stop trying */
            }
        }
        if (i == s->nodes->len)
            break;
        NodeObj *nv = &g_array_index (s->nodes, NodeObj, i);
        if (!nv->world_visible || nv->alpha <= 0)
            continue;
        if (nv->fx_skip)
            continue;   /* solid / compose layer with a broken effect */
        if (nv->dbg_skip)
            continue;   /* WALPE_SKIP_IDS */
        walpe_clip_suspend (baking || i < clip_from || (i == clip_from && clip_from_parts));
        /* GPUPROF: one row per layer (its passes are rows of their own) */
        WALPE_PROF_LAYER_BEGIN ((int) i, nv->name,
                                nv->weparts || nv->parts ? "particles" : nv->compose ? "compose"
                                : nv->video >= 0 ? "video" : nv->kind == NK_TEXT ? "text" : "image");
        guint prof_run0 = 0, prof_cached0 = 0;
        const gboolean prof = s->profile_frame > 0 && (int) s->frame_no == s->profile_frame;
        static __thread GLuint prof_q;
        gboolean prof_px = FALSE;
        if (prof) {
            fx_pass_stats (&prof_run0, &prof_cached0, FALSE);
            /* pixels shaded for this layer (effect passes + its draw), where
             * the driver counts them */
            prof_px = epoxy_is_desktop_gl () &&
                      epoxy_has_gl_extension ("GL_ARB_pipeline_statistics_query");
            if (prof_px) {
                if (!prof_q)
                    glGenQueries (1, &prof_q);
                glBeginQuery (GL_FRAGMENT_SHADER_INVOCATIONS_ARB, prof_q);
            }
        }
        if (nv->tex_lazy) {
            /* hidden until now: its texture, the first time it is shown */
            node_load_deferred (s, nv, i, &tail, w, h, win_w, win_h);
            SCENE_PICTURE_CHANGED (s);
        }

        if (nv->video >= 0) {
            SceneVideo *sv = &g_array_index (s->videos, SceneVideo, nv->video);
            nv->tex = sv->tex;           /* borrowed: never deleted here */
            nv->tex_ready = sv->tex != 0;
            if (sv->tex) {
                /* the layer keeps the video's own size (effect targets and
                 * resolution uniforms) whether the decoder hands out full
                 * or half-size frames */
                nv->tex_w = nv->tex_full_w = sv->full_w > 0 ? sv->full_w : sv->w;
                nv->tex_h = nv->tex_full_h = sv->full_h > 0 ? sv->full_h : sv->h;
            }
        }

        /* layer colour: "color" x "brightness" (g_Color4.rgb) */
        const float btint[3] = { nv->tint[0] * nv->bright, nv->tint[1] * nv->bright,
                                 nv->tint[2] * nv->bright };
        WalpeMat3 m, quad;
        node_clip_matrix (nv, &tail, &m, &quad);
        if (nv->ds_state != 2 && nv->tex_ready)
            node_tex_fit (s, nv, i, &m, w, h);

        if (nv->video >= 0) {
            /* on-screen pixel size of the quad (edges = 2 * columns of m
             * in clip units, w/2 resp. h/2 pixels each) */
            SceneVideo *sv = &g_array_index (s->videos, SceneVideo, nv->video);
            float pw = hypotf (m.m[0] * w, m.m[1] * h), ph = hypotf (m.m[3] * w, m.m[4] * h);
            sv->view_w = MAX (sv->view_w, (int) ceilf (pw));
            sv->view_h = MAX (sv->view_h, (int) ceilf (ph));
            sv->view_seen = TRUE;
            if (fx_has_effects (nv->fxset) && !s->nofx) {
                /* Effects read the frame into targets of the working size
                 * (fx_working_scale). At exactly half - a 4K video layer
                 * on a 1080p monitor - the first pass samples it 2:1,
                 * which averages the same 2x2 texels a half-size frame
                 * holds (node_fx_input_fit does this for file textures),
                 * so the decoder may hand out half-size frames: a quarter
                 * of the upload, the plane textures and the RGBA copy
                 * (31.6 -> 7.9 MiB for 4K). At any other scale, or with
                 * texture downscaling off, the effects get every texel. */
                int fw = nv->tex_w ? nv->tex_w : 256, fh = nv->tex_h ? nv->tex_h : 256;
                if (!s->downscale ||
                    fx_working_scale (nv, &m, win_w, win_h, fw, fh, node_fx_kmin (s, nv, i)) != 0.5f)
                    sv->view_full = TRUE;
            }
        }

        /* A layer whose quad lies entirely outside the view: neither its
         * effect passes nor its draw can change a pixel, so both are
         * skipped (they used to run in full: scenes park layers off the
         * canvas). Not for puppets (the mesh may leave the quad), compose
         * layers (clipped to the view further down) or effects that keep
         * a picture between frames or move the layer (fx_cullable). One
         * pixel of slack; NaN matrices compare false and are drawn. */
        gboolean culled = FALSE;
        if (nv->tex_ready && !nv->compose && !nv->puppet &&
            (s->nofx || fx_cullable (nv->fxset))) {
            float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
            for (int k = 0; k < 4; k++) {
                float qx = (k & 1) ? 1.0f : -1.0f, qy = (k & 2) ? 1.0f : -1.0f;
                float cx = m.m[0] * qx + m.m[3] * qy + m.m[6];
                float cy = m.m[1] * qx + m.m[4] * qy + m.m[7];
                x0 = fminf (x0, cx); x1 = fmaxf (x1, cx);
                y0 = fminf (y0, cy); y1 = fmaxf (y1, cy);
            }
            const float ex = 2.0f / (float) w, ey = 2.0f / (float) h;
            culled = x1 < -1.0f - ex || x0 > 1.0f + ex || y1 < -1.0f - ey || y0 > 1.0f + ey;
            /* Nor can a quad put a pixel on screen whose bounds hold no
             * pixel centre (scaled to nothing, or thinner than a pixel
             * between two columns / rows): the rasterizer fills a pixel
             * only when its centre is inside. Its effect passes used to
             * run at full working size all the same. */
            const float eps = 1e-3f;
            float px0 = (x0 * 0.5f + 0.5f) * (float) w, px1 = (x1 * 0.5f + 0.5f) * (float) w;
            float py0 = (y0 * 0.5f + 0.5f) * (float) h, py1 = (y1 * 0.5f + 0.5f) * (float) h;
            if (floorf (px1 + eps - 0.5f) < ceilf (px0 - eps - 0.5f) ||
                floorf (py1 + eps - 0.5f) < ceilf (py0 - eps - 0.5f))
                culled = TRUE;
            /* ... or entirely under other windows ("Skip hidden areas",
             * clip.h; always FALSE without a clip) */
            culled = culled || !walpe_clip_hits (x0, y0, x1, y1);
        }
        {
            /* what the layer's draw writes: its bounds within the view (a
             * compose layer with effects also copies that much of the
             * frame and runs at least one pass over it). See cost_px. */
            float x0 = -1, x1 = 1, y0 = -1, y1 = 1;
            if (nv->compose != 2) {
                x0 = y0 = 1e9f;
                x1 = y1 = -1e9f;
                for (int k = 0; k < 4; k++) {
                    float qx = (k & 1) ? 1.0f : -1.0f, qy = (k & 2) ? 1.0f : -1.0f;
                    float cx = m.m[0] * qx + m.m[3] * qy + m.m[6];
                    float cy = m.m[1] * qx + m.m[4] * qy + m.m[7];
                    x0 = fminf (x0, cx); x1 = fmaxf (x1, cx);
                    y0 = fminf (y0, cy); y1 = fmaxf (y1, cy);
                }
                x0 = fmaxf (x0, -1); y0 = fmaxf (y0, -1);
                x1 = fminf (x1, 1); y1 = fminf (y1, 1);
            }
            float area = x1 > x0 && y1 > y0 ? (x1 - x0) * (y1 - y0) * 0.25f * (float) w * (float) h : 0.0f;
            nv->cost_px = !nv->tex_ready || culled ? 0.0f
                        : area * (nv->compose && !s->nofx && fx_has_effects (nv->fxset) ? 3.0f : 1.0f);
        }
        if (nv->kind == NK_TEXT && nv->tex_ready && !nv->text_mips && !culled) {
            /* text texture drawn smaller than 2:1 for the first time: now
             * its mip chain is sampled, so build it (make_text_texture) */
            float pw = hypotf (m.m[0] * w, m.m[1] * h), ph = hypotf (m.m[3] * w, m.m[4] * h);
            if (!(nv->tex_w <= pw * 1.9f && nv->tex_h <= ph * 1.9f)) {
                glBindTexture (GL_TEXTURE_2D, nv->tex);
                walpe_gl_tex_gen_mipmaps (nv->tex_w, nv->tex_h);
                walpe_gl_tex_lod_bias (-1.0f);
                nv->text_mips = TRUE;
            }
        }

        /* this object draws (or reads) the frame: particles of the objects
         * before it that are still in the reduced layer go on first */
        if (nv->tex_ready || nv->parts)
            walpe_weparticles_flush ();

        if (nv->compose && nv->tex_ready && !s->nofx) {
            /* compose layer: region of the frame under the layer (axis-
             * aligned bounds of its quad, clamped to the screen), or the
             * whole frame, copied upright (row 0 = top, the upload
             * convention effect passes keep) */
            float x0 = -1, x1 = 1, y0 = -1, y1 = 1;
            if (nv->compose == 1) {
                x0 = y0 = 1e9f;
                x1 = y1 = -1e9f;
                for (int k = 0; k < 4; k++) {
                    float qx = (k & 1) ? 1.0f : -1.0f, qy = (k & 2) ? 1.0f : -1.0f;
                    float cx = m.m[0] * qx + m.m[3] * qy + m.m[6];
                    float cy = m.m[1] * qx + m.m[4] * qy + m.m[7];
                    x0 = fminf (x0, cx); x1 = fmaxf (x1, cx);
                    y0 = fminf (y0, cy); y1 = fmaxf (y1, cy);
                }
                /* both ends inside the view: a layer far outside it, or one
                 * placed with "nan" (x0 stays 1e9 then), gave pixel numbers
                 * no int holds - undefined in the casts below. A layer that
                 * is outside comes out empty either way. */
                x0 = fminf (fmaxf (x0, -1), 1); y0 = fminf (fmaxf (y0, -1), 1);
                x1 = fmaxf (fminf (x1, 1), -1); y1 = fmaxf (fminf (y1, 1), -1);
            }
            int px0 = (int) floorf ((x0 * 0.5f + 0.5f) * w), px1 = (int) ceilf ((x1 * 0.5f + 0.5f) * w);
            int py0 = (int) floorf ((y0 * 0.5f + 0.5f) * h), py1 = (int) ceilf ((y1 * 0.5f + 0.5f) * h);
            int cw = px1 - px0, chh = py1 - py0;
            if (cw >= 2 && chh >= 2 && fx_post_dropped (nv->fxset)) {
                /* post-processing "off" and every effect of this layer is a
                 * purely cosmetic post pass: the frame under it is neither
                 * captured nor drawn back */
                fx_acct_compose (nv->fxset, cw, chh, 1.0f, (guint64) cw * chh, TRUE);
            } else if (cw >= 2 && chh >= 2) {
                /* the copy goes into a target lent from the effect pool
                 * until this layer is drawn (fx_capture_get: compose
                 * layers of one size share it, and it has no alpha
                 * channel unless the driver cannot render to RGB8). Each
                 * compose layer used to keep an RGBA8 texture of its own
                 * (nv->ctex / nv->cfbo, no longer made). */
                guint cap_fbo = 0, cap_tex = 0;
                gboolean cap_opaque = FALSE;
                int cap = fx_capture_get (cw, chh, &cap_fbo, &cap_tex, &cap_opaque);
                /* a mirrored layer (negative scale, e.g. 2929592935's audio
                 * bars: scale y -0.459) sees the frame mirrored in its own
                 * texture space: WE draws the effects there (SHAPE "Top"
                 * bars land at the layer's bottom on screen), so copy
                 * mirrored and draw back mirrored */
                gboolean mx = nv->compose == 1 && m.m[0] < 0;
                gboolean my = nv->compose == 1 && m.m[4] < 0;
                glBindFramebuffer (GL_READ_FRAMEBUFFER, scene_fb);
                glBindFramebuffer (GL_DRAW_FRAMEBUFFER, cap_fbo);
                glBlitFramebuffer (px0, py0, px1, py1, mx ? cw : 0, my ? 0 : chh, mx ? 0 : cw,
                                   my ? chh : 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
                if (!cap_opaque) {
                    /* the window's alpha is not the scene's: opaque (an
                     * RGB8 capture reads alpha 1 without this second
                     * write of the whole texture) */
                    glColorMask (GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
                    glClearColor (0, 0, 0, 1);
                    glClear (GL_COLOR_BUFFER_BIT);
                    glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                }
                glBindFramebuffer (GL_FRAMEBUFFER, scene_fb);
                double now = s->fx_time >= 0 ? s->fx_time :
                             ((held_us ? held_us : g_get_monotonic_time ()) - s->t0) / 1000000.0;
                fx_set_fullframe (cap_tex, cw, chh);
                /* a compose / project layer's effects run in a buffer of
                 * the layer's "size" (scene units) in WE whatever the
                 * monitor: on a 3840x2160 scene shown at 1920x1080 the
                 * shaders must still see 3840x2160 (texel-step blurs were
                 * twice too wide). The targets stay at the captured
                 * pixels (working factor); the fullscreen layer follows
                 * the frame itself. */
                int vw = cw, vh = chh;
                float ck = 1.0f;
                if (nv->compose == 1 && nv->size[0] >= 2 && nv->size[1] >= 2) {
                    /* pixels per layer unit (object scale and view included) */
                    float sx = hypotf (m.m[0] * w, m.m[1] * h) / nv->size[0];
                    float sy = hypotf (m.m[3] * w, m.m[4] * h) / nv->size[1];
                    if (sx > 1e-3f && sy > 1e-3f) {
                        vw = MAX (2, (int) lroundf ((float) cw / sx));
                        vh = MAX (2, (int) lroundf ((float) chh / sy));
                        ck = fmaxf ((float) cw / (float) vw, (float) chh / (float) vh);
                    }
                } else if (scaled) {
                    /* the fullscreen layer at a reduced render scale: its
                     * shaders still see the monitor's frame size */
                    vw = MAX (2, (int) lroundf ((float) cw * (float) win_w / (float) w));
                    vh = MAX (2, (int) lroundf ((float) chh * (float) win_h / (float) h));
                    ck = fmaxf ((float) cw / (float) vw, (float) chh / (float) vh);
                }
                fx_set_input_tint (nv->fxset, NULL);
                /* the layer's quad in layer units for vertex shaders that
                 * work on a_Position (skew / foliage sway / transform in
                 * Vertex mode); nothing is folded: this block draws the
                 * result on the captured rectangle itself */
                fx_set_layer_geometry (0.0f, 0.0f, (float) vw, (float) vh, FALSE);
                {
                    /* "Skip hidden areas": the part of the captured
                     * rectangle that is seen (-1 without a clip) */
                    float cuv[WALPE_CLIP_MAX][4];
                    int cn = walpe_clip_rect_uv (px0, py0, px1, py1, mx, my, cuv);
                    if (cn >= 0)
                        fx_set_clip_uv (&cuv[0][0], cn);
                }
                fx_set_run_compose (TRUE);   /* post passes: the post-processing level applies */
                {
                    /* fx_region.h: all of the result is read; passes
                     * whose target nothing reads in this run are left out */
                    static const float whole[1][4] = { { 0.0f, 0.0f, 1.0f, 1.0f } };
                    fx_set_need (whole, 1);
                }
                GLuint out = fx_run_for_object (nv->fxset, cap_tex, vw, vh, vw, vh, now, ck,
                                                FALSE);
                fx_set_run_compose (FALSE);
                fx_acct_compose (nv->fxset, vw, vh, ck, (guint64) cw * chh, FALSE);
                if (scene_fb)   /* the passes leave framebuffer 0 bound */
                    glBindFramebuffer (GL_FRAMEBUFFER, scene_fb);
                fx_set_fullframe (0, 0, 0);
                glViewport (0, 0, w, h);
                WalpeMat3 rm = { { (x1 - x0) / 2, 0, 0, 0, (y1 - y0) / 2, 0,
                                   (x0 + x1) / 2, (y0 + y1) / 2, 1 } };
                /* snap to the copied pixels */
                rm.m[0] = (mx ? -1.0f : 1.0f) * cw / w;
                rm.m[4] = (my ? -1.0f : 1.0f) * chh / h;
                rm.m[6] = (float) (px0 + px1) / w - 1.0f;
                rm.m[7] = (float) (py0 + py1) / h - 1.0f;
                walpe_glutil_draw_quad_tint (out, FALSE, nv->alpha, rm.m, nv->blend, btint);
                fx_capture_put (cap);   /* drawn: the next compose layer may take it */
            }
        } else if (nv->tex_ready && !nv->compose && !culled) {
            /* Tier 2: run the effect chains over the object texture, then
             * draw the result with the object transform. Time is
             * content-local (monotonic-since-boot loses float precision
             * and breaks phase math after a few days of uptime). */
            double now = s->fx_time >= 0 ? s->fx_time :
                         ((held_us ? held_us : g_get_monotonic_time ()) - s->t0) / 1000000.0;
            /* the layer's own quad in layer units (scene pixels, y up):
             * what WE's effect vertex shaders get as a_Position */
            if (!s->nofx && nv->fxset) {
                if (nv->kind == NK_TEXT && nv->tquad[2] > 0)
                    fx_set_layer_geometry (nv->tquad[0], nv->tquad[1], nv->tquad[2],
                                           nv->tquad[3], TRUE);
                else
                    fx_set_layer_geometry (-nv->anchor[0] * nv->size[0] / 2.0f,
                                           -nv->anchor[1] * nv->size[1] / 2.0f, nv->size[0],
                                           nv->size[1], TRUE);
            }
            /* $mediaThumbnail layers show the album art while there is
             * one (straight alpha), else their own default texture */
            GLuint src_tex = nv->tex;
            gboolean src_pm = nv->premult;
            if (nv->systex && s->media_tex[nv->systex]) {
                src_tex = s->media_tex[nv->systex];
                src_pm = FALSE;
            }
            int fw = nv->tex_w ? nv->tex_w : 256, fh = nv->tex_h ? nv->tex_h : 256;
            /* the working factor (and what is stored for it) is the one of
             * the window's own size; a reduced render scale then shrinks
             * the targets with the frame */
            float fk = fx_working_scale (nv, &m, win_w, win_h, fw, fh,
                                         node_fx_kmin (s, nv, i));
            /* quality knob fx_res: the factor below the default one, and what
             * each level would take off (accounting only at "full") */
            float fx_keep[3] = { 1.0f, 1.0f, 1.0f };
            const float fk_full = fk;
            const gboolean fx_res_on = !s->nofx && nv->fxset &&
                                       (s->q.fx_res != WALPE_FXRES_FULL || s->base.stats_wanted);
            if (fx_res_on)
                fk = fx_res_scale (s->q.fx_res, nv, &m, win_w, win_h, fw, fh, fk_full,
                                   node_fx_kmin (s, nv, i), fx_keep);
            if (nv->fxin_state != 2 && !s->nofx && src_tex == nv->tex &&
                fx_has_effects (nv->fxset)) {
                node_fx_input_fit (s, nv, i, fk, node_view_share (&m));
                src_tex = nv->tex;
                src_pm = nv->premult;
            }
            if (scaled)
                fk *= (float) w / (float) win_w;
            if (fx_res_on)
                fx_set_run_res (fk_full * (scaled ? (float) w / (float) win_w : 1.0f), fx_keep);
            /* the layer colour belongs to the layer's own material pass,
             * before its effects (text bakes it into the glyphs) */
            gboolean pre_tint = !s->nofx && !(nv->kind == NK_TEXT && nv->text_baked);
            fx_set_input_tint (nv->fxset, pre_tint ? btint : NULL);
            /* and so does its opacity (g_Color4.a; text: the glyphs'
             * coverage times it): an effect that adds to the alpha adds
             * to the layer's opacity. WALPE_FX_ALPHA=after keeps it for
             * the draw of the result. */
            gboolean pre_alpha = !s->nofx && fx_set_input_alpha (nv->fxset, nv->alpha);
            if (!s->nofx && nv->fxset) {
                /* fx_region.h: what the draw below samples of the
                 * effects' result - a puppet only where its mesh has
                 * triangles, a quad only where it is inside the view. The
                 * passes leave the rest out. */
                float need[6][4];
                int nn = nv->puppet && nv->size[0] > 0 && nv->size[1] > 0
                             ? walpe_puppet_uv_cover (nv->puppet, need, 6)
                             : node_visible_uv (&m, need[0]);
                if (nn <= 0) {
                    /* all of it (then only passes nothing reads are left out) */
                    need[0][0] = need[0][1] = 0.0f;
                    need[0][2] = need[0][3] = 1.0f;
                    nn = 1;
                }
                fx_set_need ((const float (*)[4]) need, nn);
            }
            if (!s->nofx && nv->fxset && !nv->puppet) {
                /* "Skip hidden areas": the part of the layer's picture the
                 * visible boxes show (-1 without a clip; a puppet's mesh
                 * maps it in ways the quad does not tell) */
                float cuv[WALPE_CLIP_MAX][4];
                int cn = walpe_clip_quad_uv (m.m, cuv);
                if (cn >= 0)
                    fx_set_clip_uv (&cuv[0][0], cn);
            }
            GLuint draw_tex = s->nofx ? src_tex
                : fx_run_for_object (nv->fxset, src_tex, fw, fh,
                                     nv->tex_full_w ? nv->tex_full_w : nv->tex_w,
                                     nv->tex_full_h ? nv->tex_full_h : nv->tex_h,
                                     now, fk, nv->video < 0 && src_tex == nv->tex);
            if (draw_tex == src_tex && src_tex == nv->tex && nv->video < 0 && !s->nofx &&
                fx_has_effects (nv->fxset) && node_chain_pending (nv)) {
                /* none of its effects ran (all switched off by an option or a
                 * script, or their shaders are missing): the texture is drawn
                 * straight, like a plain layer's, and like a plain layer's
                 * it gets its mip chain once it is drawn beyond 2:1 */
                float pw = hypotf (m.m[0] * w, m.m[1] * h), ph = hypotf (m.m[3] * w, m.m[4] * h);
                if (pw >= 1 && ph >= 1 && (nv->phys_w > pw * 2.02f || nv->phys_h > ph * 2.02f)) {
                    node_chain_build (nv);
                    SCENE_PICTURE_CHANGED (s);
                    draw_tex = src_tex = nv->tex;
                }
            }
            if (scene_fb)   /* the passes leave framebuffer 0 bound */
                glBindFramebuffer (GL_FRAMEBUFFER, scene_fb);
            float af[6];
            if (!s->nofx && fx_layer_affine (nv->fxset, af)) {
                /* transform effects in Vertex mode at the end of the
                 * chain move the layer itself: world * affine * quad */
                WalpeMat3 am = { { af[0], af[1], 0, af[2], af[3], 0, af[4], af[5], 1 } };
                m = tail;
                walpe_mat3_mul (&m, &m, &nv->world);
                walpe_mat3_mul (&m, &m, &am);
                walpe_mat3_mul (&m, &m, &quad);
            }
            /* fx passes leave their working-resolution viewport bound
             * (texture size ≠ screen size = the "oddly stretched" bug) */
            glViewport (0, 0, w, h);
            /* effect outputs are straight alpha (WE shaders) */
            static const float white3[3] = { 1, 1, 1 };
            const float *dtint = (nv->kind == NK_TEXT && nv->text_baked) ||
                                 (pre_tint && draw_tex != src_tex) ? white3 : btint;
            /* the opacity went in before the effects; a run that handed
             * the input back (nothing ran) leaves it to this draw */
            const float dalpha = pre_alpha && draw_tex != src_tex ? 1.0f : nv->alpha;
            if (nv->puppet && nv->size[0] > 0 && nv->size[1] > 0)
                walpe_puppet_draw (nv->puppet, now, nv->size[0] / 2.0f, nv->size[1] / 2.0f,
                                   draw_tex, draw_tex == src_tex ? src_pm : FALSE,
                                   dalpha, m.m, nv->blend, dtint);
            else
                walpe_glutil_draw_quad_tint (draw_tex, draw_tex == src_tex ? src_pm : FALSE,
                                             dalpha, m.m, nv->blend, dtint);
            /* quality accounting: the quad's on-screen area (the unit quad
             * spans 2 x 2 clip units), at most the frame */
            s->qa.base += fmin (fabs ((double) m.m[0] * m.m[4] - (double) m.m[1] * m.m[3]) * w * h,
                                (double) w * h);
        }
        if (nv->weparts) {
            walpe_clip_suspend (baking || i < clip_from);
            /* system local space -> scene (world, no size) -> clip; the
             * system was advanced before the frame was compared (above):
             * this only draws */
            WalpeMat3 pm = tail;
            walpe_mat3_mul (&pm, &pm, &nv->world);
            walpe_weparticles_render (nv->weparts, pm.m, dt, nv->alpha, nv->tint);
            if (s->probe && s->frame_no % 90 == 1) {
                /* per system: the object's own, then its child systems */
                GString *per = g_string_new (NULL);
                for (int k = 0, n; (n = walpe_weparticles_sys_count (nv->weparts, k)) >= 0; k++)
                    g_string_append_printf (per, "%s%d", k ? "+" : "", n);
                g_debug ("scenenode: particles node %u (id %d): %d live (dt %.4f), per system %s", i,
                         nv->id, walpe_weparticles_count (nv->weparts), dt, per->str);
                g_string_free (per, TRUE);
            }
        }
        if (nv->parts) {
            if (!s->warned_particle_xform) {
                s->warned_particle_xform = TRUE;
                g_debug ("scenenode: particles render in scene space (object transform approximated)");
            }
            walpe_particles_render (nv->parts, s->scene_w, s->scene_h, w, h, px, py);
        }
        if (prof) {
            guint r1 = 0, c1 = 0;
            guint64 shaded = 0;
            if (prof_px) {
                glEndQuery (GL_FRAGMENT_SHADER_INVOCATIONS_ARB);
                glGetQueryObjectui64v (prof_q, GL_QUERY_RESULT, &shaded);
            }
            fx_pass_stats (&r1, &c1, FALSE);
            guint dm = s->nofx ? 0 : fx_dynamic_inputs (nv->fxset);
            float pw = hypotf (m.m[0] * w, m.m[1] * h), ph = hypotf (m.m[3] * w, m.m[4] * h);
            char alive[48] = "";
            if (nv->weparts)
                g_snprintf (alive, sizeof alive, ", %d particles alive",
                            walpe_weparticles_count (nv->weparts));
            /* an unrotated quad over the whole view, drawn plainly */
            gboolean covers = nv->tex_ready && !nv->compose && !nv->puppet && nv->kind == NK_IMAGE &&
                              m.m[1] == 0 && m.m[3] == 0 &&
                              fabsf (m.m[6]) + 1.0f <= fabsf (m.m[0]) + 1e-4f &&
                              fabsf (m.m[7]) + 1.0f <= fabsf (m.m[4]) + 1e-4f;
            g_message ("profile: #%u id %d %s%s tex %dx%d on screen %.0fx%.0f alpha %.2f blend %d | "
                       "shaded %.2f Mpx (%.2f x screen) | "
                       "effects %d, passes drawn %u cached %u, working factor %.3f, follows:%s%s%s%s%s%s%s | "
                       "%s, unchanged for %d frames%s%s",
                       i, nv->id,
                       nv->weparts || nv->parts ? "particles" : nv->kind == NK_TEXT ? "text"
                       : nv->compose ? "compose" : nv->canvas ? "solid" : "image",
                       nv->puppet ? "+puppet" : nv->video >= 0 ? "+video" : "",
                       nv->tex_w, nv->tex_h, (double) pw, (double) ph, (double) nv->alpha, nv->blend,
                       shaded / 1e6, (double) shaded / ((double) w * h),
                       fx_effect_count (nv->fxset), r1 - prof_run0, c1 - prof_cached0,
                       (double) nv->fx_k,
                       dm & FX_DYN_TIME ? " time" : "", dm & FX_DYN_AUDIO ? " audio" : "",
                       dm & FX_DYN_POINTER ? " pointer" : "", dm & FX_DYN_DAYTIME ? " daytime" : "",
                       dm & FX_DYN_SYSTEX ? " albumart" : "", dm & FX_DYN_FRAME ? " frame-below" : "",
                       dm & FX_DYN_FEEDBACK ? " own-last-frame" : "",
                       nv->sig_vol ? "ANIMATED" : "static", nv->stable, alive,
                       covers ? ", covers the view" : "");
        }
        WALPE_PROF_LAYER_END ();   /* GPUPROF */
    }
    walpe_clip_suspend (FALSE);
    walpe_weparticles_end_frame ();
    if (s->pt_stats && s->frame_no % 150 == 0) {
        WalpePtStats ps;
        walpe_weparticles_stats (&ps, TRUE);
        if (ps.renders)
            g_message ("particles: %.1f live, %.1f drawn (+%.1f off screen) in %.1f draws, "
                       "%.1f KiB uploaded, %.2f screen areas of quads -> %.2f blended "
                       "(%.1f systems in the reduced layer, %.1f composites), %.2f screen "
                       "areas of the frame copied for refraction (%.1f copies), CPU %.3f ms "
                       "simulation + %.3f ms geometry per frame (avg of 150)",
                       ps.live / 150.0, ps.drawn / 150.0, ps.culled / 150.0, ps.draws / 150.0,
                       ps.bytes / 150.0 / 1024.0, ps.area / 150.0, ps.shaded / 150.0,
                       ps.lowres_systems / 150.0, ps.composites / 150.0, ps.copied / 150.0,
                       ps.copies / 150.0, ps.sim_ms / 150.0, ps.build_ms / 150.0);
    }
    WALPE_PROF_STAGE (WALPE_PROF_ST_POST);   /* GPUPROF */
    walpe_cph_to (WALPE_CPH_POST);
    if (scene_fb) {
        /* the reduced frame, scaled up to the window */
        glBindFramebuffer (GL_READ_FRAMEBUFFER, scene_fb);
        glBindFramebuffer (GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer (0, 0, w, h, 0, 0, win_w, win_h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glBindFramebuffer (GL_FRAMEBUFFER, 0);
        glViewport (0, 0, win_w, win_h);
    }
    /* "Adjust layers": outline of the highlighted layers, on the window (a
     * frame that is not drawn keeps the one that has it) */
    if (s->hl && s->hl->len)
        scene_layers_outline (s, &tail, win_w, win_h);
frame_done:
    walpe_cph_to (WALPE_CPH_POST);
    for (guint i = 0; i < s->videos->len; i++) {
        SceneVideo *sv = &g_array_index (s->videos, SceneVideo, i);
        if (sv->view_seen)
            walpe_video_tex_set_view (sv->v, sv->view_full ? 0 : sv->view_w,
                                      sv->view_full ? 0 : sv->view_h);
        sv->view_w = sv->view_h = 0;
        sv->view_seen = sv->view_full = FALSE;
    }
    /* "Adjust layers": the layer list is data, not a picture - it goes out
     * also when this frame was not drawn */
    if (s->lay_publish) {
        s->lay_publish = FALSE;
        scene_layers_publish (s);
    }
    if (s->fx_stats && s->frame_no % 150 == 0) {
        guint run = 0, cached = 0;
        fx_pass_stats (&run, &cached, TRUE);
        if (s->fx_count > 0)
            g_message ("fx: %.1f effect passes/frame drawn, %.1f served from the static cache "
                       "(avg of 150 frames)", run / 150.0, cached / 150.0);
        g_message ("scene: of 150 frames %u drawn, %u unchanged (not drawn); %.1f layers/frame "
                   "taken from the static bake", s->n_drawn, s->n_skipped,
                   s->n_drawn ? (double) s->n_baked / s->n_drawn : 0.0);
        s->n_drawn = s->n_skipped = s->n_baked = 0;
    }
    scenenode_px_end (s, w, h);
    if (s->pprobe && !s->pprobe_done) {
        if ((int) s->frame_no >= walpe_probe_frame ()) {
            s->pprobe_done = TRUE;
            scenenode_probe_dump (s);
        } else if (!s->animates && !s->paused) {
            /* a static scene renders once: keep going until the digest */
            c->next_frame_us = g_get_monotonic_time () + 20000;
        }
    }
    if (!s->summary_done) {
        s->summary_done = TRUE;
        scenenode_log_summary (s, w, h);
    }
    if (s->paused)
        scenenode_drop_targets (s);   /* the one frame drawn under a cover */
    else if (!s->animates)
        fx_targets_release ();   /* no frame clock: nothing borrows them again soon */
    scenenode_trim_heap (s);
    if (s->animates && !s->paused) {
        gint64 iv = 1000000 / (s->fps_cap > 0 ? s->fps_cap : 30);
        /* a scene animated only by video needs no more frames than the
         * video has (a 30 fps clip under a 60 fps cap) */
        if (s->only_video_anim && video_iv > iv)
            iv = video_iv;
        c->next_frame_us = walpe_frame_clock_next (&s->due_us, iv);
    }
    walpe_cph_to (WALPE_CPH_NONE);
    return !c->frame_unchanged;
}

static void scenenode_gl_free (WalpeContent *c)
{
    SceneNodeContent *s = (SceneNodeContent *) c;
    for (guint i = 0; i < s->nodes->len; i++) {
        NodeObj *nv = &g_array_index (s->nodes, NodeObj, i);
        if (nv->tex && nv->video < 0)
            walpe_gl_tex_release (nv->tex); /* shared file texture or own */
        node_tex_job_done (nv);
        if (nv->ctex)
            glDeleteTextures (1, &nv->ctex);
        if (nv->cfbo)
            glDeleteFramebuffers (1, &nv->cfbo);
        nv->ctex = nv->cfbo = 0;
        nv->tex = 0;
        nv->tex_ready = FALSE;
        g_clear_pointer (&nv->parts, walpe_particles_free);
        g_clear_pointer (&nv->weparts, walpe_weparticles_free);
        g_clear_pointer (&nv->text_value, g_free);
        walpe_puppet_gl_free (nv->puppet);
        fx_free_for_object (nv->fxset);
        nv->fxset = NULL;
    }
    bake_drop (s);
    scene_target_drop (&s->rs.fbo, &s->rs.tex);
    fx_targets_release ();   /* the pool of this render thread */
    if (s->px_query) {   /* quality accounting */
        if (s->px_state == 1)
            glEndQuery (GL_FRAGMENT_SHADER_INVOCATIONS_ARB);
        glDeleteQueries (1, &s->px_query);
        s->px_query = 0;
        s->px_state = 0;
    }
    fx_set_quality (1, FALSE, WALPE_POST_FULL, 60.0, 0);
    if (s->audio_ref) {
        s->audio_ref = FALSE;
        walpe_audio_unref ();
    }
    if (s->text_held) {
        s->text_held = FALSE;
        walpe_text_release ();
    }
    fx_set_audio_frame (NULL);
    scene_media_hold (s, FALSE);
    for (int k = 1; k < 3; k++)
        media_upload (s, k, NULL, 0, 0);
    fx_set_system_textures (0, 0, 0, 0, 0, 0);
    g_clear_pointer (&s->ss, walpe_ss_free);
    if (s->hl_tex) {
        glDeleteTextures (1, &s->hl_tex);
        s->hl_tex = 0;
    }
    /* GL texture + decoder detach on the render thread that owns it */
    for (guint i = 0; i < s->videos->len; i++) {
        SceneVideo *sv = &g_array_index (s->videos, SceneVideo, i);
        g_clear_pointer (&sv->v, walpe_video_tex_free);
        sv->tex = 0;
    }
}

/* render thread: live fps cap change (next frame uses the new interval) */
static void scenenode_set_fps (WalpeContent *c, int fps)
{
    SceneNodeContent *s = (SceneNodeContent *) c;
    s->fps_cap = fps > 0 ? fps : 30;
    s->due_us = 0;
}

/* render thread: live render scale change. The next frame is drawn at the
 * new size (its signature holds the size), targets follow by themselves. */
static void scenenode_set_render_scale (WalpeContent *c, int percent)
{
    SceneNodeContent *s = (SceneNodeContent *) c;
    if (s->rs_fixed)
        return;   /* WALPE_RENDER_SCALE decides */
    s->render_scale = percent > 0 ? CLAMP (percent, 25, 100) : 100;
}

static void scenenode_set_paused (WalpeContent *c, gboolean p)
{
    SceneNodeContent *s = (SceneNodeContent *) c;
    s->paused = p;
    s->due_us = 0; /* re-phase the frame clock on resume */
    s->last_frame_us = 0; /* particles resume where they stopped */
    /* the scene clock (g_Time, SceneScript engine.runtime, timers, property
     * animations) stops while paused: a covered monitor resumes where it
     * left off instead of jumping ahead by the covered time */
    gint64 now = g_get_monotonic_time ();
    if (p && !s->paused_at) {
        s->paused_at = now;
        g_debug ("scenenode: scene clock stopped at %.3f s", (now - s->t0) / 1e6);
    } else if (!p && s->paused_at) {
        s->t0 += now - s->paused_at;
        g_message ("scenenode: scene clock resumes at %.3f s (paused %.1f s, not skipped)",
                   (now - s->t0) / 1e6, (now - s->paused_at) / 1e6);
        s->paused_at = 0;
    }
    if (p) {
        scenenode_drop_targets (s);
        scenenode_trim_heap (s);
    }
    /* capture only while some audio-reactive scene is actually shown */
    if (s->needs_audio && !g_getenv ("WALPE_NO_AUDIO") && p == s->audio_ref) {
        s->audio_ref = !p;
        if (p)
            walpe_audio_unref ();
        else
            walpe_audio_ref ();
    }
    /* media integration stays subscribed while paused (gl_init .. gl_free):
     * the MPRIS state keeps updating, and the first frame after a resume
     * dispatches what changed, so a media widget is right when uncovered */
    /* the shared decoder sleeps once every user (monitor) is paused */
    for (guint i = 0; i < s->videos->len; i++) {
        SceneVideo *sv = &g_array_index (s->videos, SceneVideo, i);
        if (sv->v)
            walpe_video_tex_set_paused (sv->v, p);
    }
}

/* render thread; no main-loop source references the content any more
 * (the frame clock is the render thread's next_frame_us), so the free
 * is synchronous */
static void scenenode_free (WalpeContent *c)
{
    SceneNodeContent *s = (SceneNodeContent *) c;
    scene_layers_free (s);
    g_free (s->cur_layers);
    g_clear_pointer (&s->fx_miss_logged, g_hash_table_unref);
    if (s->ptr_q)
        g_array_unref (s->ptr_q);
    if (s->root)
        json_node_unref (s->root);
    if (s->nodes) {
        for (guint i = 0; i < s->nodes->len; i++) {
            NodeObj *nv = &g_array_index (s->nodes, NodeObj, i);
            g_free (nv->text_value);
            g_free (nv->name);
            node_tex_job_done (nv);
            g_free (nv->attach);
            g_free (nv->tex_src);
            g_free (nv->inst);
            if (nv->tstyle) {
                walpe_text_style_clear (nv->tstyle);
                g_free (nv->tstyle);
            }
            g_clear_pointer (&nv->puppet, walpe_puppet_free);
        }
        g_array_free (s->nodes, TRUE);
    }
    if (s->user_props)
        json_object_unref (s->user_props);
    if (s->user_types)
        json_object_unref (s->user_types);
    g_clear_pointer (&s->pending_scripts, g_ptr_array_unref);
    g_clear_pointer (&s->ss, walpe_ss_free);
    if (s->videos) {
        for (guint i = 0; i < s->videos->len; i++) {
            SceneVideo *sv = &g_array_index (s->videos, SceneVideo, i);
            /* normally gone in gl_free; a content freed without gl_free
             * (failed gl_init) still detaches its decoders */
            if (sv->v)
                walpe_video_tex_free (sv->v);
            g_free (sv->path);
        }
        g_array_free (s->videos, TRUE);
    }
    g_free (s->dir);
    g_free (s->origin);
    g_free (s->preset_dir);
    g_free (s->base.path);
    g_free (s->base.title);
    g_free (s);
}

/* scene.c sniffs "objects" and delegates here with the parsed root
 * (ownership of root transfers to us) */
WalpeContent *walpe_scenenode_new_from_root (JsonNode *root, const char *project_dir,
                                             const WalpeLoadOpts *opts, GError **err)
{
    JsonObject *ro = json_node_get_object (root);
    SceneNodeContent *s = g_new0 (SceneNodeContent, 1);
    scene_thread_numeric_c ();
    walpe_gl_tex_live_stats (NULL, NULL, TRUE);   /* this load's peak: scenenode_log_summary */
    s->root = root;
    s->dir = g_strdup (project_dir);
    s->origin = g_strdup (opts && opts->origin_dir ? opts->origin_dir : project_dir);
    s->fps_cap = opts && opts->fps > 0 ? opts->fps : 30;
    s->t0 = g_get_monotonic_time ();
    s->probe = g_getenv ("WALPE_PROBE") != NULL;
    s->pprobe = walpe_probe_on ();
    walpe_probe_begin ();
    s->nofx = g_getenv ("WALPE_NOFX") != NULL;
    s->fx_time = g_getenv ("WALPE_FX_TIME") ? g_ascii_strtod (g_getenv ("WALPE_FX_TIME"), NULL) : -1;
    s->fx_stats = g_getenv ("WALPE_FX_STATS") || g_getenv ("WALPE_GPU_MEM");
    s->pt_stats = g_getenv ("WALPE_PT_STATS") != NULL;
    {
        const char *ds = g_getenv ("WALPE_TEX_DOWNSCALE");
        s->downscale = ds && *ds ? (*ds == '2' ? 2 : *ds != '0') : !(opts && opts->no_tex_downscale);
    }
    {
        /* A/B switches: WALPE_FRAME_SKIP=0 draws every frame, WALPE_BAKE=0
         * composites every layer every frame (as rounds up to 12 did);
         * WALPE_RENDER_SCALE=<percent> overrides the render_scale setting */
        const char *e = g_getenv ("WALPE_FRAME_SKIP");
        s->no_skip = e && *e == '0';
        e = g_getenv ("WALPE_BAKE");
        s->no_bake = e && *e == '0';
        e = g_getenv ("WALPE_SCENE_PROFILE");
        s->profile_frame = e && *e ? atoi (e) : 0;
        e = g_getenv ("WALPE_RENDER_SCALE");
        s->rs_fixed = e && *e;
        int rs = s->rs_fixed ? atoi (e) : opts ? opts->render_scale : 0;
        s->render_scale = rs > 0 ? CLAMP (rs, 25, 100) : 100;
    }
    s->base.set_render_scale = scenenode_set_render_scale;
    s->base.type = WALPE_TYPE_SCENE;
    s->base.path = g_strdup (project_dir);
    s->base.title = g_path_get_basename (project_dir);
    s->base.gl_init = scenenode_gl_init;
    s->base.gl_render = scenenode_gl_render;
    s->base.gl_free = scenenode_gl_free;
    s->base.set_paused = scenenode_set_paused;
    s->base.set_fps = scenenode_set_fps;
    s->base.pointer = scenenode_pointer;
    s->base.free = scenenode_free;
    /* quality knobs: High (nothing changes) unless the host says otherwise */
    walpe_quality_preset (WALPE_PRESET_HIGH, &s->q);
    if (opts && opts->quality) {
        s->q = *opts->quality;
        walpe_quality_sanitize (&s->q);
    }
    s->px_can = -1;
    s->base.set_quality = scenenode_set_quality;
    s->base.quality_stats = scenenode_quality_stats;
    s->base.quality_describe = scenenode_quality_describe;

    JsonObject *gen0 = wpj_obj (ro, "general");
    JsonObject *orth = wpj_obj (gen0, "orthogonalprojection");
    collect_inert_scripts (s, -1, gen0, NULL);   /* scripts on scene settings (camera zoom ...) */
    /* node-graph scenes keep width/height at root, in general, or in
     * general.orthogonalprojection (orthographic-camera 2D scenes) */
    s->scene_w = (float) wpj_num (ro, "width", wpj_num (gen0, "width",
                     wpj_num (orth, "width", 1920.0)));
    s->scene_h = (float) wpj_num (ro, "height", wpj_num (gen0, "height",
                     wpj_num (orth, "height", 1080.0)));
    if (s->scene_w < 1 || s->scene_h < 1) {
        s->scene_w = 1920;
        s->scene_h = 1080;
    }

    s->nodes = g_array_new (FALSE, FALSE, sizeof (NodeObj));
    s->cam_id = s->cam_node = -1;
    s->cam_zoom = 1.0f;
    s->videos = g_array_new (FALSE, TRUE, sizeof (SceneVideo));
    s->preset_dir = g_strdup (opts ? opts->preset_dir : NULL);
    s->user_props = collect_user_props (s->dir, s->origin, ro, opts ? opts->preset : NULL,
                                        &s->user_types);
    parse_user_props = s->user_props;
    {
        /* workshop id: the opened dir's name (a preset's base item id is
         * what WE compares, i.e. the dir the scene comes from) */
        char *bn = g_path_get_basename (opts && opts->preset_dir ? project_dir : s->origin);
        guint64 pid = strspn (bn, "0123456789") == strlen (bn) ? g_ascii_strtoull (bn, NULL, 10) : 0;
        if (!pid) {
            /* pkg cache dirs are "<id>-<hash>" */
            pid = g_ascii_strtoull (bn, NULL, 10);
        }
        fx_set_project_id (pid);
        g_free (bn);
    }
    {
        /* general.clearcolor / general.zoom, plain or user-bound. WE clears
         * to this colour unless "clearenabled" is false. */
        float c3[3];
        parse_vec (prop_str (gen0, "clearcolor"), c3, 3, (const float[3]) { 0.0f, 0.0f, 0.0f });
        gboolean clear_on = wpj_bool (gen0, "clearenabled", TRUE);
        for (int i = 0; i < 3; i++)
            s->clear[i] = clear_on && isfinite (c3[i]) ? CLAMP (c3[i], 0.0f, 1.0f) : 0.0f;
        double gz = prop_num (gen0, "zoom", 1.0);
        s->gen_zoom = isfinite (gz) && gz > 0.01 ? (float) gz : 1.0f;
        /* perspective-rendered particle systems: WE draws them with a
         * perspective camera of the scene's "fov" (50 by default) placed
         * where the z = 0 plane fills the orthographic view. Read off
         * WE's own preset previews (256 x 256 scenes): "Rain perspective"
         * sets fov 100 and squeezes the system's depth to 0.1 behind
         * z -64 (camera at 107); the starfield's stars (z speed 1500-2000
         * over 1 s, drag 0.2, depth scale 0.18) reach 285 as they fade,
         * the camera is at 274; snow (depth 0..1000, scale 0.24) ends at
         * 240. Those presets are made for 1080 lines: 1158. */
        double fov = prop_num (gen0, "fov", 50.0);
        if (!isfinite (fov) || fov < 1.0 || fov > 170.0)
            fov = 50.0;
        s->persp_dist = (float) ((s->scene_h / 2.0) / tan (fov * G_PI / 360.0));
    }
    {
        const char *sl = g_getenv ("WALPE_TEX_SLACK");
        double pct = sl && *sl ? g_ascii_strtod (sl, NULL) : opts && opts->no_tex_slack ? 0.0 : 2.0;
        s->slack_pct = isfinite (pct) ? (float) CLAMP (pct, 0.0, 10.0) : 2.0f;
    }
    {
        const char *tb = g_getenv ("WALPE_TEX_BUDGET_MB");
        double mb = tb && *tb ? g_ascii_strtod (tb, NULL) : 1024.0;
        s->tex_budget = isfinite (mb) && mb > 0 ? (gint64) (mb * 1048576.0) : 0;
    }
    /* effect textures and layer textures that cannot wait for the first
     * frame are loaded while the objects are parsed: counted, and held to
     * the budget in load order (glutil.h) */
    walpe_gl_tex_scope_begin (s->tex_budget);
    parse_nodes (wpj_arr (ro, "objects"), s->nodes, s->dir, s);
    s->tex_eager_bytes = walpe_gl_tex_scope_end ();
    parse_user_props = NULL;
    /* resolve "parent" ids to node indices */
    GHashTable *byid = g_hash_table_new (g_direct_hash, g_direct_equal);
    for (guint i = 0; i < s->nodes->len; i++) {
        NodeObj *nv = &g_array_index (s->nodes, NodeObj, i);
        if (nv->id >= 0)
            g_hash_table_insert (byid, GINT_TO_POINTER (nv->id), GUINT_TO_POINTER (i + 1));
    }
    for (guint i = 0; i < s->nodes->len; i++) {
        NodeObj *nv = &g_array_index (s->nodes, NodeObj, i);
        guint pi = nv->parent_id >= 0
            ? GPOINTER_TO_UINT (g_hash_table_lookup (byid, GINT_TO_POINTER (nv->parent_id))) : 0;
        nv->parent = pi && pi - 1 != i ? (int) pi - 1 : -1;
    }
    /* node_world () recurses once per ancestor, every frame: a scene whose
     * objects name each other as parent 60000 deep overflowed the render
     * thread's stack. Real scenes nest a handful of levels; an object more
     * than SCENENODE_MAX_PARENTS below a root becomes a root itself (as
     * the members of a parent cycle do). */
    for (guint i = 0; i < s->nodes->len; i++) {
        int hops = 0;
        for (int k = g_array_index (s->nodes, NodeObj, i).parent;
             k >= 0 && hops <= SCENENODE_MAX_PARENTS; hops++)
            k = g_array_index (s->nodes, NodeObj, k).parent;
        if (hops > SCENENODE_MAX_PARENTS)
            g_array_index (s->nodes, NodeObj, i).parent = -1;
    }
    if (s->cam_found && s->cam_id >= 0)
        s->cam_node = (int) GPOINTER_TO_UINT (g_hash_table_lookup (byid, GINT_TO_POINTER (s->cam_id))) - 1;
    g_hash_table_unref (byid);
    /* layers attached to a puppet's attachment point (node_world): they
     * move, turn and scale with a bone, so their size on screen is not
     * settled (textures and effects as for a scripted layer) */
    for (guint i = 0; i < s->nodes->len; i++) {
        NodeObj *nv = &g_array_index (s->nodes, NodeObj, i);
        if (!nv->attach)
            continue;
        NodeObj *pn = nv->parent >= 0 ? &g_array_index (s->nodes, NodeObj, nv->parent) : NULL;
        float af[6];
        if (pn && walpe_puppet_attachment (pn->puppet, nv->attach, 0.0, af)) {
            nv->scripted = TRUE;
            g_debug ("scenenode: layer %s follows the attachment point \"%s\" of %s",
                     nv->name ? nv->name : "?", nv->attach, pn->name ? pn->name : "?");
        } else {
            g_message ("scenenode: layer %s names the attachment point \"%s\", which its parent "
                       "%s does not have: placed relative to the parent itself",
                       nv->name ? nv->name : "?", nv->attach,
                       pn ? (pn->name ? pn->name : "?") : "(none)");
        }
    }

    {
        /* WALPE_SKIP_IDS="637,353": debugging aid - never draw these object
         * ids. Read here, once: it used to be a getenv () on the render
         * thread for every frame. */
        const char *skip_ids = g_getenv ("WALPE_SKIP_IDS");
        char *wrapped = skip_ids && *skip_ids ? g_strconcat (",", skip_ids, ",", NULL) : NULL;
        for (guint i = 0; wrapped && i < s->nodes->len; i++) {
            NodeObj *nv = &g_array_index (s->nodes, NodeObj, i);
            char idb[24];
            snprintf (idb, sizeof (idb), ",%d,", nv->id);
            nv->dbg_skip = strstr (wrapped, idb) != NULL;
        }
        g_free (wrapped);
    }
    /* diagnostics: what did the parse actually produce? */
    for (guint i = 0; i < s->nodes->len; i++) {
        NodeObj *nv = &g_array_index (s->nodes, NodeObj, i);
        g_debug ("scenenode: node %u kind=%d texready=%d tex=%ux%u origin=%.0f,%.0f "
                 "size=%.0fx%.0f scale=%.2f,%.2f alpha=%.2f visible=%d",
                 i, nv->kind, nv->tex_ready, nv->tex_w, nv->tex_h,
                 nv->origin[0], nv->origin[1], nv->size[0], nv->size[1],
                 nv->scale[0], nv->scale[1], nv->alpha, nv->visible);
    }

    JsonObject *gen = wpj_obj (ro, "general");
    s->base.props = wpj_obj (gen, "properties");
    return &s->base;
}
