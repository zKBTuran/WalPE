/* weparticles.c - Wallpaper Engine particle systems (see weparticles.h).
 *
 * Semantics follow the WE editor's particle JSON as far as it can be
 * inferred from real workshop files (no reference renderer here):
 *   - space: system-local (model space), Y up (rain velocity "0 -3000 0"
 *     falls). Sprites, trails and ropes are built in model space like
 *     WE's genericparticle shaders do and go through the object's
 *     transform: its scale, rotation and mirroring reach them.
 *   - "size": the engine hands the shaders size / 2 (see the calibration
 *     note at SIZE_ATTR): a sprite is size / 2 wide, a rope is size wide;
 *     rotation in radians
 *   - life-fraction operators (alphafade fadein/fadeout, sizechange,
 *     alphachange, colorchange start/end times) take 0..1 of the lifetime
 *   - sprite sheets: animationmode "randomframe" = one random frame per
 *     particle; "sequence" (also the default) = frames advance over the
 *     particle's lifetime x sequencemultiplier; "once" = same, clamped.
 *     Consecutive frames are blended (WE's SPRITESHEETBLEND) unless the
 *     system has the "no frame blending" flag (2).
 * Renderers: sprite, spritetrail (genericparticle.vert: quad along the
 * velocity, "length"/"maxlength"/"minlength" multiply the particle size),
 * rope (all particles joined in spawn order) and ropetrail (a ribbon over
 * each particle's position history), see genericropeparticle.*.
 * Systems with the "perspective rendering" flag (4) are projected with a
 * perspective camera whose z = 0 plane matches the scene (its distance
 * follows the scene's fov, see walpe_weparticles_set_view).
 * "World space" systems (flag 1) keep their sprites, trails and ribbon
 * widths in scene units: the object's scale and rotation place and move
 * the particles but do not stretch what is drawn. */
#include "weparticles.h"
#include "glutil.h"
#include "clip.h"
#include "cpuphase.h"
#include "common/util.h"
#include "common/lazy.h"
#include "common/tex.h"
#include "common/premult.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define PT_HARD_CAP 20000   /* per system, after the count override */
/* Live particles of all systems of the process. One system holds at most
 * PT_HARD_CAP and one particle object at most MAX_SYSTEMS systems, but a
 * scene has as many particle objects as its file lists: each could fill
 * 2.56 million particles (400 MiB, and a ropetrail's history on top) from a
 * few hundred bytes of JSON, a hundred objects more memory than the machine
 * has. Past the total a system counts as full. The particle files of the 32
 * Workshop scenes with particles allow 35900 at most in one scene. */
#define PT_TOTAL_CAP (20 * PT_HARD_CAP)
#define MAX_EMIT 4
#define MAX_OPS 16
#define MAX_CHILD 40
#define MAX_DEPTH 3
/* systems of one particle object, children of children included. WE
 * scenes use a handful; without a total, 40 children on 3 levels allowed
 * 65641 systems (a file that lists itself), each with its own pool and
 * sprite texture */
#define MAX_SYSTEMS 128

enum { EM_BOX, EM_SPHERE };
enum { OP_MOVE, OP_ANGMOVE, OP_ALPHAFADE, OP_ALPHACHANGE, OP_SIZECHANGE, OP_COLORCHANGE,
       OP_OSC_ALPHA, OP_OSC_SIZE, OP_OSC_POS, OP_TURB, OP_ATTRACT, OP_VORTEX };
enum { RD_NONE, RD_SPRITE, RD_TRAIL, RD_ROPE, RD_ROPETRAIL };
#define MAX_CP 8
#define ROPE_SEG_MAX 64
/* What the shaders receive as the particle size (in_ParticleSize): half
 * the JSON size. genericparticle.vert spans size_attr * (uv - 0.5) (a
 * sprite is size / 2 wide), genericropeparticle offsets both ribbon edges
 * by size_attr (a rope is size wide). The factor itself is the engine's
 * and not in WE's files; it is calibrated on real WE screenshots
 * (2901582518 star glints, 3151551777 birds: full-size sprites came out
 * twice too large) and matches the open KDE scene renderer.
 * WALPE_PT_SIZE=full in the environment gives version 0.11's full-size
 * sprites back, to compare on a real wallpaper. */
#define SIZE_ATTR (pt_size_attr ())
/* perspective-flag systems: camera distance from the z = 0 plane (scene
 * units) until the scene says otherwise (set_view): a 1080 high scene at
 * WE's default fov of 50 degrees. At z = 0 the projection equals the
 * orthographic scene. */
#define PERSP_DIST_DEFAULT 1158.0f
/* version 0.12's camera: this far from z = 0 in every scene, whatever its
 * height and fov (also what the open renderers do). WALPE_PT_CAMERA=fixed
 * in the environment selects it, to compare on a scene that is not 1080
 * high: the distance that follows the scene is the least certain of the
 * version 0.12.1 particle changes (WE's own preset previews support it, no
 * screenshot of a real wallpaper decides it). */
#define PERSP_DIST_FIXED 1000.0f
/* nearer to the camera than this fraction of its distance: not drawn */
#define PERSP_NEAR 0.02f
enum { AN_SEQUENCE, AN_RANDOM, AN_ONCE };

/* the two run-time switches above, read once */
static float pt_size_attr (void)
{
    static gsize once;
    static float attr = 0.5f;
    if (g_once_init_enter (&once)) {
        const char *e = g_getenv ("WALPE_PT_SIZE");
        if (e && !g_ascii_strcasecmp (e, "full")) {
            attr = 1.0f;
            g_message ("particles: WALPE_PT_SIZE=full - sprites as wide as their size (version 0.11)");
        }
        g_once_init_leave (&once, 1);
    }
    return attr;
}

static gboolean pt_camera_fixed (void)
{
    static gsize once;
    static gboolean fixed = FALSE;
    if (g_once_init_enter (&once)) {
        const char *e = g_getenv ("WALPE_PT_CAMERA");
        if (e && !g_ascii_strcasecmp (e, "fixed")) {
            fixed = TRUE;
            g_message ("particles: WALPE_PT_CAMERA=fixed - perspective systems seen from "
                       "%.0f units in every scene (version 0.12)", (double) PERSP_DIST_FIXED);
        }
        g_once_init_leave (&once, 1);
    }
    return fixed;
}

/* WALPE_PT_WORLD=scaled: "world space" systems (flag 1) are drawn through
 * their layer's scale and rotation like every other system, as in version 0.12
 * (2901582518's shooting stars, layer scale -4 4: thick bright streaks
 * instead of thin ones). Version 0.12.1 keeps their sprites, trails and ribbon
 * widths in scene units, which is what WE's Workshop screenshot of that
 * wallpaper shows; the switch is for comparing the two on a real screen.
 * Only what is drawn changes: gravity in scene axes is older than the
 * change and stays. One place decides for every renderer and for both
 * sprite submissions (instanced records and expanded quads), because the
 * axes are computed on the CPU before either. */
static gboolean pt_world_scaled (void)
{
    static gsize once;
    static gboolean scaled = FALSE;
    if (g_once_init_enter (&once)) {
        const char *e = g_getenv ("WALPE_PT_WORLD");
        if (e && !g_ascii_strcasecmp (e, "scaled")) {
            scaled = TRUE;
            g_message ("particles: WALPE_PT_WORLD=scaled - world-space systems are drawn "
                       "through their layer's scale (version 0.12)");
        }
        g_once_init_leave (&once, 1);
    }
    return scaled;
}

/* WALPE_PT_CAP=count: the cap of live particles follows the instance's
 * "count" (file maxcount x count), as in version 0.12 and early
 * 0.12.1 builds. The default follows Wallpaper
 * Engine's documentation: the cap is the particle file's maxcount whatever
 * count is. The scenes of the test library speak for the default where
 * they say anything: three authors sized maxcount to what their "count"
 * slider can ask for at its top (2932157836: 1600 = 400 alive x 4,
 * 3042492564's shooting stars: 100 = 20 x 5, 2582765611: 25000 for 1017 x
 * 25), which nobody needs to do if the cap grows with count. Against it:
 * 19 of 97 count values sit on systems that are full with or without them
 * and so change nothing (3163060610's "Count" slider on its dust,
 * 2733091473's count 7 on a flock of maxcount 30). The switch is for
 * comparing the two readings on a real screen; read once. */
static gboolean pt_cap_count (void)
{
    static gsize once;
    static gboolean follows = FALSE;
    if (g_once_init_enter (&once)) {
        const char *e = g_getenv ("WALPE_PT_CAP");
        if (e && !g_ascii_strcasecmp (e, "count")) {
            follows = TRUE;
            g_message ("particles: WALPE_PT_CAP=count - the cap of live particles is the file's "
                       "maxcount x the instance's count (version 0.12)");
        }
        g_once_init_leave (&once, 1);
    }
    return follows;
}

/* WALPE_PT_FADE=span: the "alphafade" operator reads "fadeouttime" as the
 * LENGTH of the fade-out at the end of the life (a fraction of it), not as
 * the point of the life where it starts. For the common 0.1 / 0.9 pair
 * that is: in over the first tenth, then out over the other nine tenths,
 * instead of a plateau from 0.1 to 0.9. The files do not decide (0.1 / 0.1
 * and 0.1 / 0.9 are both frequent in WE's own presets and each is the
 * other's shape under the other reading; "0.1 / 1" and "0.5 / 0.01" each
 * make sense under one of them only). A Wallpaper Engine
 * screenshot of 2901582518 (2560x1440) does speak: its 400 small blinking
 * stars (0.1 / 0.9, oscillatealpha 0.2 .. 1) have a brightness histogram
 * that falls steadily towards the top (median 16 - 19 of 255), which is
 * what a fade over 90 % of the life gives; the default reading gives a
 * plateau up to the maximum (median 30 - 35) and is what the earlier build draws. One
 * frame of one wallpaper is not enough to change every scene that sets
 * both times, so the default stays; the switch is for comparing on a real
 * screen. Read once. */
static gboolean pt_fade_span (void)
{
    static gsize once;
    static gboolean span = FALSE;
    if (g_once_init_enter (&once)) {
        const char *e = g_getenv ("WALPE_PT_FADE");
        if (e && !g_ascii_strcasecmp (e, "span")) {
            span = TRUE;
            g_message ("particles: WALPE_PT_FADE=span - alphafade: fadeouttime is the length of "
                       "the fade-out, not where it starts");
        }
        g_once_init_leave (&once, 1);
    }
    return span;
}

/* a system (by its flags) whose drawn size stays in scene units (see above) */
static gboolean pt_draws_in_world (int flags)
{
    return (flags & 1) != 0 && !pt_world_scaled ();
}
enum { CH_STATIC, CH_FOLLOW, CH_SPAWN, CH_DEATH };

typedef struct {
    int kind;
    float origin[3], dmin[3], dmax[3], dirs[3], sign[3];
    float rate, speedmin, speedmax, duration;
    int instantaneous;
    gboolean audio;            /* audioprocessingmode: rate follows the level */
    double acc, t;
    gboolean burst_done;
} PEmitter;

typedef struct {
    int kind;
    float a, b, c, d, e, f;    /* scalar parameters (meaning per kind) */
    int cp;                    /* control point index (attract, vortex) */
    float v0[3], v1[3];        /* vector parameters */
} POp;

typedef struct {
    float p[3], v[3];
    float rot, av;             /* z rotation (rad) + angular velocity */
    float rxy[2], avxy[2];     /* rotation about x / y (a sprite tilted out of the
                                * screen plane is drawn foreshortened) */
    float col[3], alpha, size; /* initial values */
    float age, life;
    float frame;               /* randomframe index */
    float rnd[4];              /* per-particle seeds for oscillators */
    int owner;                 /* particle of an attached eventfollow child (WeSys.attached):
                                * index of the parent system's particle it moves with;
                                * -1 = none (any more) */
    float dp[3];               /* how far the running step moved it (attached followers
                                * are moved along) */
    float first;               /* > 0: born inside the running step, this many
                                * seconds before its end (rate emitters) */
    guint seq;                 /* spawn order (rope renderer) */
    union {
        struct {
            int hn;            /* ropetrail: history points stored */
            float hacc;        /* ropetrail: time since the newest point */
        };
        /* sprite renderer, systems whose particles never turn (WeSys.
         * rot_fixed): cos / sin of rot, computed when the sprite is first
         * drawn and kept for its life; both 0 (as spawn leaves them) = not
         * computed yet. A sprite with a random angle and no angular
         * velocity used to cost two libm calls a frame. The two renderers
         * never meet in one system, so the words are shared and a
         * particle is no larger than before (the simulation's cost is
         * the memory it walks). */
        struct {
            float rc, rs;
        };
    };
    /* per-frame derived values */
    float d_alpha, d_size, d_col[3], d_off[3];
} Pt;

typedef struct WeSys WeSys;

/* how much fine detail a sprite picture has: decides whether a system may
 * be drawn into the reduced-resolution layer (lr_decide) */
typedef struct {
    gboolean ok;               /* measured (uncompressed picture, upright frames) */
    float d1[2], d2[2];        /* second difference of the premultiplied picture over
                                * 1 and 2 texels, along x and y (0..2): the value
                                * 99.9 % of its texels stay under */
    float edge;                /* largest value on a frame's border (0..1) */
    int fw, fh;                /* frame size, texels */
} SoftInfo;

typedef struct {
    WeSys *sys;
    int type;
    float origin[3];
    float probability;
    int slot;                  /* eventfollow: which of the parent's per-particle
                                * instance states is this child's (WeSys.fol) */
} PChild;

struct WeSys {
    int maxcount;
    /* instance overrides (see weparticles.h): the factor on the emission
     * and the system's clock; maxcount as creation left it and the count
     * it was made with (taken at the first walpe_weparticles_set_override) */
    float emit_mul, time_mul;
    int maxcount0;
    float count0;
    float starttime;
    PEmitter em[MAX_EMIT];
    int nem;
    /* initializers */
    float life_min, life_max, life_exp;
    float size_min, size_max, size_exp;
    float col_min[3], col_max[3];
    float a_min, a_max;
    gboolean has_vel;
    float vel_min[3], vel_max[3];
    gboolean has_rot;
    float rot_min[3], rot_max[3];
    gboolean has_av;
    float av_min[3], av_max[3];
    gboolean has_turbv;
    float turbv_min, turbv_max;
    POp ops[MAX_OPS];
    int nops;
    int renderer;
    float trail_len, trail_max, trail_min;
    float rope_len;            /* ropetrail "length": seconds of history */
    int rope_seg;              /* ropetrail history points */
    float *hist;               /* ropetrail: cap * rope_seg * 3, newest first */
    guint seq_next;
    int flags;                 /* 1 world space, 2 no frame blending, 4 perspective */
    gboolean persp;            /* flags & 4, inherited from the parent system */
    float cscale[3];           /* child system "scale" (root: 1) */
    float cp[MAX_CP][3];       /* control point offsets (system local) */
    guint cp_mouse;            /* bit n: control point n follows the cursor */
    guint cp_world;            /* bit n: control point n is placed in scene coordinates
                                * ("Worldspace", flags bit 1), not relative to the system */
    float cpw[MAX_CP][3];      /* ... that place; cp[n] is then where it lies in
                                * system-local space this frame (sys_set_frame) */
    float cur[3];              /* cursor in system-local space (this frame) */
    gboolean have_cur;
    float winv[4], winv_z;     /* inverse of the object's world 2x2 / scale z */
    int anim;
    float seqmult;
    int blend;
    gboolean refract;          /* REFRACT combo: ntex_path = normal map (slot 1) */
    char *tex_path;            /* slot 0: albedo / alpha shape */
    char *ntex_path;
    GLuint tex, ntex;
    int nfmt;                  /* normal map layout: 0 DXT/BC (x in a), 1 RG88, 2 RGBA (x in a) */
    float refract_amount;      /* g_RefractAmount (WE default 0.05) */
    float overbright;          /* g_Overbright: genericparticle color.rgb *= it (WE default 1) */
    gboolean no_emit;          /* SceneScript pause()/stop(): no new particles */
    gboolean cp0_mouse;        /* control point 0 "link mouse" (flags & 1) */
    float cp0_offset[3];
    int tex_w, tex_h;
    float aspect;              /* sprite height / width (frame or texture) */
    WalpeTexFrame *frames;
    int nframes;
    GLuint ftex;               /* sprite sheet: the frames' uv rectangles (frame_table) */
    int frow;                  /* ... frames per row of it */
    float rbox[4];             /* refracting system, this frame: clip-space bounds of what
                                * its quads can read of the frame - each quad's own bounds
                                * grown by how far its normal map can move a sample
                                * (x0 y0 x1 y1; x0 > x1: unknown, the whole view) */
    float dbox[4];             /* this frame: clip-space bounds of the quads it draws, while
                                * a copy of the frame is kept track of (rc; x0 > x1: unknown) */
    SoftInfo soft;             /* how smooth the sprite is (reduced-resolution layer) */
    int lr_f;                  /* the layer factor it was drawn with last frame (0: directly) */
    gboolean tex_premult;
    WalpeWeParticleOverride ov;
    Pt *pts;
    int count, cap;
    GRand *rng;
    gboolean rot_fixed;        /* no angular velocity at birth and no operator that
                                * gives one: a particle's rot is its spawn value */
    PChild child[MAX_CHILD];
    int nchild;
    /* Child systems that follow events (eventfollow / eventspawn /
     * eventdeath). Wallpaper Engine runs one instance of such a child per
     * parent particle: the child file's maxcount is the cap of ONE instance
     * and the child reference's "maxcount" the number of instances at a
     * time. Here all instances of a child share one system: inst_cap is
     * what one instance may hold (0: not an event child) and maxcount the
     * pool of all of them (parse_children). */
    int inst_cap;
    gboolean attached;         /* eventfollow child without the world-space flag: its
                                * particles move with the parent particle they belong to */
    int nfol;                  /* eventfollow children of this system */
    float *fol;                /* cap * nfol: per particle and eventfollow child, the
                                * emission accumulator of that particle's instance;
                                * < 0 = the particle has none ("probability") */
    double time;
    /* One step (sys_update), while particles die in it and an attached
     * eventfollow child has particles: rm_now[i] = where the particle that
     * was number i when the step began is now (-1: gone), rm_who[k] = which
     * of them sits in slot k. rm_n = how many there were; 0 = not kept. */
    int *rm_now, *rm_who;
    int rm_n, rm_cap;
    /* WALPE_PT_STATS=2: this system's totals, logged when it is freed */
    guint st_draws, st_lr;
    guint64 st_drawn, st_culled;
    double st_area, st_w, st_h;
    float st_err, st_saved;    /* reduced layer: last predicted error (1/255), last saving */
};

struct WalpeWeParticles {
    WeSys *root;
    float audio_level;
    gboolean gl_ready;
    float cursor[3];           /* system-local cursor (CP0 link mouse) */
    gboolean have_cursor;
    float wm[6];               /* object's world transform: 2x2 (column-major) + translation */
    float wz, wsz;             /* object origin z, scale z (perspective systems) */
    float clip[9];             /* scene -> clip (perspective systems are projected on the CPU) */
    float pc[2];               /* perspective centre = the view centre, scene units */
    float pdist;               /* perspective camera distance from z = 0 */
    gboolean have_view;
    float mvp[9];              /* this frame: system local space -> clip */
    float lmat[2][9];          /* mvp / clip into the reduced layer (lmat_f: for which factor) */
    int lmat_f[2];
    gboolean need_prewarm;
    gboolean stepped;          /* walpe_weparticles_step ran: the next render only draws */
};

/* per render thread: two programs (instanced sprite quads; ribbons), the
 * vertex array objects of their layouts, one stream buffer that every
 * system's vertex data goes through, scratch memory to build it in and a
 * fallback soft-dot texture */
typedef struct {
    GLuint prog;
    GLint mvp, refract, nfmt, ramount, vp, sheet, straight;
    int refract_set, sheet_set, straight_set;   /* the uniform values the program holds */
} PtProg;
static __thread PtProg pt_quad, pt_rib;
static __thread GLuint pt_dot;
enum { QL_SHEET = 1, QL_TAN = 2 };         /* quad record: + frames, + tangents */
#define QUAD_FLOATS 10                     /* centre2 axes4 colour4 */
#define RIB_FLOATS 8                       /* pos2 uv2 colour4 */
#define EXP_FLOATS 15                      /* pos2 uv2 colour4 tangents4 uvb2 blend1 */
#define FRAME_ROW 256                      /* frames per row of a frame table */
/* clip-space bound beyond which a quad is off screen: the view's edge
 * (1) plus slack for rounding, 1/512 of the view (4 pixels of 1920) */
#define CULL_EDGE (1.0f + 1.0f / 256.0f)
/* ribbons are submitted in chunks of at most this many floats (a system
 * of long trails would otherwise need a buffer for all of them at once) */
#define RIB_CHUNK_FLOATS (1u << 18)
static __thread GLuint pt_qvao[4], pt_rvao, pt_evao, pt_stream;
static __thread float *pt_scratch;
static __thread gsize pt_scratch_cap;      /* floats */
static __thread guint8 *pt_tmp;            /* rope: order + points */
static __thread gsize pt_tmp_cap;
static __thread int pt_users;              /* particle objects with GL state on this thread */

/* The reduced-resolution layer (per render thread). Large soft sprites -
 * fog, glows, light shafts - are drawn into a buffer of 1/2 or 1/4 of the
 * view's size and the buffer is drawn over the frame once: a screen-sized
 * quad costs 1/4 or 1/16 of the blending, and what several of them cover
 * together is paid at full size once instead of once each.
 * The buffer holds premultiplied colour like the frame: "normal" systems
 * are blended into it as into the frame (colour and alpha), additive ones
 * add their colour and leave its alpha alone, and the composite is
 * frame * (1 - alpha) + colour. For normal systems followed by additive
 * ones that is the arithmetic of drawing them on the frame one after the
 * other, including where the frame saturates. A normal system AFTER
 * additive ones is different (the frame would have clipped at white
 * before it was dimmed), so it starts a new layer (sys_submit). What
 * changes is the resolution of the particles' own picture (see lr_decide
 * for when that is allowed) and the rounding of several blends into one.
 * Layer pixel (i, j) covers the F x F view pixels from ((i - 1) F,
 * (j - 1) F): one pixel of margin around the view, so the bilinear
 * composite never reads past the buffer's edge. */
#define LR_TX 32                       /* tiles across / down: where the layer has */
#define LR_TY 18                       /* content (only those are composited) */
typedef struct {
    gboolean have_target;              /* begin_frame was called for this frame */
    GLuint target;                     /* the framebuffer the scene draws to */
    int tw, th;                        /* its size */
    GLuint fbo, tex;
    int f, lw, lh;                     /* the buffer's factor and size */
    gboolean broken;                   /* could not be made: everything is drawn directly */
    gboolean pending;                  /* holds particles that are not on the frame yet */
    gboolean unclipped;                /* ... some of them drawn while no clip applied
                                        * (clip.h): the composite covers the whole frame */
    gboolean bound;                    /* it is the draw framebuffer right now */
    gboolean has_over, has_add;        /* what is in it: normal / additive systems.
                                        * Without normal ones it has no alpha and
                                        * commutes with additive draws on the frame */
    guint32 tiles[LR_TY];
    int hint_f, seen_f;                /* smallest factor of the last frame / this one */
    int idle;                          /* frames since anything used it */
} PtLayer;
static __thread PtLayer lr;
static __thread gboolean pt_in_render;   /* inside walpe_weparticles_render (its sample query runs) */
static __thread struct { GLuint prog; GLint map; } pt_blit;
static __thread GLuint pt_bvao;
static __thread SoftInfo pt_dot_soft;
/* copy of the frame drawn so far, for REFRACT systems (_rt_FullFrameBuffer) */
static __thread GLuint pt_scene;
static __thread int pt_scene_w, pt_scene_h;

/* ------------------------------------------------------------ settings */

/* user settings, shared by all render threads (walpe_weparticles_set_*);
 * WALPE_PT_LOWRES / WALPE_PT_DENSITY / WALPE_PT_INSTANCED in the
 * environment override them */
static gint pt_lowres_mode = WALPE_PT_LOWRES_AUTO;
static gint pt_density_pct = 100;
static gint pt_instancing = 1;
static gint pt_env_lowres = -1, pt_env_density = -1, pt_env_inst = -1;
/* How a refracting system gets its copy of the frame (the picture is the
 * same in all three): 0 = what is missing of the part it can read, 1 = that
 * part every time (WALPE_PT_REFRACT_COPY=each), 2 = the whole frame every
 * time (=whole, walpe_weparticles_set_refract_whole: the reference) */
enum { RC_MISSING = 0, RC_EACH = 1, RC_WHOLE = 2 };
static gint pt_refract_copy;
/* counts the changes of the three: what the systems draw changed without
 * anything in a scene having changed (walpe_weparticles_settings_serial) */
static gint pt_settings_serial;
static float pt_lowres_tol = 8.0f;     /* predicted error allowed, 1/255 units (lr_decide) */

/* The environment is read once, by one thread; a render thread that
 * arrives meanwhile waits for it (g_once_init_enter). A flag that was set
 * when the reading was done let two monitors' threads both do it, and the
 * tolerance is a plain float: one could read it while the other stored it. */
static void pt_env (void)
{
    static gsize once;
    if (!g_once_init_enter (&once))
        return;
    const char *e = g_getenv ("WALPE_PT_LOWRES");
    if (e && *e)
        g_atomic_int_set (&pt_env_lowres, !g_strcmp0 (e, "all") || *e == '2' ? WALPE_PT_LOWRES_ALL
                          : !g_strcmp0 (e, "off") || *e == '0' ? WALPE_PT_LOWRES_OFF
                          : WALPE_PT_LOWRES_AUTO);
    e = g_getenv ("WALPE_PT_DENSITY");
    if (e && *e)
        g_atomic_int_set (&pt_env_density, CLAMP (atoi (e), 5, 100));
    e = g_getenv ("WALPE_PT_LOWRES_TOL");
    if (e && *e)
        pt_lowres_tol = (float) CLAMP (g_ascii_strtod (e, NULL), 0.0, 64.0);
    e = g_getenv ("WALPE_PT_INSTANCED");
    if (e && *e)
        g_atomic_int_set (&pt_env_inst, *e != '0');
    e = g_getenv ("WALPE_PT_REFRACT_COPY");
    if (e && (!strcmp (e, "whole") || !strcmp (e, "each")))
        g_atomic_int_set (&pt_refract_copy, *e == 'w' ? RC_WHOLE : RC_EACH);
    g_once_init_leave (&once, 1);
}

/* every setting goes through here: a new value counts as a change of the
 * picture (a renderer that skips unchanged frames has to draw one) */
static void pt_setting (gint *which, gint value)
{
    if (g_atomic_int_get (which) == value)
        return;
    g_atomic_int_set (which, value);
    g_atomic_int_inc (&pt_settings_serial);
}

void walpe_weparticles_set_lowres (int mode)
{
    pt_setting (&pt_lowres_mode, CLAMP (mode, WALPE_PT_LOWRES_OFF, WALPE_PT_LOWRES_ALL));
}

void walpe_weparticles_set_density (int percent)
{
    pt_setting (&pt_density_pct, CLAMP (percent, 5, 100));
}

void walpe_weparticles_set_instanced (gboolean on)
{
    pt_setting (&pt_instancing, on ? 1 : 0);
}

void walpe_weparticles_set_refract_whole (gboolean on)
{
    pt_env ();
    g_atomic_int_set (&pt_refract_copy, on ? RC_WHOLE : RC_MISSING);
}

guint walpe_weparticles_settings_serial (void)
{
    return (guint) g_atomic_int_get (&pt_settings_serial);
}

/* sprites as instanced quads, or as six full vertices each like before
 * the instanced path (the same numbers, 360 bytes a particle): a reference
 * to compare the instanced path with, and a way around it on a driver
 * that mishandles it */
static gboolean pt_instanced (void)
{
    pt_env ();
    int e = g_atomic_int_get (&pt_env_inst);
    return e >= 0 ? e : g_atomic_int_get (&pt_instancing);
}

static int lowres_mode (void)
{
    pt_env ();
    int e = g_atomic_int_get (&pt_env_lowres);
    return e >= 0 ? e : g_atomic_int_get (&pt_lowres_mode);
}

/* fraction of its particles a system emits (the "particle density"
 * setting): rate, bursts and the live cap. Not for ropes (their particles
 * are the rope's joints) and one- or two-particle systems (a single
 * sprite that is the whole effect). */
static float sys_density (const WeSys *s)
{
    pt_env ();
    int e = g_atomic_int_get (&pt_env_density);
    int pct = e >= 0 ? e : g_atomic_int_get (&pt_density_pct);
    if (pct >= 100 || s->renderer == RD_ROPE || s->maxcount <= 2)
        return 1.0f;
    return (float) pct / 100.0f;
}

void walpe_weparticles_settings_get (int *lowres, int *density, gboolean *instanced)
{
    pt_env ();
    int e = g_atomic_int_get (&pt_env_density);
    if (lowres)
        *lowres = lowres_mode ();
    if (density)
        *density = e >= 0 ? e : g_atomic_int_get (&pt_density_pct);
    if (instanced)
        *instanced = pt_instanced ();
}

/* ---------------------------------------------------------- statistics */

/* per render thread; collected only when asked for (WALPE_PT_STATS or a
 * call to walpe_weparticles_stats): the counters cost a clipped-area sum
 * per quad and a sample query per system */
static __thread WalpePtStats pt_stats;
static gint pt_stats_on = -1;

static inline gboolean stats_on (void)
{
    gint v = g_atomic_int_get (&pt_stats_on);
    if (G_UNLIKELY (v < 0)) {
        const char *e = g_getenv ("WALPE_PT_STATS");
        v = e ? (*e == '2' ? 2 : 1) : 0;   /* 2: also one line per system when it is freed */
        g_atomic_int_set (&pt_stats_on, v);
    }
    return v > 0;
}

void walpe_weparticles_stats (WalpePtStats *out, gboolean reset)
{
    if (g_atomic_int_get (&pt_stats_on) <= 0 && !stats_on ())
        g_atomic_int_set (&pt_stats_on, 1);
    if (out)
        *out = pt_stats;
    if (reset)
        memset (&pt_stats, 0, sizeof (pt_stats));
}

static double thread_cpu_ms (void)
{
    struct timespec ts;
    if (clock_gettime (CLOCK_THREAD_CPUTIME_ID, &ts) != 0)
        return 0.0;
    return (double) ts.tv_sec * 1e3 + (double) ts.tv_nsec / 1e6;
}

/* area of a convex polygon (n <= 8 corners, clip units) inside the view
 * (-1..1 in x and y), in screen areas */
static double poly_view_area (const float (*in)[2], int n)
{
    float a[12][2], b[12][2];
    memcpy (a, in, sizeof (float) * 2 * (gsize) n);
    for (int e = 0; e < 4; e++) {
        int axis = e >> 1;
        float sgn = (e & 1) ? -1.0f : 1.0f;   /* keep sgn * v <= 1 */
        int m = 0;
        for (int i = 0; i < n; i++) {
            const float *p = a[i], *q = a[(i + 1) % n];
            float dp = 1.0f - sgn * p[axis], dq = 1.0f - sgn * q[axis];
            if (dp >= 0) {
                b[m][0] = p[0]; b[m][1] = p[1]; m++;
            }
            if ((dp >= 0) != (dq >= 0)) {
                float t = dp / (dp - dq);
                b[m][0] = p[0] + (q[0] - p[0]) * t;
                b[m][1] = p[1] + (q[1] - p[1]) * t;
                m++;
            }
        }
        n = m;
        if (n < 3)
            return 0.0;
        memcpy (a, b, sizeof (float) * 2 * (gsize) n);
    }
    double s = 0;
    for (int i = 0; i < n; i++) {
        const float *p = a[i], *q = a[(i + 1) % n];
        s += (double) p[0] * q[1] - (double) q[0] * p[1];
    }
    return fabs (s) * 0.5 / 4.0;
}

/* summed on-screen area of a triangle list (pos = the first two floats of
 * each vertex, drawn through the column-major mat3 m) */
static double tris_view_area (const float *v, int nv, int stride, const float *m)
{
    double s = 0;
    for (int i = 0; i + 2 < nv; i += 3) {
        float t[3][2];
        for (int k = 0; k < 3; k++) {
            const float *p = v + (gsize) (i + k) * stride;
            t[k][0] = m[0] * p[0] + m[3] * p[1] + m[6];
            t[k][1] = m[1] * p[0] + m[4] * p[1] + m[7];
        }
        s += poly_view_area ((const float (*)[2]) t, 3);
    }
    return s;
}

/* summed on-screen area of n quad records (centre, half axes x, y first);
 * *sw, *sh += the quads' widths and heights as fractions of the view's */
static double quads_view_area (const float *v, int n, int stride, const float *m, double *sw,
                               double *sh)
{
    static const float sg[4][2] = { { -1, 1 }, { 1, 1 }, { 1, -1 }, { -1, -1 } };
    double s = 0;
    for (int i = 0; i < n; i++, v += stride) {
        float q[4][2];
        *sw += hypotf (m[0] * v[2] + m[3] * v[3], m[1] * v[2] + m[4] * v[3]);
        *sh += hypotf (m[0] * v[4] + m[3] * v[5], m[1] * v[4] + m[4] * v[5]);
        for (int k = 0; k < 4; k++) {
            float x = v[0] + sg[k][0] * v[2] + sg[k][1] * v[4];
            float y = v[1] + sg[k][0] * v[3] + sg[k][1] * v[5];
            q[k][0] = m[0] * x + m[3] * y + m[6];
            q[k][1] = m[1] * x + m[4] * y + m[7];
        }
        s += poly_view_area ((const float (*)[2]) q, 4);
    }
    return s;
}

/* Sprites and trails are instanced quads: one record per particle - the
 * centre and the two half axes of its quad (drawn space), the colour and,
 * for a sprite sheet, the two frames it shows and their blend (the frames'
 * rectangles are in a small float texture, see frame_table); refracting
 * systems add the screen tangents. The six corners come from gl_VertexID in
 * the order the CPU used to write them, and every sum below has the
 * operands and the order of the CPU code it replaces: the picture is the
 * same, the upload is 40 - 68 bytes a particle instead of 360. */
static const char *PT_VS_QUAD =
    "in vec2 a_c;\n"
    "in vec4 a_ax;\n"
    "in vec4 a_col;\n"
    "in vec3 a_fr;\n"
    "in vec4 a_tan;\n"
    "uniform mat3 u_mvp;\n"
    "uniform highp sampler2D u_frames;\n"
    "uniform int u_sheet;\n"
    "out vec2 v_uv;\n"
    "out vec4 v_col;\n"
    "out vec4 v_tan;\n"
    "out vec3 v_uvb;\n"
    "const vec2 ST[6] = vec2[6](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0),\n"
    "                           vec2(1.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));\n"
    "vec2 frame_uv(int f, vec2 st){\n"
    "  ivec2 at = ivec2(f % u_sheet, 2 * (f / u_sheet));\n"
    "  vec4 a = texelFetch(u_frames, at, 0);\n"
    "  vec4 b = texelFetch(u_frames, at + ivec2(0, 1), 0);\n"
    "  return vec2(a.x + st.x * a.z + st.y * b.x, a.y + st.x * a.w + st.y * b.y); }\n"
    "void main(){\n"
    "  vec2 st = ST[gl_VertexID];\n"
    "  vec2 p = a_c + (2.0 * st.x - 1.0) * a_ax.xy + (1.0 - 2.0 * st.y) * a_ax.zw;\n"
    "  if (u_sheet > 0) {\n"
    "    v_uv = frame_uv(int(a_fr.x), st);\n"
    "    v_uvb = vec3(frame_uv(int(a_fr.y), st), a_fr.z);\n"
    "  } else {\n"
    "    v_uv = st;\n"
    "    v_uvb = vec3(st, 0.0);\n"
    "  }\n"
    "  v_col = a_col; v_tan = a_tan;\n"
    "  gl_Position = vec4((u_mvp * vec3(p, 1.0)).xy, 0.0, 1.0); }\n";

/* triangles built on the CPU: ropes and rope trails (put_ribbon: position,
 * uv and colour per vertex, the other two inputs constant) and, with
 * WALPE_PT_INSTANCED=0, sprites written out corner by corner as they were
 * before the instanced path (quads_expand) */
static const char *PT_VS_TRI =
    "in vec2 a_pos;\n"
    "in vec2 a_uv;\n"
    "in vec4 a_col;\n"
    "in vec4 a_tan;\n"
    "in vec3 a_uvb;\n"
    "uniform mat3 u_mvp;\n"
    "out vec2 v_uv;\n"
    "out vec4 v_col;\n"
    "out vec4 v_tan;\n"
    "out vec3 v_uvb;\n"
    "void main(){ v_uv = a_uv; v_col = a_col; v_tan = a_tan; v_uvb = a_uvb;\n"
    "  gl_Position = vec4((u_mvp * vec3(a_pos, 1.0)).xy, 0.0, 1.0); }\n";

/* the layer over the frame: clip-space rectangles (its tiles with content) */
static const char *PT_VS_BLIT =
    "in vec2 a_pos;\n"
    "uniform vec4 u_map;\n"
    "out vec2 v_uv;\n"
    "void main(){ v_uv = a_pos * u_map.xy + u_map.zw; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
static const char *PT_FS_BLIT =
    "in vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "out vec4 frag;\n"
    "void main(){ frag = texture(u_tex, v_uv); }\n";

static const char *PT_FS =
    "in vec2 v_uv;\n"
    "in vec4 v_col;\n"
    "in vec4 v_tan;\n"
    "in vec3 v_uvb;\n"
    "uniform sampler2D u_tex;\n"
    "uniform sampler2D u_ntex;\n"
    "uniform sampler2D u_scene;\n"
    "uniform float u_refract;\n"
    "uniform float u_straight;\n"
    "uniform int u_nfmt;\n"
    "uniform float u_ramount;\n"
    "uniform vec4 u_vp;\n"
    "out vec4 frag;\n"
    "void main(){ vec4 c = texture(u_tex, v_uv);\n"
    /* sprite sheet: the frame the animation is moving to, mixed in by how
     * far it got (genericparticle.frag, SPRITESHEETBLEND: mix (frame,
     * next frame, v_TexCoordBlend)). v_uvb.z is 0 for everything else. */
    "  if (v_uvb.z > 0.0) c = mix(c, texture(u_tex, v_uvb.xy), v_uvb.z);\n"
    /* REFRACT (genericparticle.frag + common_particles.h): color =
     * v_Color * albedo; color.rgb *= scene(screenUV + offset), offset =
     * (v_ScreenTangents.xy * normal.x + v_ScreenTangents.zw * normal.y) *
     * g_RefractAmount * mask * v_Color.a, y flipped for GL. v_tan =
     * (right.x, up.x, right.y, up.y): the sprite's axes on screen
     * (ComputeScreenRefractionTangents), so the normal map turns with a
     * rotated sprite / a trail. Normal decode = DecompressNormalWithMask
     * of common_fragment.h. The albedo is straight alpha here. Without a
     * normal map (slot 1 null: no NORMALMAP combo, u_nfmt 3) WE still
     * multiplies by the scene behind, unshifted. */
    "  if (u_refract > 0.5) {\n"
    "    vec2 off = vec2(0.0);\n"
    "    if (u_nfmt != 3) {\n"
    "      vec4 n = texture(u_ntex, v_uv);\n"
    "      vec2 nxy; float mask;\n"
    "      if (u_nfmt == 1) { nxy = n.gr * 2.0 - 1.0; mask = n.a; }\n"
    "      else { nxy = vec2(n.a, n.g) * 2.0 - (u_nfmt == 0 ? vec2(0.965, 1.0) : vec2(1.0)); mask = n.r; }\n"
    "      off = (v_tan.xy * nxy.x + v_tan.zw * nxy.y) * u_ramount * mask * v_col.a;\n"
    "      off.y = -off.y;\n"
    "    }\n"
    "    vec2 suv = (gl_FragCoord.xy - u_vp.xy) / u_vp.zw + off;\n"
    "    vec4 col = v_col * c;\n"
    "    col.rgb *= texture(u_scene, suv).rgb;\n"
    "    frag = vec4(col.rgb * col.a, col.a); return; }\n"
    /* texture is premultiplied, or straight alpha (a sprite kept DXT
     * compressed: u_straight); v_col = (rgb tint, alpha) */
    "  if (u_straight > 0.5) c.rgb *= c.a;\n"
    "  frag = vec4(c.rgb * v_col.rgb, c.a) * v_col.a; }\n";

/* ------------------------------------------------------------ parsing */

static float jnum (JsonObject *o, const char *k, float d)
{
    JsonNode *n = o ? json_object_get_member (o, k) : NULL;
    if (!n || json_node_get_node_type (n) != JSON_NODE_VALUE)
        return d;
    GType t = json_node_get_value_type (n);
    if (t == G_TYPE_STRING) {
        const char *s = json_node_get_string (n);
        char *e = NULL;
        double v = g_ascii_strtod (s, &e);
        return e != s ? (float) v : d;
    }
    if (t == G_TYPE_BOOLEAN)
        return json_node_get_boolean (n) ? 1.0f : 0.0f;
    double v = json_node_get_double (n);
    return isfinite (v) ? (float) v : d;
}

/* "a b c" string or a scalar (broadcast) */
static void jvec3 (JsonObject *o, const char *k, float *v, float d0, float d1, float d2)
{
    v[0] = d0; v[1] = d1; v[2] = d2;
    JsonNode *n = o ? json_object_get_member (o, k) : NULL;
    if (!n || json_node_get_node_type (n) != JSON_NODE_VALUE)
        return;
    if (json_node_get_value_type (n) == G_TYPE_STRING) {
        const char *s = json_node_get_string (n);
        float t[3];
        int got = sscanf (s, "%f %f %f", &t[0], &t[1], &t[2]);
        for (int i = 0; i < got && i < 3; i++)
            v[i] = isfinite (t[i]) ? t[i] : 0.0f;
        if (got == 1)
            v[1] = v[2] = v[0];
    } else {
        float s = jnum (o, k, d0);
        v[0] = v[1] = v[2] = s;
    }
}

/* a rotation: "x y z" (radians), or one number = about z (the screen
 * axis; older particle files store just that) */
static void jrot3 (JsonObject *o, const char *k, float *v, float dz)
{
    v[0] = v[1] = 0.0f;
    v[2] = dz;
    JsonNode *n = o ? json_object_get_member (o, k) : NULL;
    if (!n || json_node_get_node_type (n) != JSON_NODE_VALUE)
        return;
    if (json_node_get_value_type (n) == G_TYPE_STRING) {
        float t[3];
        int got = sscanf (json_node_get_string (n), "%f %f %f", &t[0], &t[1], &t[2]);
        if (got == 1)
            v[2] = isfinite (t[0]) ? t[0] : dz;
        else
            for (int i = 0; i < got && i < 3; i++)
                v[i] = isfinite (t[i]) ? t[i] : 0.0f;
    } else {
        v[2] = jnum (o, k, dz);
    }
}

static char *resolve_in (const char *dir, const char *rel)
{
    /* below dir only (no "..", no absolute path), a regular file */
    return walpe_asset_path (dir, rel);
}

static char *resolve_asset2 (const char *dir, const char *we, const char *rel)
{
    char *p = resolve_in (dir, rel);
    return p ? p : resolve_in (we, rel);
}

/* particle material: blending + first texture name -> .tex path */
static void parse_material (WeSys *s, const char *dir, const char *we, const char *mref)
{
    s->blend = WALPE_BLEND_NORMAL;
    s->overbright = 1.0f;
    char *mp = resolve_asset2 (dir, we, mref);
    JsonNode *mj = mp ? wpj_parse_file (mp) : NULL;
    g_free (mp);
    if (!mj)
        return;
    JsonObject *pass = json_node_get_node_type (mj) == JSON_NODE_OBJECT
        ? wpj_arr_obj (wpj_arr (json_node_get_object (mj), "passes"), 0) : NULL;
    if (pass) {
        const char *bl = wpj_str (pass, "blending", "translucent");
        if (!g_ascii_strcasecmp (bl, "additive"))
            s->blend = WALPE_BLEND_ADDITIVE;
        JsonArray *ta = wpj_arr (pass, "textures");
        /* genericparticle.frag: color.rgb *= g_Overbright after the
         * texture/colour product (2901582518's hex/circle bokeh: 0.25;
         * ignoring it drew them 4x too bright over the whole scene) */
        JsonObject *csv = wpj_obj (pass, "constantshadervalues");
        s->overbright = CLAMP (jnum (csv, "ui_editor_properties_overbright",
                                     jnum (csv, "overbright", 1.0f)), 0.0f, 5.0f);
        if (jnum (wpj_obj (pass, "combos"), "REFRACT", 0) > 0 && ta &&
            json_array_get_length (ta) > 1) {
            s->refract = TRUE;  /* slot 1 = normal map, scene distorted */
            JsonObject *cs = wpj_obj (pass, "constantshadervalues");
            s->refract_amount = jnum (cs, "ui_editor_properties_refract_amount",
                                      jnum (cs, "refractamount", 0.05f));
        }
        for (guint ti = 0; ta && ti < MIN (2u, json_array_get_length (ta)); ti++) {
            if (ti == 1 && !s->refract)
                break;
            JsonNode *e0 = json_array_get_element (ta, ti);
            const char *tn = e0 && json_node_get_node_type (e0) == JSON_NODE_VALUE &&
                             json_node_get_value_type (e0) == G_TYPE_STRING
                ? json_node_get_string (e0) : NULL;
            if (!tn)
                continue;
            /* builtin textures (particle/halo, particle/fog/fog1, ...)
             * come from the WE install's materials/ */
            char *rel = g_strconcat ("materials/", tn, ".tex", NULL);
            char *path = resolve_asset2 (dir, we, rel);
            g_free (rel);
            if (!path)
                path = resolve_asset2 (dir, we, tn);
            if (!path)
                g_message ("weparticles: texture %s not found (%s)", tn,
                           we ? "not in the project or the WE assets" : "no WE assets dir: soft dot");
            if (ti == 0)
                s->tex_path = path;
            else
                s->ntex_path = path;
        }
    }
    json_node_unref (mj);
}

static void parse_emitter (WeSys *s, JsonObject *e)
{
    if (s->nem >= MAX_EMIT)
        return;
    const char *n = wpj_str (e, "name", "");
    PEmitter *m = &s->em[s->nem];
    memset (m, 0, sizeof (*m));
    if (!g_strcmp0 (n, "boxrandom"))
        m->kind = EM_BOX;
    else if (!g_strcmp0 (n, "sphererandom"))
        m->kind = EM_SPHERE;
    else
        return;
    jvec3 (e, "origin", m->origin, 0, 0, 0);
    jvec3 (e, "distancemin", m->dmin, 0, 0, 0);
    jvec3 (e, "distancemax", m->dmax, 256, 256, 256);
    jvec3 (e, "directions", m->dirs, 1, 1, 0);
    jvec3 (e, "sign", m->sign, 0, 0, 0);
    m->rate = jnum (e, "rate", 5);
    m->speedmin = jnum (e, "speedmin", 0);
    m->speedmax = jnum (e, "speedmax", 0);
    m->duration = jnum (e, "duration", 0);
    m->instantaneous = walpe_d2i (jnum (e, "instantaneous", 0));
    m->audio = jnum (e, "audioprocessingmode", 0) > 0;
    s->nem++;
}

static void parse_initializer (WeSys *s, JsonObject *e)
{
    const char *n = wpj_str (e, "name", "");
    if (!g_strcmp0 (n, "lifetimerandom")) {
        s->life_min = jnum (e, "min", 0);
        s->life_max = jnum (e, "max", 1);
        s->life_exp = jnum (e, "exponent", 1);
    } else if (!g_strcmp0 (n, "sizerandom")) {
        s->size_min = jnum (e, "min", 0);
        s->size_max = jnum (e, "max", 20);
        s->size_exp = jnum (e, "exponent", 1);
    } else if (!g_strcmp0 (n, "colorrandom")) {
        jvec3 (e, "min", s->col_min, 255, 255, 255);
        jvec3 (e, "max", s->col_max, 255, 255, 255);
        for (int i = 0; i < 3; i++) {
            s->col_min[i] /= 255.0f;
            s->col_max[i] /= 255.0f;
        }
    } else if (!g_strcmp0 (n, "alpharandom")) {
        s->a_min = jnum (e, "min", 0.05f);
        s->a_max = jnum (e, "max", 1);
    } else if (!g_strcmp0 (n, "velocityrandom")) {
        s->has_vel = TRUE;
        jvec3 (e, "min", s->vel_min, -32, -32, 0);
        jvec3 (e, "max", s->vel_max, 32, 32, 0);
    } else if (!g_strcmp0 (n, "rotationrandom")) {
        /* genericparticle's ComputeParticleTangents takes all three
         * angles (WE's rain splash rings: "1.047 0 0" .. "1.047 0 6.283",
         * a ring lying on the ground) */
        s->has_rot = TRUE;
        jrot3 (e, "min", s->rot_min, 0);
        jrot3 (e, "max", s->rot_max, (float) (2 * G_PI));
    } else if (!g_strcmp0 (n, "angularvelocityrandom")) {
        s->has_av = TRUE;
        jrot3 (e, "min", s->av_min, -5);
        jrot3 (e, "max", s->av_max, 5);
    } else if (!g_strcmp0 (n, "turbulentvelocityrandom")) {
        s->has_turbv = TRUE;
        s->turbv_min = jnum (e, "speedmin", 100);
        s->turbv_max = jnum (e, "speedmax", 250);
    }
}

static gboolean parse_operator (WeSys *s, JsonObject *e)
{
    if (s->nops >= MAX_OPS)
        return TRUE;
    const char *n = wpj_str (e, "name", "");
    POp *o = &s->ops[s->nops];
    memset (o, 0, sizeof (*o));
    if (!g_strcmp0 (n, "movement")) {
        o->kind = OP_MOVE;
        jvec3 (e, "gravity", o->v0, 0, 0, 0);
        o->a = jnum (e, "drag", 0);
    } else if (!g_strcmp0 (n, "angularmovement")) {
        o->kind = OP_ANGMOVE;
        jrot3 (e, "force", o->v0, 0);
        o->a = jnum (e, "drag", 0);
    } else if (!g_strcmp0 (n, "alphafade")) {
        o->kind = OP_ALPHAFADE;
        o->a = jnum (e, "fadeintime", 0.5f);
        o->b = jnum (e, "fadeouttime", 0.5f);
    } else if (!g_strcmp0 (n, "alphachange") || !g_strcmp0 (n, "sizechange")) {
        o->kind = n[0] == 'a' ? OP_ALPHACHANGE : OP_SIZECHANGE;
        o->a = jnum (e, "starttime", 0);
        o->b = jnum (e, "endtime", 1);
        o->c = jnum (e, "startvalue", 1);
        o->d = jnum (e, "endvalue", 0);
    } else if (!g_strcmp0 (n, "colorchange")) {
        o->kind = OP_COLORCHANGE;
        o->a = jnum (e, "starttime", 0);
        o->b = jnum (e, "endtime", 1);
        jvec3 (e, "startvalue", o->v0, 1, 1, 1);
        jvec3 (e, "endvalue", o->v1, 1, 1, 1);
    } else if (!g_strcmp0 (n, "oscillatealpha") || !g_strcmp0 (n, "oscillatesize") ||
               !g_strcmp0 (n, "oscillateposition")) {
        o->kind = !g_strcmp0 (n, "oscillatealpha") ? OP_OSC_ALPHA
                : !g_strcmp0 (n, "oscillatesize") ? OP_OSC_SIZE : OP_OSC_POS;
        o->a = jnum (e, "frequencymin", 0);
        o->b = jnum (e, "frequencymax", 10);
        o->c = jnum (e, "scalemin", o->kind == OP_OSC_SIZE ? 0.8f : 0);
        o->d = jnum (e, "scalemax", o->kind == OP_OSC_SIZE ? 1.2f : 1);
        o->e = jnum (e, "phasemin", 0);
        o->f = jnum (e, "phasemax", (float) (2 * G_PI));
        jvec3 (e, "mask", o->v0, 1, 1, 0);
    } else if (!g_strcmp0 (n, "turbulence")) {
        o->kind = OP_TURB;
        o->a = jnum (e, "speedmin", 500);
        o->b = jnum (e, "speedmax", 1000);
        o->c = jnum (e, "timescale", 1);
        o->d = jnum (e, "scale", 0.005f);
        jvec3 (e, "mask", o->v0, 1, 1, 0);
    } else if (!g_strcmp0 (n, "controlpointattract")) {
        /* "Control point force": inside "threshold" of the control point
         * (+ origin) the particle is accelerated towards it by "scale"
         * (negative = pushed away: the cursor repelling dust motes) */
        o->kind = OP_ATTRACT;
        o->cp = CLAMP (walpe_d2i (jnum (e, "controlpoint", 0)), 0, MAX_CP - 1);
        jvec3 (e, "origin", o->v0, 0, 0, 0);
        o->a = jnum (e, "scale", 512);
        o->b = jnum (e, "threshold", 512);
    } else if (!g_strcmp0 (n, "vortex")) {
        /* whirl around the control point (+ offset) about "axis":
         * tangential acceleration speedinner inside distanceinner,
         * speedouter beyond distanceouter, blended in between */
        o->kind = OP_VORTEX;
        o->cp = CLAMP (walpe_d2i (jnum (e, "controlpoint", 0)), 0, MAX_CP - 1);
        jvec3 (e, "offset", o->v0, 0, 0, 0);
        jvec3 (e, "axis", o->v1, 0, 0, 1);
        o->a = jnum (e, "distanceinner", 500);
        o->b = jnum (e, "distanceouter", 650);
        o->c = jnum (e, "speedinner", 2500);
        o->d = jnum (e, "speedouter", 0);
    } else {
        return FALSE;          /* collisions, boids, remapvalue, ... */
    }
    s->nops++;
    return TRUE;
}

/* one particle object being built: how many systems it has so far and the
 * child files open on the way down (a child that names one of them again
 * is a cycle) */
typedef struct {
    int nsys;
    gboolean warned;
    const char *chain[MAX_DEPTH + 2];
    int nchain;
} ParseCx;

static WeSys *sys_new (JsonObject *j, const char *dir, const char *we,
                       const WalpeWeParticleOverride *ov, int depth, ParseCx *cx);

/* a child system and everything below it take over the scale of the
 * nesting and the parent's perspective flag */
static void sys_inherit (WeSys *s, const float f[3], gboolean persp)
{
    for (int k = 0; k < 3; k++)
        s->cscale[k] *= f[k];
    s->persp = s->persp || persp;
    for (int c = 0; c < s->nchild; c++)
        sys_inherit (s->child[c].sys, f, persp);
}

/* What the instance overrides "count" and "rate" in s->ov do (weparticles.h).
 * scripted: WALPE_WE_OV_* bits of the values a script drives; those, and
 * with WALPE_WE_STATIC_OVERRIDES_AS_WE the values of the scene file too,
 * have Wallpaper Engine's meaning: count is the factor on the emission,
 * rate the system's clock, and 0 is a value. Otherwise version 0.12's reading:
 * count moves only the cap of live particles, rate multiplies the
 * emission, 0 = not set. */
#define CAP_FOLLOWS_COUNT (WALPE_WE_CAP_FOLLOWS_COUNT || !WALPE_WE_STATIC_OVERRIDES_AS_WE || \
                           pt_cap_count ())
static void sys_factors (WeSys *s, guint scripted)
{
    gboolean cw = WALPE_WE_STATIC_OVERRIDES_AS_WE || (scripted & WALPE_WE_OV_COUNT);
    gboolean rw = WALPE_WE_STATIC_OVERRIDES_AS_WE || (scripted & WALPE_WE_OV_RATE);
    float count = cw ? MAX (s->ov.count, 0.0f) : s->ov.count > 0 ? s->ov.count : 1.0f;
    float rate = rw ? MAX (s->ov.rate, 0.0f) : s->ov.rate > 0 ? s->ov.rate : 1.0f;
    s->emit_mul = (cw ? count : 1.0f) * (rw ? 1.0f : rate);
    s->time_mul = rw ? rate : 1.0f;
    /* the cap of live particles: the particle file's maxcount, whatever
     * count is (WALPE_WE_CAP_FOLLOWS_COUNT, weparticles.h; maxcount0 is then
     * the file's value). Not before creation is over: a child reference
     * may still lower maxcount. */
    if (s->maxcount0 > 0) {
        float cnt = CAP_FOLLOWS_COUNT ? (float) s->maxcount0 * count / s->count0
                                      : (float) s->maxcount0;
        s->maxcount = cnt > 0 ? (int) CLAMP (cnt, 1, PT_HARD_CAP) : 0;
    }
}

/* An event child of a "world space" system (flag 1) is in the world too:
 * each instance sits on a particle that was left in the world, so the
 * object's scale reaches its sprites as little as it reaches its parent's
 * (pt_draws_in_world; gravity in scene axes with it). WE's shooting star
 * preset is the case: streak file flag 1 with a size of 3 - 12, glow child
 * without the flag and a size of 100 - 150, used under object scales from
 * 0.48 to 7.1 in the test library (2901582518: -4 4). Through the object's
 * matrix the glow would be 4 times (3306713517: 7 times) the size its
 * author saw next to an unscaled streak; WE's Workshop screenshot of
 * 2901582518 has small heads. Emitter extents and velocities stay in the
 * object's space, as the parent's own do. A static child is a system of
 * its own at the object's origin: its file's flag decides, as before. */
static void sys_to_world (WeSys *s)
{
    s->flags |= 1;
    for (int i = 0; i < s->nchild; i++)
        if (s->child[i].type != CH_STATIC)
            sys_to_world (s->child[i].sys);
}

static void parse_children (WeSys *s, JsonArray *ch, const char *dir, const char *we, int depth,
                            ParseCx *cx)
{
    guint n = wpj_arr_len (ch);
    for (guint i = 0; i < n && s->nchild < MAX_CHILD; i++) {
        JsonObject *c = wpj_arr_obj (ch, i);
        const char *name = wpj_str (c, "name", NULL);
        if (!name)
            continue;
        if (cx->nsys >= MAX_SYSTEMS) {
            if (!cx->warned)
                g_warning ("weparticles: more than %d nested systems in one particle object; "
                           "the rest is ignored", MAX_SYSTEMS);
            cx->warned = TRUE;
            break;
        }
        char *pp = resolve_asset2 (dir, we, name);
        /* a file that (directly or through its children) lists itself */
        gboolean cycle = FALSE;
        for (int k = 0; pp && k < cx->nchain; k++)
            cycle |= !strcmp (cx->chain[k], pp);
        if (cycle) {
            g_debug ("weparticles: child %s is its own ancestor, skipped", name);
            g_free (pp);
            continue;
        }
        JsonNode *pj = pp ? wpj_parse_file (pp) : NULL;
        if (!pj) {
            g_free (pp);
            continue;
        }
        if (json_node_get_node_type (pj) == JSON_NODE_OBJECT) {
            /* children inherit the instance overrides except count */
            WalpeWeParticleOverride cov = s->ov;
            if (cx->nchain < (int) G_N_ELEMENTS (cx->chain))
                cx->chain[cx->nchain++] = pp;
            WeSys *cs = sys_new (json_node_get_object (pj), dir, we, &cov, depth + 1, cx);
            if (cx->nchain > 0 && cx->chain[cx->nchain - 1] == pp)
                cx->nchain--;
            if (cs) {
                /* (a number of any size or no number at all in the file; the
                 * child's own limit, used below) */
                int mc = CLAMP (walpe_d2i (jnum (c, "maxcount", 0)), 0, PT_HARD_CAP);
                PChild *pc = &s->child[s->nchild++];
                pc->sys = cs;
                const char *t = wpj_str (c, "type", "static");
                pc->type = !g_strcmp0 (t, "eventfollow") ? CH_FOLLOW
                         : !g_strcmp0 (t, "eventspawn") ? CH_SPAWN
                         : !g_strcmp0 (t, "eventdeath") ? CH_DEATH : CH_STATIC;
                if (pc->type == CH_STATIC) {
                    /* one instance, on its own: as before */
                    if (mc > 0 && mc < cs->maxcount)
                        cs->maxcount = mc;
                } else {
                    /* One instance per parent particle: the file's maxcount
                     * caps one instance, the reference's maxcount says how
                     * many instances run at a time (WE's shooting star
                     * preset: glow file 4, reference 100 = the parent's own
                     * maxcount; rain splash drops 5 and 350; pentagram
                     * shine 1 and 500). Without it: one per particle the
                     * parent can hold. The instances share this system,
                     * which was capped at ONE instance's maxcount before:
                     * 4 glows for 35 stars, 5 drops for 350 splashes. */
                    int ninst = mc > 0 ? mc : s->maxcount;
                    cs->inst_cap = cs->maxcount;
                    cs->maxcount = (int) CLAMP ((gint64) cs->inst_cap * ninst, 1, PT_HARD_CAP);
                    if (pc->type == CH_FOLLOW) {
                        pc->slot = s->nfol++;
                        /* in the system's own space unless the file says
                         * world space: the instance sits on its particle */
                        cs->attached = !(cs->flags & 1);
                    }
                    if (s->flags & 1)
                        sys_to_world (cs);
                }
                jvec3 (c, "origin", pc->origin, 0, 0, 0);
                pc->probability = jnum (c, "probability", 1);
                /* child "scale": the nested system's own transform -
                 * emitter extents, velocities and sprite sizes (sprites
                 * live in model space) scale with it */
                float cs3[3];
                jvec3 (c, "scale", cs3, 1, 1, 1);
                for (int k = 0; k < 3; k++)
                    if (cs3[k] == 0)
                        cs3[k] = 1.0f;
                sys_inherit (cs, cs3, s->persp);
            }
        }
        json_node_unref (pj);
        g_free (pp);
    }
}

static WeSys *sys_new (JsonObject *j, const char *dir, const char *we,
                       const WalpeWeParticleOverride *ov, int depth, ParseCx *cx)
{
    if (!j || depth > MAX_DEPTH || !json_object_has_member (j, "emitter"))
        return NULL;
    cx->nsys++;
    WeSys *s = g_new0 (WeSys, 1);
    s->ov = *ov;
    s->overbright = 1.0f;      /* no material: genericparticle default */
    s->rng = g_rand_new ();
    s->maxcount = walpe_d2i (jnum (j, "maxcount", 100));
    float cnt = s->maxcount * (CAP_FOLLOWS_COUNT && ov->count > 0 ? ov->count : 1.0f);
    s->maxcount = (int) CLAMP (cnt, 1, PT_HARD_CAP);
    sys_factors (s, 0);
    s->starttime = CLAMP (jnum (j, "starttime", 0), 0, 30);
    /* control point 0 with "link mouse" (flags bit 0): the system emits at
     * the cursor (+ the point's offset) - WE's cursor trails / sparkles.
     * 1171326472's sparkles sat at the layer origin (screen centre) */
    JsonArray *cpa = wpj_arr (j, "controlpoint");
    for (guint ci = 0; cpa && ci < json_array_get_length (cpa); ci++) {
        JsonObject *cp = wpj_arr_obj (cpa, ci);
        int id = cp ? walpe_d2i (jnum (cp, "id", -1)) : -1;
        if (id < 0 || id >= MAX_CP)
            continue;
        jvec3 (cp, "offset", s->cp[id], 0, 0, 0);
        if (walpe_d2i (jnum (cp, "flags", 0)) & 1)
            s->cp_mouse |= 1u << id;
        /* "Worldspace" (bit 1): the point's position - the file's offset
         * or the instance's "controlpointN" - is a place in the scene.
         * 3042492564 hands such a point the cursor's world position
         * (event.worldPosition) for a vortex and an attraction around the
         * mouse; taken relative to the system, they sat off by the
         * object's origin (half a screen away from the pointer). */
        if (walpe_d2i (jnum (cp, "flags", 0)) & 2) {
            s->cp_world |= 1u << id;
            memcpy (s->cpw[id], s->cp[id], sizeof (s->cpw[id]));
        }
        if (id == 0 && (s->cp_mouse & 1)) {
            s->cp0_mouse = TRUE;
            jvec3 (cp, "offset", s->cp0_offset, 0, 0, 0);
        }
    }
    /* the scene object's "controlpointN" instance overrides replace the
     * offsets (3151551777's birds fly towards two placed points) */
    for (int ci = 0; ci < MAX_CP; ci++)
        if (ov->cp_set & (1u << ci)) {
            memcpy (s->cp[ci], ov->cp[ci], sizeof (s->cp[ci]));
            memcpy (s->cpw[ci], ov->cp[ci], sizeof (s->cpw[ci]));
        }
    s->flags = walpe_d2i (jnum (j, "flags", 0));
    s->persp = (s->flags & 4) != 0;
    s->cscale[0] = s->cscale[1] = s->cscale[2] = 1.0f;
    s->winv[0] = s->winv[3] = s->winv_z = 1.0f;
    s->life_min = s->life_max = 1;
    s->life_exp = s->size_exp = 1;
    s->size_min = s->size_max = 20;
    s->a_min = s->a_max = 1;
    for (int i = 0; i < 3; i++)
        s->col_min[i] = s->col_max[i] = 1;
    const char *am = wpj_str (j, "animationmode", "sequence");
    s->anim = !g_strcmp0 (am, "randomframe") ? AN_RANDOM : !g_strcmp0 (am, "once") ? AN_ONCE
            : AN_SEQUENCE;
    s->seqmult = jnum (j, "sequencemultiplier", 1);
    if (s->seqmult <= 0)
        s->seqmult = 1;

    JsonArray *a;
    guint n;
    a = wpj_arr (j, "emitter");
    n = wpj_arr_len (a);
    for (guint i = 0; i < n; i++)
        if (wpj_arr_obj (a, i))
            parse_emitter (s, wpj_arr_obj (a, i));
    a = wpj_arr (j, "initializer");
    n = wpj_arr_len (a);
    for (guint i = 0; i < n; i++)
        if (wpj_arr_obj (a, i))
            parse_initializer (s, wpj_arr_obj (a, i));
    a = wpj_arr (j, "operator");
    n = wpj_arr_len (a);
    GString *unsup = NULL;
    for (guint i = 0; i < n; i++) {
        JsonObject *o = wpj_arr_obj (a, i);
        if (o && !parse_operator (s, o)) {
            if (!unsup)
                unsup = g_string_new (NULL);
            g_string_append_printf (unsup, " %s", wpj_str (o, "name", "?"));
        }
    }
    a = wpj_arr (j, "renderer");
    n = wpj_arr_len (a);
    /* an empty "renderer" list draws nothing (spawner parents); a file
     * WITHOUT the member is the older format and is drawn as sprites
     * (WE's own snowflat / light_shafts_1 presets have none) */
    s->renderer = n == 0 && json_object_has_member (j, "renderer") ? RD_NONE : RD_SPRITE;
    for (guint i = 0; i < n; i++) {
        JsonObject *r = wpj_arr_obj (a, i);
        const char *rn = wpj_str (r, "name", "");
        if (!g_strcmp0 (rn, "spritetrail")) {
            /* g_RenderVar0 = (length, maxlength, minlength) of
             * ComputeParticleTrailTangents: up = dir * max (minlength,
             * min (|v| * length, maxlength)), a multiplier of the
             * particle size, not a length in pixels */
            s->renderer = RD_TRAIL;
            s->trail_len = jnum (r, "length", 0.05f);
            s->trail_max = jnum (r, "maxlength", 10);
            s->trail_min = jnum (r, "minlength", 0);
        } else if (!g_strcmp0 (rn, "rope")) {
            s->renderer = RD_ROPE;
        } else if (!g_strcmp0 (rn, "ropetrail")) {
            /* "length" = seconds of position history behind the particle
             * (WE's shooting star preset: 0.2), "segments" = history
             * points */
            s->renderer = RD_ROPETRAIL;
            s->rope_len = CLAMP (jnum (r, "length", 0.5f), 0.01f, 60.0f);
            s->rope_seg = CLAMP (walpe_d2i (jnum (r, "segments", 24)), 2, ROPE_SEG_MAX);
        }
    }
    if (unsup) {
        g_debug ("weparticles: unsupported:%s", unsup->str);
        g_string_free (unsup, TRUE);
    }
    const char *mat = wpj_str (j, "material", NULL);
    if (mat)
        parse_material (s, dir, we, mat);
    parse_children (s, wpj_arr (j, "children"), dir, we, depth, cx);
    if (s->nem == 0 && s->nchild == 0) {
        g_rand_free (s->rng);
        g_free (s->tex_path);
        g_free (s->ntex_path);   /* (a refracting material's normal map leaked here) */
        g_free (s);
        return NULL;
    }
    s->cap = MIN (s->maxcount, 256);
    s->pts = g_new0 (Pt, s->cap);
    if (s->renderer == RD_ROPETRAIL)
        s->hist = g_new0 (float, (gsize) s->cap * s->rope_seg * 3);
    if (s->nfol > 0)
        s->fol = g_new0 (float, (gsize) s->cap * s->nfol);
    return s;
}

/* ----------------------------------------------------------- simulate */

static float rnd (WeSys *s) { return (float) g_rand_double (s->rng); }
static float rrange (WeSys *s, float a, float b) { return a + (b - a) * rnd (s); }
static float rexp (WeSys *s, float a, float b, float ex)
{
    float r = rnd (s);
    if (ex > 0 && ex != 1)
        r = powf (r, ex);
    return a + (b - a) * r;
}

static gint pt_live;        /* particles alive in every system (atomic: several render threads) */

/* n particles of some system are gone */
static void pt_live_sub (int n)
{
    if (n > 0)
        g_atomic_int_add (&pt_live, -n);
}

static Pt *spawn (WeSys *s)
{
    /* the live cap, thinned by the particle density setting (never below
     * one particle; a cap of 0 stays 0) */
    float dn = sys_density (s);
    int cap = s->maxcount;
    if (dn < 1.0f && cap > 0)
        cap = MAX (1, (int) ceilf ((float) cap * dn));
    if (s->count >= cap)
        return NULL;
    if (g_atomic_int_add (&pt_live, 1) >= PT_TOTAL_CAP) {
        static gint said;
        g_atomic_int_add (&pt_live, -1);
        if (g_atomic_int_compare_and_exchange (&said, 0, 1))
            g_warning ("weparticles: %d particles are alive, no more are emitted until some end "
                       "(the limit for all particle systems together)", PT_TOTAL_CAP);
        return NULL;
    }
    if (s->count >= s->cap) {
        int nc = MIN (s->cap * 2, s->maxcount);
        s->pts = g_renew (Pt, s->pts, nc);
        if (s->hist)
            s->hist = g_renew (float, s->hist, (gsize) nc * s->rope_seg * 3);
        if (s->fol)
            s->fol = g_renew (float, s->fol, (gsize) nc * s->nfol);
        s->cap = nc;
    }
    Pt *p = &s->pts[s->count++];
    memset (p, 0, sizeof (*p));
    p->owner = -1;
    p->seq = s->seq_next++;
    float lm = s->ov.lifetime > 0 ? s->ov.lifetime : 1;
    /* one sample: MAX () evaluates its arguments twice, so the random
     * lifetime compared with the floor was not the one stored (a 0 s life
     * got through, and every particle used up two random numbers) */
    float life = rexp (s, s->life_min, s->life_max, s->life_exp) * lm;
    p->life = MAX (0.01f, life);
    p->size = rexp (s, s->size_min, s->size_max, s->size_exp) * s->ov.size * s->cscale[0];
    /* colorrandom: "a random initial color between two provided colors" -
     * one point on the line between them (per-channel random numbers
     * gave colours neither end has) */
    float ct = rnd (s);
    for (int i = 0; i < 3; i++)
        p->col[i] = (s->col_min[i] + (s->col_max[i] - s->col_min[i]) * ct) * s->ov.color[i];
    p->alpha = rrange (s, s->a_min, s->a_max) * s->ov.alpha;
    if (s->has_vel)
        for (int i = 0; i < 3; i++)
            p->v[i] = rrange (s, s->vel_min[i], s->vel_max[i]) * s->ov.speed * s->cscale[i];
    if (s->has_turbv) {
        float ang = rnd (s) * 2.0f * (float) G_PI;
        float sp = rrange (s, s->turbv_min, s->turbv_max) * s->ov.speed;
        p->v[0] += cosf (ang) * sp * s->cscale[0];
        p->v[1] += sinf (ang) * sp * s->cscale[1];
    }
    if (s->has_rot) {
        p->rxy[0] = rrange (s, s->rot_min[0], s->rot_max[0]);
        p->rxy[1] = rrange (s, s->rot_min[1], s->rot_max[1]);
        p->rot = rrange (s, s->rot_min[2], s->rot_max[2]);
    }
    if (s->has_av) {
        p->avxy[0] = rrange (s, s->av_min[0], s->av_max[0]);
        p->avxy[1] = rrange (s, s->av_min[1], s->av_max[1]);
        p->av = rrange (s, s->av_min[2], s->av_max[2]);
    }
    for (int i = 0; i < 4; i++)
        p->rnd[i] = rnd (s);
    if (s->nframes > 1)
        p->frame = (float) g_rand_int_range (s->rng, 0, s->nframes);
    return p;
}

static void child_event (WeSys *s, int type, const float *pos);
static void follow_start (WeSys *s, int pi);

static Pt *emit_one (WeSys *s, PEmitter *m, const float *at)
{
    Pt *p = spawn (s);
    if (!p)
        return NULL;
    float d[3] = { 0, 0, 0 };
    if (m->kind == EM_BOX) {
        for (int i = 0; i < 3; i++) {
            float r = rrange (s, -m->dmax[i], m->dmax[i]);
            d[i] = r * m->dirs[i];
        }
    } else {
        /* random direction in the masked axes, radius in [min, max] */
        float dir[3];
        float len = 0;
        for (int tries = 0; tries < 8 && len < 1e-4f; tries++) {
            for (int i = 0; i < 3; i++)
                dir[i] = rrange (s, -1, 1) * (m->dirs[i] != 0 ? 1.0f : 0.0f);
            len = sqrtf (dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        }
        if (len < 1e-4f) {
            dir[0] = 1; dir[1] = dir[2] = 0; len = 1;
        }
        for (int i = 0; i < 3; i++) {
            dir[i] /= len;
            if (m->sign[i] > 0)
                dir[i] = fabsf (dir[i]);
            else if (m->sign[i] < 0)
                dir[i] = -fabsf (dir[i]);
        }
        float r = rrange (s, m->dmin[0], m->dmax[0]);
        for (int i = 0; i < 3; i++)
            d[i] = dir[i] * r * fabsf (m->dirs[i]);
        if (m->speedmax > 0) {
            float sp = rrange (s, m->speedmin, m->speedmax) * s->ov.speed;
            for (int i = 0; i < 3; i++)
                p->v[i] += dir[i] * sp * s->cscale[i];
        }
    }
    for (int i = 0; i < 3; i++)
        p->p[i] = (at ? at[i] : 0) + (m->origin[i] + d[i]) * s->cscale[i];
    child_event (s, CH_SPAWN, p->p);
    if (s->nfol > 0)
        follow_start (s, (int) (p - s->pts));
    return p;
}

/* eventspawn/eventdeath children: a burst at the parent particle */
static void child_event (WeSys *s, int type, const float *pos)
{
    for (int c = 0; c < s->nchild; c++) {
        PChild *pc = &s->child[c];
        if (pc->type != type)
            continue;
        if (pc->probability < 1 && rnd (s) > pc->probability)
            continue;
        WeSys *cs = pc->sys;
        float at[3] = { pos[0] + pc->origin[0], pos[1] + pc->origin[1], pos[2] + pc->origin[2] };
        float dn = sys_density (cs);
        for (int e = 0; e < cs->nem; e++) {
            int n = cs->em[e].instantaneous > 0 ? cs->em[e].instantaneous : 1;
            /* one instance holds at most its file's maxcount */
            if (cs->inst_cap > 0)
                n = MIN (n, cs->inst_cap);
            if (dn < 1.0f)
                n = MAX (1, (int) lroundf ((float) n * dn));
            /* stop at the first refusal (system full): a crafted
             * "instantaneous" of 2e9 per parent death hung the thread */
            for (int k = 0; k < n; k++)
                if (!emit_one (cs, &cs->em[e], at))
                    break;
        }
    }
}

/* eventfollow children: particle pi of s was born, its instances start.
 * "probability" is the chance that it gets one (WE's bird formations: a
 * lead bird with three followers at 0.5 each); the instance's
 * "instantaneous" emitters fire now (the glow at the head of WE's shooting
 * star is such a burst of one - with only the per-frame rate emission of
 * sys_update it never appeared), its rate emitters run while the particle
 * lives. */
static void follow_start (WeSys *s, int pi)
{
    for (int c = 0; c < s->nchild; c++) {
        PChild *pc = &s->child[c];
        if (pc->type != CH_FOLLOW)
            continue;
        float *acc = &s->fol[(gsize) pi * s->nfol + pc->slot];
        *acc = 0.0f;
        if (pc->probability < 1 && rnd (s) > pc->probability) {
            *acc = -1.0f;
            continue;
        }
        WeSys *cs = pc->sys;
        const float *pos = s->pts[pi].p;
        float at[3] = { pos[0] + pc->origin[0], pos[1] + pc->origin[1], pos[2] + pc->origin[2] };
        float dn = sys_density (cs);
        for (int e = 0; e < cs->nem; e++) {
            int n = cs->em[e].instantaneous;
            if (n <= 0)
                continue;
            n = MIN (n, MAX (cs->inst_cap, 1));
            if (dn < 1.0f)
                n = MAX (1, (int) lroundf ((float) n * dn));
            for (int k = 0; k < n; k++) {
                Pt *q = emit_one (cs, &cs->em[e], at);
                if (!q)
                    break;
                q->owner = cs->attached ? pi : -1;
            }
        }
    }
}

/* particle i of s is gone: the last one takes its place. Followers that
 * moved with it stay where they are and end on their own; those of the
 * particle that changes its index follow it there.
 * The followers are not touched here: every particle of every attached
 * child was looked at for every particle that died, parent deaths x
 * follower count in one step (20000 short-lived particles with 40 attached
 * children of 20000 each: a step of minutes). The step keeps where each
 * of its particles went (sys_follow_track) and re-points the followers
 * once when all deaths are known (sys_follow_settle): the same indices as
 * one death at a time gave. */
static void sys_remove (WeSys *s, int i)
{
    int last = --s->count;
    pt_live_sub (1);
    if (s->rm_n > 0) {
        s->rm_now[s->rm_who[i]] = -1;
        if (i != last) {
            int b = s->rm_who[last];
            s->rm_who[i] = b;
            s->rm_now[b] = i;
        }
    }
    if (i != last) {
        s->pts[i] = s->pts[last];
        if (s->hist)
            memcpy (s->hist + (gsize) i * s->rope_seg * 3, s->hist + (gsize) last * s->rope_seg * 3,
                    sizeof (float) * (gsize) s->rope_seg * 3);
        if (s->fol)
            memcpy (s->fol + (gsize) i * s->nfol, s->fol + (gsize) last * s->nfol,
                    sizeof (float) * (gsize) s->nfol);
    }
}

/* does a particle of some child sit on a particle of s */
static gboolean sys_has_followers (const WeSys *s)
{
    for (int c = 0; c < s->nchild; c++)
        if (s->child[c].type == CH_FOLLOW && s->child[c].sys->attached && s->child[c].sys->count > 0)
            return TRUE;
    return FALSE;
}

/* before the first death of a step: from now on sys_remove keeps track */
static void sys_follow_track (WeSys *s)
{
    int n = s->count;
    if (n > s->rm_cap) {
        s->rm_now = g_renew (int, s->rm_now, n);
        s->rm_who = g_renew (int, s->rm_who, n);
        s->rm_cap = n;
    }
    for (int k = 0; k < n; k++)
        s->rm_now[k] = s->rm_who[k] = k;
    s->rm_n = n;
}

/* after the last death of a step: the followers' particles by their new numbers */
static void sys_follow_settle (WeSys *s)
{
    for (int c = 0; c < s->nchild; c++) {
        WeSys *cs = s->child[c].sys;
        if (s->child[c].type != CH_FOLLOW || !cs->attached)
            continue;
        for (int k = 0; k < cs->count; k++) {
            Pt *q = &cs->pts[k];
            if (q->owner >= 0 && q->owner < s->rm_n)
                q->owner = s->rm_now[q->owner];
        }
    }
    s->rm_n = 0;
}

static void sys_update (WeSys *s, double dt, float audio, const float *at_origin);

static void sys_emit (WeSys *s, double dt, float audio, const float *at)
{
    for (int e = 0; e < s->nem; e++) {
        PEmitter *m = &s->em[e];
        m->t += dt;
        /* the emitter's time is up: nothing more at its rate. Its burst is
         * due at time 0 and is not lost when the first step is longer than
         * the duration (a pre-simulated "starttime" steps 1/30 s: an
         * "instantaneous" emitter with a shorter duration never fired). */
        gboolean over = m->duration > 0 && m->t > m->duration;
        if (over && (m->instantaneous <= 0 || m->burst_done))
            continue;
        /* particle density setting: 1 / density of the emission
         * accumulator buys one particle - the scene's own rate and count
         * factors, whatever they are, are multiplied by it */
        float dn = sys_density (s);
        const double need = dn < 1.0f ? 1.0 / (double) dn : 1.0;
        if (m->instantaneous > 0) {
            if (!m->burst_done) {
                int nb = dn < 1.0f ? MAX (1, (int) lroundf ((float) m->instantaneous * dn))
                                   : m->instantaneous;
                for (int k = 0; k < nb; k++)
                    if (!emit_one (s, m, at))
                        break;   /* full: the rest would be refused too */
                m->burst_done = TRUE;
            }
            if (m->rate <= 0 || over)
                continue;
        }
        float rate = m->rate * s->emit_mul;
        if (m->audio)
            rate *= audio;
        m->acc += rate * dt;
        int guard = 0;
        gboolean full = FALSE;
        while (m->acc >= need && guard++ < 10000) {
            m->acc -= need;
            if (full)
                continue;
            Pt *p = emit_one (s, m, at);
            /* The emitter releases one particle every 1 / rate seconds,
             * not all of a step's particles at its end: this one was due
             * acc / rate seconds ago. Without it everything born in one
             * step shares one age, and a system that lives 0.2 s (rain
             * ripples, 2278738483) shows 6 phases at 30 fps and a single
             * one below 10 fps instead of rings at every stage. */
            if (p && rate > 0)
                p->first = (float) CLAMP (m->acc / rate, 1e-6, dt);
            if (!p) {
                /* Full: nothing ends inside this loop, so every further
                 * round would be refused as well - and there were up to
                 * 10000 of them per emitter and step, each a call that
                 * comes back empty (a file with 128 full systems of four
                 * emitters at a rate of 1e9 and a "starttime" spent a
                 * minute on them before its first frame). What the rounds
                 * leave in the accumulator is worked out instead, to the
                 * same value: with the usual need of 1 each takes 1 off
                 * (exact below 2^53; above it the backlog is dropped
                 * below either way); other needs keep counting down,
                 * without the calls. */
                if (m->acc >= 9007199254740992.0)
                    break;
                if (need == 1.0) {
                    double k = MIN ((double) (10000 - guard), floor (m->acc));
                    m->acc -= k;
                    break;
                }
                full = TRUE;
            }
        }
        if (m->acc > need)
            m->acc = 0;  /* full: drop the backlog */
    }
}

static float life_frac (const Pt *p) { return p->life > 0 ? CLAMP (p->age / p->life, 0, 1) : 1; }

static float window (float f, float a, float b)
{
    if (b <= a)
        return f >= b ? 1.0f : 0.0f;
    return CLAMP ((f - a) / (b - a), 0, 1);
}

/* cheap smooth pseudo-noise (turbulence): sum of incommensurate sines */
static float snoise (float x, float y, float t, float seed)
{
    return 0.5f * sinf (x * 1.7f + t * 1.3f + seed * 6.28f) +
           0.3f * sinf (y * 2.3f - t * 0.7f + seed * 3.1f) +
           0.2f * sinf ((x + y) * 3.1f + t * 2.1f);
}

static void sys_update (WeSys *s, double dt, float audio, const float *at_origin)
{
    pt_stats.systems++;
    s->time += dt;
    if (!s->no_emit)
        sys_emit (s, dt, audio, at_origin);
    if (s->count == 0)
        pt_stats.sim_skipped++;   /* nothing alive: only the clocks moved */
    const float sdt = (float) dt;
    /* can a particle's angle change after its birth? (sys_draw keeps the
     * cos / sin of sprites that never turn: Pt.rc / rs) */
    gboolean turns = s->has_av;
    for (int k = 0; k < s->nops; k++)
        if (s->ops[k].kind == OP_ANGMOVE)
            turns = TRUE;
    s->rot_fixed = !turns;
    /* (no particle is given a follower while this loop runs: they come
     * with the emission above and with the children's below) */
    const gboolean followed = sys_has_followers (s);
    for (int i = 0; i < s->count; i++) {
        Pt *p = &s->pts[i];
        /* a particle born inside this step lives only the rest of it */
        float fdt = sdt;
        if (p->first > 0) {
            fdt = MIN (p->first, sdt);
            p->first = 0;
        }
        p->age += fdt;
        if (p->age >= p->life) {
            child_event (s, CH_DEATH, p->p);
            if (followed && s->rm_n == 0)
                sys_follow_track (s);
            sys_remove (s, i);
            i--;
            continue;
        }
        float f = life_frac (p);
        float am = 1, sm = 1, cm[3] = { 1, 1, 1 };
        float off[3] = { 0, 0, 0 };
        for (int k = 0; k < s->nops; k++) {
            POp *o = &s->ops[k];
            switch (o->kind) {
            case OP_MOVE: {
                /* "world space" systems (flag 1) take gravity in scene
                 * axes: the object's scale / rotation / mirroring must not
                 * reach it (2901582518's shooting stars, object scale -4 4
                 * and 15 degrees: straight streaks with a slight droop in
                 * WE, 4x-gravity arcs when it is applied in model space).
                 * The simulation stays in model space, so the vector goes
                 * through the inverse of the object's matrix. */
                float g[3] = { o->v0[0], o->v0[1], o->v0[2] };
                if (s->flags & 1) {
                    g[0] = s->winv[0] * o->v0[0] + s->winv[2] * o->v0[1];
                    g[1] = s->winv[1] * o->v0[0] + s->winv[3] * o->v0[1];
                    g[2] = o->v0[2] * s->winv_z;
                }
                for (int c = 0; c < 3; c++)
                    p->v[c] += g[c] * fdt;
                if (o->a > 0) {
                    float d = MAX (0.0f, 1.0f - o->a * fdt);
                    for (int c = 0; c < 3; c++)
                        p->v[c] *= d;
                }
                break;
            }
            case OP_ANGMOVE: {
                float d = o->a > 0 ? MAX (0.0f, 1.0f - o->a * fdt) : 1.0f;
                p->avxy[0] = (p->avxy[0] + o->v0[0] * fdt) * d;
                p->avxy[1] = (p->avxy[1] + o->v0[1] * fdt) * d;
                p->av = (p->av + o->v0[2] * fdt) * d;
                break;
            }
            case OP_ALPHAFADE:
                /* fade in until life fraction fadeintime, fade out FROM
                 * fadeouttime to the end (WE's own presets: 0.1 / 0.9,
                 * 0.3 / 0.7, 0.1 / 1 = never). Read as a duration before,
                 * which dimmed 0.1 / 0.9 stars for 90 % of their life. */
                if (pt_fade_span ()) {
                    /* fadeouttime = the last part of the life; where the
                     * two ramps overlap both apply */
                    if (o->a > 0 && f < o->a)
                        am *= f / o->a;
                    if (o->b > 0 && 1 - f < o->b)
                        am *= (1 - f) / o->b;
                    break;
                }
                if (o->a > 0 && f < o->a)
                    am *= f / o->a;
                else if (f > o->b && o->b < 1)
                    am *= (1 - f) / (1 - o->b);
                break;
            case OP_ALPHACHANGE:
                am *= o->c + (o->d - o->c) * window (f, o->a, o->b);
                break;
            case OP_SIZECHANGE:
                sm *= o->c + (o->d - o->c) * window (f, o->a, o->b);
                break;
            case OP_COLORCHANGE: {
                float w = window (f, o->a, o->b);
                for (int c = 0; c < 3; c++)
                    cm[c] *= o->v0[c] + (o->v1[c] - o->v0[c]) * w;
                break;
            }
            case OP_OSC_ALPHA:
            case OP_OSC_SIZE:
            case OP_OSC_POS: {
                float fr = o->a + (o->b - o->a) * p->rnd[0];
                float sc = o->c + (o->d - o->c) * p->rnd[1];
                float ph = o->e + (o->f - o->e) * p->rnd[2] + (float) k;
                float sn = sinf (2.0f * (float) G_PI * fr * p->age + ph);
                if (o->kind == OP_OSC_ALPHA)
                    /* "oscillate a particle's opacity between two
                     * values": scalemin .. scalemax */
                    am *= CLAMP (o->c + (o->d - o->c) * (0.5f + 0.5f * sn), 0, 1);
                else if (o->kind == OP_OSC_SIZE)
                    sm *= o->c + (o->d - o->c) * (0.5f + 0.5f * sn);
                else
                    for (int c = 0; c < 3; c++)
                        off[c] += o->v0[c] * sc * sinf (2.0f * (float) G_PI * fr * p->age +
                                                        ph + (float) c * 1.7f);
                break;
            }
            case OP_ATTRACT:
            case OP_VORTEX: {
                gboolean mouse = (s->cp_mouse >> o->cp) & 1;
                if (mouse && !s->have_cur)
                    break;               /* no pointer yet: no force */
                float c3[3], d3[3];
                for (int c = 0; c < 3; c++) {
                    c3[c] = s->cp[o->cp][c] + o->v0[c] + (mouse ? s->cur[c] : 0.0f);
                    d3[c] = c3[c] - p->p[c];
                }
                float dist = sqrtf (d3[0] * d3[0] + d3[1] * d3[1] + d3[2] * d3[2]);
                if (dist < 1e-4f)
                    break;
                if (o->kind == OP_ATTRACT) {
                    if (dist < o->b)
                        for (int c = 0; c < 3; c++)
                            p->v[c] += d3[c] / dist * o->a * fdt;
                } else {
                    /* tangent = axis x (particle - centre) */
                    float t3[3] = { o->v1[1] * -d3[2] - o->v1[2] * -d3[1],
                                    o->v1[2] * -d3[0] - o->v1[0] * -d3[2],
                                    o->v1[0] * -d3[1] - o->v1[1] * -d3[0] };
                    float tl = sqrtf (t3[0] * t3[0] + t3[1] * t3[1] + t3[2] * t3[2]);
                    if (tl < 1e-6f)
                        break;
                    float sp = dist <= o->a ? o->c : dist >= o->b ? o->d
                             : o->c + (o->d - o->c) * (dist - o->a) / MAX (1e-3f, o->b - o->a);
                    for (int c = 0; c < 3; c++)
                        p->v[c] += t3[c] / tl * sp * fdt;
                }
                break;
            }
            case OP_TURB: {
                float sp = o->a + (o->b - o->a) * p->rnd[3];
                float tt = (float) s->time * o->c;
                float nx = snoise (p->p[0] * o->d, p->p[1] * o->d, tt, p->rnd[0]);
                float ny = snoise (p->p[1] * o->d + 3.3f, p->p[0] * o->d, tt, p->rnd[1]);
                p->v[0] += nx * sp * o->v0[0] * fdt;
                p->v[1] += ny * sp * o->v0[1] * fdt;
                break;
            }
            }
        }
        for (int c = 0; c < 3; c++) {
            p->dp[c] = p->v[c] * fdt;
            p->p[c] += p->dp[c];
        }
        p->rot += p->av * fdt;
        p->rxy[0] += p->avxy[0] * fdt;
        p->rxy[1] += p->avxy[1] * fdt;
        if (s->hist) {
            /* ropetrail: one history point every length / segments
             * seconds, newest first; the head segment runs from the
             * particle to hist[0] */
            float *h = s->hist + (gsize) i * s->rope_seg * 3;
            float iv = s->rope_len / (float) s->rope_seg;
            p->hacc += fdt;
            if (p->hn == 0 || p->hacc >= iv) {
                /* A step longer than the interval owes more than one point
                 * (the stock shooting star: 0.2 s in 24 points = 8.3 ms, a
                 * 60 Hz frame is two of them, a 30 Hz frame four). With one
                 * point per step, whatever its length, the 24 points spanned
                 * 24 frames: the tail was `length` seconds long at 120 Hz
                 * and above, twice that at 60 Hz, four times at 30 Hz. The
                 * points a step owes lie on the way the particle went in
                 * it; the newest is where it is now, as before. */
                float rest = p->hn == 0 ? 0.0f : fmodf (p->hacc, iv);
                int owed = p->hn == 0 ? 1 : (int) MIN (p->hacc / iv, (float) s->rope_seg);
                for (int j = MAX (owed, 1) - 1; j >= 0; j--) {
                    int keep = MIN (p->hn, s->rope_seg - 1);
                    memmove (h + 3, h, sizeof (float) * 3 * (gsize) keep);
                    if (j == 0 || !(fdt > 0)) {
                        for (int c = 0; c < 3; c++)
                            h[c] = p->p[c] + off[c];
                    } else {
                        /* (rest + j intervals) ago */
                        float back = MIN ((rest + (float) j * iv) / fdt, 1.0f);
                        for (int c = 0; c < 3; c++)
                            h[c] = p->p[c] - p->dp[c] * back + off[c];
                    }
                    p->hn = keep + 1;
                }
                p->hacc = p->hn == 1 ? 0.0f : rest;
            }
        }
        p->d_alpha = CLAMP (p->alpha * am, 0, 1);
        p->d_size = MAX (0.0f, p->size * sm);
        for (int c = 0; c < 3; c++) {
            p->d_col[c] = p->col[c] * cm[c];
            p->d_off[c] = off[c];
        }
    }
    if (s->rm_n > 0)
        sys_follow_settle (s);
    /* children: static ones run on their own; eventfollow ones emit at
     * their rate from every live parent particle */
    for (int c = 0; c < s->nchild; c++) {
        PChild *pc = &s->child[c];
        WeSys *cs = pc->sys;
        if (pc->type == CH_FOLLOW) {
            /* followers that are not in world space sit on their particle:
             * they go where this step took it (before the new ones are
             * emitted at where it is now) */
            if (cs->attached)
                for (int k = 0; k < cs->count; k++) {
                    Pt *q = &cs->pts[k];
                    if (q->owner < 0 || q->owner >= s->count)
                        continue;
                    const float *dp = s->pts[q->owner].dp;
                    for (int a = 0; a < 3; a++)
                        q->p[a] += dp[a];
                }
            /* particle density setting, as in sys_emit */
            const float fneed = 1.0f / sys_density (cs);
            float rate = 0;
            for (int e = 0; e < cs->nem; e++)
                rate += cs->em[e].rate;
            rate *= cs->emit_mul;
            /* every particle's instance emits at its rate (its bursts went
             * out when the particle was born: follow_start) */
            for (int i = 0; i < s->count && cs->nem > 0 && rate > 0; i++) {
                Pt *p = &s->pts[i];
                float *acc = &s->fol[(gsize) i * s->nfol + pc->slot];
                if (*acc < 0)
                    continue;            /* this particle has no instance */
                *acc += rate * sdt;
                while (*acc >= fneed) {
                    *acc -= fneed;
                    float at[3] = { p->p[0] + pc->origin[0], p->p[1] + pc->origin[1],
                                    p->p[2] + pc->origin[2] };
                    Pt *q = emit_one (cs, &cs->em[0], at);
                    if (!q) {
                        *acc = 0;
                        break;
                    }
                    q->owner = cs->attached ? i : -1;
                }
            }
            /* advance existing child particles without their own emission */
            double t = cs->time;
            float saved[MAX_EMIT];
            for (int e = 0; e < cs->nem; e++) {
                saved[e] = cs->em[e].rate;
                cs->em[e].rate = 0;
                cs->em[e].burst_done = TRUE;
            }
            sys_update (cs, dt, audio, NULL);
            for (int e = 0; e < cs->nem; e++)
                cs->em[e].rate = saved[e];
            (void) t;
        } else if (pc->type == CH_STATIC) {
            /* static children sit at their own offset in the parent's space */
            float at[3];
            for (int k = 0; k < 3; k++)
                at[k] = (at_origin ? at_origin[k] : 0) + pc->origin[k];
            sys_update (cs, dt, audio, at);
        } else {
            /* event bursts were emitted by child_event: just advance */
            float saved[MAX_EMIT];
            for (int e = 0; e < cs->nem; e++) {
                saved[e] = cs->em[e].rate;
                cs->em[e].rate = 0;
                cs->em[e].burst_done = TRUE;
            }
            sys_update (cs, dt, audio, NULL);
            for (int e = 0; e < cs->nem; e++)
                cs->em[e].rate = saved[e];
        }
    }
}

/* ---------------------------------------------------------------- GL */

static GLuint compile (GLenum type, const char *hdr, const char *body)
{
    char *src = g_strconcat (hdr, body, NULL);
    GLuint sh = glCreateShader (type);
    glShaderSource (sh, 1, (const char **) &src, NULL);
    glCompileShader (sh);
    g_free (src);
    GLint ok = 0;
    glGetShaderiv (sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog (sh, sizeof (log), NULL, log);
        g_warning ("weparticles: shader: %s", log);
    }
    return sh;
}

static void soft_measure (guint (*hist)[511], float *edge, const guint8 *px, gsize stride, int x0,
                          int y0, int w, int h);
static void soft_finish (SoftInfo *si, guint (*hist)[511]);

static void prog_build (PtProg *p, const char *vs_body, gboolean quad)
{
    GLuint vs = compile (GL_VERTEX_SHADER, walpe_gl_shader_header (), vs_body);
    GLuint fs = compile (GL_FRAGMENT_SHADER, walpe_gl_shader_fs_header (), PT_FS);
    p->prog = glCreateProgram ();
    glAttachShader (p->prog, vs);
    glAttachShader (p->prog, fs);
    if (quad) {
        glBindAttribLocation (p->prog, 0, "a_c");
        glBindAttribLocation (p->prog, 1, "a_ax");
        glBindAttribLocation (p->prog, 2, "a_col");
        glBindAttribLocation (p->prog, 3, "a_fr");
        glBindAttribLocation (p->prog, 4, "a_tan");
    } else {
        glBindAttribLocation (p->prog, 0, "a_pos");
        glBindAttribLocation (p->prog, 1, "a_uv");
        glBindAttribLocation (p->prog, 2, "a_col");
        glBindAttribLocation (p->prog, 3, "a_tan");
        glBindAttribLocation (p->prog, 4, "a_uvb");
    }
    glLinkProgram (p->prog);
    glDeleteShader (vs);
    glDeleteShader (fs);
    GLint ok = 0;
    glGetProgramiv (p->prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog (p->prog, sizeof (log), NULL, log);
        g_warning ("weparticles: program: %s", log);
    }
    p->mvp = glGetUniformLocation (p->prog, "u_mvp");
    p->refract = glGetUniformLocation (p->prog, "u_refract");
    p->nfmt = glGetUniformLocation (p->prog, "u_nfmt");
    p->ramount = glGetUniformLocation (p->prog, "u_ramount");
    p->vp = glGetUniformLocation (p->prog, "u_vp");
    p->sheet = glGetUniformLocation (p->prog, "u_sheet");
    p->straight = glGetUniformLocation (p->prog, "u_straight");
    /* the texture units never change: sprite 0, normal map 1, the frame
     * behind 2, frame table 3 */
    glUseProgram (p->prog);
    glUniform1i (glGetUniformLocation (p->prog, "u_tex"), 0);
    glUniform1i (glGetUniformLocation (p->prog, "u_ntex"), 1);
    glUniform1i (glGetUniformLocation (p->prog, "u_scene"), 2);
    glUniform1i (glGetUniformLocation (p->prog, "u_frames"), 3);
    p->refract_set = p->sheet_set = p->straight_set = 0;   /* a new program's uniforms are 0 */
}

static void quad_attrib (GLuint index, int size, GLsizei stride, int *at)
{
    glEnableVertexAttribArray (index);
    glVertexAttribPointer (index, size, GL_FLOAT, GL_FALSE, stride,
                           (void *) ((gsize) *at * sizeof (float)));
    glVertexAttribDivisor (index, 1);
    *at += size;
}

static void thread_gl_init (void)
{
    if (pt_quad.prog)
        return;
    prog_build (&pt_quad, PT_VS_QUAD, TRUE);
    prog_build (&pt_rib, PT_VS_TRI, FALSE);
    /* every layout reads this thread's stream buffer from its start */
    glGenBuffers (1, &pt_stream);
    glGenVertexArrays (4, pt_qvao);
    for (int l = 0; l < 4; l++) {
        int fl = QUAD_FLOATS + ((l & QL_SHEET) ? 3 : 0) + ((l & QL_TAN) ? 4 : 0), at = 0;
        GLsizei stride = (GLsizei) (fl * sizeof (float));
        glBindVertexArray (pt_qvao[l]);
        glBindBuffer (GL_ARRAY_BUFFER, pt_stream);
        quad_attrib (0, 2, stride, &at);
        quad_attrib (1, 4, stride, &at);
        quad_attrib (2, 4, stride, &at);
        if (l & QL_SHEET)
            quad_attrib (3, 3, stride, &at);
        if (l & QL_TAN)
            quad_attrib (4, 4, stride, &at);
    }
    glGenVertexArrays (1, &pt_rvao);
    glBindVertexArray (pt_rvao);
    glBindBuffer (GL_ARRAY_BUFFER, pt_stream);
    const GLsizei rstride = RIB_FLOATS * sizeof (float);
    glEnableVertexAttribArray (0);
    glVertexAttribPointer (0, 2, GL_FLOAT, GL_FALSE, rstride, (void *) 0);
    glEnableVertexAttribArray (1);
    glVertexAttribPointer (1, 2, GL_FLOAT, GL_FALSE, rstride, (void *) (2 * sizeof (float)));
    glEnableVertexAttribArray (2);
    glVertexAttribPointer (2, 4, GL_FLOAT, GL_FALSE, rstride, (void *) (4 * sizeof (float)));
    glGenVertexArrays (1, &pt_evao);
    glBindVertexArray (pt_evao);
    glBindBuffer (GL_ARRAY_BUFFER, pt_stream);
    const GLsizei estride = EXP_FLOATS * sizeof (float);
    static const int esize[5] = { 2, 2, 4, 4, 3 };
    for (int i = 0, at = 0; i < 5; at += esize[i], i++) {
        glEnableVertexAttribArray ((GLuint) i);
        glVertexAttribPointer ((GLuint) i, esize[i], GL_FLOAT, GL_FALSE, estride,
                               (void *) ((gsize) at * sizeof (float)));
    }
    glBindVertexArray (0);
    /* soft round dot (premultiplied) for builtin WE particle textures
     * (particle/halo etc. live in the WE install's assets) */
    /* (on the stack, 16 KiB: as a static it was one buffer that every
     * monitor's render thread filled while another handed it to GL) */
    enum { D = 64 };
    guint8 px[D * D * 4];
    for (int y = 0; y < D; y++)
        for (int x = 0; x < D; x++) {
            float dx = (x + 0.5f) / D * 2 - 1, dy = (y + 0.5f) / D * 2 - 1;
            float r = sqrtf (dx * dx + dy * dy);
            float a = CLAMP (1.0f - r, 0, 1);
            a = a * a * (3 - 2 * a);
            guint8 v = (guint8) (a * 255.0f + 0.5f);
            px[(y * D + x) * 4 + 0] = v;
            px[(y * D + x) * 4 + 1] = v;
            px[(y * D + x) * 4 + 2] = v;
            px[(y * D + x) * 4 + 3] = v;
        }
    glGenTextures (1, &pt_dot);
    glBindTexture (GL_TEXTURE_2D, pt_dot);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, D, D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    memset (&pt_dot_soft, 0, sizeof (pt_dot_soft));
    pt_dot_soft.ok = TRUE;
    pt_dot_soft.fw = pt_dot_soft.fh = D;
    guint (*dh)[511] = g_malloc0 (sizeof (guint[4][511]));
    soft_measure (dh, &pt_dot_soft.edge, px, D * 4, 0, 0, D, D);
    soft_finish (&pt_dot_soft, dh);
    g_free (dh);

    /* the reduced-resolution layer's composite */
    GLuint bvs = compile (GL_VERTEX_SHADER, walpe_gl_shader_header (), PT_VS_BLIT);
    GLuint bfs = compile (GL_FRAGMENT_SHADER, walpe_gl_shader_fs_header (), PT_FS_BLIT);
    pt_blit.prog = glCreateProgram ();
    glAttachShader (pt_blit.prog, bvs);
    glAttachShader (pt_blit.prog, bfs);
    glBindAttribLocation (pt_blit.prog, 0, "a_pos");
    glLinkProgram (pt_blit.prog);
    glDeleteShader (bvs);
    glDeleteShader (bfs);
    pt_blit.map = glGetUniformLocation (pt_blit.prog, "u_map");
    glUseProgram (pt_blit.prog);
    glUniform1i (glGetUniformLocation (pt_blit.prog, "u_tex"), 0);
    glGenVertexArrays (1, &pt_bvao);
    glBindVertexArray (pt_bvao);
    glBindBuffer (GL_ARRAY_BUFFER, pt_stream);
    glEnableVertexAttribArray (0);
    glVertexAttribPointer (0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof (float), (void *) 0);
    glBindVertexArray (0);
}

static void frame_rect (const WeSys *s, int fi, float uv[6]);

/* |a[i] - 2 * b[i] + c[i]| of n bytes, counted. Eight tables, one per
 * byte of a word, summed by the caller: a count is a load, an add and a
 * store on one table entry, and in a picture most neighbours give the
 * same value, so with one table every count waited for the one before
 * it. Words in which all three rows are zero (the transparent ground of
 * a sprite, premultiplied) are counted at once into *zeros: they are
 * that many differences of 0. The sums are the counts of the plain loop,
 * byte for byte (tests/particle_sheet_test.c compares them). */
static inline void soft_d2_run (const guint8 *a, const guint8 *b, const guint8 *c, gsize n,
                                guint (*t)[511], guint64 *zeros)
{
#define SOFT_D2(j) abs ((int) a[i + (j)] - 2 * (int) b[i + (j)] + (int) c[i + (j)])
    gsize i = 0;
    guint64 z = 0;
    for (; i + 8 <= n; i += 8) {
        guint64 x, y, w;
        memcpy (&x, a + i, 8);
        memcpy (&y, b + i, 8);
        memcpy (&w, c + i, 8);
        if ((x | y | w) == 0) {
            z += 8;
            continue;
        }
        t[0][SOFT_D2 (0)]++;
        t[1][SOFT_D2 (1)]++;
        t[2][SOFT_D2 (2)]++;
        t[3][SOFT_D2 (3)]++;
        t[4][SOFT_D2 (4)]++;
        t[5][SOFT_D2 (5)]++;
        t[6][SOFT_D2 (6)]++;
        t[7][SOFT_D2 (7)]++;
    }
    for (; i < n; i++)
        t[0][SOFT_D2 (0)]++;
    *zeros += z;
#undef SOFT_D2
}

/* second differences along one axis of a w x h block of premultiplied
 * RGBA (px, `stride` bytes between rows) at step k texels, counted into
 * hist[0..510]: a straight ramp gives 0, noise and sharp edges are large */
static void soft_d2 (const guint8 *px, gsize stride, int w, int h, int k, gboolean along_y,
                     guint *hist)
{
    guint t[8][511];
    guint64 zeros = 0;
    memset (t, 0, sizeof (t));
    if (along_y) {
        for (int y = k; y < h - k; y++)
            soft_d2_run (px + (gsize) (y - k) * stride, px + (gsize) y * stride,
                         px + (gsize) (y + k) * stride, (gsize) w * 4, t, &zeros);
    } else if (w > 2 * k) {
        for (int y = 0; y < h; y++) {
            const guint8 *r = px + (gsize) y * stride;
            soft_d2_run (r, r + (gsize) k * 4, r + (gsize) k * 8, (gsize) (w - 2 * k) * 4, t,
                         &zeros);
        }
    }
    for (int i = 0; i <= 510; i++)
        hist[i] += t[0][i] + t[1][i] + t[2][i] + t[3][i] + t[4][i] + t[5][i] + t[6][i] + t[7][i];
    hist[0] += (guint) zeros;
}

/* SOFT_PCT of the picture's second differences are at most this (0..2) */
#define SOFT_PCT 0.999
static float soft_pct (const guint *hist)
{
    guint64 n = 0, acc = 0;
    for (int i = 0; i <= 510; i++)
        n += hist[i];
    guint64 want = (guint64) ((double) n * SOFT_PCT);
    for (int i = 0; i <= 510; i++) {
        acc += hist[i];
        if (acc >= want)
            return (float) i / 255.0f;
    }
    return 2.0f;
}

/* one frame (x0, y0, w, h) of a premultiplied RGBA picture: its second
 * differences into hist[axis * 2 + (step 1 / 2)], its border into *edge */
static void soft_measure (guint (*hist)[511], float *edge, const guint8 *px, gsize stride, int x0,
                          int y0, int w, int h)
{
    const guint8 *f = px + (gsize) y0 * stride + (gsize) x0 * 4;
    for (int ax = 0; ax < 2; ax++) {
        soft_d2 (f, stride, w, h, 1, ax, hist[ax * 2]);
        soft_d2 (f, stride, w, h, 2, ax, hist[ax * 2 + 1]);
    }
    int e = 0;
    for (int x = 0; x < w * 4; x++) {
        e = MAX (e, f[x]);
        e = MAX (e, f[(gsize) (h - 1) * stride + x]);
    }
    for (int y = 0; y < h; y++)
        for (int c = 0; c < 4; c++) {
            e = MAX (e, f[(gsize) y * stride + c]);
            e = MAX (e, f[(gsize) y * stride + (gsize) (w - 1) * 4 + c]);
        }
    *edge = MAX (*edge, (float) e / 255.0f);
}

/* the four counts of soft_measure () alone (no GL; the unit test's entry) */
void walpe_weparticles_soft_hist (const guint8 *px, gsize stride, int w, int h, guint (*hist)[511])
{
    float edge = 0;
    if (px && hist && w > 0 && h > 0 && stride >= (gsize) w * 4)
        soft_measure (hist, &edge, px, stride, 0, 0, w, h);
}

static void soft_finish (SoftInfo *si, guint (*hist)[511])
{
    for (int ax = 0; ax < 2; ax++) {
        si->d1[ax] = soft_pct (hist[ax * 2]);
        si->d2[ax] = soft_pct (hist[ax * 2 + 1]);
    }
}

static void soft_log (const WeSys *s)
{
    const SoftInfo *si = &s->soft;
    g_debug ("weparticles: %s: detail x %.3f/%.3f y %.3f/%.3f edge %.3f (frame %dx%d)%s",
             s->tex_path, si->d1[0], si->d2[0], si->d1[1], si->d2[1], si->edge, si->fw, si->fh,
             si->ok ? "" : " - not measured");
}

/* the sprite picture as loaded (straight alpha, 4 bytes a pixel) -> how
 * smooth it is, over all its sheet frames. FALSE: not a picture this
 * looks at (nothing measured, nothing logged). */
static gboolean soft_analyze (WeSys *s, const WalpeTex *t)
{
    SoftInfo *si = &s->soft;
    memset (si, 0, sizeof (*si));
    if (s->refract || !t->data || t->format != WALPE_TEX_RGBA8 || t->channels != 0 ||
        t->width < 4 || t->height < 4 || (gint64) t->width * t->height > 4096 * 4096 ||
        t->len < (gsize) t->width * t->height * 4)
        return FALSE;
    gsize n = (gsize) t->width * t->height, stride = (gsize) t->width * 4;
    guint8 *pm = g_malloc (n * 4);
    for (gsize i = 0; i < n; i++) {
        guint a = t->data[i * 4 + 3];
        pm[i * 4 + 0] = (guint8) ((t->data[i * 4 + 0] * a + 127) / 255);
        pm[i * 4 + 1] = (guint8) ((t->data[i * 4 + 1] * a + 127) / 255);
        pm[i * 4 + 2] = (guint8) ((t->data[i * 4 + 2] * a + 127) / 255);
        pm[i * 4 + 3] = (guint8) a;
    }
    si->ok = TRUE;
    guint (*hist)[511] = g_malloc0 (sizeof (guint[4][511]));
    if (t->nframes > 1 && t->frames) {
        si->fw = si->fh = 0;
        for (int f = 0; f < t->nframes && si->ok; f++) {
            const WalpeTexFrame *fr = &t->frames[f];
            /* The rectangle comes from the file as four floats. They are
             * checked as floats, each against the picture, before they are
             * made integers: x = w1 = 2147483520 passed "x0 + w > width"
             * (the sum wraps to -256) and the measuring below read 8 GiB
             * past the picture - a 16 x 16 sprite sheet took the daemon
             * down when its wallpaper was opened. */
            float tw = (float) t->width, th = (float) t->height;
            gboolean inside = fr->x >= 0 && fr->x <= tw && fr->w1 >= 4 && fr->w1 <= tw &&
                              fr->y >= 0 && fr->y <= th && fr->h1 >= 4 && fr->h1 <= th;
            int x0 = inside ? (int) fr->x : 0, y0 = inside ? (int) fr->y : 0;
            int w = inside ? (int) fr->w1 : 0, h = inside ? (int) fr->h1 : 0;
            /* turned or mirrored frames: not measured, never reduced */
            if (fr->w2 != 0 || fr->h2 != 0 || !inside || x0 + w > t->width || y0 + h > t->height) {
                si->ok = FALSE;
                break;
            }
            soft_measure (hist, &si->edge, pm, stride, x0, y0, w, h);
            si->fw = si->fw ? MIN (si->fw, w) : w;
            si->fh = si->fh ? MIN (si->fh, h) : h;
        }
    } else {
        soft_measure (hist, &si->edge, pm, stride, 0, 0, t->width, t->height);
        si->fw = t->width;
        si->fh = t->height;
    }
    soft_finish (si, hist);
    g_free (hist);
    g_free (pm);
    soft_log (s);
    return TRUE;
}

/* The measurement of a sprite file, kept for the next system that draws
 * with the same file: six systems of one object, the child systems of a
 * burst, the other monitor's copy of the scene and the scene loaded again
 * all measured the same picture again, each with a decode of its own.
 * The result is a function of the file's bytes alone, so it is kept by
 * path and given out only while the file is the one that was measured
 * (device, inode, size, both times). Any thread (render threads, at
 * load). */
typedef struct {
    dev_t dev;
    ino_t ino;
    goffset size;
    struct timespec mtime, ctime;
    SoftInfo soft;
    gboolean looked;               /* soft_analyze () took the picture (and logged it) */
} SoftKept;

static GMutex soft_kept_lock;
static GHashTable *soft_kept;      /* file path -> SoftKept */
static gint soft_n_measured, soft_n_kept;
#define SOFT_KEPT_MAX 512          /* files; all are dropped when it is reached */

static gboolean soft_kept_stat (const char *path, SoftKept *k)
{
    struct stat st;
    if (stat (path, &st) != 0 || !S_ISREG (st.st_mode))
        return FALSE;
    memset (k, 0, sizeof (*k));
    k->dev = st.st_dev;
    k->ino = st.st_ino;
    k->size = st.st_size;
    k->mtime = st.st_mtim;
    k->ctime = st.st_ctim;
    return TRUE;
}

static gboolean soft_kept_same (const SoftKept *a, const SoftKept *b)
{
    return a->dev == b->dev && a->ino == b->ino && a->size == b->size &&
           a->mtime.tv_sec == b->mtime.tv_sec && a->mtime.tv_nsec == b->mtime.tv_nsec &&
           a->ctime.tv_sec == b->ctime.tv_sec && a->ctime.tv_nsec == b->ctime.tv_nsec;
}

/* the system's sprite: measured (the file is decoded for it), or what an
 * earlier measurement of the same file gave */
static void soft_for_sprite (WeSys *s)
{
    SoftKept now;
    gboolean have = soft_kept_stat (s->tex_path, &now);
    if (have) {
        g_mutex_lock (&soft_kept_lock);
        SoftKept *k = soft_kept ? g_hash_table_lookup (soft_kept, s->tex_path) : NULL;
        gboolean hit = k && soft_kept_same (k, &now), looked = FALSE;
        if (hit) {
            s->soft = k->soft;
            looked = k->looked;
        }
        g_mutex_unlock (&soft_kept_lock);
        if (hit) {
            g_atomic_int_inc (&soft_n_kept);
            if (looked)
                soft_log (s);   /* its line, as when it was measured */
            return;
        }
    }
    /* the reduced-resolution layer's sprite analysis reads the decoded
     * picture; the cached texture load hands out no pixels, so the file is
     * decoded once more for it, at load time only */
    WalpeTex *at = walpe_tex_load_ex (s->tex_path, WALPE_TEX_LOAD_WE_COLOR);
    if (!at)
        return;
    gboolean looked = soft_analyze (s, at);
    walpe_tex_free (at);
    g_atomic_int_inc (&soft_n_measured);
    /* kept only when the file did not change while it was read */
    SoftKept after;
    if (!have || !soft_kept_stat (s->tex_path, &after) || !soft_kept_same (&now, &after))
        return;
    now.soft = s->soft;
    now.looked = looked;
    g_mutex_lock (&soft_kept_lock);
    if (!soft_kept)
        soft_kept = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
    if (g_hash_table_size (soft_kept) >= SOFT_KEPT_MAX)
        g_hash_table_remove_all (soft_kept);
    g_hash_table_replace (soft_kept, g_strdup (s->tex_path), g_memdup2 (&now, sizeof (now)));
    g_mutex_unlock (&soft_kept_lock);
}

void walpe_weparticles_soft_stats (guint *measured, guint *kept)
{
    if (measured)
        *measured = (guint) g_atomic_int_get (&soft_n_measured);
    if (kept)
        *kept = (guint) g_atomic_int_get (&soft_n_kept);
}

/* sprite sheet: the frames' uv rectangles as a float texture the vertex
 * shader reads (texelFetch) - frame f is two texels, (u0, v0, s axis) and
 * (t axis), at column f % FRAME_ROW of rows 2 * (f / FRAME_ROW) and + 1.
 * The values are the ones frame_rect computed per particle before. */
static void frame_table (WeSys *s)
{
    if (s->nframes <= 1 || s->tex_w < 1 || s->tex_h < 1)
        return;
    int row = MIN (s->nframes, FRAME_ROW), rows = (s->nframes + row - 1) / row;
    float *px = g_new0 (float, (gsize) row * rows * 2 * 4);
    for (int f = 0; f < s->nframes; f++) {
        float uv[6];
        frame_rect (s, f, uv);
        float *a = px + ((gsize) (2 * (f / row)) * row + (gsize) (f % row)) * 4;
        float *b = a + (gsize) row * 4;
        a[0] = uv[0]; a[1] = uv[1]; a[2] = uv[2]; a[3] = uv[3];
        b[0] = uv[4]; b[1] = uv[5];
    }
    glGenTextures (1, &s->ftex);
    glBindTexture (GL_TEXTURE_2D, s->ftex);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA32F, row, rows * 2, 0, GL_RGBA, GL_FLOAT, px);
    g_free (px);
    s->frow = row;
}

/* WALPE_TEX_DXT_SPRITES=0: DXT sprites are decoded to RGBA8 and
 * premultiplied at upload, as before */
static gboolean sprite_dxt (void)
{
    static WalpeLazy on_cell = WALPE_LAZY_UNSET;   /* read once (common/lazy.h) */
    int on = walpe_lazy_get (&on_cell);
    if (on < 0) {
        const char *e = g_getenv ("WALPE_TEX_DXT_SPRITES");
        on = walpe_lazy_set (&on_cell, walpe_gl_has_s3tc () && !g_getenv ("WALPE_NO_DXT") &&
                                       !(e && *e == '0'));
    }
    return on;
}

static void sys_gl_init (WeSys *s)
{
    if (s->tex_path) {
        /* color sprites: R8/RG88 as WE's genericparticle reads them
         * (white + alpha / luminance + alpha); refract sprites stay
         * straight alpha (the refract shader premultiplies).
         * Through the shared texture cache: six systems (or child systems,
         * or two monitors) with one sprite file are one decode and one GL
         * texture, where each had its own. A DXT sprite stays block
         * compressed (straight alpha: the shader premultiplies, u_straight);
         * an R8/RG88 one takes one or two bytes per texel on the GPU. */
        /* how smooth the sprite is (the reduced-resolution layer), once
         * per file */
        if (!s->refract)
            soft_for_sprite (s);
        WalpeTexInfo ti;
        WalpeTexFrame *frames = NULL;
        s->tex = walpe_gl_tex_cached_frames (s->tex_path, WALPE_TEX_LOAD_WE_COLOR |
                                             (sprite_dxt () ? WALPE_TEX_LOAD_NATIVE_DXT : 0),
                                             GL_CLAMP_TO_EDGE, !s->refract, &ti, &frames);
        if (s->tex) {
            /* the chain a sprite's file calls for: sprites are drawn at any size */
            if (ti.want_mips)
                walpe_gl_tex_ensure_mipmaps (s->tex);
            s->tex_premult = ti.premult;
            s->tex_w = ti.width;
            s->tex_h = ti.height;
            s->aspect = ti.nframes > 0 && ti.frame_w > 0
                ? (float) ti.frame_h / (float) ti.frame_w
                : (float) ti.height / (float) MAX (1, ti.width);
            s->aspect = CLAMP (s->aspect, 0.05f, 20.0f);
            if (ti.nframes > 0 && frames) {
                s->frames = g_steal_pointer (&frames);
                s->nframes = ti.nframes;
                g_debug ("weparticles: sprite sheet %s: %d frames (%s)", s->tex_path,
                         s->nframes, s->anim == AN_RANDOM ? "randomframe"
                         : s->anim == AN_ONCE ? "once" : "sequence");
            }
        }
        g_free (frames);
    }
    frame_table (s);
    if (s->refract && s->ntex_path) {
        /* normal map: raw channels; layout per TEX1FORMAT (RG88 kept
         * native reads (R, G, 0, 1): its decode takes .gr) */
        WalpeTexInfo ni;
        s->ntex = walpe_gl_tex_cached (s->ntex_path, WALPE_TEX_LOAD_NATIVE_RG |
                                       (sprite_dxt () ? WALPE_TEX_LOAD_NATIVE_DXT : 0),
                                       GL_CLAMP_TO_EDGE, FALSE, &ni);
        if (s->ntex) {
            s->nfmt = ni.we_format >= 3 && ni.we_format <= 7 ? 0 : ni.we_format == 8 ? 1 : 2;
            if (ni.want_mips)
                walpe_gl_tex_ensure_mipmaps (s->ntex);
        }
    }
    /* REFRACT with slot 1 null stays on: genericparticle multiplies the
     * sprite by _rt_FullFrameBuffer without an offset (1718945416's embers
     * are scene-coloured glints; as plain sprites they were flat pink) */
    for (int c = 0; c < s->nchild; c++)
        sys_gl_init (s->child[c].sys);
}

void walpe_we_particle_override_default (WalpeWeParticleOverride *ov)
{
    memset (ov, 0, sizeof (*ov));
    ov->rate = ov->count = ov->size = ov->lifetime = ov->alpha = ov->speed = 1;
    ov->color[0] = ov->color[1] = ov->color[2] = 1;
}

static void sys_set_override (WeSys *s, const WalpeWeParticleOverride *ov, guint scripted, int depth);

WalpeWeParticles *walpe_weparticles_new (JsonObject *sys, const char *dir, const char *we_assets,
                                         const WalpeWeParticleOverride *ov)
{
    WalpeWeParticleOverride d;
    if (!ov) {
        walpe_we_particle_override_default (&d);
        ov = &d;
    }
    ParseCx cx = { 0 };
    WeSys *root = sys_new (sys, dir, we_assets, ov, 0, &cx);
    if (!root)
        return NULL;
    if (WALPE_WE_STATIC_OVERRIDES_AS_WE)
        sys_set_override (root, ov, 0, 0);   /* 0 is a value there */
    WalpeWeParticles *p = g_new0 (WalpeWeParticles, 1);
    p->root = root;
    p->audio_level = 1.0f;
    p->wm[0] = p->wm[3] = 1.0f;
    p->wsz = 1.0f;
    p->pdist = PERSP_DIST_DEFAULT;
    return p;
}

void walpe_weparticles_set_world (WalpeWeParticles *p, const float *m)
{
    /* m: column-major mat3 of the object's world transform (scene units) */
    if (!p || !isfinite (m[0] * m[4] - m[3] * m[1]))
        return;
    p->wm[0] = m[0]; p->wm[1] = m[1];
    p->wm[2] = m[3]; p->wm[3] = m[4];
    p->wm[4] = m[6]; p->wm[5] = m[7];
}

void walpe_weparticles_set_view (WalpeWeParticles *p, const float *scene_to_clip, float cx,
                                 float cy, float origin_z, float scale_z, float persp_dist)
{
    if (!p)
        return;
    if (pt_camera_fixed ())
        p->pdist = PERSP_DIST_FIXED;
    else if (isfinite (persp_dist) && persp_dist > 1.0f)
        p->pdist = persp_dist;
    memcpy (p->clip, scene_to_clip, sizeof (p->clip));
    p->pc[0] = cx;
    p->pc[1] = cy;
    p->wz = isfinite (origin_z) ? origin_z : 0.0f;
    p->wsz = isfinite (scale_z) && scale_z != 0 ? scale_z : 1.0f;
    p->have_view = TRUE;
}

static void prewarm (WeSys *s, float secs)
{
    const double step = 1.0 / 30.0;
    for (double t = 0; t < secs; t += step)
        sys_update (s, step, 1.0f, NULL);
}

void walpe_weparticles_gl_init (WalpeWeParticles *p)
{
    thread_gl_init ();
    sys_gl_init (p->root);
    pt_users++;
    p->gl_ready = TRUE;
    p->need_prewarm = p->root->starttime > 0;
}

/* Which frames of an n-frame sheet a particle shows at life fraction
 * `life` (0..1): *f0 = the current frame, *f1 = the one it is moving to,
 * return = how far (0..1, 0 = f0 only).
 * WE stretches the animation over the particle's lifetime ("The entire
 * particle animation will always be stretched out across the lifetime of
 * each individual particle", x sequencemultiplier) and blends neighbouring
 * frames unless the system has "no frame blending". common_particles.h,
 * ComputeSpriteFrame (called with frac (lifetime)):
 *     currentFrame = floor (lifetime * numFrames);
 *     nextFrame = min (numFrames - 1, currentFrame + 1);
 *     frameBlend = frac (lifetime * numFrames);
 * so the last frame is held, never blended back into frame 0. Without the
 * blend WE's own smoke preset (64 frames, sequencemultiplier 2, 3-4 s)
 * changes 32-43 times a second and its fog preset (64 frames, 3-5 s)
 * 13-21 times, however fast the wallpaper renders: the stepping reported
 * as "smoke and fog are 30 fps". */
float walpe_weparticles_sheet_frames (int n, int mode, float seqmult, float life,
                                      int random_frame, gboolean blend, int *f0, int *f1)
{
    *f0 = *f1 = 0;
    if (n <= 1)
        return 0.0f;
    if (mode == AN_RANDOM) {
        *f0 = *f1 = CLAMP (random_frame, 0, n - 1);
        return 0.0f;
    }
    if (!(seqmult > 0) || !isfinite (seqmult))
        seqmult = 1.0f;
    life = life >= 0.0f ? MIN (life, 1.0f) : 0.0f;   /* NaN -> 0 */
    float x;                   /* position in frames */
    if (mode == AN_ONCE) {
        x = MIN (life * seqmult, 0.9999f) * (float) n;
    } else {
        float f = life * seqmult;
        x = (f - floorf (f)) * (float) n;
    }
    int i = CLAMP ((int) x, 0, n - 1);
    *f0 = i;
    *f1 = MIN (n - 1, i + 1);
    if (!blend || *f1 == i)
        return 0.0f;
    return CLAMP (x - (float) i, 0.0f, 1.0f);
}

static void frame_rect (const WeSys *s, int fi, float uv[6])
{
    const WalpeTexFrame *fr = &s->frames[fi];
    float W = (float) s->tex_w, H = (float) s->tex_h;
    uv[0] = fr->x / W;
    uv[1] = fr->y / H;
    uv[2] = fr->w1 / W;
    uv[3] = fr->w2 / H;
    uv[4] = fr->h2 / W;
    uv[5] = fr->h1 / H;
}

/* the two sheet frames a particle shows (*f0, and *f1 it blends towards:
 * the same frame when there is no blend); return = the blend (0 = none) */
static float frame_idx (WeSys *s, const Pt *p, int *f0, int *f1)
{
    float t = walpe_weparticles_sheet_frames (s->nframes, s->anim, s->seqmult, life_frac (p),
                                              (int) p->frame, !(s->flags & 2), f0, f1);
    if (!(t > 0.0f))
        *f1 = *f0;
    return t;
}

/* this thread's scratch memory for one system's vertex data (contents do
 * not outlive the draw) */
static float *scratch (gsize floats)
{
    if (pt_scratch_cap < floats) {
        pt_scratch_cap = MAX (floats, pt_scratch_cap * 2);
        g_free (pt_scratch);
        pt_scratch = g_new (float, pt_scratch_cap);
    }
    return pt_scratch;
}

static void *tmp_bytes (gsize bytes)
{
    if (pt_tmp_cap < bytes) {
        pt_tmp_cap = MAX (bytes, pt_tmp_cap * 2);
        g_free (pt_tmp);
        pt_tmp = g_malloc (pt_tmp_cap);
    }
    return pt_tmp;
}

/* model-space point -> where it is drawn. Ordinary systems: unchanged
 * (the object's matrix is in u_mvp). Perspective systems: world position,
 * then a perspective camera on the view centre at w->pdist whose z = 0
 * plane equals the orthographic scene; *f = the magnification there.
 * FALSE = at or behind the camera. */
static gboolean pt_project (const WalpeWeParticles *w, const WeSys *s, const float m[3],
                            float out[2], float *f)
{
    if (!s->persp || !w->have_view) {
        out[0] = m[0];
        out[1] = m[1];
        *f = 1.0f;
        return TRUE;
    }
    float wx = w->wm[0] * m[0] + w->wm[2] * m[1] + w->wm[4];
    float wy = w->wm[1] * m[0] + w->wm[3] * m[1] + w->wm[5];
    float dz = w->pdist - (w->wz + m[2] * w->wsz);
    if (dz < w->pdist * PERSP_NEAR)
        return FALSE;
    *f = w->pdist / dz;
    out[0] = w->pc[0] + (wx - w->pc[0]) * *f;
    out[1] = w->pc[1] + (wy - w->pc[1]) * *f;
    return TRUE;
}

/* a sprite axis -> drawn space. The axis is in model space, or - for a
 * "world space" system (flag 1) - already in scene units: WE keeps those
 * particles in the world, where the object's scale and rotation do not
 * reach the sprite (its "Rain splashes" preview narrows the emitter with
 * scale 0.275 1 1 and the rings stay round).
 * Drawn space is the model (the object's matrix is in u_mvp) or, for
 * perspective systems, the scene (points are projected on the CPU). */
/* The four cases are told apart once per system (ax_mode), not once per
 * axis: the sprite loop of sys_draw calls ax_apply twice per particle. */
enum { AX_NONE, AX_WINV, AX_F, AX_WORLD_F };
static inline int ax_mode (const WalpeWeParticles *w, const WeSys *s)
{
    gboolean ws = pt_draws_in_world (s->flags);
    if (!s->persp || !w->have_view)
        return ws ? AX_WINV : AX_NONE;
    return ws ? AX_F : AX_WORLD_F;
}

static inline void ax_apply (int mode, const WalpeWeParticles *w, const WeSys *s, float f,
                             float a[2])
{
    if (mode == AX_WINV) {
        /* scene -> model: the inverse of the object's 2x2 */
        float x = s->winv[0] * a[0] + s->winv[2] * a[1];
        float y = s->winv[1] * a[0] + s->winv[3] * a[1];
        a[0] = x;
        a[1] = y;
    } else if (mode == AX_F) {
        a[0] *= f;
        a[1] *= f;
    } else if (mode == AX_WORLD_F) {
        float x = (w->wm[0] * a[0] + w->wm[2] * a[1]) * f;
        float y = (w->wm[1] * a[0] + w->wm[3] * a[1]) * f;
        a[0] = x;
        a[1] = y;
    }
}

/* v_ScreenTangents of ComputeScreenRefractionTangents: the quad's right
 * and up axes on screen, normalized: (right.x, up.x, right.y, up.y) */
static void screen_tangents (const WalpeWeParticles *w, const WeSys *s, const float hx[2],
                             const float hy[2], float tan[4])
{
    float r[2] = { hx[0], hx[1] }, u[2] = { hy[0], hy[1] };
    if (!s->persp || !w->have_view) {
        r[0] = w->wm[0] * hx[0] + w->wm[2] * hx[1];
        r[1] = w->wm[1] * hx[0] + w->wm[3] * hx[1];
        u[0] = w->wm[0] * hy[0] + w->wm[2] * hy[1];
        u[1] = w->wm[1] * hy[0] + w->wm[3] * hy[1];
    }
    float rl = hypotf (r[0], r[1]), ul = hypotf (u[0], u[1]);
    if (rl < 1e-9f || ul < 1e-9f) {
        tan[0] = 1; tan[1] = 0; tan[2] = 0; tan[3] = 1;
        return;
    }
    tan[0] = r[0] / rl;
    tan[1] = u[0] / ul;
    tan[2] = r[1] / rl;
    tan[3] = u[1] / ul;
}

static inline float *put_vert (float *o, float x, float y, float u, float v, const float rgba[4])
{
    o[0] = x; o[1] = y; o[2] = u; o[3] = v;
    memcpy (o + 4, rgba, sizeof (float) * 4);
    return o + RIB_FLOATS;
}

static int seq_cmp (gconstpointer a, gconstpointer b, gpointer u)
{
    const Pt *pts = u;
    guint sa = pts[*(const int *) a].seq, sb = pts[*(const int *) b].seq;
    return sa < sb ? -1 : sa > sb;
}

/* one ribbon through n points (drawn space) with per-point half width,
 * colour and v coordinate: genericropeparticle's strip - both edges offset
 * by the size along the normal of the local direction */
static float *put_ribbon (float *o, int n, const float (*pt)[2], const float *hw,
                          const float (*rgba)[4], const float *vc)
{
    float nx0 = 0, ny0 = 0;
    for (int k = 0; k < n; k++) {
        /* direction: neighbours on both sides where they exist */
        int a = MAX (k - 1, 0), b = MIN (k + 1, n - 1);
        float tx = pt[b][0] - pt[a][0], ty = pt[b][1] - pt[a][1];
        float tl = hypotf (tx, ty);
        float nx = tl > 1e-6f ? ty / tl : nx0, ny = tl > 1e-6f ? -tx / tl : ny0;
        if (k > 0) {
            float ax = pt[k - 1][0], ay = pt[k - 1][1], bx = pt[k][0], by = pt[k][1];
            float h0 = hw[k - 1], h1 = hw[k];
            o = put_vert (o, ax - nx0 * h0, ay - ny0 * h0, 0, vc[k - 1], rgba[k - 1]);
            o = put_vert (o, ax + nx0 * h0, ay + ny0 * h0, 1, vc[k - 1], rgba[k - 1]);
            o = put_vert (o, bx - nx * h1, by - ny * h1, 0, vc[k], rgba[k]);
            o = put_vert (o, ax + nx0 * h0, ay + ny0 * h0, 1, vc[k - 1], rgba[k - 1]);
            o = put_vert (o, bx + nx * h1, by + ny * h1, 1, vc[k], rgba[k]);
            o = put_vert (o, bx - nx * h1, by - ny * h1, 0, vc[k], rgba[k]);
        }
        nx0 = nx;
        ny0 = ny;
    }
    return o;
}

/* GL state of one walpe_weparticles_render call: what is already set is
 * not set again for the next system of the object (other draws come
 * between two objects, so nothing is assumed across calls) */
enum { BL_NONE = -1, BL_NORMAL, BL_ADD, BL_ADD_LAYER };
typedef struct {
    WalpeWeParticles *w;
    float alpha;
    const float *tint;
    PtProg *prog;              /* program in use */
    const float *mvp[2];       /* the matrix each program has: [0] quads, [1] ribbons */
    GLuint vao, tex0;
    int blend;                 /* BL_*: the blend function set */
} DrawCtx;

/* ---- the reduced-resolution layer (PtLayer) ---- */

static void rc_forget (void);   /* (the copy of the frame refracting systems read: below) */

static void lr_unbind (void)
{
    if (!lr.bound)
        return;
    glBindFramebuffer (GL_FRAMEBUFFER, lr.target);
    glViewport (0, 0, lr.tw, lr.th);
    lr.bound = FALSE;
}

/* the layer over the frame, where it has content; d = the render call
 * this happens in (its cached state is brought up to date) or NULL */
static void lr_flush (DrawCtx *d)
{
    if (!lr.pending)
        return;
    lr_unbind ();
    lr.pending = FALSE;
    /* one rectangle per run of tiles in a row */
    float v[LR_TY * (LR_TX / 2 + 1) * 12], *o = v;
    int ntiles = 0;
    for (int j = 0; j < LR_TY; j++) {
        guint32 row = lr.tiles[j];
        lr.tiles[j] = 0;
        for (int i = 0; i < LR_TX; i++) {
            if (!(row >> i & 1))
                continue;
            int i1 = i;
            while (i1 + 1 < LR_TX && (row >> (i1 + 1) & 1))
                i1++;
            float x0 = -1.0f + 2.0f * (float) i / LR_TX, x1 = -1.0f + 2.0f * (float) (i1 + 1) / LR_TX;
            float y0 = -1.0f + 2.0f * (float) j / LR_TY, y1 = -1.0f + 2.0f * (float) (j + 1) / LR_TY;
            const float q[12] = { x0, y0, x1, y0, x0, y1, x1, y0, x1, y1, x0, y1 };
            memcpy (o, q, sizeof (q));
            o += 12;
            ntiles += i1 - i + 1;
            i = i1;
        }
    }
    if (o == v)
        return;
    rc_forget ();   /* the layer goes onto the frame */
    glEnable (GL_BLEND);
    glBlendFunc (GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram (pt_blit.prog);
    /* view pixel x (clip cx) is layer texel x / F + 1 of lw (the margin) */
    float kx = (float) lr.tw / ((float) lr.f * (float) lr.lw);
    float ky = (float) lr.th / ((float) lr.f * (float) lr.lh);
    glUniform4f (pt_blit.map, 0.5f * kx, 0.5f * ky, 0.5f * kx + 1.0f / (float) lr.lw,
                 0.5f * ky + 1.0f / (float) lr.lh);
    glActiveTexture (GL_TEXTURE0);
    glBindTexture (GL_TEXTURE_2D, lr.tex);
    glBindVertexArray (pt_bvao);
    glBindBuffer (GL_ARRAY_BUFFER, pt_stream);
    glBufferData (GL_ARRAY_BUFFER, (GLsizeiptr) ((gsize) (o - v) * sizeof (float)), v, GL_STREAM_DRAW);
    /* "Skip hidden areas" (clip.h): the layer goes onto the visible boxes
     * only - unless a system in it was drawn while the clip was suspended
     * or off (a later layer reads the frame under it: all of it is needed) */
    if (lr.unclipped) {
        glDrawArrays (GL_TRIANGLES, 0, (GLsizei) ((o - v) / 2));
    } else {
        for (int it = 0; walpe_clip_next (&it, NULL);)
            glDrawArrays (GL_TRIANGLES, 0, (GLsizei) ((o - v) / 2));
    }
    lr.unclipped = FALSE;
    if (stats_on ()) {
        double a = (double) ntiles / (LR_TX * LR_TY);
        pt_stats.draws++;
        pt_stats.uploads++;
        pt_stats.composites++;
        pt_stats.verts += (guint64) ((o - v) / 2);
        pt_stats.bytes += (guint64) ((o - v) * sizeof (float));
        pt_stats.shaded += a;
    }
    if (d) {
        d->prog = NULL;
        d->vao = pt_bvao;
        d->tex0 = lr.tex;
        d->blend = BL_NORMAL;
    } else {
        glBindVertexArray (0);
    }
}

static void lr_release (void)
{
    if (lr.fbo)
        glDeleteFramebuffers (1, &lr.fbo);
    if (lr.tex)
        glDeleteTextures (1, &lr.tex);
    lr.fbo = lr.tex = 0;
    lr.f = lr.lw = lr.lh = 0;
    lr.pending = lr.bound = FALSE;
    memset (lr.tiles, 0, sizeof (lr.tiles));
}

/* make the layer the draw target for a system drawn at 1 / f; FALSE = no
 * layer (the system is then drawn directly). d->tex0 is kept valid. */
static gboolean lr_bind (DrawCtx *d, int f)
{
    if (lr.pending && lr.f != f)
        lr_flush (d);
    int lw = (lr.tw + f - 1) / f + 2, lh = (lr.th + f - 1) / f + 2;
    if (!lr.fbo || lr.f != f || lr.lw != lw || lr.lh != lh) {
        lr_release ();
        glGenTextures (1, &lr.tex);
        glActiveTexture (GL_TEXTURE0);
        glBindTexture (GL_TEXTURE_2D, lr.tex);
        d->tex0 = lr.tex;
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        /* 8 bits like the frame: additive systems then round exactly as
         * they do on the frame itself (a half-float buffer was tried: no
         * closer to the direct picture, whose own rounding of every blend
         * is what remains) */
        glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, lw, lh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glGenFramebuffers (1, &lr.fbo);
        glBindFramebuffer (GL_FRAMEBUFFER, lr.fbo);
        glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, lr.tex, 0);
        gboolean ok = glCheckFramebufferStatus (GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        glBindFramebuffer (GL_FRAMEBUFFER, lr.target);
        if (!ok) {
            g_message ("weparticles: no reduced-resolution layer on this GL (framebuffer "
                       "incomplete): large soft particles are drawn at full size");
            lr_release ();
            lr.broken = TRUE;
            return FALSE;
        }
        lr.f = f;
        lr.lw = lw;
        lr.lh = lh;
    }
    if (!lr.bound) {
        glBindFramebuffer (GL_FRAMEBUFFER, lr.fbo);
        glViewport (0, 0, lr.lw, lr.lh);
        lr.bound = TRUE;
    }
    if (!lr.pending) {
        glClearColor (0.0f, 0.0f, 0.0f, 0.0f);
        glClear (GL_COLOR_BUFFER_BIT);
        lr.pending = TRUE;
        lr.has_over = lr.has_add = FALSE;
    }
    return TRUE;
}

/* a system's matrix into the layer: the view's clip space scaled and
 * shifted to the buffer's (which is a pixel larger on every side) */
static const float *lr_matrix (WalpeWeParticles *w, gboolean persp, gboolean *fresh)
{
    int k = persp ? 1 : 0;
    float *m = w->lmat[k];
    *fresh = w->lmat_f[k] != lr.f;
    if (*fresh) {
        const float *src = persp ? w->clip : w->mvp;
        float sx = (float) lr.tw / ((float) lr.f * (float) lr.lw);
        float sy = (float) lr.th / ((float) lr.f * (float) lr.lh);
        float ox = sx + 2.0f / (float) lr.lw - 1.0f, oy = sy + 2.0f / (float) lr.lh - 1.0f;
        m[0] = sx * src[0]; m[1] = sy * src[1]; m[2] = 0.0f;
        m[3] = sx * src[3]; m[4] = sy * src[4]; m[5] = 0.0f;
        m[6] = sx * src[6] + ox; m[7] = sy * src[7] + oy; m[8] = 1.0f;
        w->lmat_f[k] = lr.f;
    }
    return m;
}

/* what the build loop gathers about a system that may go into the layer */
typedef struct {
    gboolean on;
    double area;               /* on-screen area of its quads, screen areas */
    float gain;                /* largest alpha x colour of a particle */
    float lx2, ly2;            /* smallest squared half width / height of a quad, pixels */
    guint32 tiles[LR_TY];      /* filled by lr_mark_all, when lr_decide needs them */
    gboolean edges;            /* some quad's edge may be on screen */
    double st_area;            /* statistics: the exact area */
    const float *v, *M;        /* the system's records (n of rf floats) and its matrix */
    int n, rf;
} LrProbe;

/* The tiles a quad touches (clip space: centre c, half axes a and b; its
 * bounds clipped to the view are x0..y1), with room for the composite's
 * interpolation (two layer pixels at 1/4). A quad that covers the whole
 * view has no edge on screen (pr->edges stays as it is). */
/* Does the quad (clip space: centre c, half axes a and b) cover the whole
 * view? The view's four corners lie inside it. */
static inline gboolean quad_covers_view (float cx, float cy, float ax, float ay, float bx, float by)
{
    float det = ax * by - ay * bx;
    if (!(fabsf (det) > 1e-12f))
        return FALSE;
    const float inv = 1.0f / det;
    for (int k = 0; k < 4; k++) {
        float dx = ((k & 1) ? 1.0f : -1.0f) - cx, dy = ((k & 2) ? 1.0f : -1.0f) - cy;
        if (!(fabsf ((dx * by - dy * bx) * inv) <= 1.0f && fabsf ((ax * dy - ay * dx) * inv) <= 1.0f))
            return FALSE;
    }
    return TRUE;
}

static gboolean lr_mark (LrProbe *pr, float cx, float cy, float ax, float ay, float bx, float by,
                         float x0, float y0, float x1, float y1)
{
    const float mx = 18.0f / (float) lr.tw, my = 18.0f / (float) lr.th;
    int i0 = (int) floorf ((x0 - mx + 1.0f) * (LR_TX / 2.0f));
    int i1 = (int) floorf ((x1 + mx + 1.0f) * (LR_TX / 2.0f));
    int j0 = (int) floorf ((y0 - my + 1.0f) * (LR_TY / 2.0f));
    int j1 = (int) floorf ((y1 + my + 1.0f) * (LR_TY / 2.0f));
    i0 = CLAMP (i0, 0, LR_TX - 1); i1 = CLAMP (i1, 0, LR_TX - 1);
    j0 = CLAMP (j0, 0, LR_TY - 1); j1 = CLAMP (j1, 0, LR_TY - 1);
    const guint32 row = (guint32) ((((guint64) 1 << (i1 - i0 + 1)) - 1) << i0);
    float det = ax * by - ay * bx;
    if (fabsf (det) > 1e-12f) {
        /* a point p in the quad's own coordinates: (u, v) in -1..1 inside */
        const float inv = 1.0f / det;
#define LR_U(X, Y) ((((X) - cx) * by - ((Y) - cy) * bx) * inv)
#define LR_V(X, Y) ((ax * ((Y) - cy) - ay * ((X) - cx)) * inv)
        gboolean covers = TRUE;
        for (int k = 0; k < 4 && covers; k++) {
            float vx = (k & 1) ? 1.0f : -1.0f, vy = (k & 2) ? 1.0f : -1.0f;
            covers = fabsf (LR_U (vx, vy)) <= 1.0f && fabsf (LR_V (vx, vy)) <= 1.0f;
        }
        if (covers) {
            for (int j = 0; j < LR_TY; j++)
                pr->tiles[j] = 0xffffffffu;
            return TRUE;
        }
        pr->edges = TRUE;
        if ((i1 - i0 + 1) * (j1 - j0 + 1) > 6) {
            /* a large turned quad: only the tiles it really reaches (a
             * tile and the quad are apart when they are along one of the
             * quad's axes; along the view's they are not, by i0..j1) */
            for (int j = j0; j <= j1; j++) {
                float ty0 = -1.0f + 2.0f * (float) j / LR_TY - my;
                float ty1 = -1.0f + 2.0f * (float) (j + 1) / LR_TY + my;
                for (int i = i0; i <= i1; i++) {
                    float tx0 = -1.0f + 2.0f * (float) i / LR_TX - mx;
                    float tx1 = -1.0f + 2.0f * (float) (i + 1) / LR_TX + mx;
                    float u0 = LR_U (tx0, ty0), u1 = LR_U (tx1, ty0), u2 = LR_U (tx0, ty1),
                          u3 = LR_U (tx1, ty1);
                    float v0 = LR_V (tx0, ty0), v1 = LR_V (tx1, ty0), v2 = LR_V (tx0, ty1),
                          v3 = LR_V (tx1, ty1);
                    if (MAX (MAX (u0, u1), MAX (u2, u3)) < -1.0f ||
                        MIN (MIN (u0, u1), MIN (u2, u3)) > 1.0f ||
                        MAX (MAX (v0, v1), MAX (v2, v3)) < -1.0f ||
                        MIN (MIN (v0, v1), MIN (v2, v3)) > 1.0f)
                        continue;
                    pr->tiles[j] |= 1u << i;
                }
            }
            return FALSE;
        }
#undef LR_U
#undef LR_V
    }
    pr->edges = TRUE;
    for (int j = j0; j <= j1; j++)
        pr->tiles[j] |= row;
    return FALSE;
}

static float soft_err (float d1, float d2, float step)
{
    /* straight-line interpolation between samples `step` texels apart */
    return step <= 2.0f ? d1 * step / 4.0f : d2 * step / 8.0f;
}

/* Does this system go into the reduced layer this frame, and at which
 * factor (0 = drawn directly)?
 * Quality ("auto"): one layer pixel is F view pixels; a sprite whose texel
 * is m pixels wide is then sampled every F / m texels and interpolated in
 * between. The picture's second differences bound what that changes
 * (soft_err); with the frame border's value (a sprite that does not fade
 * out at its edge gets a blurred edge - unless every quad covers the
 * whole view and no edge is on screen) and the particle's alpha and
 * colour, the predicted error must stay under pt_lowres_tol / 255. The
 * layer is also never coarser than the picture: one layer pixel is at
 * most one texel of the sprite as it is drawn. Fog, smoke, glows and light shafts
 * pass at 1/4 or 1/2; stars, rain, bokeh shapes with outlines and
 * anything drawn near its texture's resolution do not.
 * "all" also takes whatever is drawn large (softened visibly).
 * Cost: the layer is worth it when the blending saved (area x (1 - 1/F^2))
 * exceeds the composite of the tiles the system adds. */
/* The tiles the system's quads touch, from its records (the same numbers
 * the sprite loop culled with). Not done in that loop: most systems never
 * get this far (too small, too crisp), and for a fog of large turned
 * quads this is the expensive part of the question. */
static void lr_mark_all (LrProbe *pr)
{
    const float *M = pr->M;
    memset (pr->tiles, 0, sizeof (pr->tiles));
    const float *o = pr->v;
    for (int i = 0; i < pr->n; i++, o += pr->rf) {
        float px = M[0] * o[0] + M[3] * o[1] + M[6];
        float py = M[1] * o[0] + M[4] * o[1] + M[7];
        float hxx = M[0] * o[2] + M[3] * o[3], hxy = M[1] * o[2] + M[4] * o[3];
        float hyx = M[0] * o[4] + M[3] * o[5], hyy = M[1] * o[4] + M[4] * o[5];
        float ex = fabsf (hxx) + fabsf (hyx), ey = fabsf (hxy) + fabsf (hyy);
        float x0 = MAX (px - ex, -1.0f), x1 = MIN (px + ex, 1.0f);
        float y0 = MAX (py - ey, -1.0f), y1 = MIN (py + ey, 1.0f);
        if (!(x1 > x0 && y1 > y0))
            continue;
        if (lr_mark (pr, px, py, hxx, hxy, hyx, hyy, x0, y0, x1, y1))
            return;   /* every tile */
        if ((x1 - x0) * (y1 - y0) > 0.25f) {
            /* large quads fill the grid quickly: stop when it is full */
            guint32 all = 0xffffffffu;
            for (int j = 0; j < LR_TY; j++)
                all &= pr->tiles[j];
            if (all == 0xffffffffu)
                return;
        }
    }
}

static int lr_decide (WeSys *s, LrProbe *pr)
{
    int mode = lowres_mode ();
    if (!pr->on || mode == WALPE_PT_LOWRES_OFF || !(pr->area > 0.0))
        return 0;
    const SoftInfo *si = s->tex ? &s->soft : &pt_dot_soft;
    float qx = 2.0f * sqrtf (pr->lx2), qy = 2.0f * sqrtf (pr->ly2);   /* smallest quad, pixels */
    int f = 0;
    if (si->ok && si->fw > 0 && si->fh > 0) {
        float mx = qx / (float) si->fw, my = qy / (float) si->fh;       /* pixels per texel */
        for (int k = 4; k >= 2 && !f; k /= 2) {
            float sx = (float) k / MAX (mx, 1e-6f), sy = (float) k / MAX (my, 1e-6f);
            /* never coarser than the picture itself: a layer pixel is at
             * most one texel of the sprite as drawn */
            if (sx > 1.0f || sy > 1.0f)
                continue;
            float e = soft_err (si->d1[0], si->d2[0], sx) + soft_err (si->d1[1], si->d2[1], sy) +
                      (pr->edges ? si->edge * 0.5f : 0.0f);
            s->st_err = e * pr->gain * 255.0f;
            if (e * pr->gain * 255.0f <= pt_lowres_tol)
                f = k;
        }
    }
    if (!f && mode == WALPE_PT_LOWRES_ALL && qx >= 48.0f && qy >= 48.0f)
        f = 2;
    if (!f)
        return 0;
    /* one factor for the frame where possible (two would be two composites) */
    if (lr.hint_f && lr.hint_f < f)
        f = lr.hint_f;
    if (lr.pending && lr.f < f)
        f = lr.f;
    /* in the layer last frame: stay unless it costs; else come in when it
     * saves a quarter of a screen */
    const double want = s->lr_f ? 0.0 : 0.25;
    double best = pr->area * (1.0 - 1.0 / (double) (f * f));   /* no tile to add */
    if (best < want) {
        s->st_saved = (float) best;
        return 0;
    }
    lr_mark_all (pr);
    int added = 0;
    gboolean join = lr.pending && lr.f == f;
    for (int j = 0; j < LR_TY; j++)
        added += __builtin_popcount (join ? pr->tiles[j] & ~lr.tiles[j] : pr->tiles[j]);
    double saved = pr->area * (1.0 - 1.0 / (double) (f * f)) - (double) added / (LR_TX * LR_TY);
    s->st_saved = (float) saved;
    return saved >= want ? f : 0;
}

enum { SUB_QUADS, SUB_RIBBON, SUB_EXPANDED };

/* ---- what the copy of the frame holds (refracting systems, pt_scene) ----
 * Every refracting system used to copy the part of the frame it can read
 * right before its draw, also when the system before it had just copied
 * the same pixels: a row of splash or droplet systems, each reading a wide
 * box around a few small sprites, copied most of the frame once per system.
 * Within a frame the copy is now kept track of:
 *   fresh  rectangles of the view copied since the last rc_forget ();
 *   dirty  rectangles of the view drawn on since (by particle systems,
 *          directly on the frame: the bounds of their quads).
 * A pixel of pt_scene inside a fresh rectangle and outside every dirty one
 * holds what the frame holds. A system that needs a rectangle copies what
 * of it lies outside one fresh rectangle (when that is one strip) and what
 * was drawn on inside, as one rectangle around both: one copy call or none
 * where there was one, and never more pixels; the bytes it then reads are
 * those a copy of the whole rectangle would have put there. Everything is
 * forgotten when something else may have drawn on the frame: a new frame,
 * the scene's other layers (walpe_weparticles_flush is called before each
 * of them), the reduced-resolution layer going onto the frame, a draw whose
 * bounds are not known (ribbons), another viewport, a new pt_scene. All in
 * view pixels, x1 / y1 exclusive. */
typedef struct { int x0, y0, x1, y1; } RcBox;
#define RC_FRESH 6
#define RC_DIRTY 12
static __thread struct {
    RcBox fresh[RC_FRESH], dirty[RC_DIRTY];
    int nfresh, ndirty;
    int vw, vh;         /* the view the rectangles are in */
} rc;

static void rc_forget (void)
{
    rc.nfresh = rc.ndirty = 0;
}

static inline gint64 rc_area (const RcBox *b)
{
    return b->x1 > b->x0 && b->y1 > b->y0 ? (gint64) (b->x1 - b->x0) * (b->y1 - b->y0) : 0;
}

static inline gboolean rc_inside (const RcBox *a, const RcBox *b)
{
    return a->x0 >= b->x0 && a->y0 >= b->y0 && a->x1 <= b->x1 && a->y1 <= b->y1;
}

static inline RcBox rc_common (const RcBox *a, const RcBox *b)
{
    RcBox r = { MAX (a->x0, b->x0), MAX (a->y0, b->y0), MIN (a->x1, b->x1), MIN (a->y1, b->y1) };
    return r;
}

/* n without f, when that is nothing (TRUE, *rest empty) or one rectangle
 * (TRUE, *rest); FALSE when it is more than one or all of n */
static gboolean rc_minus (const RcBox *n, const RcBox *f, RcBox *rest)
{
    *rest = *n;
    if (f->x0 <= n->x0 && f->x1 >= n->x1) {
        if (f->y0 <= n->y0 && f->y1 > n->y0) {
            rest->y0 = MIN (f->y1, n->y1);
            return TRUE;
        }
        if (f->y1 >= n->y1 && f->y0 < n->y1) {
            rest->y1 = MAX (f->y0, n->y0);
            return TRUE;
        }
    }
    if (f->y0 <= n->y0 && f->y1 >= n->y1) {
        if (f->x0 <= n->x0 && f->x1 > n->x0) {
            rest->x0 = MIN (f->x1, n->x1);
            return TRUE;
        }
        if (f->x1 >= n->x1 && f->x0 < n->x1) {
            rest->x1 = MAX (f->x0, n->x0);
            return TRUE;
        }
    }
    return FALSE;
}

/* one rectangle of the frame into pt_scene (bound on the active unit) */
static void rc_copy (const RcBox *b, const GLint *vp)
{
    if (rc_area (b) <= 0)
        return;
    glCopyTexSubImage2D (GL_TEXTURE_2D, 0, b->x0, b->y0, vp[0] + b->x0, vp[1] + b->y0,
                         b->x1 - b->x0, b->y1 - b->y0);
    if (stats_on ()) {
        pt_stats.copies++;
        pt_stats.copied += (double) rc_area (b) / ((double) vp[2] * vp[3]);
    }
}

/* pt_scene gets the frame's pixels in n, the part of the view a system is
 * about to read; track = FALSE: all of n, nothing remembered */
static void rc_fill (const RcBox *n, const GLint *vp, gboolean track)
{
    if (!track || vp[2] != rc.vw || vp[3] != rc.vh) {
        rc_forget ();
        rc.vw = vp[2];
        rc.vh = vp[3];
    }
    if (!track) {
        rc_copy (n, vp);
        return;
    }
    /* the fresh rectangle that leaves the least to copy */
    RcBox rest = *n;
    gint64 todo = rc_area (n);
    gboolean have = FALSE;
    for (int i = 0; i < rc.nfresh; i++) {
        RcBox r;
        if (rc_minus (n, &rc.fresh[i], &r) && rc_area (&r) < todo) {
            rest = r;
            todo = rc_area (&r);
            have = TRUE;
        }
    }
    /* ... and what was drawn on in the part taken from it: one rectangle
     * around all of that, so a system makes one copy at most, as it always
     * did (pixels of it that the copy holds already are copied again: they
     * are the frame's either way) */
    if (have)
        for (int j = 0; j < rc.ndirty; j++) {
            RcBox q = rc_common (&rc.dirty[j], n);
            if (rc_area (&q) <= 0)
                continue;
            if (rc_area (&rest) <= 0) {
                rest = q;
            } else {
                rest.x0 = MIN (rest.x0, q.x0);
                rest.y0 = MIN (rest.y0, q.y0);
                rest.x1 = MAX (rest.x1, q.x1);
                rest.y1 = MAX (rest.y1, q.y1);
            }
        }
    rc_copy (&rest, vp);
    /* all of n is the frame's now: nothing in it is dirty, and it is fresh */
    int nd = 0;
    for (int j = 0; j < rc.ndirty; j++)
        if (!rc_inside (&rc.dirty[j], n))
            rc.dirty[nd++] = rc.dirty[j];
    rc.ndirty = nd;
    int nf = 0, small = 0;
    for (int i = 0; i < rc.nfresh; i++) {
        if (rc_inside (n, &rc.fresh[i]))
            return;                     /* (already known as part of a larger one) */
        if (!rc_inside (&rc.fresh[i], n))
            rc.fresh[nf++] = rc.fresh[i];
    }
    rc.nfresh = nf;
    if (rc.nfresh < RC_FRESH) {
        rc.fresh[rc.nfresh++] = *n;
        return;
    }
    for (int i = 1; i < rc.nfresh; i++)
        if (rc_area (&rc.fresh[i]) < rc_area (&rc.fresh[small]))
            small = i;
    if (rc_area (n) > rc_area (&rc.fresh[small]))
        rc.fresh[small] = *n;
}

/* system s was drawn directly on the frame: the copy no longer holds the
 * frame where its quads are (two pixels around their bounds for the
 * rasterizer's and our own rounding); bounds not known: nothing is known */
static void rc_drawn (const WeSys *s)
{
    if (!rc.nfresh)
        return;
    const float fx0 = (s->dbox[0] * 0.5f + 0.5f) * (float) rc.vw - 2.0f;
    const float fy0 = (s->dbox[1] * 0.5f + 0.5f) * (float) rc.vh - 2.0f;
    const float fx1 = (s->dbox[2] * 0.5f + 0.5f) * (float) rc.vw + 2.0f;
    const float fy1 = (s->dbox[3] * 0.5f + 0.5f) * (float) rc.vh + 2.0f;
    if (!(s->dbox[0] <= s->dbox[2] && s->dbox[1] <= s->dbox[3]) || !isfinite (fx0 + fx1 + fy0 + fy1) ||
        rc.ndirty == RC_DIRTY) {
        rc_forget ();
        return;
    }
    /* (clamped as floats: any finite number fits afterwards) */
    RcBox b = { (int) floorf (CLAMP (fx0, 0.0f, (float) rc.vw)),
                (int) floorf (CLAMP (fy0, 0.0f, (float) rc.vh)),
                (int) ceilf (CLAMP (fx1, 0.0f, (float) rc.vw)),
                (int) ceilf (CLAMP (fy1, 0.0f, (float) rc.vh)) };
    if (rc_area (&b) > 0)
        rc.dirty[rc.ndirty++] = b;
}

/* How far a refracting quad reads the frame from where it is drawn. PT_FS
 * samples the copy at the fragment's own place plus
 *     off = (tan.xy * n.x + tan.zw * n.y) * amount * mask * alpha
 * in units of the view (y negated), where tan = (right.x, up.x, right.y,
 * up.y) is what screen_tangents gave the quad, the same at each of its
 * corners; n is the decoded normal: both components within [-1, 1], or n.x
 * within [-0.965, 1.035] for the block compressed layout (u_nfmt 0); mask
 * is a texture channel, at most 1. So
 *     |off.x| <= (|right.x| * NX + |right.y|) * |amount| * |alpha|
 *     |off.y| <= (|up.x| * NX + |up.y|) * |amount| * |alpha|
 * with NX = 1.035, and a quad without a normal map (u_nfmt 3) reads only
 * where it is drawn. Returned: clip units (two per view) per unit of the
 * bracket times alpha, a thousandth added for the shader's own rounding. */
#define REFRACT_NX 1.035f
static float refract_reach (const WeSys *s)
{
    return s->ntex ? 2.0f * 1.001f * fabsf (s->refract_amount) : 0.0f;
}

/* program, matrix, texture, refraction inputs and blend function of a
 * system; then its vertex data and the draw. count = instances
 * (SUB_QUADS, record layout `layout`) or vertices. pr = the system's
 * layer probe (NULL: drawn directly). */
static void sys_submit (DrawCtx *d, WeSys *s, int kind, int layout, const float *v,
                        gsize floats, int count, LrProbe *pr)
{
    WalpeWeParticles *w = d->w;
    gboolean quads = kind == SUB_QUADS;
    PtProg *pg = quads ? &pt_quad : &pt_rib;
    gboolean st = stats_on ();
    gboolean additive = s->blend == WALPE_BLEND_ADDITIVE;
    int lf = pr ? lr_decide (s, pr) : 0;
    if (WALPE_CPH_HASHING ()) {
        WalpeCphStats *cs = walpe_cph_stats ();
        const int ph = walpe_cph_to (WALPE_CPH_NONE);   /* hashing is no phase's time */
        cs->pt_vtx_hash = walpe_cph_fnv (cs->pt_vtx_hash, v, floats * sizeof (float));
        walpe_cph_to (ph);
    }
    const int cph_prev = walpe_cph_to (WALPE_CPH_PT_GL);
    WALPE_CPH_COUNT (pt_draws, 1);
    WALPE_CPH_COUNT (pt_bytes, floats * sizeof (float));
    /* a normal system cannot follow additive ones in one layer (PtLayer) */
    if (lf && lr.pending && !additive && lr.has_add)
        lr_flush (d);
    if (lf && !lr_bind (d, lf))
        lf = 0;
    s->lr_f = lf;
    if (lf) {
        lf = lr.f;
        for (int j = 0; j < LR_TY; j++)
            lr.tiles[j] |= pr->tiles[j];
        if (additive)
            lr.has_add = TRUE;
        else
            lr.has_over = TRUE;
        lr.seen_f = lr.seen_f ? MIN (lr.seen_f, lf) : lf;
    } else if (lr.pending) {
        /* on the frame itself. An additive draw and an all-additive layer
         * can go on in either order; anything else needs the layer on
         * the frame first (refraction also reads the frame). */
        if (!additive || lr.has_over || s->refract)
            lr_flush (d);
        else
            lr_unbind ();
    }
    if (d->blend == BL_NONE) {
        /* the first draw of this call (an object without live particles
         * makes no GL call at all) */
        glEnable (GL_BLEND);   /* effect passes leave it off */
        glActiveTexture (GL_TEXTURE0);
    }
    if (d->prog != pg) {
        glUseProgram (pg->prog);
        d->prog = pg;
        if (st)
            pt_stats.state_changes++;
    }
    gboolean persp = s->persp && w->have_view;
    /* The layer's matrix is kept in one place per object and rewritten
     * when the layer's factor changes inside a render call (a child system
     * that is sharper than its parent: the pending 1/4 layer is flushed and
     * a 1/2 one begun). The pointer is then the one the program was given
     * before, with other numbers behind it: both programs take it again.
     * (They kept the 1/4 matrix, which differs by the layer's margin: the
     * system was drawn up to two pixels off towards the view's edge.) */
    gboolean fresh = FALSE;
    const float *m = lf ? lr_matrix (w, persp, &fresh) : persp ? w->clip : w->mvp;
    if (fresh)
        d->mvp[0] = d->mvp[1] = NULL;
    if (d->mvp[quads ? 0 : 1] != m) {
        glUniformMatrix3fv (pg->mvp, 1, GL_FALSE, m);
        d->mvp[quads ? 0 : 1] = m;
        if (st)
            pt_stats.state_changes++;
    }
    GLuint vao = quads ? pt_qvao[layout] : kind == SUB_RIBBON ? pt_rvao : pt_evao;
    if (d->vao != vao) {
        glBindVertexArray (vao);
        d->vao = vao;
        if (kind == SUB_RIBBON) {
            /* what a ribbon vertex does not carry: no refraction tangents,
             * no second sheet frame */
            glVertexAttrib4f (3, 1.0f, 0.0f, 0.0f, 1.0f);
            glVertexAttrib3f (4, 0.0f, 0.0f, 0.0f);
        }
    }
    /* a new store for the buffer every time (the draws still queued keep
     * theirs): no wait for the GPU, nothing copied back */
    glBindBuffer (GL_ARRAY_BUFFER, pt_stream);
    glBufferData (GL_ARRAY_BUFFER, (GLsizeiptr) (floats * sizeof (float)), v, GL_STREAM_DRAW);
    GLuint tex = s->tex ? s->tex : pt_dot;
    if (d->tex0 != tex) {
        glBindTexture (GL_TEXTURE_2D, tex);   /* unit 0 is the active one */
        d->tex0 = tex;
        if (st)
            pt_stats.state_changes++;
    }
    int sheet = quads && (layout & QL_SHEET) ? s->frow : 0;
    if (pg->sheet_set != sheet) {
        glUniform1i (pg->sheet, sheet);
        pg->sheet_set = sheet;
    }
    if (sheet) {
        glActiveTexture (GL_TEXTURE3);
        glBindTexture (GL_TEXTURE_2D, s->ftex);
        glActiveTexture (GL_TEXTURE0);
    }
    gboolean refr = s->refract;
    if (pg->refract_set != refr) {
        glUniform1f (pg->refract, refr ? 1.0f : 0.0f);
        pg->refract_set = refr;
        if (st)
            pt_stats.state_changes++;
    }
    /* a sprite that stayed block compressed has straight
     * alpha: the shader premultiplies it (u_straight) */
    int straight = s->tex && !s->tex_premult && !refr;
    if (pg->straight_set != straight) {
        glUniform1f (pg->straight, straight ? 1.0f : 0.0f);
        pg->straight_set = straight;
    }
    if (refr) {
        /* the scene behind: copy the frame drawn so far (once per system
         * per frame, like WE's _rt_FullFrameBuffer resolve) */
        GLint vp[4];
        glGetIntegerv (GL_VIEWPORT, vp);
        /* unit 2 BEFORE (re)allocating: binding pt_scene while unit 0 was
         * active replaced the sprite with the scene copy for that draw */
        glActiveTexture (GL_TEXTURE2);
        if (!pt_scene || pt_scene_w != vp[2] || pt_scene_h != vp[3]) {
            if (!pt_scene)
                glGenTextures (1, &pt_scene);
            glBindTexture (GL_TEXTURE_2D, pt_scene);
            glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, vp[2], vp[3], 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            pt_scene_w = vp[2];
            pt_scene_h = vp[3];
            rc_forget ();
        }
        glBindTexture (GL_TEXTURE_2D, pt_scene);
        /* Only the part the system can read: what its quads cover plus,
         * for each quad, the farthest its normal map can shift a sample
         * (sys_draw keeps these bounds in rbox, see refract_reach) - and a
         * pixel for the filter, one for the rounding. Splashes on the
         * ground or drops on an umbrella copied the whole frame for a
         * strip. Of that part, what the copy does not hold already (rc). */
        const int rc_mode = g_atomic_int_get (&pt_refract_copy);
        int cx = 0, cy = 0, cw = vp[2], ch = vp[3];
        if (s->rbox[0] <= s->rbox[2] && s->rbox[1] <= s->rbox[3] && rc_mode != RC_WHOLE) {
            const float mx = 2.0f, my = 2.0f;
            float fx0 = (s->rbox[0] * 0.5f + 0.5f) * (float) vp[2] - mx;
            float fx1 = (s->rbox[2] * 0.5f + 0.5f) * (float) vp[2] + mx;
            float fy0 = (s->rbox[1] * 0.5f + 0.5f) * (float) vp[3] - my;
            float fy1 = (s->rbox[3] * 0.5f + 0.5f) * (float) vp[3] + my;
            if (fx0 >= 0.0f && fx0 < (float) vp[2])   /* NaN and out of range: keep the edge */
                cx = (int) fx0;
            if (fy0 >= 0.0f && fy0 < (float) vp[3])
                cy = (int) fy0;
            if (fx1 >= 0.0f && fx1 < (float) vp[2])
                cw = (int) fx1 + 1;
            if (fy1 >= 0.0f && fy1 < (float) vp[3])
                ch = (int) fy1 + 1;
            cw = MAX (cw - cx, 1);
            ch = MAX (ch - cy, 1);
        }
        {
            const RcBox need = { cx, cy, cx + cw, cy + ch };
            /* (kept track of only in the frame's own viewport: that is
             * where the other systems' bounds are) */
            rc_fill (&need, vp, rc_mode == RC_MISSING && vp[0] == 0 && vp[1] == 0 &&
                                vp[2] == lr.tw && vp[3] == lr.th);
        }
        glActiveTexture (GL_TEXTURE1);
        glBindTexture (GL_TEXTURE_2D, s->ntex ? s->ntex : pt_dot);
        glActiveTexture (GL_TEXTURE0);
        glUniform1i (pg->nfmt, s->ntex ? s->nfmt : 3);
        glUniform1f (pg->ramount, s->refract_amount);
        glUniform4f (pg->vp, (float) vp[0], (float) vp[1], (float) vp[2], (float) vp[3]);
    }
    int blend = !additive ? BL_NORMAL : lf ? BL_ADD_LAYER : BL_ADD;
    if (d->blend != blend) {
        if (blend == BL_ADD)
            glBlendFunc (GL_ONE, GL_ONE);
        else if (blend == BL_ADD_LAYER)
            glBlendFuncSeparate (GL_ONE, GL_ONE, GL_ZERO, GL_ONE);   /* colour added, alpha kept */
        else
            glBlendFunc (GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        d->blend = blend;
        if (st)
            pt_stats.state_changes++;
    }
    /* once, or once per visible box of the frame (clip.h): particles
     * under other windows are simulated but not filled. (Into the
     * reduced-resolution layer: always once, it is not the frame.) */
    if (lf && !walpe_clip_active ())
        lr.unclipped = TRUE;
    for (int it = 0; walpe_clip_next (&it, NULL);) {
        if (quads)
            glDrawArraysInstanced (GL_TRIANGLES, 0, 6, count);
        else
            glDrawArrays (GL_TRIANGLES, 0, count);
    }
    if (!lf)
        rc_drawn (s);   /* on the frame itself: a copy of it is stale there */
    if (st) {
        pt_stats.draws++;
        pt_stats.uploads++;
        pt_stats.verts += quads ? (guint64) count * 6 : (guint64) count;   /* per vertex shader run */
        pt_stats.bytes += (guint64) floats * sizeof (float);
        if (lf) {
            /* its area was counted at full size (sys_draw) */
            s->st_lr++;
            pt_stats.lowres_systems++;
            pt_stats.shaded -= pr->st_area * (1.0 - 1.0 / (double) (lf * lf));
        }
    }
    walpe_cph_to (cph_prev);
}

/* ribbon vertices written so far: account for them, draw them */
static void rib_flush (DrawCtx *d, WeSys *s, const float *v, const float *o, double *tb)
{
    int nv = (int) ((o - v) / RIB_FLOATS);
    gboolean st = stats_on ();
    if (st) {
        pt_stats.build_ms += thread_cpu_ms () - *tb;
        if (nv > 0) {
            double ar = tris_view_area (v, nv, RIB_FLOATS,
                                        s->persp && d->w->have_view ? d->w->clip : d->w->mvp);
            pt_stats.drawn += (guint) (nv / 6);
            pt_stats.area += ar;
            pt_stats.shaded += ar;
            s->st_draws++;
            s->st_drawn += (guint64) (nv / 6);
            s->st_area += ar;
        }
    }
    if (nv > 0)
        sys_submit (d, s, SUB_RIBBON, 0, v, (gsize) nv * RIB_FLOATS, nv, NULL);
    if (st)
        *tb = thread_cpu_ms ();
}

/* n quad records -> six vertices each (what PT_VS_QUAD computes); pr as
 * for sys_submit (it reads the records, which stay as they are) */
static void quads_expand (DrawCtx *d, WeSys *s, const float *rec, int n, int rf, LrProbe *pr)
{
    static const float st[6][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
    static const float notan[4] = { 1, 0, 0, 1 };
    gboolean sheet = s->ftex != 0;
    float *e = tmp_bytes ((gsize) n * 6 * EXP_FLOATS * sizeof (float)), *o = e;
    for (int i = 0; i < n; i++, rec += rf) {
        const float *c2 = rec, *hx = rec + 2, *hy = rec + 4, *col = rec + 6;
        const float *x = rec + QUAD_FLOATS;
        float uv[6] = { 0, 0, 1, 0, 0, 1 }, uvb[6] = { 0, 0, 1, 0, 0, 1 }, fblend = 0.0f;
        if (sheet) {
            frame_rect (s, (int) x[0], uv);
            frame_rect (s, (int) x[1], uvb);
            fblend = x[2];
            x += 3;
        }
        const float *tan = s->refract ? x : notan;
        for (int k = 0; k < 6; k++, o += EXP_FLOATS) {
            float ss = st[k][0], tt = st[k][1];
            o[0] = c2[0] + (2 * ss - 1) * hx[0] + (1 - 2 * tt) * hy[0];
            o[1] = c2[1] + (2 * ss - 1) * hx[1] + (1 - 2 * tt) * hy[1];
            o[2] = uv[0] + ss * uv[2] + tt * uv[4];
            o[3] = uv[1] + ss * uv[3] + tt * uv[5];
            memcpy (o + 4, col, sizeof (float) * 4);
            memcpy (o + 8, tan, sizeof (float) * 4);
            o[12] = uvb[0] + ss * uvb[2] + tt * uvb[4];
            o[13] = uvb[1] + ss * uvb[3] + tt * uvb[5];
            o[14] = fblend;
        }
    }
    sys_submit (d, s, SUB_EXPANDED, 0, e, (gsize) n * 6 * EXP_FLOATS, n * 6, pr);
}

static void sys_draw (DrawCtx *d, WeSys *s)
{
    WalpeWeParticles *w = d->w;
    const float alpha = d->alpha, *tint = d->tint;
    for (int c = 0; c < s->nchild; c++)
        sys_draw (d, s->child[c].sys);
    if (s->renderer == RD_NONE || s->count == 0)
        return;
    gboolean st = stats_on ();
    double tb = st ? thread_cpu_ms () : 0.0;
    gboolean persp = s->persp && w->have_view;
    /* overbright scales colour only (premultiplied: rgb, not alpha) */
    float ob = s->overbright;
    /* drawn-space length of a model-space unit (ribbon widths in
     * perspective systems, where points are projected on the CPU) */
    float mscale = sqrtf (fabsf (w->wm[0] * w->wm[3] - w->wm[1] * w->wm[2]));
    gboolean ws = pt_draws_in_world (s->flags);
    /* a ribbon of a world-space system is as wide as its size in scene
     * units whatever the object's scale (2901582518's shooting stars,
     * object scale -4 4: thin lines in WE, 4x too wide through the matrix) */
    float wscale = persp ? (ws ? 1.0f : mscale) : (ws && mscale > 1e-6f ? 1.0f / mscale : 1.0f);

    /* ribbons: a refracting one reads the whole frame (no bounds kept) */
    s->rbox[0] = 1.0f;
    s->rbox[2] = -1.0f;
    s->dbox[0] = 1.0f;
    s->dbox[2] = -1.0f;
    if (s->renderer == RD_ROPE) {
        /* "all existing particles connected as a single rope, in order of
         * spawn time"; v = 1 at the oldest particle, 0 at the newest */
        int n = s->count;
        float *v = scratch ((gsize) n * 6 * RIB_FLOATS), *o = v;
        guint8 *tmp = tmp_bytes ((gsize) n * (sizeof (int) + 8 * sizeof (float)));
        float *buf = (float *) tmp;
        int *idx = (int *) (tmp + (gsize) n * 8 * sizeof (float));
        for (int i = 0; i < n; i++)
            idx[i] = i;
        g_qsort_with_data (idx, n, sizeof (int), seq_cmp, s->pts);
        float (*pt)[2] = (float (*)[2]) buf;
        float (*rgba)[4] = (float (*)[4]) (buf + (gsize) n * 2);
        float *hw = buf + (gsize) n * 6, *vc = buf + (gsize) n * 7;
        int m = 0;
        for (int k = 0; k < n; k++) {
            const Pt *p = &s->pts[idx[k]];
            float mp[3] = { p->p[0] + p->d_off[0], p->p[1] + p->d_off[1], p->p[2] + p->d_off[2] };
            float f;
            if (!pt_project (w, s, mp, pt[m], &f))
                continue;
            hw[m] = p->d_size * SIZE_ATTR * f * wscale;
            rgba[m][0] = p->d_col[0] * tint[0] * ob;
            rgba[m][1] = p->d_col[1] * tint[1] * ob;
            rgba[m][2] = p->d_col[2] * tint[2] * ob;
            rgba[m][3] = p->d_alpha * alpha;
            m++;
        }
        for (int k = 0; k < m; k++)
            vc[k] = m > 1 ? 1.0f - (float) k / (float) (m - 1) : 0.0f;
        if (m >= 2)
            o = put_ribbon (o, m, (const float (*)[2]) pt, hw, (const float (*)[4]) rgba, vc);
        rib_flush (d, s, v, o, &tb);
        return;
    }
    if (s->renderer == RD_ROPETRAIL) {
        /* a ribbon over each particle's position history: the texture
         * runs once along the trail, v = 0 at the particle */
        float iv = s->rope_len / (float) s->rope_seg;
        float pt[ROPE_SEG_MAX + 1][2], hw[ROPE_SEG_MAX + 1], vc[ROPE_SEG_MAX + 1];
        float rgba[ROPE_SEG_MAX + 1][4];
        const gsize one = (gsize) s->rope_seg * 6 * RIB_FLOATS;   /* the longest trail */
        int rib_culled = 0;
        gsize cap = MIN ((gsize) s->count * one, MAX (one, (gsize) RIB_CHUNK_FLOATS));
        float *v = scratch (cap), *o = v;
        for (int i = 0; i < s->count; i++) {
            const Pt *p = &s->pts[i];
            float a = p->d_alpha * alpha;
            if (a <= 0.002f || p->d_size <= 0.01f || p->hn < 1)
                continue;
            const float *h = s->hist + (gsize) i * s->rope_seg * 3;
            gboolean full = p->hn >= s->rope_seg;
            /* a full trail gives up its oldest stretch as the newest grows */
            float total = full ? (float) (p->hn - 1) * iv : p->hacc + (float) (p->hn - 1) * iv;
            if (total < 1e-4f)
                continue;
            float col[4] = { p->d_col[0] * tint[0] * ob, p->d_col[1] * tint[1] * ob,
                             p->d_col[2] * tint[2] * ob, a };
            int m = 0;
            gboolean ok = TRUE;
            for (int k = -1; k < p->hn && ok; k++) {
                float mp[3], t;
                if (k < 0) {
                    mp[0] = p->p[0] + p->d_off[0];
                    mp[1] = p->p[1] + p->d_off[1];
                    mp[2] = p->p[2] + p->d_off[2];
                    t = 0;
                } else {
                    memcpy (mp, h + k * 3, sizeof (mp));
                    t = p->hacc + (float) k * iv;
                    if (full && k == p->hn - 1 && k > 0) {
                        float fr = CLAMP (p->hacc / iv, 0.0f, 1.0f);
                        for (int c = 0; c < 3; c++)
                            mp[c] += (h[(k - 1) * 3 + c] - mp[c]) * fr;
                        t = total;
                    }
                }
                float f;
                if (!pt_project (w, s, mp, pt[m], &f)) {
                    ok = FALSE;
                    break;
                }
                hw[m] = p->d_size * SIZE_ATTR * f * wscale;
                vc[m] = CLAMP (t / total, 0.0f, 1.0f);
                memcpy (rgba[m], col, sizeof (col));
                m++;
            }
            /* a trail whose points are all one point (a particle that never
             * moved) is triangles without area: nothing to send */
            gboolean flat = TRUE;
            for (int k = 1; k < m && flat; k++)
                flat = pt[k][0] == pt[0][0] && pt[k][1] == pt[0][1];
            if (ok && m >= 2 && flat)
                rib_culled++;
            if (ok && m >= 2 && !flat) {
                if ((gsize) (o - v) + (gsize) (m - 1) * 6 * RIB_FLOATS > cap) {
                    rib_flush (d, s, v, o, &tb);   /* the chunk is full */
                    o = v;
                }
                o = put_ribbon (o, m, (const float (*)[2]) pt, hw, (const float (*)[4]) rgba, vc);
            }
        }
        rib_flush (d, s, v, o, &tb);
        if (st) {
            pt_stats.culled += (guint) rib_culled;
            s->st_culled += (guint64) rib_culled;
        }
        return;
    }

    /* sprites and sprite trails: one record per particle (PT_VS_QUAD) */
    gboolean sheet = s->ftex != 0;
    int layout = (sheet ? QL_SHEET : 0) | (s->refract ? QL_TAN : 0);
    int rf = QUAD_FLOATS + (sheet ? 3 : 0) + (s->refract ? 4 : 0);
    float *v = scratch ((gsize) s->count * rf), *o = v;
    const float *M = persp ? w->clip : w->mvp;
    int culled = 0;
    gboolean additive = s->blend == WALPE_BLEND_ADDITIVE;
    /* could this system go into the reduced layer? (lr_decide) */
    LrProbe pr;
    /* (whichever way the quads are submitted: "particle_instancing=off"
     * used to switch the layer off as well, so the setting that is there
     * to compare the two submissions also compared two pictures) */
    pr.on = lr.have_target && !lr.broken && !s->refract &&
            lowres_mode () != WALPE_PT_LOWRES_OFF;
    if (pr.on) {
        pr.area = pr.st_area = 0.0;
        pr.gain = 0.0f;
        pr.lx2 = pr.ly2 = G_MAXFLOAT;
        pr.edges = FALSE;
    }
    const float pxw = (float) lr.tw * 0.5f, pxh = (float) lr.th * 0.5f;
    float rtan[4] = { 1.0f, 0.0f, 0.0f, 1.0f };   /* a refracting quad's screen tangents */
    s->rbox[0] = s->rbox[1] = G_MAXFLOAT;
    s->rbox[2] = s->rbox[3] = -G_MAXFLOAT;
    gboolean rbox_ok = TRUE;
    /* where the quads are, while a copy of the frame is kept track of (rc:
     * this system draws on the frame the copy was made of) */
    const gboolean dbox_on = s->refract || rc.nfresh > 0;
    s->dbox[0] = s->dbox[1] = G_MAXFLOAT;
    s->dbox[2] = s->dbox[3] = -G_MAXFLOAT;
    /* clip units a sample can move per unit of (tangent . normal) x alpha:
     * nothing without a normal map (the shader adds no offset) */
    const float reach_k = s->refract ? refract_reach (s) : 0.0f;
    /* the same for every particle of the system (each used to be asked
     * for again per particle, two of them behind a g_once): */
    const float size_attr = SIZE_ATTR;
    const float ratio = s->aspect > 0 ? s->aspect : 1.0f;
    const int axm = ax_mode (w, s);
    const gboolean sheet_random = sheet && s->anim == AN_RANDOM && s->nframes > 1;
    /* (the cache shares its words with the ropetrail history counters) */
    const gboolean rot_cached = s->rot_fixed && s->renderer == RD_SPRITE && !s->hist;
    for (int i = 0; i < s->count; i++) {
        Pt *p = &s->pts[i];
        float a = p->d_alpha * alpha;
        if (a <= 0.002f || p->d_size <= 0.01f)
            continue;
        float mp[3] = { p->p[0] + p->d_off[0], p->p[1] + p->d_off[1], p->p[2] + p->d_off[2] };
        float c2[2], f;
        if (!persp) {
            /* (pt_project's first case) */
            c2[0] = mp[0];
            c2[1] = mp[1];
            f = 1.0f;
        } else if (!pt_project (w, s, mp, c2, &f)) {
            continue;
        }
        float hx[2], hy[2];  /* half axes of the quad, model space */
        float hs = p->d_size * size_attr * 0.5f;
        gboolean drawn_axes = FALSE;   /* hx / hy are in drawn space already */
        if (s->renderer == RD_TRAIL && persp) {
            /* a trail in a perspective system is a quad in 3D along
             * the velocity (ComputeParticleTrailTangents: up = the
             * velocity's direction, right = eye direction x velocity):
             * both ends are projected. A velocity towards the camera
             * gives the streaks of WE's starfield preset (z speed
             * 1500-2000), which a quad laid along the x / y velocity
             * alone never showed. */
            float vl = sqrtf (p->v[0] * p->v[0] + p->v[1] * p->v[1] + p->v[2] * p->v[2]);
            if (vl < 1e-4f)
                continue;
            float vs = vl;             /* speed that sets the length */
            if (ws) {
                float wv[3] = { w->wm[0] * p->v[0] + w->wm[2] * p->v[1],
                                w->wm[1] * p->v[0] + w->wm[3] * p->v[1], p->v[2] * w->wsz };
                vs = sqrtf (wv[0] * wv[0] + wv[1] * wv[1] + wv[2] * wv[2]);
            }
            float len = MAX (s->trail_min, MIN (vs * s->trail_len, s->trail_max));
            /* half length along the velocity, in model units (a
             * world-space system: the length it has in the scene) */
            float hl = hs * ratio * len * (ws && vs > 1e-6f ? vl / vs : 1.0f);
            float e0[3], e1[3], a2[2], b2[2], fa, fb;
            for (int c = 0; c < 3; c++) {
                e0[c] = mp[c] - p->v[c] / vl * hl;
                e1[c] = mp[c] + p->v[c] / vl * hl;
            }
            gboolean oka = pt_project (w, s, e0, a2, &fa), okb = pt_project (w, s, e1, b2, &fb);
            if (!oka && !okb)
                continue;
            if (!oka) { a2[0] = c2[0]; a2[1] = c2[1]; }   /* passes the camera: half of it */
            if (!okb) { b2[0] = c2[0]; b2[1] = c2[1]; }
            float ex = (b2[0] - a2[0]) * 0.5f, ey = (b2[1] - a2[1]) * 0.5f;
            float el = hypotf (ex, ey);
            if (el < 1e-5f)
                continue;              /* seen end-on */
            c2[0] = (a2[0] + b2[0]) * 0.5f;
            c2[1] = (a2[1] + b2[1]) * 0.5f;
            /* width: the size at the particle's depth (scene units) */
            float hw = hs * f * (ws ? 1.0f : mscale);
            hx[0] = ey / el * hw; hx[1] = -ex / el * hw;
            hy[0] = ex; hy[1] = ey;
            drawn_axes = TRUE;
        } else if (s->renderer == RD_TRAIL) {
            /* ComputeParticleTrailTangents + ComputeParticlePosition:
             * right = normalize (eye x velocity) * size, up = dir *
             * size * textureRatio * max (minlength, min (|v| *
             * length, maxlength)); the quad is centred on the
             * particle. A world-space system takes the velocity the
             * particle has in the scene. */
            float pv[3] = { p->v[0], p->v[1], p->v[2] };
            if (ws) {
                pv[0] = w->wm[0] * p->v[0] + w->wm[2] * p->v[1];
                pv[1] = w->wm[1] * p->v[0] + w->wm[3] * p->v[1];
                pv[2] = p->v[2] * w->wsz;
            }
            float vl = sqrtf (pv[0] * pv[0] + pv[1] * pv[1] + pv[2] * pv[2]);
            float vxy = hypotf (pv[0], pv[1]);
            float len = MAX (s->trail_min, MIN (vl * s->trail_len, s->trail_max));
            float dx = vl > 1e-4f ? pv[0] / vl : 0, dy = vl > 1e-4f ? pv[1] / vl : 1;
            float rx = vxy > 1e-4f ? pv[1] / vxy : 1, ry = vxy > 1e-4f ? -pv[0] / vxy : 0;
            hx[0] = rx * hs; hx[1] = ry * hs;
            hy[0] = dx * hs * ratio * len; hy[1] = dy * hs * ratio * len;
        } else {
            /* ComputeParticleTangents: rotation about z, then x, then
             * y; right / up = the first two rows of that matrix. On
             * screen (z dropped): right = (cz cy - sz sx sy, -sz cx),
             * up = (sz cy + cz sx sy, cz cx); height = size * ratio.
             * About z alone that is (cos r, -sin r) / (sin r, cos r);
             * an x or y angle foreshortens the sprite. */
            float cz = 1.0f, sz = 0.0f;   /* cos, sin of 0 */
            if (p->rot != 0.0f) {
                if (!rot_cached) {
                    cz = cosf (p->rot);
                    sz = sinf (p->rot);
                    if (st)
                        pt_stats.trig++;
                } else {
                    if (p->rc == 0.0f && p->rs == 0.0f) {
                        /* first time drawn (cos and sin are never both 0;
                         * a NaN angle gives NaNs, which stay) */
                        p->rc = cosf (p->rot);
                        p->rs = sinf (p->rot);
                        if (st)
                            pt_stats.trig++;
                    }
                    cz = p->rc;
                    sz = p->rs;
                }
            }
            if (p->rxy[0] == 0.0f && p->rxy[1] == 0.0f) {
                /* no tilt (cx = cy = 1, sx = sy = 0): what the general
                 * form below evaluates to, without its four sin / cos */
                hx[0] = cz * hs; hx[1] = -sz * hs;
                hy[0] = sz * hs * ratio; hy[1] = cz * hs * ratio;
            } else {
                float cx = cosf (p->rxy[0]), sx = sinf (p->rxy[0]);
                float cy = cosf (p->rxy[1]), sy = sinf (p->rxy[1]);
                hx[0] = (cz * cy - sz * sx * sy) * hs; hx[1] = -sz * cx * hs;
                hy[0] = (sz * cy + cz * sx * sy) * hs * ratio; hy[1] = cz * cx * hs * ratio;
            }
        }
        if (!drawn_axes && axm != AX_NONE) {
            ax_apply (axm, w, s, f, hx);
            ax_apply (axm, w, s, f, hy);
        }
        {
            /* a quad whose bounds lie outside the view covers no pixel:
             * not sent (emitters wider than the screen, particles that
             * left it and live on). A NaN compares false and is sent. */
            float px = M[0] * c2[0] + M[3] * c2[1] + M[6];
            float py = M[1] * c2[0] + M[4] * c2[1] + M[7];
            float hxx = M[0] * hx[0] + M[3] * hx[1], hxy = M[1] * hx[0] + M[4] * hx[1];
            float hyx = M[0] * hy[0] + M[3] * hy[1], hyy = M[1] * hy[0] + M[4] * hy[1];
            float ex = fabsf (hxx) + fabsf (hyx), ey = fabsf (hxy) + fabsf (hyy);
            if (fabsf (px) - ex > CULL_EDGE || fabsf (py) - ey > CULL_EDGE) {
                culled++;
                continue;
            }
            if (dbox_on) {
                if (!isfinite (px + ex) || !isfinite (py + ey))
                    rbox_ok = FALSE;
                if (!(px - ex >= s->dbox[0]))
                    s->dbox[0] = px - ex;
                if (!(py - ey >= s->dbox[1]))
                    s->dbox[1] = py - ey;
                if (!(px + ex <= s->dbox[2]))
                    s->dbox[2] = px + ex;
                if (!(py + ey <= s->dbox[3]))
                    s->dbox[3] = py + ey;
            }
            if (s->refract) {
                /* what the quad can read of the frame (sys_submit): where
                 * it is, and from there as far as its own tangents and
                 * alpha let the normal map shift a sample */
                screen_tangents (w, s, hx, hy, rtan);
                const float ra = reach_k * fabsf (a);
                const float gx = ex + (fabsf (rtan[0]) * REFRACT_NX + fabsf (rtan[2])) * ra;
                const float gy = ey + (fabsf (rtan[1]) * REFRACT_NX + fabsf (rtan[3])) * ra;
                if (!isfinite (px + gx) || !isfinite (py + gy))
                    rbox_ok = FALSE;   /* a quad without numbers: the whole view */
                if (!(px - gx >= s->rbox[0]))
                    s->rbox[0] = px - gx;
                if (!(py - gy >= s->rbox[1]))
                    s->rbox[1] = py - gy;
                if (!(px + gx <= s->rbox[2]))
                    s->rbox[2] = px + gx;
                if (!(py + gy <= s->rbox[3]))
                    s->rbox[3] = py + gy;
            }
            if (pr.on) {
                float lx2 = hxx * hxx * pxw * pxw + hxy * hxy * pxh * pxh;
                float ly2 = hyx * hyx * pxw * pxw + hyy * hyy * pxh * pxh;
                if (!(lx2 >= 64.0f && ly2 >= 64.0f)) {
                    pr.on = FALSE;   /* a quad under 16 pixels: never reduced */
                } else {
                    float x0 = MAX (px - ex, -1.0f), x1 = MIN (px + ex, 1.0f);
                    float y0 = MAX (py - ey, -1.0f), y1 = MIN (py + ey, 1.0f);
                    if (x1 > x0 && y1 > y0) {
                        /* the part of the quad in the view, screen areas */
                        float qa = fabsf (hxx * hyy - hxy * hyx);
                        float in = (x1 - x0) * (y1 - y0) / (4.0f * ex * ey);
                        if (in >= 0.999f || qa >= 0.999f * ex * ey) {
                            /* all of it, or an upright quad: the share of
                             * its bounds in the view is its own */
                            pr.area += qa * in;
                            pr.edges = TRUE;
                        } else if (x0 <= -1.0f && x1 >= 1.0f && y0 <= -1.0f && y1 >= 1.0f &&
                                   quad_covers_view (px, py, hxx, hxy, hyx, hyy)) {
                            pr.area += 1.0;
                        } else {
                            /* a turned quad cut by the view's edge (a
                             * light shaft across the screen): clipped. The
                             * share of its bounds would be far too small,
                             * and the system would leave the layer the
                             * moment one of its quads stops covering the
                             * view */
                            const float q[4][2] = {
                                { px - hxx + hyx, py - hxy + hyy }, { px + hxx + hyx, py + hxy + hyy },
                                { px + hxx - hyx, py + hxy - hyy }, { px - hxx - hyx, py - hxy - hyy },
                            };
                            pr.area += poly_view_area (q, 4);
                            pr.edges = TRUE;
                        }
                    }
                    pr.lx2 = MIN (pr.lx2, lx2);
                    pr.ly2 = MIN (pr.ly2, ly2);
                    float cm = MAX (MAX (p->d_col[0] * tint[0], p->d_col[1] * tint[1]),
                                    p->d_col[2] * tint[2]) * ob;
                    float g = cm * a;
                    if (!additive) {
                        g = MAX (g, a);   /* its alpha also dims what is behind */
                        if (cm > 1.0f)
                            pr.on = FALSE;   /* brighter than its alpha: it can
                                              * clip the frame like an additive one */
                    }
                    pr.gain = MAX (pr.gain, g);
                }
            }
        }
        /* the corners are c2 + (2s - 1) hx + (1 - 2t) hy for (s, t) in
         * {0, 1} (t = 0 is the top row of the frame, scene Y is up) */
        o[0] = c2[0]; o[1] = c2[1];
        o[2] = hx[0]; o[3] = hx[1];
        o[4] = hy[0]; o[5] = hy[1];
        o[6] = p->d_col[0] * tint[0] * ob;
        o[7] = p->d_col[1] * tint[1] * ob;
        o[8] = p->d_col[2] * tint[2] * ob;
        o[9] = a;
        o += QUAD_FLOATS;
        if (sheet_random) {
            /* "randomframe": the particle's own frame for life, no blend
             * (what frame_idx gives for it, without the life fraction) */
            int fr = CLAMP ((int) p->frame, 0, s->nframes - 1);
            o[0] = (float) fr; o[1] = (float) fr; o[2] = 0.0f;
            o += 3;
        } else if (sheet) {
            int f0, f1;
            float fblend = frame_idx (s, p, &f0, &f1);
            o[0] = (float) f0; o[1] = (float) f1; o[2] = fblend;
            o += 3;
        }
        if (s->refract) {
            memcpy (o, rtan, sizeof (rtan));   /* (computed with the quad's bounds above) */
            o += 4;
        }
    }
    if (!rbox_ok) {
        s->rbox[0] = 1.0f;
        s->rbox[2] = -1.0f;
    }
    if (!rbox_ok || !dbox_on) {
        s->dbox[0] = 1.0f;
        s->dbox[2] = -1.0f;
    }
    int n = (int) ((o - v) / rf);
    if (st) {
        pt_stats.build_ms += thread_cpu_ms () - tb;
        double area = quads_view_area (v, n, rf, M, &s->st_w, &s->st_h);
        pr.st_area = area;
        pt_stats.drawn += (guint) n;
        pt_stats.culled += (guint) culled;
        pt_stats.area += area;
        pt_stats.shaded += area;
        s->st_draws++;
        s->st_drawn += (guint64) n;
        s->st_culled += (guint64) culled;
        s->st_area += area;
    }
    if (n > 0) {
        pr.v = v; pr.M = M; pr.n = n; pr.rf = rf;
        if (!pt_instanced ())
            quads_expand (d, s, v, n, rf, pr.on ? &pr : NULL);
        else
            sys_submit (d, s, SUB_QUADS, layout, v, (gsize) n * rf, n, pr.on ? &pr : NULL);
    }
    else
        s->lr_f = 0;
}

static int sys_count (WeSys *s);

/* WALPE_CPU_PHASES=2: every live particle's state, in array order, folded
 * into h (two builds given the same seed must agree bit for bit) */
static guint64 sys_state_hash (WeSys *s, guint64 h)
{
    h = walpe_cph_fnv (h, &s->count, sizeof (s->count));
    for (int i = 0; i < s->count; i++) {
        const Pt *p = &s->pts[i];
        h = walpe_cph_fnv (h, p->p, sizeof (p->p));
        h = walpe_cph_fnv (h, p->v, sizeof (p->v));
        h = walpe_cph_fnv (h, &p->rot, sizeof (p->rot));
        h = walpe_cph_fnv (h, p->rxy, sizeof (p->rxy));
        h = walpe_cph_fnv (h, &p->age, sizeof (p->age));
        h = walpe_cph_fnv (h, &p->d_alpha, sizeof (p->d_alpha));
        h = walpe_cph_fnv (h, &p->d_size, sizeof (p->d_size));
        h = walpe_cph_fnv (h, p->d_col, sizeof (p->d_col));
        h = walpe_cph_fnv (h, p->d_off, sizeof (p->d_off));
        h = walpe_cph_fnv (h, &p->seq, sizeof (p->seq));
    }
    for (int c = 0; c < s->nchild; c++)
        h = sys_state_hash (s->child[c].sys, h);
    return h;
}

/* this frame's cursor (system-local) for mouse-linked control points and
 * the inverse object matrix for world-space forces */
static void sys_set_frame (WeSys *s, const WalpeWeParticles *w)
{
    memcpy (s->cur, w->cursor, sizeof (s->cur));
    s->have_cur = w->have_cursor;
    float det = w->wm[0] * w->wm[3] - w->wm[2] * w->wm[1];
    if (fabsf (det) > 1e-8f) {
        s->winv[0] = w->wm[3] / det;  s->winv[2] = -w->wm[2] / det;
        s->winv[1] = -w->wm[1] / det; s->winv[3] = w->wm[0] / det;
    } else {
        s->winv[0] = s->winv[3] = 1.0f;
        s->winv[1] = s->winv[2] = 0.0f;
    }
    s->winv_z = 1.0f / w->wsz;
    /* control points placed in the scene -> system-local space, where the
     * operators read them (a point that also follows the mouse keeps its
     * offset: the cursor is system-local already) */
    for (int n = 0; n < MAX_CP && s->cp_world; n++) {
        if (!((s->cp_world >> n) & 1) || ((s->cp_mouse >> n) & 1))
            continue;
        float x = s->cpw[n][0] - w->wm[4], y = s->cpw[n][1] - w->wm[5];
        s->cp[n][0] = s->winv[0] * x + s->winv[2] * y;
        s->cp[n][1] = s->winv[1] * x + s->winv[3] * y;
        s->cp[n][2] = (s->cpw[n][2] - w->wz) * s->winv_z;
    }
    for (int c = 0; c < s->nchild; c++)
        sys_set_frame (s->child[c].sys, w);
}

/* the simulation half of a frame: advance by dt. The scene steps every
 * system before it decides whether the frame has to be drawn at all (a
 * system with no particle alive draws nothing); walpe_weparticles_render
 * then only draws. Without a step before it, render does both as ever. */
/* One simulation step. Two callers share the name (integration): the scene
 * steps every system before it compares the frame with the one on screen
 * (the render call that follows then only draws), and the
 * unit test steps a bare system that has no GL objects
 * (tests/weparticles_test.c). */
void walpe_weparticles_step (WalpeWeParticles *w, double dt)
{
    if (!w || !w->root)
        return;
    dt = CLAMP (dt, 0.0, 0.1);
    if (!w->gl_ready) {
        /* no GL: no cursor, no prewarm, nothing drawn afterwards */
        sys_set_frame (w->root, w);
        sys_update (w->root, MIN (dt * w->root->time_mul, 1.0), w->audio_level, NULL);
        return;
    }
    const int cph_prev = walpe_cph_to (WALPE_CPH_PT_SIM);
    float cur[3];
    const float *at = NULL;
    if (w->root->cp0_mouse && w->have_cursor) {
        for (int i = 0; i < 3; i++)
            cur[i] = w->cursor[i] + w->root->cp0_offset[i];
        at = cur;
    }
    sys_set_frame (w->root, w);
    gboolean st = stats_on ();
    double t0 = st ? thread_cpu_ms () : 0.0;
    if (w->need_prewarm) {
        /* "starttime": pre-simulated here, not at gl_init - the object's
         * matrix (world-space gravity) is only known now */
        w->need_prewarm = FALSE;
        prewarm (w->root, MIN (w->root->starttime, 10.0f));
    }
    /* the "rate" override is the system's clock: it scales the step and
     * nothing a particle stores (trail lengths come from its velocity).
     * One step stays bounded. */
    sys_update (w->root, MIN (dt * w->root->time_mul, 1.0), w->audio_level, at);
    if (st)   /* the simulation runs in the scene's pre-step: its time is counted here */
        pt_stats.sim_ms += thread_cpu_ms () - t0;
    w->stepped = TRUE;
    if (G_UNLIKELY (walpe_cph_level_get () != 0)) {
        WalpeCphStats *cs = walpe_cph_stats ();
        if (cs) {
            walpe_cph_to (WALPE_CPH_NONE);   /* the hash is not simulation time */
            cs->pt_steps++;
            cs->pt_live += (guint64) sys_count (w->root);
            if (walpe_cph_level_get () >= 2)
                cs->pt_sim_hash = sys_state_hash (w->root, cs->pt_sim_hash);
        }
    }
    walpe_cph_to (cph_prev);
}

void walpe_weparticles_render (WalpeWeParticles *w, const float *mvp, double dt,
                               float alpha, const float tint[3])
{
    if (!w->gl_ready)
        return;
    if (!w->stepped)
        walpe_weparticles_step (w, dt);
    w->stepped = FALSE;
    const int cph_prev = walpe_cph_to (WALPE_CPH_PT_BUILD);
    memcpy (w->mvp, mvp, sizeof (w->mvp));
    gboolean st = stats_on ();
    double t1 = st ? thread_cpu_ms () : 0.0;
    GLuint q = 0;
    if (st) {
        pt_stats.renders++;
        pt_stats.live += (guint) sys_count (w->root);
        if (epoxy_is_desktop_gl ()) {
            glGenQueries (1, &q);
            glBeginQuery (GL_SAMPLES_PASSED, q);
        }
    }
    DrawCtx d = { .w = w, .alpha = alpha, .tint = tint, .blend = BL_NONE };
    w->lmat_f[0] = w->lmat_f[1] = 0;   /* this frame's matrices */
    pt_in_render = TRUE;
    sys_draw (&d, w->root);
    /* what a draw changed goes back to what the layer draws expect; a
     * reduced layer stays pending for the next particle object (the scene
     * flushes it before it draws anything else) */
    lr_unbind ();
    if (d.vao)
        glBindVertexArray (0);
    if (d.blend == BL_ADD || d.blend == BL_ADD_LAYER)
        glBlendFunc (GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    pt_in_render = FALSE;
    if (st) {
        pt_stats.draw_ms += thread_cpu_ms () - t1;
        if (q) {
            GLuint n = 0;
            glEndQuery (GL_SAMPLES_PASSED);
            glGetQueryObjectuiv (q, GL_QUERY_RESULT, &n);
            glDeleteQueries (1, &q);
            pt_stats.samples += n;
        }
    }
    walpe_cph_to (cph_prev);
}

void walpe_weparticles_begin_frame (guint target_fbo, int w, int h)
{
    rc_forget ();
    lr.target = target_fbo;
    lr.tw = w;
    lr.th = h;
    lr.have_target = w >= 64 && h >= 64;
    lr.hint_f = lr.seen_f;
    lr.seen_f = 0;
    /* a layer nothing used for a while (the setting went off, the fog's
     * layer is hidden) gives its memory back */
    if (lr.hint_f)
        lr.idle = 0;
    else if (lr.fbo && ++lr.idle > 600)
        lr_release ();
}

void walpe_weparticles_flush (void)
{
    /* the caller is about to draw on the frame (or the frame is over): a
     * copy of it kept for refracting systems holds the frame no longer */
    rc_forget ();
    if (!lr.pending)
        return;
    GLuint q = 0;
    if (stats_on () && !pt_in_render && epoxy_is_desktop_gl ()) {
        glGenQueries (1, &q);
        glBeginQuery (GL_SAMPLES_PASSED, q);
    }
    const int cph_prev = walpe_cph_to (WALPE_CPH_PT_GL);
    lr_flush (NULL);
    walpe_cph_to (cph_prev);
    if (q) {
        GLuint n = 0;
        glEndQuery (GL_SAMPLES_PASSED);
        glGetQueryObjectuiv (q, GL_QUERY_RESULT, &n);
        glDeleteQueries (1, &q);
        pt_stats.samples += n;
    }
}

void walpe_weparticles_end_frame (void)
{
    walpe_weparticles_flush ();
    lr.have_target = FALSE;
}

/* the copy of the frame that refracting systems read: a frame of RGBA8
 * that used to stay for the life of the render thread once one such
 * system had been drawn. Made again by the next one. */
static void pt_scene_release (void)
{
    if (pt_scene)
        glDeleteTextures (1, &pt_scene);
    pt_scene = 0;
    pt_scene_w = pt_scene_h = 0;
}

void walpe_weparticles_drop_targets (void)
{
    rc_forget ();
    lr_release ();
    pt_scene_release ();
}

gboolean walpe_weparticles_follows_cursor (WalpeWeParticles *p)
{
    return p && p->root && p->root->cp0_mouse;
}

static gboolean sys_uses_cursor (WeSys *s)
{
    if (s->cp_mouse)
        return TRUE;
    for (int c = 0; c < s->nchild; c++)
        if (sys_uses_cursor (s->child[c].sys))
            return TRUE;
    return FALSE;
}

gboolean walpe_weparticles_uses_cursor (WalpeWeParticles *p)
{
    return p && p->root && sys_uses_cursor (p->root);
}

static gboolean sys_reads_frame (WeSys *s)
{
    if (s->refract)
        return TRUE;
    for (int c = 0; c < s->nchild; c++)
        if (sys_reads_frame (s->child[c].sys))
            return TRUE;
    return FALSE;
}

gboolean walpe_weparticles_reads_frame (WalpeWeParticles *p)
{
    return p && p->root && sys_reads_frame (p->root);
}

void walpe_weparticles_set_cursor (WalpeWeParticles *p, float x, float y)
{
    if (!p)
        return;
    p->cursor[0] = x;
    p->cursor[1] = y;
    p->cursor[2] = 0;
    p->have_cursor = TRUE;
}

static void sys_set_emit (WeSys *s, gboolean on, gboolean clear)
{
    s->no_emit = !on;
    if (clear) {
        pt_live_sub (s->count);
        s->count = 0;
        /* stopped: the emitters are at their start again. A burst that has
         * fired stayed "done" and an emitter whose duration was over stayed
         * over, so the play () after a stop () emitted nothing from them: a
         * burst-only system (a click effect) was empty for good. */
        for (int e = 0; e < s->nem; e++) {
            s->em[e].t = 0;
            s->em[e].acc = 0;
            s->em[e].burst_done = FALSE;
        }
    }
    for (int c = 0; c < s->nchild; c++)
        sys_set_emit (s->child[c].sys, on, clear);
}

/* SceneScript IParticleSystem: play() = emit again, pause() = stop emitting
 * (live particles keep simulating), stop() = also remove them */
/* instance overrides changed while the system runs (SceneScript). Live
 * particles are left to end on their own: 3042492564's click sparks are
 * count = n / frametime for one frame and 0 otherwise, 3151551777 switches
 * its night birds and petals off with count 0. */
static void sys_set_override (WeSys *s, const WalpeWeParticleOverride *ov, guint scripted, int depth)
{
    if (!s || depth > MAX_DEPTH)
        return;
    if (s->maxcount0 <= 0) {
        s->maxcount0 = MAX (s->maxcount, 1);
        s->count0 = s->ov.count > 0 ? s->ov.count : 1.0f;
    }
    guint cp_kept = s->ov.cp_set;
    s->ov = *ov;
    s->ov.cp_set |= cp_kept;
    sys_factors (s, scripted);
    for (int ci = 0; ci < MAX_CP; ci++)
        if (ov->cp_set & (1u << ci)) {
            memcpy (s->cp[ci], ov->cp[ci], sizeof (s->cp[ci]));
            memcpy (s->cpw[ci], ov->cp[ci], sizeof (s->cpw[ci]));
        }
    for (int c = 0; c < s->nchild; c++)
        sys_set_override (s->child[c].sys, ov, scripted, depth + 1);
}

void walpe_weparticles_set_override (WalpeWeParticles *p, const WalpeWeParticleOverride *ov, guint scripted)
{
    if (p && p->root && ov)
        sys_set_override (p->root, ov, scripted, 0);
}

/* no GL (the unit test's entries): one step of dt seconds of the frame
 * clock, and the i-th live particle of the root system */
gboolean walpe_weparticles_peek (WalpeWeParticles *w, int i, float pos[3], float vel[3], float *age)
{
    if (!w || !w->root || i < 0 || i >= w->root->count)
        return FALSE;
    const Pt *p = &w->root->pts[i];
    memcpy (pos, p->p, sizeof (float) * 3);
    memcpy (vel, p->v, sizeof (float) * 3);
    *age = p->age;
    return TRUE;
}

/* system number n of the tree under s (s itself = 0, then each child with
 * its children); *n counts down */
static WeSys *sys_nth (WeSys *s, int *n)
{
    if (*n == 0)
        return s;
    (*n)--;
    for (int c = 0; c < s->nchild; c++) {
        WeSys *r = sys_nth (s->child[c].sys, n);
        if (r)
            return r;
    }
    return NULL;
}

gboolean walpe_weparticles_sys_peek (WalpeWeParticles *w, int sys, int i, float pos[3], float vel[3],
                                     float *age)
{
    WeSys *s = w && w->root && sys >= 0 ? sys_nth (w->root, &sys) : NULL;
    if (!s || i < 0 || i >= s->count)
        return FALSE;
    const Pt *p = &s->pts[i];
    memcpy (pos, p->p, sizeof (float) * 3);
    memcpy (vel, p->v, sizeof (float) * 3);
    *age = p->age;
    return TRUE;
}

int walpe_weparticles_sys_count (WalpeWeParticles *w, int sys)
{
    WeSys *s = w && w->root && sys >= 0 ? sys_nth (w->root, &sys) : NULL;
    return s ? s->count : -1;
}

/* ropetrail systems: how many history points the i-th live particle of
 * system `sys` has (0: none, -1: no such particle or no ropetrail) and
 * where the oldest of them lies */
int walpe_weparticles_sys_trail (WalpeWeParticles *w, int sys, int i, float oldest[3])
{
    WeSys *s = w && w->root && sys >= 0 ? sys_nth (w->root, &sys) : NULL;
    if (!s || !s->hist || i < 0 || i >= s->count)
        return -1;
    const Pt *p = &s->pts[i];
    if (p->hn > 0)
        memcpy (oldest, s->hist + ((gsize) i * s->rope_seg + (gsize) (p->hn - 1)) * 3, sizeof (float) * 3);
    return p->hn;
}

/* the alpha the i-th live particle of system `sys` was last drawn with
 * (its own alpha x fades x oscillation; -1 = no such particle) */
float walpe_weparticles_sys_alpha (WalpeWeParticles *w, int sys, int i)
{
    WeSys *s = w && w->root && sys >= 0 ? sys_nth (w->root, &sys) : NULL;
    return s && i >= 0 && i < s->count ? s->pts[i].d_alpha : -1.0f;
}

void walpe_weparticles_set_playing (WalpeWeParticles *p, int mode)
{
    if (p && p->root)
        sys_set_emit (p->root, mode > 0, mode < 0);
}

gboolean walpe_weparticles_is_playing (WalpeWeParticles *p)
{
    return p && p->root && (!p->root->no_emit || walpe_weparticles_count (p) > 0);
}

void walpe_weparticles_set_audio_level (WalpeWeParticles *p, float level)
{
    p->audio_level = CLAMP (level, 0.0f, 4.0f);
}

static int sys_count (WeSys *s)
{
    int n = s->count;
    for (int c = 0; c < s->nchild; c++)
        n += sys_count (s->child[c].sys);
    return n;
}

int walpe_weparticles_count (WalpeWeParticles *p)
{
    return p ? sys_count (p->root) : 0;
}

static int sys_systems (WeSys *s)
{
    int n = 1;
    for (int c = 0; c < s->nchild; c++)
        n += sys_systems (s->child[c].sys);
    return n;
}

int walpe_weparticles_systems (WalpeWeParticles *p)
{
    return p ? sys_systems (p->root) : 0;
}

static void sys_free (WeSys *s)
{
    for (int c = 0; c < s->nchild; c++)
        sys_free (s->child[c].sys);
    if (g_atomic_int_get (&pt_stats_on) == 2 && s->st_draws) {
        static const char *rd[] = { "none", "sprite", "trail", "rope", "ropetrail" };
        double n = s->st_draws, q = MAX (1.0, (double) s->st_drawn);
        char *base = s->tex_path ? g_path_get_basename (s->tex_path) : g_strdup ("(dot)");
        g_message ("weparticles: [system] %s %s%s%s%s tex %dx%d/%d: %u draws, %.1f quads + %.1f "
                   "off screen per draw, %.4f screen areas per draw, quad %.4f x %.4f of the view; "
                   "detail x %.3f/%.3f y %.3f/%.3f edge %.3f%s; reduced layer: %u draws, last "
                   "predicted error %.2f/255, last saving %.2f",
                   base, rd[CLAMP (s->renderer, 0, 4)],
                   s->blend == WALPE_BLEND_ADDITIVE ? " additive" : "", s->refract ? " refract" : "",
                   s->persp ? " perspective" : "", s->tex_w, s->tex_h, s->nframes, s->st_draws,
                   s->st_drawn / n, s->st_culled / n, s->st_area / n, s->st_w / q, s->st_h / q,
                   s->soft.d1[0], s->soft.d2[0], s->soft.d1[1], s->soft.d2[1], s->soft.edge,
                   s->soft.ok ? "" : " (not measured)", s->st_lr, s->st_err, s->st_saved);
        g_free (base);
    }
    if (s->tex)
        walpe_gl_tex_release (s->tex);   /* shared, refcounted (glutil.c) */
    if (s->ntex)
        walpe_gl_tex_release (s->ntex);
    if (s->ftex)
        glDeleteTextures (1, &s->ftex);
    pt_live_sub (s->count);
    g_free (s->frames);
    g_free (s->tex_path);
    g_free (s->ntex_path);
    g_free (s->pts);
    g_free (s->hist);
    g_free (s->fol);
    g_free (s->rm_now);
    g_free (s->rm_who);
    g_rand_free (s->rng);
    g_free (s);
}

void walpe_weparticles_free (WalpeWeParticles *p)
{
    if (!p)
        return;
    sys_free (p->root);
    if (p->gl_ready && --pt_users <= 0) {
        /* the last particle object of this render thread: its scratch
         * memory and the stream buffer's store go too (the programs and
         * vertex array objects stay for the next wallpaper) */
        pt_users = 0;
        g_clear_pointer (&pt_scratch, g_free);
        g_clear_pointer (&pt_tmp, g_free);
        pt_scratch_cap = pt_tmp_cap = 0;
        if (pt_stream) {
            glBindBuffer (GL_ARRAY_BUFFER, pt_stream);
            glBufferData (GL_ARRAY_BUFFER, 0, NULL, GL_STREAM_DRAW);
        }
        lr_release ();
        pt_scene_release ();
    }
    g_free (p);
}

static gboolean sys_uses_audio (WeSys *s)
{
    for (int e = 0; e < s->nem; e++)
        if (s->em[e].audio)
            return TRUE;
    for (int c = 0; c < s->nchild; c++)
        if (sys_uses_audio (s->child[c].sys))
            return TRUE;
    return FALSE;
}

gboolean walpe_weparticles_uses_audio (WalpeWeParticles *p)
{
    return p && sys_uses_audio (p->root);
}
