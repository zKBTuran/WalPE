/* fx_keys.h - statements of stock effect shaders, recognised by key
 *
 * Wallpaper Engine's own effect shaders travel with the wallpapers (a
 * scene's package holds copies of the files its author's version had) and
 * come from the user's install. A few of them the engine has to know when
 * it sees them: to move frame-constant work out of the fragment shader, to
 * skip a pass that copies its input, to bound how far a displacement
 * reaches, to stand in for the transform effect (fx_rewrite.h,
 * fx_region.h, fx_vertex_traits () in scenenode_effects.c).
 *
 * WalPE holds none of that shader text. What is compared is a KEY of the
 * statement: its length, a rolling fingerprint and a 64-bit hash, all of
 * the statement with white space removed - the form fx_active_code ()
 * gives compiled code (no white space, comments, preprocessor lines or
 * dead #if branches). A key matches wherever those characters stand in the
 * searched text, at any offset, exactly as strstr () would find the
 * statement itself: the fingerprint finds candidate positions in one
 * pass, the hash confirms them. A key can be a whole statement, the start
 * of one, or several statements in a row; the comment at each row says in
 * words which effect it belongs to and what the statement computes. A row
 * is made with the key tool of the developer tools (README, "Developer
 * tools"); tests/fx_keys_test.c checks the table against an install.
 *
 * The hash is FNV-1a over the bytes with the basis fx_region.h's pinned
 * shader hashes use (fxr_hash; it is not the textbook basis).
 *
 * Each slot has room for one more key (FXK_ALTS): tests register a
 * statement of their own there (fxk_test_register), so that they exercise
 * the recognisers without carrying stock text. Nothing else writes the
 * table. */
#pragma once
#include <glib.h>
#include <string.h>

typedef struct {
    guint16 len;     /* characters of the statement, white space removed */
    guint32 roll;    /* fxk_roll () of them */
    guint64 hash;    /* fxk_hash () of them */
} FxKey;

typedef enum {
    FXK_NONE = 0,
    /* fx_rewrite.h: frame-constant runs */
    FXK_PULSE_FIRST, FXK_PULSE_LAST,
    FXK_SHAKE_FIRST_A, FXK_SHAKE_FIRST_B, FXK_SHAKE_LAST,
    FXK_SHAKE_PHASE0, FXK_SHAKE_VBOUNDS, FXK_SHAKE_PHASEMAP,
    /* fx_rewrite.h: blur kernels, ray march, the copying pass */
    FXK_BLUR13, FXK_BLUR7, FXK_BLUR3,
    FXK_CAST_LOOP, FXK_CAST_GATHER, FXK_CAST_N50, FXK_CAST_N30, FXK_CAST_N15,
    FXK_OPACITY_MAIN,
    /* scenenode_effects.c: vertex shaders */
    FXK_POS_LOCAL, FXK_MVP_LOCAL, FXK_MVP_LOCAL_INT,
    FXK_XF_APPLY, FXK_XF_RETURN, FXK_XF_ROT_NEG, FXK_XF_ROT_POS,
    /* fx_region.h: which painted texture scales a displacement */
    FXK_RG_FLOW_READ, FXK_RG_FLOW_MASK, FXK_RG_MASK_READ, FXK_RG_MASK_ONE, FXK_RG_IRIS_MASK,
    FXK_N
} FxKeyId;

#define FXK_ALTS 2   /* [0] the stock statement, [1] a test's own (fxk_test_register) */

static FxKey fxk_table[FXK_N][FXK_ALTS] = {
    /* pulse effect, fragment shader: the start of the statement that makes
     * the pulse value from a sine of the time between two thresholds (the
     * first statement of the frame-constant run) */
    [FXK_PULSE_FIRST] = { { 61, 0xd01c885fu, 0xc1bdde98068456e6ull } },
    /* pulse effect: the statement that raises the pulse value to the
     * effect's power constant (the last statement of the run) */
    [FXK_PULSE_LAST] = { { 25, 0x533817c8u, 0x8f004d9c432eced5ull } },
    /* shake effect, fragment shader, older versions: the statement that
     * makes the shake time from speed, time and the phase (first of the run) */
    [FXK_SHAKE_FIRST_A] = { { 35, 0x2303f501u, 0x3e84a84600d3d470ull } },
    /* shake effect, newer versions: the start of the statement that makes
     * four phases at once from speed and time (first of the run) */
    [FXK_SHAKE_FIRST_B] = { { 52, 0xb00881c2u, 0x39a67b45241b1c49ull } },
    /* shake effect: the statement that brings the offset into the range
     * the vertex shader prepared from the bounds (last of the run) */
    [FXK_SHAKE_LAST] = { { 48, 0x8bf477f5u, 0x889d4df186f66b1cull } },
    /* shake effect: the declaration that starts the phase at zero (must be
     * there: no per-pixel phase so far) */
    [FXK_SHAKE_PHASE0] = { { 19, 0x55e9f7cdu, 0xa0ee8a7c31baa2e2ull } },
    /* shake effect, vertex shader: the two statements that hand the lower
     * bound and one over the width of the bounds to the fragment shader */
    [FXK_SHAKE_VBOUNDS] = { { 61, 0x22e6e6e7u, 0xc4792c0d5e426878ull } },
    /* shake effect: the start of an assignment of a texture read to the
     * phase (must NOT be there: a time-offset map makes the phase differ
     * from pixel to pixel) */
    [FXK_SHAKE_PHASEMAP] = { { 21, 0xd4d0d4fbu, 0xf8f53e37bb54045aull } },

    /* blur effect, gaussian pass: the one statement that sums the 13 taps
     * of the widest kernel with their weights */
    [FXK_BLUR13] = { { 625, 0xdfde7b80u, 0x351f7579ff1835ffull } },
    /* the same for the 7-tap kernel */
    [FXK_BLUR7] = { { 340, 0xa25ba132u, 0xa3b907c5c09a343bull } },
    /* the same for the 3-tap kernel */
    [FXK_BLUR3] = { { 139, 0xfb8cfd53u, 0x4cc3f4cb51e33ecaull } },

    /* shine / god rays, cast pass: the whole loop of the ray march (read
     * along the direction, step, add the sample with its falloff) */
    [FXK_CAST_LOOP] = { { 127, 0xf5119195u, 0x1254fea4bb375318ull } },
    /* shine: the signature of the function that gathers along one
     * direction (tells shine from god rays) */
    [FXK_CAST_GATHER] = { { 48, 0x4b15158du, 0x8124ec00a4192c80ull } },
    /* cast pass: the declaration of the sample count as 50 / 30 / 15 */
    [FXK_CAST_N50] = { { 23, 0xbb5b5460u, 0x7154945ef9795e97ull } },
    [FXK_CAST_N30] = { { 23, 0x6f565f8eu, 0x3cef165edbad3ea1ull } },
    [FXK_CAST_N15] = { { 23, 0x2851729bu, 0x4d5e685ee4bbe006ull } },

    /* opacity effect without a mask, fragment shader: the whole main
     * function - read the texel under the pixel, a mask of one, multiply
     * the alpha by mask and user alpha, write the result */
    [FXK_OPACITY_MAIN] = { { 122, 0x8954f95au, 0xc78204ba9814d037ull } },

    /* vertex shaders that work on a local copy of the position: the
     * declaration of that copy from the position attribute */
    [FXK_POS_LOCAL] = { { 24, 0x6c8c506fu, 0x4b0f8dae8811bbf6ull } },
    /* ... and the product of that copy (as a point, w written 1.0 / 1)
     * with the model-view-projection matrix */
    [FXK_MVP_LOCAL] = { { 51, 0x86fe4b12u, 0x5e3ef93c4eb817b7ull } },
    [FXK_MVP_LOCAL_INT] = { { 49, 0x1a537238u, 0xac50a85db493bab9ull } },

    /* transform effect in "Vertex" mode: the statement that puts the
     * position's xy through the effect's function */
    [FXK_XF_APPLY] = { { 33, 0x7aa789f1u, 0x097524ea8c8e72ceull } },
    /* transform effect: the return statement of that function (offset,
     * scale, back from the centre) */
    [FXK_XF_RETURN] = { { 38, 0x13838fd2u, 0x7b1ef28961e30dafull } },
    /* transform effect: the rotation about the centre by the negative /
     * by the positive direction constant (two versions of the file) */
    [FXK_XF_ROT_NEG] = { { 40, 0x0ed963d2u, 0x73a81db825caa60full } },
    [FXK_XF_ROT_POS] = { { 39, 0xd5991b57u, 0x69993b1fa3d42cdcull } },

    /* shake effect: the read of the two-channel flow map on unit 1 at the
     * mask coordinate */
    [FXK_RG_FLOW_READ] = { { 56, 0xe0a70081u, 0xf202b6b90a9043dcull } },
    /* shake effect: the statement that centres the flow map's value and
     * doubles it */
    [FXK_RG_FLOW_MASK] = { { 51, 0x7402d2deu, 0x4c72128499c6c043ull } },
    /* water waves: the read of the opacity mask's red channel on unit 1 */
    [FXK_RG_MASK_READ] = { { 50, 0x49b1054au, 0xc504141e0f988333ull } },
    /* water waves: the mask declared as one (the MASK combo is off) */
    [FXK_RG_MASK_ONE] = { { 14, 0x7b121c5du, 0x24e24db676815e74ull } },
    /* iris movement: the mask declared as one and then multiplied by the
     * red channel of the mask texture on unit 1 (two statements) */
    [FXK_RG_IRIS_MASK] = { { 60, 0x4ebba93du, 0xaad4265f3f5337d0ull } },
};

#define FXK_ROLL_BASE 16777619u

/* FNV-1a, 64 bit, with fxr_hash ()'s basis */
static inline guint64 fxk_hash (const char *s, gsize n)
{
    guint64 h = 1469598103934665603ull;
    for (gsize i = 0; i < n; i++) {
        h ^= (guint8) s[i];
        h *= 1099511628211ull;
    }
    return h;
}

/* sum of s[i] * base ^ (n - 1 - i), modulo 2^32: can be moved along a
 * text one character at a time (fxk_scan) */
static inline guint32 fxk_roll (const char *s, gsize n)
{
    guint32 r = 0;
    for (gsize i = 0; i < n; i++)
        r = r * FXK_ROLL_BASE + (guint8) s[i];
    return r;
}

/* the first place in s[0 .. n) whose k->len characters have key k */
static inline const char *fxk_scan (const char *s, gsize n, const FxKey *k)
{
    const gsize len = k->len;
    if (!len || n < len)
        return NULL;
    guint32 top = 1;   /* base ^ (len - 1) */
    for (gsize i = 1; i < len; i++)
        top *= FXK_ROLL_BASE;
    guint32 r = fxk_roll (s, len);
    for (gsize i = 0;; i++) {
        if (r == k->roll && fxk_hash (s + i, len) == k->hash)
            return s + i;
        if (i + len >= n)
            return NULL;
        r = (r - (guint32) (guint8) s[i] * top) * FXK_ROLL_BASE + (guint8) s[i + len];
    }
}

/* strstr () for a slot: the first place in s where a statement of slot id
 * stands, or NULL; *len (may be NULL) = how many characters it has */
static inline const char *fxk_find (const char *s, FxKeyId id, gsize *len)
{
    const char *best = NULL;
    if (!s || id <= FXK_NONE || id >= FXK_N)
        return NULL;
    const gsize n = strlen (s);
    for (int a = 0; a < FXK_ALTS; a++) {
        const FxKey *k = &fxk_table[id][a];
        const char *p = fxk_scan (s, n, k);
        if (p && (!best || p < best)) {
            best = p;
            if (len)
                *len = k->len;
        }
    }
    return best;
}

static inline gboolean fxk_has (const char *s, FxKeyId id)
{
    return fxk_find (s, id, NULL) != NULL;
}

/* g_str_has_prefix () for a slot: s starts with a statement of slot id;
 * *len (may be NULL) = how many characters it has */
static inline gboolean fxk_at (const char *s, FxKeyId id, gsize *len)
{
    if (!s || id <= FXK_NONE || id >= FXK_N)
        return FALSE;
    for (int a = 0; a < FXK_ALTS; a++) {
        const FxKey *k = &fxk_table[id][a];
        gsize have = 0;
        while (have < k->len && s[have])
            have++;
        if (!k->len || have < k->len)
            continue;
        if (fxk_roll (s, k->len) == k->roll && fxk_hash (s, k->len) == k->hash) {
            if (len)
                *len = k->len;
            return TRUE;
        }
    }
    return FALSE;
}

/* Tests only: slot id also recognises `text` (white space is removed
 * here, as in compiled code); NULL takes that back. The stock key of the
 * slot is not touched. */
G_GNUC_UNUSED static inline void fxk_test_register (FxKeyId id, const char *text)
{
    if (id <= FXK_NONE || id >= FXK_N)
        return;
    FxKey *k = &fxk_table[id][FXK_ALTS - 1];
    memset (k, 0, sizeof *k);
    if (!text)
        return;
    char *t = g_malloc (strlen (text) + 1);
    gsize n = 0;
    for (const char *p = text; *p; p++)
        if (!g_ascii_isspace (*p))
            t[n++] = *p;
    t[n] = 0;
    if (n > 0 && n <= G_MAXUINT16) {
        k->len = (guint16) n;
        k->roll = fxk_roll (t, n);
        k->hash = fxk_hash (t, n);
    }
    g_free (t);
}
