/* puppet_test.c - puppet-warp models (.mdl) with hostile animation headers
 * (version 0.12 audit). The model is written here, loaded with the real loader
 * and skinned at several times; the GL calls of walpe_puppet_draw are
 * replaced by functions that read the buffers they are handed, so the test
 * needs no display. A wrong frame index reads outside the frame table:
 * AddressSanitizer reports it, a plain build crashes.
 *
 *   - "length" = INT_MAX with a huge fps: the next frame's index wrapped
 *     to INT_MIN and indexed ~77 GB below the table
 *   - loop / mirror / single with a negative rate and absurd times
 *   - a sane animation still interpolates between its frames
 *   - the vertex buffer is sent when the pose or the layer's size changed,
 *     not with every draw, and always holds what a fresh upload would
 *
 * puppet.c is included so the test can see the pose it computes.
 * Run: meson test -C build puppet -v */
#include "engine/glutil.h"
#define walpe_glutil_draw_mesh_tint test_draw_mesh
static void test_draw_mesh (GLuint tex, gboolean premult, float opacity, const float *mvp,
                            int blend, const float tint[3], GLuint vao, int nidx)
{
    (void) tex; (void) premult; (void) opacity; (void) mvp; (void) blend; (void) tint;
    (void) vao; (void) nidx;
}
#include "engine/puppet.c"
#include <glib/gstdio.h>
#include <stdlib.h>
#include <unistd.h>

static int failures;
#define CHECK(c) do { if (!(c)) { g_printerr ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                                 failures++; } } while (0)

/* ------------------------------------------------------------ GL stand-ins */
static void s_gen (GLsizei n, GLuint *a) { for (int i = 0; i < n; i++) a[i] = 1; }
static void s_bind1 (GLuint a) { (void) a; }
static void s_bind2 (GLenum t, GLuint a) { (void) t; (void) a; }
static void s_read (const void *d, GLsizeiptr sz)
{
    volatile unsigned s = 0;   /* a driver reads sz bytes of d */
    const unsigned char *p = d;
    for (GLsizeiptr i = 0; p && i < sz; i++)
        s += p[i];
    (void) s;   /* only added to: GCC 16 says so (-Wunused-but-set-variable=3) */
}
static void s_bufdata (GLenum t, GLsizeiptr sz, const void *d, GLenum u) { (void) t; (void) u; s_read (d, sz); }
/* the vertex buffer "on the GPU" of the puppet being drawn, and how often it was written */
static unsigned char *gpu_buf;
static int sub_calls;
static void s_bufsub (GLenum t, GLintptr o, GLsizeiptr sz, const void *d)
{
    (void) t;
    s_read (d, sz);
    sub_calls++;
    if (gpu_buf && o == 0 && sz <= 64)
        memcpy (gpu_buf, d, (size_t) sz);
}
static void s_en (GLuint a) { (void) a; }
static void s_vap (GLuint i, GLint n, GLenum t, GLboolean no, GLsizei st, const void *p)
{ (void) i; (void) n; (void) t; (void) no; (void) st; (void) p; }
static void s_del (GLsizei n, const GLuint *a) { (void) n; (void) a; }

/* ------------------------------------------------------------ .mdl writer */
static void put32 (GByteArray *a, guint32 v)
{
    guint8 b[4] = { (guint8) v, (guint8) (v >> 8), (guint8) (v >> 16), (guint8) (v >> 24) };
    g_byte_array_append (a, b, 4);
}
static void put16 (GByteArray *a, guint16 v)
{
    guint8 b[2] = { (guint8) v, (guint8) (v >> 8) };
    g_byte_array_append (a, b, 2);
}
static void putf (GByteArray *a, float f)
{
    guint32 u;
    memcpy (&u, &f, 4);
    put32 (a, u);
}
static void puts0 (GByteArray *a, const char *s)
{
    g_byte_array_append (a, (const guint8 *) s, (guint) strlen (s) + 1);
}

static int mdat_mode;   /* write_mdl: the attachment section, 0 = none */

/* a quad on two bones (root, arm) with one animation of `nframes` stored
 * frames per bone: frame i moves the bone to x = i */
static char *write_mdl (const char *dir, const char *name, const char *mode, float fps,
                        guint32 length, int nframes)
{
    GByteArray *b = g_byte_array_new ();
    g_byte_array_append (b, (const guint8 *) "MDLV0019\0", 9);
    put32 (b, 0); put32 (b, 1); put32 (b, 1);
    puts0 (b, "materials/x.json");
    put32 (b, 0);
    put32 (b, 0x0180000F);
    put32 (b, 4 * 80);
    static const float xs[4] = { -50, 50, 50, -50 }, ys[4] = { -50, -50, 50, 50 };
    static const float us[4] = { 0, 1, 1, 0 }, vs[4] = { 1, 1, 0, 0 };
    for (int i = 0; i < 4; i++) {
        const float pnt[10] = { xs[i], ys[i], 0, 0, 0, 1, 1, 0, 0, 1 };
        for (int k = 0; k < 10; k++)
            putf (b, pnt[k]);
        put32 (b, 1); put32 (b, 0); put32 (b, 0); put32 (b, 0);   /* bone ids: arm */
        putf (b, 1); putf (b, 0); putf (b, 0); putf (b, 0);       /* weights */
        putf (b, us[i]); putf (b, vs[i]);
    }
    put32 (b, 12);
    static const guint16 idx[6] = { 0, 1, 2, 0, 2, 3 };
    for (int i = 0; i < 6; i++)
        put16 (b, idx[i]);
    /* bones */
    gsize ms = b->len;
    GByteArray *body = g_byte_array_new ();
    put16 (body, 2); put16 (body, 0);
    for (int i = 0; i < 2; i++) {
        puts0 (body, i ? "arm" : "root");
        put32 (body, 0);
        put32 (body, (guint32) (i - 1));
        put32 (body, 64);
        for (int k = 0; k < 16; k++)
            putf (body, k % 5 == 0 ? 1.0f : 0.0f);   /* identity */
        puts0 (body, "{}");
    }
    g_byte_array_append (b, (const guint8 *) "MDLS0002\0", 9);
    put32 (b, (guint32) (ms + 9 + 4 + body->len));
    g_byte_array_append (b, body->data, body->len);
    g_byte_array_unref (body);
    /* attachment points (mdat_mode): 1 = "hand" on the arm, 10 right of and
     * 20 above it, and one that names a bone the model does not have;
     * 2 = a count of 65535 with one point behind it; 3 = a point cut off
     * inside its matrix */
    if (mdat_mode) {
        gsize md = b->len;
        body = g_byte_array_new ();
        put16 (body, mdat_mode == 1 ? 2 : mdat_mode == 2 ? 0xffff : 3);
        put16 (body, 1);
        puts0 (body, "hand");
        for (int k = 0; k < (mdat_mode == 3 ? 8 : 16); k++)
            putf (body, k % 5 == 0 ? 1.0f : k == 12 ? 10.0f : k == 13 ? 20.0f : 0.0f);
        if (mdat_mode == 1) {
            put16 (body, 7);
            puts0 (body, "nowhere");
            for (int k = 0; k < 16; k++)
                putf (body, k % 5 == 0 ? 1.0f : 0.0f);
        }
        g_byte_array_append (b, (const guint8 *) "MDAT0001\0", 9);
        put32 (b, (guint32) (md + 9 + 4 + body->len));
        g_byte_array_append (b, body->data, body->len);
        g_byte_array_unref (body);
    }
    /* one animation */
    g_byte_array_append (b, (const guint8 *) "MDLA0003\0", 9);
    put32 (b, 0);
    put32 (b, 1);
    put32 (b, 1); put32 (b, 0);
    puts0 (b, "a");
    puts0 (b, mode);
    putf (b, fps);
    put32 (b, length);
    put32 (b, 0);
    put32 (b, 2);
    for (int bone = 0; bone < 2; bone++) {
        put32 (b, 0);
        put32 (b, (guint32) nframes * 36);
        for (int f = 0; f < nframes; f++) {
            const float fr[9] = { (float) f, 0, 0, 0, 0, 0, 1, 1, 1 };
            for (int k = 0; k < 9; k++)
                putf (b, fr[k]);
        }
    }
    char *path = g_build_filename (dir, name, NULL);
    if (!g_file_set_contents (path, (const char *) b->data, b->len, NULL))
        g_error ("cannot write %s", path);
    g_byte_array_unref (b);
    return path;
}

static WalpePuppet *load_with_layers (const char *path, float rate)
{
    WalpePuppet *p = walpe_puppet_load (path);
    if (p && p->nanims > 0) {
        /* as the scene's "animationlayers" would bind them */
        p->layers = g_new0 (PLayer, 2);
        p->layers[p->nlayers++] = (PLayer) { 0, rate, 1.0f, TRUE, FALSE };
        p->layers[p->nlayers++] = (PLayer) { 0, -2.5f * rate, 0.5f, TRUE, TRUE };
    }
    return p;
}

static void draw_times (WalpePuppet *p)
{
    static const double ts[] = { 0.0, 0.016, 0.5, 1.0, 7.3, 3600.0, 1e9, 1e30, -5.0 };
    const float mvp[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 }, tint[3] = { 1, 1, 1 };
    for (guint i = 0; i < G_N_ELEMENTS (ts); i++)
        walpe_puppet_draw (p, ts[i], 100, 100, 1, FALSE, 1, mvp, 0, tint);
}

int main (int argc, char **argv)
{
    (void) argc; (void) argv;
    epoxy_glGenVertexArrays = s_gen;
    epoxy_glBindVertexArray = s_bind1;
    epoxy_glGenBuffers = s_gen;
    epoxy_glBindBuffer = s_bind2;
    epoxy_glBufferData = s_bufdata;
    epoxy_glBufferSubData = s_bufsub;
    epoxy_glEnableVertexAttribArray = s_en;
    epoxy_glVertexAttribPointer = s_vap;
    epoxy_glDeleteBuffers = s_del;
    epoxy_glDeleteVertexArrays = s_del;

    char *dir = g_dir_make_tmp ("walpe-puppet-test-XXXXXX", NULL);
    if (!dir)
        g_error ("no temp dir");

    /* a sane animation: 4 frames, 30 fps, length 3 */
    char *path = write_mdl (dir, "ok.mdl", "single", 30.0f, 3, 4);
    WalpePuppet *p = walpe_puppet_load (path);
    CHECK (p && p->nverts == 4 && p->nidx == 6 && p->nbones == 2 && p->nanims == 1);
    if (p && p->nanims == 1) {
        CHECK (p->anims[0].nframes == 4 && p->anims[0].length == 3);
        PFrame f;
        anim_sample (&p->anims[0], 1, 0.05, 1.0f, &f);      /* frame 1.5 */
        CHECK (fabsf (f.p[0] - 1.5f) < 1e-4f);
        anim_sample (&p->anims[0], 1, 100.0, 1.0f, &f);     /* past the end: last frame */
        CHECK (fabsf (f.p[0] - 3.0f) < 1e-4f);
        anim_sample (&p->anims[0], 1, -1.0, 1.0f, &f);      /* before the start */
        CHECK (fabsf (f.p[0]) < 1e-4f);
    }
    walpe_puppet_free (p);
    g_unlink (path);
    g_free (path);

    /* loop: frame index wraps to the first frame */
    path = write_mdl (dir, "loop.mdl", "loop", 30.0f, 4, 4);
    p = walpe_puppet_load (path);
    CHECK (p && p->nanims == 1);
    if (p && p->nanims == 1) {
        PFrame f;
        anim_sample (&p->anims[0], 0, 3.5 / 30.0, 1.0f, &f);   /* between frame 3 and frame 0 */
        CHECK (fabsf (f.p[0] - 1.5f) < 1e-4f);
    }
    walpe_puppet_free (p);
    g_unlink (path);
    g_free (path);

    /* "length" INT_MAX / 0xffffffff, fps 1e30 or infinite, each mode, both
     * directions: must stay inside the 4 stored frames */
    static const char *const modes[] = { "single", "mirror", "loop" };
    static const guint32 lengths[] = { 0x7fffffffu, 0x7ffffffeu, 0xffffffffu, 1u << 30 };
    static const float fpss[] = { 1e30f, 3e38f, 1e-30f, 30.0f };
    for (guint m = 0; m < G_N_ELEMENTS (modes); m++)
        for (guint l = 0; l < G_N_ELEMENTS (lengths); l++)
            for (guint f = 0; f < G_N_ELEMENTS (fpss); f++)
                for (int dir_sign = -1; dir_sign <= 1; dir_sign += 2) {
                    path = write_mdl (dir, "hostile.mdl", modes[m], fpss[f], lengths[l], 4);
                    p = load_with_layers (path, (float) dir_sign);
                    if (p) {
                        draw_times (p);
                        if (p->nanims == 1) {
                            PFrame fr;
                            anim_sample (&p->anims[0], 1, 1e9, (float) dir_sign, &fr);
                            CHECK (fr.p[0] >= -1e-4f && fr.p[0] <= 3.0001f);
                        }
                        walpe_puppet_gl_free (p);
                        walpe_puppet_free (p);
                    }
                    g_unlink (path);
                    g_free (path);
                }

    /* attachment points: a frame fixed to a bone, followed through the
     * animation */
    mdat_mode = 1;
    path = write_mdl (dir, "att.mdl", "single", 30.0f, 3, 4);
    p = walpe_puppet_load (path);
    CHECK (p && p->nanims == 1 && walpe_puppet_attachment_count (p) == 1);
    if (p && p->nanims == 1) {
        float m[6] = { 0 };
        /* nothing played: the first frame's pose */
        CHECK (walpe_puppet_attachment (p, "hand", 0.0, m));
        CHECK (fabsf (m[0] - 1) < 1e-5f && fabsf (m[3] - 1) < 1e-5f && fabsf (m[1]) < 1e-5f);
        CHECK (fabsf (m[4] - 10) < 1e-4f && fabsf (m[5] - 20) < 1e-4f);
        CHECK (!walpe_puppet_attachment (p, "nowhere", 0.0, m));
        CHECK (!walpe_puppet_attachment (p, "nose", 0.0, m));
        /* the animation playing: at frame 1.5 both bones are 1.5 to the
         * right of their parent, the arm 3 in all */
        p->layers = g_new0 (PLayer, 1);
        p->layers[p->nlayers++] = (PLayer) { 0, 1.0f, 1.0f, TRUE, FALSE };
        p->pose_ok = FALSE;
        CHECK (walpe_puppet_attachment (p, "hand", 0.05, m));
        CHECK (fabsf (m[4] - 13) < 1e-3f && fabsf (m[5] - 20) < 1e-4f);
        CHECK (walpe_puppet_attachment (p, "hand", 100.0, m) && fabsf (m[4] - 16) < 1e-3f);
        draw_times (p);
        walpe_puppet_gl_free (p);
    }
    walpe_puppet_free (p);
    g_unlink (path);
    g_free (path);
    /* a count the section cannot hold, a point cut off inside its matrix:
     * what is whole is taken, nothing is read outside the section */
    for (mdat_mode = 2; mdat_mode <= 3; mdat_mode++) {
        path = write_mdl (dir, "atthostile.mdl", "loop", 30.0f, 3, 4);
        p = walpe_puppet_load (path);
        CHECK (p && p->nanims == 1 && walpe_puppet_attachment_count (p) == (mdat_mode == 2 ? 1 : 0));
        float m[6];
        CHECK (p && walpe_puppet_attachment (p, "hand", 0.5, m) == (mdat_mode == 2));
        walpe_puppet_free (p);
        g_unlink (path);
        g_free (path);
    }
    mdat_mode = 0;

    /* the vertex data goes to the GPU when it changed: p is drawn as a
     * scene draws it, q gets a new buffer before every draw and so sends
     * every time; both buffers must hold the same bytes after each step */
    path = write_mdl (dir, "rest.mdl", "single", 30.0f, 3, 4);
    p = walpe_puppet_load (path);
    WalpePuppet *q = walpe_puppet_load (path);
    CHECK (p && q && p->nanims == 1 && p->nverts == 4);
    if (p && q && p->nanims == 1 && p->nverts == 4) {
        static const struct { double t; float hw, hh; int sends; } steps[] = {
            { 0.05, 100, 100, 1 },    /* the first draw */
            { 0.05, 100, 100, 0 },    /* the same pose again */
            { 0.06, 100, 100, 1 },    /* the animation moved on */
            { 100.0, 100, 100, 1 },   /* its last frame ("single") */
            { 250.0, 100, 100, 0 },   /* still its last frame */
            { 250.0, 100, 50, 1 },    /* the layer has another size */
            { 250.0, 100, 50, 0 },
            { 0.05, 100, 50, 1 },     /* back to an earlier pose */
        };
        const float mvp[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 }, tint[3] = { 1, 1, 1 };
        unsigned char buf_p[64], buf_q[64];
        memset (buf_p, 0xaa, sizeof buf_p);
        memset (buf_q, 0x55, sizeof buf_q);
        p->layers = g_new0 (PLayer, 1);
        p->layers[p->nlayers++] = (PLayer) { 0, 1.0f, 1.0f, TRUE, FALSE };
        q->layers = g_new0 (PLayer, 1);
        q->layers[q->nlayers++] = (PLayer) { 0, 1.0f, 1.0f, TRUE, FALSE };
        for (guint i = 0; i < G_N_ELEMENTS (steps); i++) {
            int n0 = sub_calls;
            gpu_buf = buf_p;
            walpe_puppet_draw (p, steps[i].t, steps[i].hw, steps[i].hh, 1, FALSE, 1, mvp, 0, tint);
            if (sub_calls - n0 != steps[i].sends) {
                g_printerr ("FAIL %s:%d: step %u sent the vertex buffer %d times, want %d\n", __FILE__,
                            __LINE__, i, sub_calls - n0, steps[i].sends);
                failures++;
            }
            gpu_buf = buf_q;
            walpe_puppet_gl_free (q);
            n0 = sub_calls;
            walpe_puppet_draw (q, steps[i].t, steps[i].hw, steps[i].hh, 1, FALSE, 1, mvp, 0, tint);
            CHECK (sub_calls - n0 == 1);
            if (memcmp (buf_p, buf_q, sizeof buf_p)) {
                g_printerr ("FAIL %s:%d: step %u: the buffer does not hold what a fresh upload holds\n",
                            __FILE__, __LINE__, i);
                failures++;
            }
        }
        /* a new buffer (the wallpaper was paused and its GL objects given
         * back) is filled whatever the pose */
        int n0 = sub_calls;
        gpu_buf = buf_p;
        walpe_puppet_gl_free (p);
        walpe_puppet_draw (p, 0.05, 100, 50, 1, FALSE, 1, mvp, 0, tint);
        CHECK (sub_calls - n0 == 1);
        gpu_buf = NULL;
        walpe_puppet_gl_free (p);
        walpe_puppet_gl_free (q);
    }
    walpe_puppet_free (p);
    walpe_puppet_free (q);
    g_unlink (path);
    g_free (path);

    g_rmdir (dir);
    g_free (dir);
    g_printerr ("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures,
                failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
