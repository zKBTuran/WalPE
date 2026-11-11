/* bloom.h - bloom of a scene (general.bloom): bright parts of the finished
 * frame shine into their surroundings.
 *
 * The chain is Wallpaper Engine's standard one:
 *
 *   bright pass   the frame averaged over 4 x 4 pixels into a target of a
 *                 quarter of its size; of that colour c what lies above the
 *                 threshold stays: c * clamp (max (c) - threshold, 0, 1),
 *                 made more saturated (2 c - grey (c)), times strength and
 *                 tint, never below 0
 *   blur          at an eighth of the frame, a 13-tap Gaussian (sigma 7/3
 *                 taps) along x, then along y; the taps are 8 pixels apart
 *   add           the result, enlarged with linear filtering, is added to
 *                 the frame
 *
 * All values are the frame's own: display-referred 0..1, no conversion to
 * linear light (the standard chain has none), and strength / threshold /
 * tint are used as the scene file gives them (strength 2, threshold 0.65,
 * tint white when it names none).
 *
 * Sizes. Nothing assumes a resolution. The targets are fractions of the
 * frame that is being drawn (the render size); the distance between two
 * blur taps is 8 pixels of the reference size - the size the picture is
 * made for, which is what the scene's shaders see as the screen. Both are
 * the same unless the scene renders at a reduced scale; then the targets
 * (and the work) shrink with the frame while the glow keeps its reach on
 * the monitor.
 *
 * Cost. Per frame pixel the chain reads the frame once, shades 1/16 + 2/64
 * of a frame in small targets and blends one full-size quad. What it does
 * to keep that down:
 *   - a frame that already is a texture (reduced render scale) is read
 *     where it is; a frame in the window is copied once at HALF size by a
 *     linear blit (a quarter of the pixels of a full copy, and the first
 *     halving of the downsample at the same time);
 *   - a bloom that cannot change a pixel is not run at all
 *     (walpe_bloom_visible: strength 0, threshold 1, black tint);
 *   - level REDUCED (the quality knob post_processing = reduced) halves
 *     the two inner targets once more and blurs with 7 taps of the same
 *     Gaussian: a little softer, about a quarter of the small-target
 *     work (the copy and the final add stay: the saving is modest);
 *   - the chain runs only when the scene draws a frame: a scene in which
 *     nothing moves draws none, and its bloom with it.
 * GPU time has only been reasoned about, not measured on a graphics card.
 *
 * Formats. The intermediates are RGBA8, as the standard chain's are; what
 * exceeds 1 after the bright pass is clamped there. A scene that sets
 * general.hdr gets half-float intermediates when the driver can render to
 * them (asked once per context by attaching one; a tree with a capability
 * table asks it instead - bloom_format () is the one place). The HDR bloom
 * of Wallpaper Engine itself (its own thresholds, a chain of halvings) is
 * not implemented: such scenes get the standard chain with their standard
 * values.
 *
 * WALPE_BLOOM_FORMAT=8|16f forces the format of the intermediates,
 * WALPE_BLOOM_COPY=full copies a frame in the window 1:1 instead of at
 * half size (both for A/B comparisons). */
#pragma once
#include <glib.h>

G_BEGIN_DECLS

typedef struct WalpeBloom WalpeBloom;

enum { WALPE_BLOOM_FULL = 0, WALPE_BLOOM_REDUCED = 1 };

/* values a scene file may name; anything beyond is clamped */
#define WALPE_BLOOM_MAX_STRENGTH 100.0f
/* frames smaller than this on either side get no bloom */
#define WALPE_BLOOM_MIN_SIDE 16

typedef struct {
    float strength;       /* general.bloomstrength (2) */
    float threshold;      /* general.bloomthreshold (0.65) */
    float tint[3];        /* general.bloomtint (1 1 1) */
    int level;            /* WALPE_BLOOM_FULL | WALPE_BLOOM_REDUCED */
    gboolean hdr;         /* general.hdr: intermediates that do not clamp, where possible */
} WalpeBloomParams;

/* the finished frame */
typedef struct {
    guint fbo;            /* the framebuffer it is in (0 = the window) */
    guint tex;            /* the colour texture behind fbo when there is one that can be
                           * sampled, else 0 (the window: the frame is copied) */
    int w, h;             /* its size in pixels */
    int ref_w, ref_h;     /* the size the picture is made for (the monitor's, or the
                           * render size chosen for it); 0 = w, h */
} WalpeBloomFrame;

/* what the bright pass reads */
typedef enum {
    WALPE_BLOOM_SRC_TEXTURE = 0,   /* the frame's own texture */
    WALPE_BLOOM_SRC_HALF_COPY,     /* a copy of the window at half size (linear blit) */
    WALPE_BLOOM_SRC_FULL_COPY,     /* a 1:1 copy (a multisampled window cannot be scaled by a blit) */
} WalpeBloomSource;

/* sizes and steps of one run (no GL: tests look at it) */
typedef struct {
    WalpeBloomSource source;
    int src_w, src_h;          /* the texture the bright pass reads */
    int copy_w, copy_h;        /* the copy of the frame; 0 0 = none is made */
    int bright_w, bright_h;    /* target of the bright pass */
    int bright_taps;           /* linear lookups per axis that cover one target pixel: 1, 2 or 4 */
    int blur_w, blur_h;        /* the two blur targets */
    int blur_taps;             /* 13 or 7 */
    float blur_step[2];        /* distance of two blur taps in texture coordinates, x and y */
    guint64 px_small;          /* pixels shaded in the small targets (bright + 2 x blur) */
    guint64 px_add;            /* pixels of the final add (the frame) */
    guint64 bytes;             /* memory of the targets at 4 bytes a pixel */
} WalpeBloomPlan;

/* what a run did (accounting, profiler-independent) */
typedef struct {
    gboolean drawn;            /* the chain ran and was added */
    guint64 px;                /* pixels shaded: small targets + the add */
    guint passes;              /* draws: bright, blur x, blur y, add */
    gboolean copied;           /* the frame was copied first */
    gboolean half_float;       /* intermediates are RGBA16F */
} WalpeBloomStats;

/* ---- no GL --------------------------------------------------------- */

/* FALSE = these values cannot change a pixel of an 8- or 10-bit frame:
 * strength <= 0, threshold >= 1, no tint channel above 0, values that are
 * not numbers, or a glow that stays below a quarter of a 10-bit step even
 * for the brightest colour. The chain is then not run and the frame is
 * byte for byte the frame without bloom. */
gboolean walpe_bloom_visible (const WalpeBloomParams *p);
/* the bright pass on one colour (0..1 in; tint NULL = white) */
void walpe_bloom_bright (const float rgb[3], float strength, float threshold, const float tint[3],
                         float out[3]);
/* the weights of the blur for a level, centre first to last (symmetric,
 * sum 1); returns their number: 13 (full) or 7 (reduced: every second tap
 * of the same Gaussian) */
int walpe_bloom_kernel (int level, float w[13]);
/* the sizes of a run for a frame; FALSE = the frame is too small for bloom */
gboolean walpe_bloom_plan (int frame_w, int frame_h, int ref_w, int ref_h, WalpeBloomSource source,
                           int level, WalpeBloomPlan *plan);

/* ---- GL (render thread, context current) --------------------------- */

WalpeBloom *walpe_bloom_new (void);
/* Adds the bloom of the frame to the frame. Leaves bound / set what the
 * scene renderer expects after a layer: frame->fbo, a viewport of the
 * frame's size, texture unit 0, blending on with the premultiplied
 * function. The final add obeys the frame's clip ("Skip hidden areas");
 * everything before it needs the whole frame, so the caller draws the
 * layers unclipped in a frame that gets bloom.
 * TRUE = done, or nothing to do (walpe_bloom_visible, a tiny frame).
 * FALSE = this context cannot do it (no render targets, a shader that does
 * not build): nothing was drawn, the reason is logged once, and every
 * later call returns FALSE at once. stats may be NULL. */
gboolean walpe_bloom_apply (WalpeBloom *b, const WalpeBloomFrame *frame,
                            const WalpeBloomParams *p, WalpeBloomStats *stats);
/* give the render targets back (the scene is paused or bloom is off); the
 * next walpe_bloom_apply makes them again */
void walpe_bloom_drop_targets (WalpeBloom *b);
void walpe_bloom_free (WalpeBloom *b);   /* NULL is fine */

G_END_DECLS
