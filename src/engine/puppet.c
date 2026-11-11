/* puppet - WE puppet-warp layers (see puppet.h)
 *
 * File layout (reverse-engineered from MDLV0017/0019/0023 files of the test
 * scenes; all little endian):
 *   "MDLVnnnn\0" u32 flags, u32, u32, material path\0, padding ...
 *   u32 vertex-format word (0x0180000F), u32 vertex bytes, vertices of 80 B:
 *     pos f32[3], normal f32[3], tangent f32[4], bone ids u32[4],
 *     weights f32[4], uv f32[2]   (pos = image pixels around the centre, Y up)
 *   u32 index bytes, u16 triangle indices
 *   ... "MDLSnnnn\0" u32 end offset, u16 bone count, u16,
 *     per bone: name\0, u32, i32 parent, u32 64, f32[16] local bind matrix
 *     (column major, relative to the parent), simulation json\0
 *   ... "MDATnnnn\0" u32 end offset, u16 attachment count (optional section,
 *     between the bones and the animations),
 *     per attachment: u16 bone, name\0, f32[16] matrix relative to the bone
 *   ... "MDLAnnnn\0" u32 end offset, u32 animation count,
 *     per animation: i32 id, u32, name\0, mode\0 (loop/mirror/single),
 *     f32 fps, i32 length (frames), u32, u32 bone count,
 *     per bone: u32, u32 bytes, frames of f32[9] (pos xyz, angles xyz in
 *     radians, scale xyz), local to the parent; a few trailing bytes follow.
 * The bind pose is where the bones sit on the texture; the animation moves
 * them (part sheets are assembled only by the animation). */
#include "puppet.h"
#include "propprobe.h"
#include "glutil.h"
#include "common/util.h"
#include <glib/gstdio.h>
#include <math.h>
#include <sys/stat.h>
#include <string.h>

#define VSTRIDE 80
#define PUPPET_MAX_FILE_BYTES ((goffset) 256 * 1024 * 1024)

typedef struct { float p[3], a[3], s[3]; } PFrame;

typedef struct {
    int id;
    char *name;
    int mode;            /* 0 loop, 1 mirror, 2 single */
    float fps;
    int length;          /* frames */
    int nbones;
    int nframes;         /* stored frames per bone (usually length + 1) */
    PFrame *frames;      /* [bone * nframes + frame] */
} PAnim;

typedef struct {
    int anim;            /* index into anims */
    float rate, blend;
    gboolean visible, additive;
} PLayer;

typedef struct { float m[6]; } Aff;   /* 2D affine: x' = m0 x + m2 y + m4, y' = m1 x + m3 y + m5 */

/* an attachment point: a frame fixed to a bone that other layers of the
 * scene can be attached to (scene object "attachment": name) */
typedef struct {
    char *name;
    int bone;
    Aff local;           /* relative to the bone */
} PAttach;

struct WalpePuppet {
    int nverts, nidx;
    float *pos;          /* nverts * 2 */
    float *uv;           /* nverts * 2 */
    guint8 *bone;        /* nverts * 4 */
    float *weight;       /* nverts * 4 */
    guint16 *idx;
    int nbones;
    int *parent;
    Aff *bind_local;
    Aff *bind_inv_world;
    PAnim *anims;
    int nanims;
    PLayer *layers;
    int nlayers;
    PAttach *att;
    int natt;
    /* the bones' world matrices of the last attachment query, and its time
     * (several layers attached to one puppet ask in the same frame) */
    Aff *pose_world;
    double pose_t;
    gboolean pose_ok;
    /* GL (render thread) */
    GLuint vao, vbo, ebo;
    float *vbuf;         /* nverts * 4: pos.xy uv.xy */
    /* what the vertex buffer on the GPU was made from: the skinning
     * matrices and the half sizes of the last upload (up_ok: there was one
     * into the buffer that exists now) */
    Aff *up_skin;
    float up_half[2];
    gboolean up_ok;
    /* walpe_puppet_uv_cover: rectangles around the texture
     * coordinates of all triangles; cover_n -1 = not computed yet */
    int cover_n;
    float cover[8][4];
};

/* ------------------------------------------------------------ affine */

static Aff aff_mul (const Aff *a, const Aff *b)
{
    Aff r;
    r.m[0] = a->m[0] * b->m[0] + a->m[2] * b->m[1];
    r.m[1] = a->m[1] * b->m[0] + a->m[3] * b->m[1];
    r.m[2] = a->m[0] * b->m[2] + a->m[2] * b->m[3];
    r.m[3] = a->m[1] * b->m[2] + a->m[3] * b->m[3];
    r.m[4] = a->m[0] * b->m[4] + a->m[2] * b->m[5] + a->m[4];
    r.m[5] = a->m[1] * b->m[4] + a->m[3] * b->m[5] + a->m[5];
    return r;
}

static Aff aff_inv (const Aff *a)
{
    Aff r;
    float det = a->m[0] * a->m[3] - a->m[2] * a->m[1];
    if (fabsf (det) < 1e-12f)
        det = 1e-12f;
    float id = 1.0f / det;
    r.m[0] = a->m[3] * id;
    r.m[1] = -a->m[1] * id;
    r.m[2] = -a->m[2] * id;
    r.m[3] = a->m[0] * id;
    r.m[4] = -(r.m[0] * a->m[4] + r.m[2] * a->m[5]);
    r.m[5] = -(r.m[1] * a->m[4] + r.m[3] * a->m[5]);
    return r;
}

static Aff aff_trs (const PFrame *f)
{
    float c = cosf (f->a[2]), s = sinf (f->a[2]);
    Aff r = { { c * f->s[0], s * f->s[0], -s * f->s[1], c * f->s[1], f->p[0], f->p[1] } };
    return r;
}

/* ------------------------------------------------------------ reader */

typedef struct { const guint8 *b; gsize n, p; gboolean bad; } Rd;

static guint32 rd_u32 (Rd *r)
{
    if (r->p + 4 > r->n) { r->bad = TRUE; return 0; }
    guint32 v;
    memcpy (&v, r->b + r->p, 4);
    r->p += 4;
    return GUINT32_FROM_LE (v);
}

static guint16 rd_u16 (Rd *r)
{
    if (r->p + 2 > r->n) { r->bad = TRUE; return 0; }
    guint16 v;
    memcpy (&v, r->b + r->p, 2);
    r->p += 2;
    return GUINT16_FROM_LE (v);
}

static float rd_f32 (Rd *r)
{
    guint32 u = rd_u32 (r);
    float f;
    memcpy (&f, &u, 4);
    return f;
}

static char *rd_str (Rd *r)
{
    const guint8 *e = r->p < r->n ? memchr (r->b + r->p, 0, r->n - r->p) : NULL;
    if (!e) { r->bad = TRUE; return g_strdup (""); }
    char *s = g_strndup ((const char *) r->b + r->p, e - (r->b + r->p));
    r->p = (e - r->b) + 1;
    return s;
}

static gssize find_tag (const guint8 *b, gsize n, gsize from, const char *tag)
{
    for (gsize i = from; i + 8 <= n; i++)
        if (!memcmp (b + i, tag, 4) && g_ascii_isdigit (b[i + 4]) && g_ascii_isdigit (b[i + 7]))
            return (gssize) i;
    return -1;
}

static gboolean is_mode (const char *s)
{
    return !strcmp (s, "loop") || !strcmp (s, "mirror") || !strcmp (s, "single");
}

/* an animation header at q: i32 id > 0, u32 0, name\0 (< 256), mode\0 */
static gboolean anim_header_at (const guint8 *b, gsize n, gsize q)
{
    if (q + 10 > n)
        return FALSE;
    guint32 id, z;
    memcpy (&id, b + q, 4);
    memcpy (&z, b + q + 4, 4);
    if (id == 0 || id > 100000000u || z != 0)
        return FALSE;
    const guint8 *nm = b + q + 8;
    const guint8 *e = memchr (nm, 0, MIN ((gsize) 256, n - (q + 8)));
    if (!e)
        return FALSE;
    const guint8 *md = e + 1;
    gsize left = n - (md - b);
    const guint8 *e2 = memchr (md, 0, MIN ((gsize) 16, left));
    if (!e2)
        return FALSE;
    char mode[16];
    memcpy (mode, md, e2 - md);
    mode[e2 - md] = 0;
    return is_mode (mode);
}

static void anim_clear (PAnim *a)
{
    g_free (a->name);
    g_free (a->frames);
}

WalpePuppet *walpe_puppet_load (const char *path)
{
    gchar *data = NULL;
    gsize n = 0;
    /* read whole: bounded (the largest model of 88 Workshop scenes is 4 MiB;
     * a file of gigabytes named as a puppet was gigabytes of memory) */
    GStatBuf st;
    if (g_stat (path, &st) != 0 || !S_ISREG (st.st_mode) || st.st_size > PUPPET_MAX_FILE_BYTES) {
        g_message ("puppet: %s: not a model file of at most %d MiB", path,
                   (int) (PUPPET_MAX_FILE_BYTES >> 20));
        return NULL;
    }
    if (!g_file_get_contents (path, &data, &n, NULL))
        return NULL;
    const guint8 *b = (const guint8 *) data;
    WalpePuppet *p = NULL;
    Rd r = { b, n, 0, FALSE };
    if (n < 64 || memcmp (b, "MDLV", 4)) {
        g_message ("puppet: %s: not an MDLV model", path);
        goto out;
    }
    r.p = 9;
    rd_u32 (&r); rd_u32 (&r); rd_u32 (&r);
    g_free (rd_str (&r));
    /* vertex block: the format word, then a byte count that is a whole
     * number of 80-byte vertices followed by an index block that fits */
    gsize vstart = 0, vbytes = 0;
    for (gsize q = r.p; q + 12 <= n && q < r.p + 512; q++) {
        guint32 w, vb, ib;
        memcpy (&w, b + q, 4);
        if ((w & 0xFFFF0000u) != 0x01800000u || (w & 0xF) != 0xF)
            continue;
        memcpy (&vb, b + q + 4, 4);
        if (vb == 0 || vb % VSTRIDE || q + 8 + vb + 4 > n)
            continue;
        memcpy (&ib, b + q + 8 + vb, 4);
        if (ib == 0 || ib % 6 || q + 12 + vb + ib > n)
            continue;
        vstart = q + 8;
        vbytes = vb;
        break;
    }
    if (!vstart) {
        g_debug ("puppet: %s: no skinned mesh (flat layer)", path);
        goto out;
    }
    p = g_new0 (WalpePuppet, 1);
    p->cover_n = -1;
    p->nverts = (int) (vbytes / VSTRIDE);
    p->pos = g_new (float, p->nverts * 2);
    p->uv = g_new (float, p->nverts * 2);
    p->bone = g_new0 (guint8, p->nverts * 4);
    p->weight = g_new0 (float, p->nverts * 4);
    for (int i = 0; i < p->nverts; i++) {
        Rd v = { b, n, vstart + (gsize) i * VSTRIDE, FALSE };
        p->pos[i * 2] = rd_f32 (&v);
        p->pos[i * 2 + 1] = rd_f32 (&v);
        v.p += 4 + 12 + 16;          /* z, normal, tangent */
        for (int k = 0; k < 4; k++) {
            guint32 bi = rd_u32 (&v);
            p->bone[i * 4 + k] = (guint8) MIN (bi, 255u);
        }
        for (int k = 0; k < 4; k++)
            p->weight[i * 4 + k] = rd_f32 (&v);
        p->uv[i * 2] = rd_f32 (&v);
        p->uv[i * 2 + 1] = rd_f32 (&v);
    }
    r.p = vstart + vbytes;
    guint32 ib = rd_u32 (&r);
    p->nidx = (int) (ib / 2);
    p->idx = g_new (guint16, p->nidx);
    for (int i = 0; i < p->nidx; i++) {
        p->idx[i] = rd_u16 (&r);
        if (p->idx[i] >= p->nverts)
            p->idx[i] = 0;
    }

    /* bones */
    gssize ms = find_tag (b, n, r.p, "MDLS");
    if (ms >= 0) {
        r.p = ms + 9;
        guint32 send = rd_u32 (&r);
        int nb = rd_u16 (&r);
        rd_u16 (&r);
        if (nb > 0 && nb < 256) {
            p->nbones = nb;
            p->parent = g_new (int, nb);
            p->bind_local = g_new (Aff, nb);
            for (int i = 0; i < nb && !r.bad; i++) {
                g_free (rd_str (&r));
                rd_u32 (&r);
                p->parent[i] = (int) rd_u32 (&r);
                guint32 msz = rd_u32 (&r);
                float m[16] = { 0 };
                for (int k = 0; k < 16; k++)
                    m[k] = rd_f32 (&r);
                if (msz > 64)
                    r.p += msz - 64;
                Aff a = { { m[0], m[1], m[4], m[5], m[12], m[13] } };
                p->bind_local[i] = a;
                g_free (rd_str (&r));
                if (p->parent[i] >= i)
                    p->parent[i] = -1;   /* parents precede children */
            }
            if (r.bad) {
                p->nbones = 0;
            } else {
                p->bind_inv_world = g_new (Aff, nb);
                Aff *world = g_new (Aff, nb);
                for (int i = 0; i < nb; i++) {
                    world[i] = p->parent[i] >= 0
                        ? aff_mul (&world[p->parent[i]], &p->bind_local[i]) : p->bind_local[i];
                    p->bind_inv_world[i] = aff_inv (&world[i]);
                }
                g_free (world);
            }
        }
        if (send > (guint32) ms && send <= n)
            r.p = send;
        r.bad = FALSE;
    }

    /* animations */
    gssize ma = p->nbones ? find_tag (b, n, r.p, "MDLA") : -1;
    /* attachment points: an optional section in front of the animations
     * (not looked for inside them: frame data could spell the tag) */
    gssize md = p->nbones ? find_tag (b, ma >= 0 ? (gsize) ma : n, r.p, "MDAT") : -1;
    if (md >= 0) {
        Rd a = { b, ma >= 0 ? (gsize) ma : n, (gsize) md + 9, FALSE };
        rd_u32 (&a);
        int na = rd_u16 (&a);
        /* (each takes at least 67 bytes of the section) */
        na = (int) MIN ((gsize) na, (a.n - MIN (a.p, a.n)) / 67);
        if (na > 0 && !a.bad) {
            p->att = g_new0 (PAttach, na);   /* at most 65535 x 32 bytes */
            for (int i = 0; i < na; i++) {
                int bone = rd_u16 (&a);
                char *name = rd_str (&a);
                float m[16] = { 0 };
                for (int k = 0; k < 16; k++)
                    m[k] = rd_f32 (&a);
                gboolean fin = TRUE;
                for (int k = 0; k < 16; k++)
                    fin &= isfinite (m[k]) != 0;
                if (a.bad) {
                    g_free (name);
                    break;
                }
                if (bone >= p->nbones || !fin || !*name) {
                    g_free (name);   /* names no bone of this model, or nothing */
                    continue;
                }
                PAttach *at = &p->att[p->natt++];
                at->name = name;
                at->bone = bone;
                Aff l = { { m[0], m[1], m[4], m[5], m[12], m[13] } };
                at->local = l;
            }
        }
    }
    if (ma >= 0) {
        r.p = ma + 9;
        rd_u32 (&r);
        guint32 na = rd_u32 (&r);
        if (na > 0 && na < 256) {
            p->anims = g_new0 (PAnim, na);
            for (guint32 a = 0; a < na && !r.bad; a++) {
                /* resync on the next header (versions add trailing fields) */
                gsize q = r.p;
                while (q + 10 <= n && !anim_header_at (b, n, q) && q < r.p + 4096)
                    q++;
                if (!anim_header_at (b, n, q))
                    break;
                r.p = q;
                PAnim *an = &p->anims[p->nanims];
                an->id = (int) rd_u32 (&r);
                rd_u32 (&r);
                an->name = rd_str (&r);
                char *mode = rd_str (&r);
                an->mode = !strcmp (mode, "mirror") ? 1 : !strcmp (mode, "single") ? 2 : 0;
                g_free (mode);
                an->fps = rd_f32 (&r);
                an->length = (int) rd_u32 (&r);
                rd_u32 (&r);
                int nbo = (int) rd_u32 (&r);
                if (nbo != p->nbones || an->length < 1 || !(an->fps > 0) || r.bad) {
                    anim_clear (an);
                    memset (an, 0, sizeof *an);
                    break;
                }
                an->nbones = nbo;
                for (int bi = 0; bi < nbo && !r.bad; bi++) {
                    rd_u32 (&r);
                    guint32 nbytes = rd_u32 (&r);
                    int nf = (int) (nbytes / 36);
                    /* bounds first: a crafted byte count must not size the
                     * frame table (255 bones x 4 GB / 36 aborted g_new0) */
                    if (r.bad || nbytes > n - MIN (r.p, n)) { r.bad = TRUE; break; }
                    if (bi == 0) {
                        an->nframes = nf;
                        /* bones x the frames of bone 0: up to 255 x the file
                         * size when bone 0 claims the rest of the file. A
                         * request the system refuses drops the animation
                         * (g_new0 aborted the daemon) */
                        an->frames = g_try_malloc0_n ((gsize) nbo * MAX (nf, 1), sizeof (PFrame));
                        if (!an->frames) { r.bad = TRUE; break; }
                    }
                    for (int f = 0; f < nf; f++) {
                        float v[9];
                        for (int k = 0; k < 9; k++)
                            v[k] = rd_f32 (&r);
                        if (f < an->nframes) {
                            PFrame *pf = &an->frames[(gsize) bi * an->nframes + f];
                            memcpy (pf->p, v, 12);
                            memcpy (pf->a, v + 3, 12);
                            memcpy (pf->s, v + 6, 12);
                        }
                    }
                }
                if (r.bad || an->nframes < 1) {
                    anim_clear (an);
                    memset (an, 0, sizeof *an);
                    break;
                }
                p->nanims++;
            }
        }
    }
    if (!p->nbones)
        p->natt = 0;   /* (the bone table was dropped as unreadable) */
    g_debug ("puppet: %s: %d vertices, %d triangles, %d bones, %d animations, %d attachment "
             "points", path, p->nverts, p->nidx / 3, p->nbones, p->nanims, p->natt);
out:
    g_free (data);
    return p;
}

/* a layer number: plain, or {"value": v, "user": prop} bound to a user
 * property (3042492564: "rate": {"user": "umika_float_speed", "value": 1.1});
 * json_object_get_double_member() on the object read 0 with a
 * Json-CRITICAL and froze/hid the animation layer */
static float layer_num (JsonObject *lo, const char *k, JsonObject *user_props, float dflt)
{
    JsonNode *n = json_object_get_member (lo, k);
    if (!n || !JSON_NODE_HOLDS_OBJECT (n))
        return (float) wpj_num (lo, k, dflt);
    JsonObject *o = json_node_get_object (n);
    const char *up = wpj_str (o, "user", NULL);
    JsonNode *uv = up && user_props ? json_object_get_member (user_props, up) : NULL;
    if (uv && JSON_NODE_HOLDS_VALUE (uv)) {
        walpe_probe_use (up, "puppet");
        GType t = json_node_get_value_type (uv);
        if (t == G_TYPE_DOUBLE || t == G_TYPE_INT64)
            return (float) json_node_get_double (uv);
        if (t == G_TYPE_STRING)
            return (float) g_ascii_strtod (json_node_get_string (uv), NULL);
    }
    return (float) wpj_num (o, "value", dflt);
}

void walpe_puppet_set_layers (WalpePuppet *p, JsonArray *layers, JsonObject *user_props)
{
    if (!p)
        return;
    g_clear_pointer (&p->layers, g_free);
    p->nlayers = 0;
    p->pose_ok = FALSE;
    guint n = layers ? json_array_get_length (layers) : 0;
    if (!n)
        return;
    p->layers = g_new0 (PLayer, n);
    for (guint i = 0; i < n; i++) {
        JsonNode *ln = json_array_get_element (layers, i);
        if (!JSON_NODE_HOLDS_OBJECT (ln))
            continue;
        JsonObject *lo = json_node_get_object (ln);
        double idd = wpj_num (lo, "animation", -1);
        int id = idd >= 0 && idd <= G_MAXINT ? (int) idd : -1;   /* ids are > 0 */
        int ai = -1;
        for (int a = 0; a < p->nanims; a++)
            if (p->anims[a].id == id)
                ai = a;
        if (ai < 0)
            continue;
        PLayer *L = &p->layers[p->nlayers];
        L->anim = ai;
        L->rate = layer_num (lo, "rate", user_props, 1.0f);
        L->blend = layer_num (lo, "blend", user_props, 1.0f);
        L->additive = wpj_bool (lo, "additive", FALSE);
        L->visible = TRUE;
        JsonNode *vn = json_object_get_member (lo, "visible");
        if (vn && JSON_NODE_HOLDS_VALUE (vn)) {
            L->visible = json_node_get_boolean (vn);
        } else if (vn && JSON_NODE_HOLDS_OBJECT (vn)) {
            JsonObject *vo = json_node_get_object (vn);
            const char *up = json_object_has_member (vo, "user")
                && JSON_NODE_HOLDS_VALUE (json_object_get_member (vo, "user"))
                ? json_object_get_string_member (vo, "user") : NULL;
            JsonNode *uv = up && user_props ? json_object_get_member (user_props, up) : NULL;
            JsonNode *dv = json_object_get_member (vo, "value");
            JsonNode *use = uv && JSON_NODE_HOLDS_VALUE (uv) ? uv : dv;
            if (use == uv && uv)
                walpe_probe_use (up, "puppet");
            if (use && JSON_NODE_HOLDS_VALUE (use))
                L->visible = json_node_get_boolean (use);
        }
        p->nlayers++;
    }
}

/* WALPE_PROP_PROBE digest: [[animation, rate, blend, visible], ...] */
void walpe_puppet_probe (const WalpePuppet *p, JsonBuilder *b)
{
    json_builder_begin_array (b);
    for (int i = 0; p && i < p->nlayers; i++) {
        const PLayer *L = &p->layers[i];
        json_builder_begin_array (b);
        json_builder_add_int_value (b, L->anim);
        json_builder_add_double_value (b, round ((double) L->rate * 1e4) / 1e4);
        json_builder_add_double_value (b, round ((double) L->blend * 1e4) / 1e4);
        json_builder_add_boolean_value (b, L->visible);
        json_builder_end_array (b);
    }
    json_builder_end_array (b);
}

/* the animation's local frame of bone bi at time t (s), playback rate */
static void anim_sample (const PAnim *an, int bi, double t, float rate, PFrame *out)
{
    double f = t * an->fps * rate;
    double len = an->length;
    if (!isfinite (f))
        f = 0;   /* inf fps / huge rate: fmod would give NaN, (int) NaN a wild index */
    switch (an->mode) {
    case 1: { /* mirror: ping-pong */
        double c = fmod (f, 2 * len);
        f = c <= len ? c : 2 * len - c;
        break;
    }
    case 2:
        f = CLAMP (f, 0, len);
        break;
    default:
        f = fmod (f, len);
        break;
    }
    if (!(f >= 0))
        f = 0;
    if (f > len)
        f = len;
    /* 64-bit: "length" comes from the file, and with f == INT_MAX the
     * next frame f0 + 1 wrapped to INT_MIN, passed the checks below and
     * indexed far below the table. 0 <= f <= len <= INT_MAX here. */
    gint64 f0 = (gint64) floor (f), f1 = f0 + 1;
    float k = (float) (f - (double) f0);
    gint64 last = an->nframes - 1;
    if (f0 > last) f0 = last;
    if (f1 > last) f1 = an->mode == 0 && an->nframes > 1 ? (f1 % an->nframes) : last;
    const PFrame *a = &an->frames[(gsize) bi * an->nframes + (gsize) f0];
    const PFrame *b = &an->frames[(gsize) bi * an->nframes + (gsize) f1];
    for (int i = 0; i < 3; i++) {
        out->p[i] = a->p[i] + (b->p[i] - a->p[i]) * k;
        out->a[i] = a->a[i] + (b->a[i] - a->a[i]) * k;
        out->s[i] = a->s[i] + (b->s[i] - a->s[i]) * k;
    }
}

int walpe_puppet_bone_count (const WalpePuppet *p)
{
    return p ? p->nbones : 0;
}

/* bone poses at time t -> world[bone] (nbones entries; the model's pixel
 * units around its centre): the base is the first animation's first frame
 * (WE shows an animated puppet in that pose; part sheets rely on it),
 * visible layers then mix in (additive: blend * (frame(t) - frame(0)) of
 * their animation; normal: lerp towards frame(t) by blend). No animation:
 * the bind pose (= the flat texture). */
static void puppet_pose (const WalpePuppet *p, double t, Aff *world)
{
    int nb = p->nbones;
    {
        PFrame *pose = g_newa (PFrame, nb);
        for (int i = 0; i < nb; i++) {
            if (p->nanims > 0) {
                pose[i] = p->anims[0].frames[(gsize) i * p->anims[0].nframes];
            } else {
                const Aff *a = &p->bind_local[i];
                memset (&pose[i], 0, sizeof pose[i]);
                pose[i].p[0] = a->m[4];
                pose[i].p[1] = a->m[5];
                pose[i].a[2] = atan2f (a->m[1], a->m[0]);
                pose[i].s[0] = hypotf (a->m[0], a->m[1]);
                pose[i].s[1] = hypotf (a->m[2], a->m[3]);
                pose[i].s[2] = 1;
            }
        }
        for (int l = 0; l < p->nlayers; l++) {
            const PLayer *L = &p->layers[l];
            if (!L->visible || L->blend <= 0)
                continue;
            const PAnim *an = &p->anims[L->anim];
            for (int i = 0; i < nb; i++) {
                PFrame ft;
                anim_sample (an, i, t, L->rate, &ft);
                const PFrame *f0 = &an->frames[(gsize) i * an->nframes];
                for (int c = 0; c < 3; c++) {
                    if (L->additive) {
                        pose[i].p[c] += L->blend * (ft.p[c] - f0->p[c]);
                        pose[i].a[c] += L->blend * (ft.a[c] - f0->a[c]);
                        pose[i].s[c] += L->blend * (ft.s[c] - f0->s[c]);
                    } else {
                        float k = MIN (L->blend, 1.0f);
                        pose[i].p[c] += k * (ft.p[c] - pose[i].p[c]);
                        pose[i].a[c] += k * (ft.a[c] - pose[i].a[c]);
                        pose[i].s[c] += k * (ft.s[c] - pose[i].s[c]);
                    }
                }
            }
        }
        for (int i = 0; i < nb; i++) {
            Aff loc = aff_trs (&pose[i]);
            world[i] = p->parent[i] >= 0 ? aff_mul (&world[p->parent[i]], &loc) : loc;
        }
    }
}

gboolean walpe_puppet_attachment (WalpePuppet *p, const char *name, double t, float m[6])
{
    if (!p || !name || !p->nbones)
        return FALSE;
    for (int i = 0; i < p->natt; i++) {
        const PAttach *at = &p->att[i];
        if (strcmp (at->name, name))
            continue;
        if (!p->pose_ok || p->pose_t != t) {
            if (!p->pose_world)
                p->pose_world = g_new (Aff, p->nbones);
            puppet_pose (p, t, p->pose_world);
            p->pose_t = t;
            p->pose_ok = TRUE;
        }
        Aff a = aff_mul (&p->pose_world[at->bone], &at->local);
        for (int k = 0; k < 6; k++)
            if (!isfinite (a.m[k]))
                return FALSE;   /* an animation frame that is not a number */
        memcpy (m, a.m, sizeof (a.m));
        return TRUE;
    }
    return FALSE;
}

int walpe_puppet_attachment_count (const WalpePuppet *p)
{
    return p ? p->natt : 0;
}

void walpe_puppet_draw (WalpePuppet *p, double t, float half_w, float half_h,
                        GLuint tex, gboolean premult, float opacity, const float *mvp,
                        int blend, const float tint[3])
{
    if (!p || !p->nverts || half_w <= 0 || half_h <= 0)
        return;
    int nb = p->nbones;
    Aff skin[256];
    if (nb > 0) {
        Aff world[256];
        puppet_pose (p, t, world);
        for (int i = 0; i < nb; i++)
            skin[i] = aff_mul (&world[i], &p->bind_inv_world[i]);
    }

    /* The vertex data is a function of the skinning matrices and the two
     * half sizes (positions, weights and texture coordinates never change).
     * With the ones of the last upload the buffer on the GPU holds exactly
     * what would be computed and sent again: a puppet without bones, one
     * that is not animated, an animation that stands at its last frame.
     * (Compared as bytes: a matrix that is not a number is "the same" only
     * as itself, and gives the same bytes.) */
    const gboolean sent = p->vao && p->up_ok && p->up_half[0] == half_w && p->up_half[1] == half_h &&
                          (nb == 0 || !memcmp (p->up_skin, skin, (gsize) nb * sizeof (Aff)));
    if (sent) {
        walpe_glutil_draw_mesh_tint (tex, premult, opacity, mvp, blend, tint, p->vao, p->nidx);
        return;
    }
    if (!p->vbuf)
        p->vbuf = g_new (float, p->nverts * 4);
    for (int v = 0; v < p->nverts; v++) {
        float x = p->pos[v * 2], y = p->pos[v * 2 + 1];
        float ox = x, oy = y;
        if (nb > 0) {
            float sx = 0, sy = 0, ws = 0;
            for (int k = 0; k < 4; k++) {
                float w = p->weight[v * 4 + k];
                int bi = p->bone[v * 4 + k];
                if (w <= 0 || bi >= nb)
                    continue;
                const Aff *m = &skin[bi];
                sx += w * (m->m[0] * x + m->m[2] * y + m->m[4]);
                sy += w * (m->m[1] * x + m->m[3] * y + m->m[5]);
                ws += w;
            }
            if (ws > 1e-4f) {
                ox = sx / ws;
                oy = sy / ws;
            }
        }
        p->vbuf[v * 4] = ox / half_w;
        p->vbuf[v * 4 + 1] = oy / half_h;
        p->vbuf[v * 4 + 2] = p->uv[v * 2];
        p->vbuf[v * 4 + 3] = p->uv[v * 2 + 1];
    }
    if (!p->vao) {
        glGenVertexArrays (1, &p->vao);
        glBindVertexArray (p->vao);
        glGenBuffers (1, &p->vbo);
        glBindBuffer (GL_ARRAY_BUFFER, p->vbo);
        glBufferData (GL_ARRAY_BUFFER, (GLsizeiptr) p->nverts * 16, NULL, GL_STREAM_DRAW);
        glEnableVertexAttribArray (0);
        glVertexAttribPointer (0, 2, GL_FLOAT, GL_FALSE, 16, (void *) 0);
        glEnableVertexAttribArray (1);
        glVertexAttribPointer (1, 2, GL_FLOAT, GL_FALSE, 16, (void *) 8);
        glGenBuffers (1, &p->ebo);
        glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, p->ebo);
        glBufferData (GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr) p->nidx * 2, p->idx, GL_STATIC_DRAW);
        glBindVertexArray (0);
    }
    glBindBuffer (GL_ARRAY_BUFFER, p->vbo);
    glBufferSubData (GL_ARRAY_BUFFER, 0, (GLsizeiptr) p->nverts * 16, p->vbuf);
    glBindBuffer (GL_ARRAY_BUFFER, 0);
    if (nb > 0) {
        if (!p->up_skin)
            p->up_skin = g_new (Aff, p->nbones);
        memcpy (p->up_skin, skin, (gsize) nb * sizeof (Aff));
    }
    p->up_half[0] = half_w;
    p->up_half[1] = half_h;
    p->up_ok = TRUE;
    walpe_glutil_draw_mesh_tint (tex, premult, opacity, mvp, blend, tint, p->vao, p->nidx);
}

/* ---- which part of its texture the mesh samples --------------------
 * The texture coordinates of a puppet's vertices never change (the bones
 * move positions only), so what a draw can sample of the layer's texture
 * is the union of its triangles in texture space, whatever the pose. Here:
 * that union on a 32 x 32 grid (a triangle marks every cell its bounding
 * box touches - more than it covers, never less; coordinates outside 0..1
 * are clamped like the sampler clamps them), packed into at most `max`
 * rectangles (u0, v0, u1, v1) that do not overlap. Returns their number; 0
 * = no statement
 * (no mesh, a coordinate that is no number, or the rectangles cover nearly
 * everything anyway). The caller hands them to fx_set_need (): effect
 * passes then leave out what no triangle can read. */
#define PUP_GRID 32
int walpe_puppet_uv_cover (WalpePuppet *p, float (*rects)[4], int max)
{
    if (!p || !rects || max < 1 || p->nidx < 3)
        return 0;
    max = MIN (max, (int) G_N_ELEMENTS (p->cover));
    if (p->cover_n < 0 || p->cover_n > max) {
        guint32 grid[PUP_GRID] = { 0 };
        gboolean ok = TRUE;
        for (int t = 0; ok && t + 2 < p->nidx; t += 3) {
            float u0 = 2, v0 = 2, u1 = -1, v1 = -1;
            for (int c = 0; c < 3; c++) {
                float u = p->uv[p->idx[t + c] * 2], v = p->uv[p->idx[t + c] * 2 + 1];
                ok = ok && isfinite (u) && isfinite (v);
                u = CLAMP (u, 0.0f, 1.0f);
                v = CLAMP (v, 0.0f, 1.0f);
                u0 = fminf (u0, u); u1 = fmaxf (u1, u);
                v0 = fminf (v0, v); v1 = fmaxf (v1, v);
            }
            if (!ok)
                break;
            int x0 = CLAMP ((int) floorf (u0 * PUP_GRID), 0, PUP_GRID - 1);
            int x1 = CLAMP ((int) floorf (u1 * PUP_GRID), 0, PUP_GRID - 1);
            int y0 = CLAMP ((int) floorf (v0 * PUP_GRID), 0, PUP_GRID - 1);
            int y1 = CLAMP ((int) floorf (v1 * PUP_GRID), 0, PUP_GRID - 1);
            guint32 bits = (x1 - x0 >= 31 ? 0xffffffffu : ((1u << (x1 - x0 + 1)) - 1u)) << x0;
            for (int y = y0; y <= y1; y++)
                grid[y] |= bits;
        }
        /* runs of cells per row; a run that continues the same columns of
         * the row above extends that rectangle. A mesh of many small
         * islands gives too many rectangles for the search below: then the
         * same on blocks of 2 x 2 cells (marked when any cell is), 4 x 4 ... */
        typedef struct { int x0, y0, x1, y1; } R;
        GArray *rs = g_array_new (FALSE, FALSE, sizeof (R));
        for (int f = 1; ok && f <= PUP_GRID; f *= 2) {
            guint32 gf[PUP_GRID] = { 0 };
            for (int y = 0; y < PUP_GRID; y++)
                for (int x = 0; x < PUP_GRID; x++)
                    if (grid[y] >> x & 1u) {
                        const int bx = x & ~(f - 1), by = y & ~(f - 1);
                        const guint32 bits = (f >= 32 ? 0xffffffffu : ((1u << f) - 1u)) << bx;
                        for (int k = 0; k < f; k++)
                            gf[by + k] |= bits;
                    }
            g_array_set_size (rs, 0);
            for (int y = 0; y < PUP_GRID; y++)
                for (int x = 0; x < PUP_GRID;) {
                    if (!(gf[y] >> x & 1u)) {
                        x++;
                        continue;
                    }
                    int e = x;
                    while (e < PUP_GRID && (gf[y] >> e & 1u))
                        e++;
                    gboolean ext = FALSE;
                    for (guint i = 0; i < rs->len && !ext; i++) {
                        R *r = &g_array_index (rs, R, i);
                        if (r->y1 == y && r->x0 == x && r->x1 == e) {
                            r->y1 = y + 1;
                            ext = TRUE;
                        }
                    }
                    if (!ext) {
                        R r = { x, y, e, y + 1 };
                        g_array_append_val (rs, r);
                    }
                    x = e;
                }
            if (rs->len <= 48)
                break;
        }
        /* Too many: join two into their bounding box, together with every
         * rectangle that box then touches (so they never overlap), and take
         * the pair that leaves the least area - again and again. */
        while (ok && (int) rs->len > max) {
            int best = G_MAXINT;
            GArray *bset = NULL;
            for (guint i = 0; i < rs->len; i++)
                for (guint j = i + 1; j < rs->len; j++) {
                    R box = g_array_index (rs, R, i);
                    const R *o = &g_array_index (rs, R, j);
                    box.x0 = MIN (box.x0, o->x0);
                    box.y0 = MIN (box.y0, o->y0);
                    box.x1 = MAX (box.x1, o->x1);
                    box.y1 = MAX (box.y1, o->y1);
                    GArray *ns = g_array_new (FALSE, FALSE, sizeof (R));
                    for (guint k = 0; k < rs->len; k++)
                        if (k != i && k != j)
                            g_array_append_val (ns, g_array_index (rs, R, k));
                    for (gboolean again = TRUE; again;) {
                        again = FALSE;
                        for (guint k = 0; k < ns->len; k++) {
                            const R *q = &g_array_index (ns, R, k);
                            if (q->x0 < box.x1 && box.x0 < q->x1 && q->y0 < box.y1 && box.y0 < q->y1) {
                                box.x0 = MIN (box.x0, q->x0);
                                box.y0 = MIN (box.y0, q->y0);
                                box.x1 = MAX (box.x1, q->x1);
                                box.y1 = MAX (box.y1, q->y1);
                                g_array_remove_index_fast (ns, k);
                                again = TRUE;
                                break;
                            }
                        }
                    }
                    g_array_append_val (ns, box);
                    int area = 0;
                    for (guint k = 0; k < ns->len; k++) {
                        const R *q = &g_array_index (ns, R, k);
                        area += (q->x1 - q->x0) * (q->y1 - q->y0);
                    }
                    if (area < best) {
                        best = area;
                        g_clear_pointer (&bset, g_array_unref);
                        bset = ns;
                    } else {
                        g_array_unref (ns);
                    }
                }
            if (!bset)
                break;
            g_array_unref (rs);
            rs = bset;
        }
        p->cover_n = 0;
        int cells = 0;
        for (guint i = 0; ok && i < rs->len; i++) {
            const R *r = &g_array_index (rs, R, i);
            p->cover[i][0] = (float) r->x0 / PUP_GRID;
            p->cover[i][1] = (float) r->y0 / PUP_GRID;
            p->cover[i][2] = (float) r->x1 / PUP_GRID;
            p->cover[i][3] = (float) r->y1 / PUP_GRID;
            cells += (r->x1 - r->x0) * (r->y1 - r->y0);
            p->cover_n++;
        }
        if (!ok || cells * 16 > PUP_GRID * PUP_GRID * 15)
            p->cover_n = 0;
        g_array_unref (rs);
    }
    for (int i = 0; i < p->cover_n; i++)
        memcpy (rects[i], p->cover[i], sizeof (p->cover[i]));
    return p->cover_n;
}

void walpe_puppet_gl_free (WalpePuppet *p)
{
    if (!p)
        return;
    if (p->vbo) glDeleteBuffers (1, &p->vbo);
    if (p->ebo) glDeleteBuffers (1, &p->ebo);
    if (p->vao) glDeleteVertexArrays (1, &p->vao);
    p->vao = p->vbo = p->ebo = 0;
    p->up_ok = FALSE;   /* the next buffer starts empty */
}

void walpe_puppet_free (WalpePuppet *p)
{
    if (!p)
        return;
    g_free (p->pos);
    g_free (p->uv);
    g_free (p->bone);
    g_free (p->weight);
    g_free (p->idx);
    g_free (p->parent);
    g_free (p->bind_local);
    g_free (p->bind_inv_world);
    for (int a = 0; a < p->nanims; a++)
        anim_clear (&p->anims[a]);
    g_free (p->anims);
    g_free (p->layers);
    for (int a = 0; a < p->natt; a++)
        g_free (p->att[a].name);
    g_free (p->att);
    g_free (p->pose_world);
    g_free (p->vbuf);
    g_free (p->up_skin);
    g_free (p);
}
