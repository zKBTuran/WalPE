/* clip.h - "Skip hidden areas": the part of the frame that can be seen.
 *
 * The daemon knows which pixels of a monitor other windows leave visible
 * (src/daemon/vistrack.c). Its render thread hands them over before each
 * frame (walpe_clip_set) and takes them back after it (walpe_clip_off);
 * everything here is per thread, like the GL context.
 *
 * Renderers do not test anything themselves: every draw that goes to the
 * frame (the layer composites, particles, a video's or image's quad) is
 * wrapped in
 *
 *     for (int it = 0; walpe_clip_next (&it, NULL);)
 *         glDrawArrays (...);
 *
 * which runs the draw once per visible box with that box as the scissor
 * rectangle. Without a clip the body runs exactly once and no GL state is
 * touched: the GL calls of a frame are then the same as before this file
 * existed. Draws into other framebuffers (effect targets, captures) are
 * recognised by the bound draw framebuffer and never clipped here - the
 * scissor test is on only inside such a loop.
 *
 * What a clipped frame guarantees: the pixels inside the boxes are the
 * ones the unclipped frame has. Pixels outside keep whatever the frame
 * target held (the scene clears the whole target first, so: its clear
 * colour). */
#pragma once
#include <glib.h>
#include "common/region.h"

#define WALPE_CLIP_MAX 6     /* scissor boxes per frame */

/* render thread, before content->gl_render: boxes of the w x h window
 * that can be seen (X orientation: y down), not overlapping. n = 0 =
 * nothing can be seen (every wrapped draw is skipped). */
void walpe_clip_set (const WalpeBox *boxes, int n, int w, int h);
/* no clip: every draw goes everywhere (the state of a new thread) */
void walpe_clip_off (void);
/* a clip is set and not suspended */
gboolean walpe_clip_active (void);
/* changes whenever the boxes do (0 = no clip) and only then - setting the
 * same boxes again, also after walpe_clip_off, gives the same number. A
 * renderer that keeps pixels between frames (a frame it does not draw
 * again, a copy of layers that stand still) must draw again when it
 * differs: such pixels were made inside the boxes of that clip only. */
guint walpe_clip_serial (void);
/* the boxes in the frame target's pixels, GL orientation (y up), into
 * out[WALPE_CLIP_MAX]; returns their number, -1 = no clip */
int walpe_clip_boxes (WalpeBox *out);
/* the frame is drawn into framebuffer `fbo` of tw x th instead of the
 * window (a reduced render size): the boxes are scaled to it, rounded
 * outwards plus the texel a linear upscale reads. Lasts until the next
 * walpe_clip_set / walpe_clip_off. */
void walpe_clip_retarget (guint fbo, int tw, int th);
/* s = TRUE: draws from here on are not clipped (a later layer reads the
 * frame at places the clip cannot bound); FALSE: clipped again */
void walpe_clip_suspend (gboolean s);

/* The draw loop (see above). ndc = the draw's bounds in clip space
 * (x0, y0, x1, y1), NULL = unknown: boxes it cannot touch are skipped. */
gboolean walpe_clip_next (int *it, const float *ndc);
/* the clip leaves something of this clip-space rectangle (TRUE without a
 * clip); one pixel of slack */
gboolean walpe_clip_hits (float x0, float y0, float x1, float y1);
/* For a quad drawn with the 3x3 matrix `mvp` (unit quad, corners +-1,
 * texture v = 0 at its +y edge: walpe_glutil_draw_quad): the parts of its
 * texture the visible boxes show, one rectangle u0, v0, u1, v1 within
 * [0, 1] per box that touches it, into uv[WALPE_CLIP_MAX][4]. Returns
 * their number (0 = none of it is seen), or -1 = all of it (no clip, or
 * the matrix cannot be inverted). */
int walpe_clip_quad_uv (const float *mvp, float uv[][4]);
/* the same for an axis-aligned rectangle of the frame target given in
 * pixels (GL orientation, y up) whose texture row 0 is its top row unless
 * flip_y, and whose column 0 is its left column unless flip_x */
int walpe_clip_rect_uv (int x0, int y0, int x1, int y1, gboolean flip_x, gboolean flip_y,
                        float uv[][4]);

/* ---- measuring (WALPE_PIX_STATS=<frames>): fragments shaded per frame,
 * counted by the GL itself (a GL_SAMPLES_PASSED query around the frame;
 * clears and blits are not fragments and are not in it). One line every
 * <frames> frames per render thread. Costs a query read-back per frame:
 * diagnostics only. */
void walpe_pixstat_begin (void);
void walpe_pixstat_end (const char *tag, int w, int h);   /* the window's size */
/* effect passes of the current frame: drawn in full, drawn clipped,
 * skipped because nothing of their output is needed */
void walpe_pixstat_fx (int full, int clipped, int skipped);
