/* clip.c - see clip.h */
#include "engine/clip.h"
#include <epoxy/gl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    gboolean on;                    /* a clip is set */
    int suspend;                    /* draws are not clipped while > 0 */
    int n;                          /* boxes (0 = nothing visible) */
    WalpeBox box[WALPE_CLIP_MAX];   /* GL window coordinates (y up) of the target */
    int w, h;                       /* size of the frame target */
    GLuint target;                  /* framebuffer the frame is drawn into */
    guint serial;
    /* as given (y down, window size), for walpe_clip_retarget. Kept by
     * walpe_clip_off: the same boxes set again are the same clip and
     * keep their serial (the daemon sets the clip before every frame and
     * takes it off after it). */
    gboolean have0;
    int n0, w0, h0;
    WalpeBox box0[WALPE_CLIP_MAX];
} Clip;

static __thread Clip C;
static gint clip_serial_src;    /* process-wide, so a serial never repeats on a thread */

static void clip_place (GLuint target, int tw, int th, int pad)
{
    C.target = target;
    C.w = tw;
    C.h = th;
    C.n = 0;
    for (int i = 0; i < C.n0; i++) {
        const WalpeBox *b = &C.box0[i];
        /* window -> target pixels, rounded outwards; y flipped */
        int x0 = (int) floor ((double) b->x0 * tw / C.w0) - pad;
        int x1 = (int) ceil ((double) b->x1 * tw / C.w0) + pad;
        int yt = (int) floor ((double) b->y0 * th / C.h0) - pad;
        int yb = (int) ceil ((double) b->y1 * th / C.h0) + pad;
        WalpeBox g = { MAX (0, x0), MAX (0, th - yb), MIN (tw, x1), MIN (th, th - yt) };
        if (!walpe_box_empty (g))
            C.box[C.n++] = g;
    }
    if (pad > 0 && C.n > 1) {
        /* grown boxes may overlap: a blended draw would hit a pixel twice */
        WalpeRegion r;
        walpe_region_init (&r);
        walpe_region_set_boxes (&r, C.box, C.n);
        C.n = walpe_region_simplify (&r, C.box, WALPE_CLIP_MAX, 0);
        walpe_region_clear (&r);
    }
}

void walpe_clip_set (const WalpeBox *boxes, int n, int w, int h)
{
    if (n < 0 || w < 1 || h < 1) {
        walpe_clip_off ();
        return;
    }
    n = MIN (n, WALPE_CLIP_MAX);
    gboolean same = C.have0 && C.n0 == n && C.w0 == w && C.h0 == h &&
                    (n == 0 || !memcmp (C.box0, boxes, (size_t) n * sizeof (WalpeBox)));
    C.suspend = 0;
    if (same && C.on && C.target == 0)
        return;
    C.on = TRUE;
    C.have0 = TRUE;
    C.n0 = n;
    C.w0 = w;
    C.h0 = h;
    if (n)
        memcpy (C.box0, boxes, (size_t) n * sizeof (WalpeBox));
    clip_place (0, w, h, 0);
    if (!same) {    /* (the same boxes again: only placed again) */
        C.serial = (guint) g_atomic_int_add (&clip_serial_src, 1) + 1;
        if (!C.serial)      /* 0 means "no clip" */
            C.serial = (guint) g_atomic_int_add (&clip_serial_src, 1) + 1;
    }
}

void walpe_clip_off (void)
{
    C.on = FALSE;
    C.suspend = 0;
    C.n = 0;
    C.target = 0;
}

gboolean walpe_clip_active (void)
{
    return C.on && !C.suspend;
}

guint walpe_clip_serial (void)
{
    return C.on ? C.serial : 0;
}

int walpe_clip_boxes (WalpeBox *out)
{
    if (!C.on || C.suspend)
        return -1;
    memcpy (out, C.box, (size_t) C.n * sizeof (WalpeBox));
    return C.n;
}

void walpe_clip_retarget (guint fbo, int tw, int th)
{
    if (!C.on || tw < 1 || th < 1)
        return;
    clip_place (fbo, tw, th, (tw != C.w0 || th != C.h0) ? 1 : 0);
}

void walpe_clip_suspend (gboolean s)
{
    C.suspend = s ? 1 : 0;
}

/* statistics of the frame in progress */
static __thread struct {
    int on;             /* -1 = not read yet, 0 = off, else frames per line */
    GLuint query;
    gboolean running;
    guint frames;
    guint64 frags;
    guint draws, clipped_draws;
    guint fx_full, fx_clipped, fx_skipped;
} P = { .on = -1 };

gboolean walpe_clip_next (int *it, const float *ndc)
{
    int i = *it;
    if (!C.on || C.suspend) {
        *it = i + 1;
        if (i == 0 && P.on > 0)
            P.draws++;
        return i == 0;
    }
    if (i < 0)
        return FALSE;               /* the one pass of a draw that is not ours */
    if (i == 0) {
        GLint fb = 0;
        glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &fb);
        if ((GLuint) fb != C.target) {
            *it = -1;               /* an effect target, a capture: never clipped */
            return TRUE;
        }
    }
    for (; i < C.n; i++) {
        const WalpeBox *b = &C.box[i];
        if (ndc) {
            /* the draw's bounds in target pixels, one pixel of slack */
            float px0 = (ndc[0] * 0.5f + 0.5f) * (float) C.w - 1.0f;
            float px1 = (ndc[2] * 0.5f + 0.5f) * (float) C.w + 1.0f;
            float py0 = (ndc[1] * 0.5f + 0.5f) * (float) C.h - 1.0f;
            float py1 = (ndc[3] * 0.5f + 0.5f) * (float) C.h + 1.0f;
            if (px1 < (float) b->x0 || px0 > (float) b->x1 || py1 < (float) b->y0 ||
                py0 > (float) b->y1)
                continue;
        }
        if (*it == 0)
            glEnable (GL_SCISSOR_TEST);
        glScissor (b->x0, b->y0, b->x1 - b->x0, b->y1 - b->y0);
        *it = i + 1;
        if (P.on > 0)
            P.clipped_draws++;
        return TRUE;
    }
    if (*it > 0)
        glDisable (GL_SCISSOR_TEST);
    *it = C.n + 1;
    return FALSE;
}

gboolean walpe_clip_hits (float x0, float y0, float x1, float y1)
{
    if (!C.on || C.suspend)
        return TRUE;
    float px0 = (x0 * 0.5f + 0.5f) * (float) C.w - 1.0f;
    float px1 = (x1 * 0.5f + 0.5f) * (float) C.w + 1.0f;
    float py0 = (y0 * 0.5f + 0.5f) * (float) C.h - 1.0f;
    float py1 = (y1 * 0.5f + 0.5f) * (float) C.h + 1.0f;
    if (!(px1 >= px0) || !(py1 >= py0))
        return TRUE;                /* NaN: let it be drawn */
    for (int i = 0; i < C.n; i++) {
        const WalpeBox *b = &C.box[i];
        if (!(px1 < (float) b->x0 || px0 > (float) b->x1 || py1 < (float) b->y0 ||
              py0 > (float) b->y1))
            return TRUE;
    }
    return FALSE;
}

int walpe_clip_quad_uv (const float *m, float uv[][4])
{
    if (!C.on || C.suspend)
        return -1;
    /* clip = M q (q = quad corner in [-1,1]^2): q = M^-1 clip */
    double a = m[0], b = m[3], c = m[1], d = m[4], tx = m[6], ty = m[7];
    double det = a * d - b * c;
    if (!(fabs (det) > 1e-12) || !isfinite (det) || !isfinite (tx) || !isfinite (ty))
        return -1;
    int n = 0;
    for (int i = 0; i < C.n; i++) {
        const WalpeBox *bx = &C.box[i];
        double u0 = 2, v0 = 2, u1 = -1, v1 = -1;
        for (int k = 0; k < 4; k++) {
            double px = (k & 1) ? bx->x1 : bx->x0, py = (k & 2) ? bx->y1 : bx->y0;
            double cx = px / C.w * 2.0 - 1.0 - tx, cy = py / C.h * 2.0 - 1.0 - ty;
            double qx = (d * cx - b * cy) / det, qy = (-c * cx + a * cy) / det;
            double u = (qx + 1.0) * 0.5, v = (1.0 - qy) * 0.5;
            u0 = MIN (u0, u); u1 = MAX (u1, u);
            v0 = MIN (v0, v); v1 = MAX (v1, v);
        }
        if (!(u1 > 0.0 && v1 > 0.0 && u0 < 1.0 && v0 < 1.0))
            continue;                   /* this box shows nothing of it (or NaN) */
        uv[n][0] = (float) CLAMP (u0, 0.0, 1.0);
        uv[n][1] = (float) CLAMP (v0, 0.0, 1.0);
        uv[n][2] = (float) CLAMP (u1, 0.0, 1.0);
        uv[n][3] = (float) CLAMP (v1, 0.0, 1.0);
        n++;
    }
    return n;
}

int walpe_clip_rect_uv (int x0, int y0, int x1, int y1, gboolean flip_x, gboolean flip_y,
                        float uv[][4])
{
    if (!C.on || C.suspend || x1 <= x0 || y1 <= y0)
        return -1;
    const WalpeBox r = { x0, y0, x1, y1 };
    float w = (float) (x1 - x0), h = (float) (y1 - y0);
    int n = 0;
    for (int i = 0; i < C.n; i++) {
        WalpeBox e = walpe_box_intersect (C.box[i], r);
        if (walpe_box_empty (e))
            continue;
        float u0 = (float) (e.x0 - x0) / w, u1 = (float) (e.x1 - x0) / w;
        float v0 = (float) (y1 - e.y1) / h, v1 = (float) (y1 - e.y0) / h;   /* row 0 = top */
        if (flip_x) {
            float t = 1.0f - u1;
            u1 = 1.0f - u0;
            u0 = t;
        }
        if (flip_y) {
            float t = 1.0f - v1;
            v1 = 1.0f - v0;
            v0 = t;
        }
        uv[n][0] = u0;
        uv[n][1] = v0;
        uv[n][2] = u1;
        uv[n][3] = v1;
        n++;
    }
    return n;
}

/* ------------------------------------------------------------ measuring */

static gboolean pixstat_on (void)
{
    if (P.on < 0) {
        const char *e = g_getenv ("WALPE_PIX_STATS");
        P.on = e && *e ? MAX (1, atoi (e)) : 0;
        if (P.on && !epoxy_is_desktop_gl ()) {
            g_message ("WALPE_PIX_STATS: needs desktop GL (GL_SAMPLES_PASSED); off");
            P.on = 0;
        }
    }
    return P.on > 0;
}

void walpe_pixstat_begin (void)
{
    if (!pixstat_on () || P.running)
        return;
    if (!P.query)
        glGenQueries (1, &P.query);
    glBeginQuery (GL_SAMPLES_PASSED, P.query);
    P.running = TRUE;
}

void walpe_pixstat_fx (int full, int clipped, int skipped)
{
    if (P.on <= 0)
        return;
    P.fx_full += (guint) full;
    P.fx_clipped += (guint) clipped;
    P.fx_skipped += (guint) skipped;
}

void walpe_pixstat_end (const char *tag, int w, int h)
{
    if (P.on <= 0 || !P.running)
        return;
    P.running = FALSE;
    glEndQuery (GL_SAMPLES_PASSED);
    GLuint v = 0;
    glGetQueryObjectuiv (P.query, GL_QUERY_RESULT, &v);
    P.frags += v;
    if (++P.frames < (guint) P.on)
        return;
    double f = (double) P.frames;
    gint64 vis = 0;
    for (int i = 0; i < C.n; i++)
        vis += walpe_box_area (C.box[i]);
    g_message ("pixstat %s: %.0f fragments/frame (%.2f x the %dx%d window), %.1f draws/frame "
               "(%.1f of them clipped), fx passes/frame: %.1f full %.1f clipped %.1f skipped; "
               "clip %s%d box(es) %.1f%% of the window (avg of %u frames)",
               tag ? tag : "", (double) P.frags / f,
               w > 0 && h > 0 ? (double) P.frags / f / ((double) w * h) : 0.0, w, h,
               (P.draws + P.clipped_draws) / f, P.clipped_draws / f, P.fx_full / f,
               P.fx_clipped / f, P.fx_skipped / f, C.on ? "" : "off, ", C.n,
               C.on && C.w > 0 ? 100.0 * (double) vis / ((double) C.w * C.h) : 100.0, P.frames);
    P.frames = 0;
    P.frags = 0;
    P.draws = P.clipped_draws = 0;
    P.fx_full = P.fx_clipped = P.fx_skipped = 0;
}
