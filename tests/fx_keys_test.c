/* fx_keys_test - statements of stock effect shaders, recognised by key
 * (src/engine/fx_keys.h and the recognisers that use it: fx_rewrite.h,
 * fx_vertex_traits () in scenenode_effects.c).
 *
 *   fx-keys-test [--strict]
 *
 * 1. Always; no GL, no file of Wallpaper Engine:
 *    - the key functions: the hash is the one of the pinned shader hashes
 *      (fxr_hash), a key finds exactly what strstr () finds for the
 *      statement it was made from (20 000 seeded texts over a small
 *      alphabet, needles cut from them, changed in one character or made
 *      up), the prefix test, texts shorter than the statement, the first
 *      of several places, the two keys of a slot, registering a statement
 *      and taking it back;
 *    - the recognisers, reached through the slots with statements written
 *      for this test: the local copy of the position in a vertex shader,
 *      the transform effect's function, a kernel statement, a ray march
 *      and its sample counts, a frame-constant run. Each is not
 *      recognised before its statements are registered;
 *    - the stock keys are untouched afterwards.
 * 2. Only when WALPE_WE_ASSETS names a Wallpaper Engine install (skipped
 *    otherwise, and the output says so): the install's own pulse, shake,
 *    blur, god rays, shine, opacity, transform, water waves and iris
 *    shaders are built the way the engine builds them, with the combos
 *    that make each statement part of the compiled code, and the stock
 *    key of every slot is looked for where that statement belongs. Then
 *    what follows from a statement found: the weights and offsets of the
 *    bilinear reads that replace each blur kernel are those of the taps
 *    they merge (read from the statement in the install), the ray march
 *    goes down one sample count, the transform effect is known with its
 *    direction, the frame-constant runs move to the vertex shader.
 *    Another version of Wallpaper Engine may word a shader differently:
 *    WalPE then simply does not rewrite that pass, and this part reports
 *    a note, not a failure. --strict turns those notes into failures (for
 *    checking the key table against a known install).
 *
 * The engine file is included so the static functions are reachable. */
#include "../src/engine/scenenode_effects.c"
#include <stdio.h>

static int fails, checks, notes;
static gboolean strict;

static void result (gboolean ok, const char *what, const char *detail)
{
    checks++;
    if (!ok)
        fails++;
    printf ("%s  %s%s%s\n", ok ? "ok  " : "FAIL", what, detail ? ": " : "", detail ? detail : "");
}

/* something that depends on the version of the install */
static void soft (gboolean ok, const char *what, const char *detail)
{
    if (ok || strict) {
        result (ok, what, detail);
        return;
    }
    notes++;
    printf ("note  %s%s%s\n", what, detail ? ": " : "", detail ? detail : "");
}

static guint32 pick (guint32 *s, guint32 n)
{
    *s = *s * 1664525u + 1013904223u;
    return (*s >> 8) % n;
}

/* ------------------------------------------------- 1. the key functions */

#define SLOT FXK_CAST_N15   /* any slot: its second key is this test's */

static void key_checks (void)
{
    /* the table as it is built in */
    gboolean rows = TRUE, empty = TRUE, distinct = TRUE;
    for (int i = FXK_NONE + 1; i < FXK_N; i++) {
        rows = rows && fxk_table[i][0].len > 0 && fxk_table[i][0].hash != 0;
        empty = empty && fxk_table[i][FXK_ALTS - 1].len == 0;
        for (int j = FXK_NONE + 1; j < i; j++)
            distinct = distinct && !(fxk_table[i][0].len == fxk_table[j][0].len &&
                                     fxk_table[i][0].hash == fxk_table[j][0].hash);
    }
    result (rows && fxk_table[FXK_NONE][0].len == 0, "every slot has its stock key", NULL);
    result (empty, "no slot has a second key before a test registers one", NULL);
    result (distinct, "no two slots have the same key", NULL);

    /* the functions */
    result (fxk_hash ("", 0) == 1469598103934665603ull && fxk_hash ("ab|cd", 5) == fxr_hash ("ab", "cd") &&
            fxk_hash ("ab|cd", 5) != fxk_hash ("ab|ce", 5) && fxk_hash ("ab|cd", 4) != fxk_hash ("ab|cd", 5),
            "the hash is the function of the pinned shader hashes (fxr_hash)", NULL);
    result (fxk_roll ("", 0) == 0 && fxk_roll ("a", 1) == 97u &&
            fxk_roll ("ab", 2) == (guint32) (97u * FXK_ROLL_BASE + 98u),
            "the fingerprint is the sum of the characters times powers of its base", NULL);

    /* a key finds what strstr () finds */
    static const char alpha[] = "ab=;(x";
    const guint na = sizeof (alpha) - 1;
    guint32 s = 20260u;
    int wrong_find = 0, wrong_at = 0, hits = 0, misses = 0;
    char text[260], needle[40];
    for (int n = 0; n < 20000; n++) {
        guint tl = pick (&s, 240), nl;
        for (guint i = 0; i < tl; i++)
            text[i] = alpha[pick (&s, na)];
        text[tl] = 0;
        if (tl > 0 && pick (&s, 3)) {
            /* a piece of the text; one in four with one character changed */
            guint a = pick (&s, tl);
            nl = 1 + pick (&s, MIN (tl - a, 32u));
            memcpy (needle, text + a, nl);
            if (pick (&s, 4) == 0)
                needle[pick (&s, nl)] = alpha[pick (&s, na)];
        } else {
            nl = 1 + pick (&s, 12);
            for (guint i = 0; i < nl; i++)
                needle[i] = alpha[pick (&s, na)];
        }
        needle[nl] = 0;
        fxk_test_register (SLOT, needle);
        gsize len = 0;
        const char *want = strstr (text, needle), *got = fxk_find (text, SLOT, &len);
        if (want != got || (got && len != nl) || fxk_has (text, SLOT) != (want != NULL))
            wrong_find++;
        if (want)
            hits++;
        else
            misses++;
        const char *from = text + (tl ? pick (&s, tl) : 0);
        len = 0;
        gboolean at = fxk_at (from, SLOT, &len);
        if (at != g_str_has_prefix (from, needle) || (at && len != nl))
            wrong_at++;
    }
    char *d = g_strdup_printf ("%d texts with the statement, %d without", hits, misses);
    result (!wrong_find && hits > 2000 && misses > 2000, "a key finds what strstr () finds, where strstr () finds it", d);
    result (!wrong_at, "a key at the start of a text is what g_str_has_prefix () says", NULL);
    g_free (d);

    /* the ends */
    fxk_test_register (SLOT, "tail;");
    result (!fxk_find ("tail", SLOT, NULL) && !fxk_find ("", SLOT, NULL) && !fxk_at ("tail", SLOT, NULL) &&
            !fxk_at ("", SLOT, NULL), "a text shorter than the statement does not hold it", NULL);
    const char *e = "head;tail;", *w = "tail;", *two = "x;tail;tail;";
    result (fxk_find (e, SLOT, NULL) == e + 5 && fxk_find (w, SLOT, NULL) == w && fxk_at (w, SLOT, NULL),
            "a statement at the end of the text, and one that is the whole text", NULL);
    result (fxk_find (two, SLOT, NULL) == two + 2 && fxk_find (two + 3, SLOT, NULL) == two + 7,
            "of several places the first one, and the next one from behind it", NULL);
    result (!fxk_has ("head;tall;", SLOT) && !fxk_has ("head;tail:", SLOT) && !fxk_has ("head;Tail;", SLOT),
            "one changed character and the statement is not there", NULL);
    fxk_test_register (SLOT, " a  = b\t+ 1 ;\n");
    result (fxk_has ("x;a=b+1;y", SLOT) && fxk_table[SLOT][FXK_ALTS - 1].len == 6,
            "a registered statement is compared without its white space, like compiled code", NULL);
    fxk_test_register (SLOT, NULL);
    result (!fxk_has ("x;a=b+1;y", SLOT) && fxk_table[SLOT][FXK_ALTS - 1].len == 0,
            "a registered statement can be taken back", NULL);
    fxk_test_register (SLOT, " \n");
    result (!fxk_has ("x;a=b+1;y", SLOT) && !fxk_at ("x", SLOT, NULL), "an empty statement is no key", NULL);
    fxk_test_register (FXK_NONE, "abc");
    fxk_test_register (FXK_N, "abc");
    result (!fxk_find ("abc", FXK_NONE, NULL) && !fxk_find ("abc", FXK_N, NULL) && !fxk_at ("abc", FXK_NONE, NULL) &&
            !fxk_at ("abc", FXK_N, NULL) && !fxk_find (NULL, SLOT, NULL) && !fxk_at (NULL, SLOT, NULL),
            "no slot or no text: nothing is found", NULL);

    /* both keys of a slot count (the first one stands in for a stock key
     * here, and is put back) */
    const FxKey keep = fxk_table[SLOT][0];
    fxk_table[SLOT][0] = (FxKey) { 6, fxk_roll ("first;", 6), fxk_hash ("first;", 6) };
    fxk_test_register (SLOT, "second-one;");
    const char *t1 = "x;second-one;first;", *t2 = "x;first;second-one;";
    gsize l1 = 0, l2 = 0, l3 = 0, l4 = 0;
    gboolean both = fxk_find (t1, SLOT, &l1) == t1 + 2 && l1 == 11 && fxk_find (t2, SLOT, &l2) == t2 + 2 && l2 == 6 &&
                    fxk_at ("first;z", SLOT, &l3) && l3 == 6 && fxk_at ("second-one;z", SLOT, &l4) && l4 == 11;
    fxk_table[SLOT][0] = keep;
    fxk_test_register (SLOT, NULL);
    result (both, "either key of a slot is found, the earlier place first, with its own length", NULL);
}

/* ------------------------------ 1. the recognisers, through the slots */

#define VS_DECL "uniform mat4 g_ModelViewProjectionMatrix;\nattribute vec3 a_Position;\n" \
                "attribute vec2 a_TexCoord;\nvarying vec2 v_TexCoord;\n"
static const char *VS_PLAIN =
    VS_DECL "void main() {\n  gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);\n"
    "  v_TexCoord = a_TexCoord;\n}\n";

/* this test's statements for the slots */
#define OWN_POS "vec3 position = vec3(a_Position);"
#define OWN_MVP "mul(vec4(position.xyz, 1.0), g_ModelViewProjectionMatrix)"
#define OWN_MVP_INT "mul(vec4(position.xyz, 1), g_ModelViewProjectionMatrix)"
#define OWN_APPLY "position.xy = nudge(position.xy);"
#define OWN_RETURN "return vec2(0.5) + g_Scale * (v + g_Offset);"
#define OWN_ROT_NEG "v = spin2(v - vec2(0.5), -g_Direction);"
#define OWN_ROT_POS "v = spin2(v - vec2(0.5), g_Direction);"
#define OWN_TAPS "vec4 albedo = (texSample2D(g_Texture0, v_TexCoord[0]) + texSample2D(g_Texture0, v_TexCoord[2])) * 0.5;"
#define OWN_LOOP "for (int i = 0; i < steps; i++) { at += stride; " \
                 "sum += texSample2D(g_Texture0, at) * (1.0 - float(i) / float(steps)); }"
#define OWN_GATHER "vec4 gather(vec2 at, vec2 stride)"
#define OWN_N(n) "const int steps = " #n ";"
#define OWN_FIRST "pulse = 0.5 + 0.5 * sin("
#define OWN_LAST "pulse = pulse * pulse * g_Curve;"

#define VS_LOCAL(product, more) \
    VS_DECL "void main() {\n  " OWN_POS "\n  gl_Position = " product ";\n  v_TexCoord = a_TexCoord" more ";\n}\n"
#define VS_XF(rot, apply) \
    VS_DECL "uniform vec2 g_Offset;\nuniform vec2 g_Scale;\nuniform float g_Direction;\n" \
    "vec2 spin2(vec2 v, float a) {\n  float c = cos(a), s = sin(a);\n" \
    "  return vec2(c * v.x - s * v.y, s * v.x + c * v.y);\n}\n" \
    "vec2 nudge(vec2 v) {\n  " rot "\n  " OWN_RETURN "\n}\n" \
    "void main() {\n  " OWN_POS "\n  " apply "\n  gl_Position = " OWN_MVP ";\n  v_TexCoord = a_TexCoord;\n}\n"

static char *built (const char *body, gboolean vertex, const char *combo, int value)
{
    GHashTable *combos = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    if (combo)
        g_hash_table_insert (combos, g_strdup (combo), GINT_TO_POINTER (value));
    char *s = fx_build_source (body, combos, vertex);
    g_hash_table_unref (combos);
    return s;
}

static void vtraits (const char *body, EffPass *pass)
{
    char *v = built (body, TRUE, NULL, 0);
    memset (pass, 0, sizeof (*pass));
    fx_vertex_traits (pass, v);
    g_free (v);
}

static void vertex_checks (void)
{
    EffPass p;
    vtraits (VS_PLAIN, &p);
    result (!p.moves_verts && !p.partial && !p.xf_sign, "the plain quad needs no key", NULL);

    /* a local copy of the position, used once in the matrix product */
    vtraits (VS_LOCAL (OWN_MVP, ""), &p);
    result (p.moves_verts && p.partial, "a local copy of the position in a wording nobody registered moves the vertices",
            NULL);
    fxk_test_register (FXK_POS_LOCAL, OWN_POS);
    vtraits (VS_LOCAL (OWN_MVP, ""), &p);
    result (p.moves_verts, "... and still does with only the declaration of the copy in its slot", NULL);
    fxk_test_register (FXK_MVP_LOCAL, OWN_MVP);
    vtraits (VS_LOCAL (OWN_MVP, ""), &p);
    result (!p.moves_verts && !p.partial && !p.xf_sign,
            "with the copy and the product in their slots the pass is the plain quad", NULL);
    vtraits (VS_LOCAL (OWN_MVP " * 2.0", ""), &p);
    result (!p.moves_verts && p.partial, "the product scaled afterwards may not cover the target", NULL);
    vtraits (VS_LOCAL (OWN_MVP, " + position.xy * 0.0"), &p);
    result (p.moves_verts, "a third use of the copy moves the vertices", NULL);
    vtraits (VS_LOCAL (OWN_MVP_INT, ""), &p);
    result (p.moves_verts, "the product with the one written as an integer is another statement", NULL);
    fxk_test_register (FXK_MVP_LOCAL_INT, OWN_MVP_INT);
    vtraits (VS_LOCAL (OWN_MVP_INT, ""), &p);
    result (!p.moves_verts && !p.partial, "... that has a slot of its own", NULL);

    /* the transform effect: apply, return and one of the two rotations */
    vtraits (VS_XF (OWN_ROT_NEG, OWN_APPLY), &p);
    result (p.xf_sign == 0, "a transform function in a wording nobody registered is not the transform effect", NULL);
    fxk_test_register (FXK_XF_APPLY, OWN_APPLY);
    fxk_test_register (FXK_XF_RETURN, OWN_RETURN);
    vtraits (VS_XF (OWN_ROT_NEG, OWN_APPLY), &p);
    result (p.xf_sign == 0, "... nor without its rotation", NULL);
    fxk_test_register (FXK_XF_ROT_NEG, OWN_ROT_NEG);
    vtraits (VS_XF (OWN_ROT_NEG, OWN_APPLY), &p);
    result (p.xf_sign == -1 && p.moves_verts, "the transform effect that turns by the negative direction", NULL);
    vtraits (VS_XF (OWN_ROT_POS, OWN_APPLY), &p);
    result (p.xf_sign == 0, "the other rotation is another statement", NULL);
    fxk_test_register (FXK_XF_ROT_POS, OWN_ROT_POS);
    vtraits (VS_XF (OWN_ROT_POS, OWN_APPLY), &p);
    result (p.xf_sign == 1, "... that has a slot of its own: the positive direction", NULL);
    vtraits (VS_XF (OWN_ROT_NEG, "position.xy += vec2(0.0);"), &p);
    result (p.xf_sign == 0, "the function without the statement that applies it is not the transform effect", NULL);

    static const FxKeyId used[] = { FXK_POS_LOCAL, FXK_MVP_LOCAL, FXK_MVP_LOCAL_INT, FXK_XF_APPLY,
                                    FXK_XF_RETURN, FXK_XF_ROT_NEG, FXK_XF_ROT_POS };
    for (guint i = 0; i < G_N_ELEMENTS (used); i++)
        fxk_test_register (used[i], NULL);
    vtraits (VS_XF (OWN_ROT_NEG, OWN_APPLY), &p);
    result (p.xf_sign == 0 && p.moves_verts, "taken back, the statements are unknown again", NULL);
}

static void rewrite_checks (void)
{
    /* a kernel statement: the one found is what gets replaced */
    static const char *fs_taps =
        "uniform sampler2D g_Texture0;\nvarying vec2 v_TexCoord[3];\n"
        "void main() {\n  " OWN_TAPS "\n  gl_FragColor = albedo;\n}\n";
    char *f = built (fs_taps, FALSE, NULL, 0), *fact = fx_active_code (f);
    result (!fxs_rw_blur (&f, fact) && strstr (f, OWN_TAPS) != NULL, "a kernel statement nobody registered stays", NULL);
    fxk_test_register (FXK_BLUR3, OWN_TAPS);
    gboolean ok = fxs_rw_blur (&f, fact);
    result (ok && !strstr (f, OWN_TAPS) && strstr (f, "vec2 _wd = v_TexCoord[2] - v_TexCoord[1];") != NULL &&
            strstr (f, "\n  gl_FragColor = albedo;\n}\n") != NULL,
            "registered as the 3-tap kernel it is replaced by that kernel's two reads, the rest stays", NULL);
    g_free (f);
    f = built (fs_taps, FALSE, NULL, 0);
    fxk_test_register (FXK_BLUR3, NULL);
    fxk_test_register (FXK_BLUR7, OWN_TAPS);
    ok = fxs_rw_blur (&f, fact);
    result (ok && !strstr (f, OWN_TAPS) && strstr (f, "vec2 _wd = v_TexCoord[4] - v_TexCoord[3];") != NULL,
            "registered as the 7-tap kernel, by that kernel's four reads", NULL);
    fxk_test_register (FXK_BLUR7, NULL);
    g_free (f);
    g_free (fact);

    /* a ray march: one sample count down, by the combo in front of the prelude */
    static const char *fs_march =
        "uniform sampler2D g_Texture0;\nvarying vec2 v_TexCoord;\n"
        "#if SAMPLES == 2\n" OWN_N (50) "\n#endif\n#if SAMPLES == 1\n" OWN_N (30) "\n#endif\n"
        "#if SAMPLES == 0\n" OWN_N (15) "\n#endif\n"
        OWN_GATHER " {\n  vec4 sum = vec4(0.0);\n  " OWN_LOOP "\n  return sum * (3.0 / float(steps));\n}\n"
        "void main() {\n  gl_FragColor = gather(v_TexCoord, (vec2(0.5) - v_TexCoord) / float(steps));\n}\n";
    char *v = built (VS_PLAIN, TRUE, "SAMPLES", 2);
    f = built (fs_march, FALSE, "SAMPLES", 2);
    fact = fx_active_code (f);
    result (!fxs_rw_cast (&v, &f, fact) && strstr (f, "#define SAMPLES 2\n") != NULL,
            "a ray march nobody registered keeps its sample count", NULL);
    fxk_test_register (FXK_CAST_LOOP, OWN_LOOP);
    result (!fxs_rw_cast (&v, &f, fact), "... also with its loop alone in the slot", NULL);
    fxk_test_register (FXK_CAST_N50, OWN_N (50));
    fxk_test_register (FXK_CAST_N30, OWN_N (30));
    fxk_test_register (FXK_CAST_N15, OWN_N (15));
    /* without the gathering function it counts as god rays: 50 reads are
     * combo value 1 there, and this source says 2 */
    result (!fxs_rw_cast (&v, &f, fact) && strstr (f, "#define SAMPLES 2\n") != NULL,
            "the god rays' count is not changed in a shader whose combo has another value for it", NULL);
    g_free (v);
    g_free (f);
    g_free (fact);
    v = built (VS_PLAIN, TRUE, "SAMPLES", 1);
    f = built ("uniform sampler2D g_Texture0;\nvarying vec2 v_TexCoord;\n"
               "#if SAMPLES == 1\n" OWN_N (50) "\n#else\n" OWN_N (30) "\n#endif\n"
               "void main() {\n  vec2 at = v_TexCoord, stride = (vec2(0.5) - at) / float(steps);\n"
               "  vec4 sum = vec4(0.0);\n  " OWN_LOOP "\n  gl_FragColor = sum * (3.0 / float(steps));\n}\n",
               FALSE, "SAMPLES", 1);
    fact = fx_active_code (f);
    ok = fxs_rw_cast (&v, &f, fact);
    result (ok && strstr (f, "#define SAMPLES 0\n") != NULL && !strstr (f, "#define SAMPLES 1\n") &&
            strstr (v, "#define SAMPLES 0\n") != NULL,
            "a march of 50 reads without the gathering function goes down like the god rays (combo 1 -> 0)", NULL);
    g_free (fact);
    fact = fx_active_code (f);
    result (!fxs_rw_cast (&v, &f, fact), "... and its 30 reads are kept", NULL);
    g_free (v);
    g_free (f);
    g_free (fact);
    /* with the gathering function it is the shine effect: 50 reads are
     * combo value 4, 30 are 3, 15 are 2 */
    fxk_test_register (FXK_CAST_GATHER, OWN_GATHER);
    static const char *fs_shine =
        "uniform sampler2D g_Texture0;\nvarying vec2 v_TexCoord;\n"
        "#if SAMPLES == 4\n" OWN_N (50) "\n#endif\n#if SAMPLES == 3\n" OWN_N (30) "\n#endif\n"
        "#if SAMPLES == 2\n" OWN_N (15) "\n#endif\n#if SAMPLES == 1\n" OWN_N (8) "\n#endif\n"
        OWN_GATHER " {\n  vec4 sum = vec4(0.0);\n  " OWN_LOOP "\n  return sum * (3.0 / float(steps));\n}\n"
        "void main() {\n  gl_FragColor = gather(v_TexCoord, (vec2(0.5) - v_TexCoord) / float(steps));\n}\n";
    int down = 0;
    for (int n = 4; n >= 1; n--) {
        v = built (VS_PLAIN, TRUE, "SAMPLES", n);
        f = built (fs_shine, FALSE, "SAMPLES", n);
        fact = fx_active_code (f);
        char *want = g_strdup_printf ("#define SAMPLES %d\n", n - 1);
        ok = fxs_rw_cast (&v, &f, fact);
        if (n >= 2 ? ok && strstr (f, want) != NULL && strstr (v, want) != NULL : !ok)
            down++;
        g_free (want);
        g_free (v);
        g_free (f);
        g_free (fact);
    }
    result (down == 4, "with the gathering function it is the shine effect: 4 -> 3, 3 -> 2, 2 -> 1, and 1 is kept", NULL);
    static const FxKeyId cast[] = { FXK_CAST_LOOP, FXK_CAST_GATHER, FXK_CAST_N50, FXK_CAST_N30, FXK_CAST_N15 };
    for (guint i = 0; i < G_N_ELEMENTS (cast); i++)
        fxk_test_register (cast[i], NULL);

    /* a frame-constant run between its first and its last statement */
    static const char *fs_wave =
        "uniform float g_Time;\nuniform float g_Rate; // {\"material\":\"rate\",\"default\":2}\n"
        "uniform float g_Curve; // {\"material\":\"curve\",\"default\":1}\n"
        "uniform sampler2D g_Texture0;\nvarying vec2 v_TexCoord;\n"
        "void main() {\n  float pulse = 1.0;\n  " OWN_FIRST "g_Rate * g_Time);\n  " OWN_LAST "\n"
        "  vec4 c = texSample2D(g_Texture0, v_TexCoord);\n  gl_FragColor = vec4(c.rgb * pulse, c.a);\n}\n";
    static const char *fs_wave_twice =
        "uniform float g_Time;\nuniform float g_Rate;\nuniform float g_Curve;\n"
        "uniform sampler2D g_Texture0;\nvarying vec2 v_TexCoord;\n"
        "void main() {\n  float pulse = 1.0;\n  " OWN_FIRST "g_Rate * g_Time);\n  " OWN_FIRST "g_Time);\n  " OWN_LAST "\n"
        "  vec4 c = texSample2D(g_Texture0, v_TexCoord);\n  gl_FragColor = vec4(c.rgb * pulse, c.a);\n}\n";
    const FxsHoist *h = NULL;
    for (guint i = 0; i < G_N_ELEMENTS (fxs_hoists); i++)
        if (fxs_hoists[i].bit == FXS_RW_PULSE)
            h = &fxs_hoists[i];
    for (int pass = 0; h && pass < 3; pass++) {
        v = built (VS_PLAIN, TRUE, NULL, 0);
        f = built (pass == 2 ? fs_wave_twice : fs_wave, FALSE, NULL, 0);
        fx_prepass (&v, &f, "fx-keys-test");
        fact = fx_active_code (f);
        char *vact = fx_active_code (v);
        ok = fxs_rw_hoist (h, &v, &f, fact, vact);
        if (pass == 0) {
            result (!ok && !strstr (f, "_we_hoist0"), "a run nobody registered stays in the fragment shader", NULL);
            fxk_test_register (FXK_PULSE_FIRST, OWN_FIRST);
            fxk_test_register (FXK_PULSE_LAST, OWN_LAST);
        } else if (pass == 1) {
            result (ok && strstr (f, "pulse = _we_hoist0;") != NULL && strstr (f, "flat in float _we_hoist0;") != NULL &&
                    !strstr (f, OWN_LAST) && strstr (v, "flat out float _we_hoist0;") != NULL &&
                    strstr (v, OWN_LAST) != NULL && strstr (v, "g_Curve") != NULL,
                    "with its first and last statement in the slots the run moves to the vertex shader, as written",
                    NULL);
        } else {
            result (!ok && !strstr (f, "_we_hoist0"), "a first statement that occurs twice names no run", NULL);
        }
        g_free (v);
        g_free (f);
        g_free (fact);
        g_free (vact);
    }
    result (h != NULL, "the hoist table has the pulse run", NULL);
    fxk_test_register (FXK_PULSE_FIRST, NULL);
    fxk_test_register (FXK_PULSE_LAST, NULL);
}

/* ------------------------------- 2. the shaders of a real install */

static const char *const slot_name[FXK_N] = {
    [FXK_PULSE_FIRST] = "pulse: start of the run",
    [FXK_PULSE_LAST] = "pulse: end of the run",
    [FXK_SHAKE_FIRST_A] = "shake: start of the run (one phase)",
    [FXK_SHAKE_FIRST_B] = "shake: start of the run (four phases)",
    [FXK_SHAKE_LAST] = "shake: end of the run",
    [FXK_SHAKE_PHASE0] = "shake: the phase starts at zero",
    [FXK_SHAKE_VBOUNDS] = "shake: the bounds prepared by the vertex shader",
    [FXK_SHAKE_PHASEMAP] = "shake: the phase from a time-offset map",
    [FXK_BLUR13] = "blur: 13-tap kernel",
    [FXK_BLUR7] = "blur: 7-tap kernel",
    [FXK_BLUR3] = "blur: 3-tap kernel",
    [FXK_CAST_LOOP] = "cast: the loop of the ray march",
    [FXK_CAST_GATHER] = "cast: shine's gathering function",
    [FXK_CAST_N50] = "cast: 50 reads",
    [FXK_CAST_N30] = "cast: 30 reads",
    [FXK_CAST_N15] = "cast: 15 reads",
    [FXK_OPACITY_MAIN] = "opacity: the main function without a mask",
    [FXK_POS_LOCAL] = "vertex: local copy of the position",
    [FXK_MVP_LOCAL] = "vertex: matrix product of the copy (1.0)",
    [FXK_MVP_LOCAL_INT] = "vertex: matrix product of the copy (1)",
    [FXK_XF_APPLY] = "transform: the position through the function",
    [FXK_XF_RETURN] = "transform: the function's result",
    [FXK_XF_ROT_NEG] = "transform: rotation by the negative direction",
    [FXK_XF_ROT_POS] = "transform: rotation by the positive direction",
    [FXK_RG_FLOW_READ] = "shake: read of the flow map",
    [FXK_RG_FLOW_MASK] = "shake: the flow map's value centred",
    [FXK_RG_MASK_READ] = "mask: read of the opacity mask",
    [FXK_RG_MASK_ONE] = "mask: declared as one",
    [FXK_RG_IRIS_MASK] = "iris: the mask from the mask texture",
};

/* the two sources of one of the install's effect shaders, built the way
 * the engine builds a pass (fx_load_pass): declarations hoisted, includes
 * expanded, the combos' defaults from both files, `spec` ("NAME=n ...")
 * on top of them. FALSE when the install has no such shader. */
static gboolean stock_build (const char *effect, const char *shader, const char *spec, char **vsrc, char **fsrc)
{
    char *root = g_build_filename (walpe_we_assets_dir (), "effects", effect, NULL);
    char *body[2] = { NULL, NULL };
    for (int k = 0; k < 2; k++) {
        char *name = g_strconcat (shader, k ? ".frag" : ".vert", NULL);
        char *path = g_build_filename (root, "shaders", "effects", name, NULL);
        char *raw = fx_read_shader (path);
        if (raw) {
            char *sdir = g_path_get_dirname (path), *h = fx_hoist_decls (raw);
            GString *o = g_string_new (NULL);
            GHashTable *seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
            fx_expand_into (o, h, sdir, root, seen, 0);
            g_hash_table_unref (seen);
            body[k] = g_string_free (o, FALSE);
            g_free (sdir);
            g_free (h);
        }
        g_free (raw);
        g_free (path);
        g_free (name);
    }
    g_free (root);
    gboolean ok = body[0] && body[1];
    if (ok) {
        GHashTable *combos = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
        fx_collect_combo_defaults (body[0], combos);
        fx_collect_combo_defaults (body[1], combos);
        char **set = g_strsplit (spec, " ", -1);
        for (char **s = set; *s; s++) {
            const char *eq = strchr (*s, '=');
            if (eq)
                g_hash_table_insert (combos, g_strndup (*s, (gsize) (eq - *s)),
                                     GINT_TO_POINTER ((int) g_ascii_strtoll (eq + 1, NULL, 10)));
        }
        g_strfreev (set);
        for (int u = 0; u < FX_MAX_BINDS; u++) {
            char k[24];
            snprintf (k, sizeof (k), "TEX%dFORMAT", u);
            if (!g_hash_table_contains (combos, k))
                g_hash_table_insert (combos, g_strdup (k), GINT_TO_POINTER (0));
        }
        *vsrc = fx_build_source (body[0], combos, TRUE);
        *fsrc = fx_build_source (body[1], combos, FALSE);
        g_hash_table_unref (combos);
    }
    g_free (body[0]);
    g_free (body[1]);
    return ok;
}

typedef struct {
    const char *effect, *shader;
    gboolean vertex;        /* the statements are in the vertex shader */
    gboolean rewrite;       /* looked for where fx_rewrite.h looks: after the source inspection */
    const char *combos;
    FxKeyId has[6];         /* the compiled code holds these statements ... */
    FxKeyId hasnt[3];       /* ... and not these */
} Stock;

static const Stock stock[] = {
    { "pulse", "pulse", FALSE, TRUE, "AUDIOPROCESSING=0", { FXK_PULSE_FIRST, FXK_PULSE_LAST }, { FXK_NONE } },
    { "shake", "shake", FALSE, TRUE, "AUDIOPROCESSING=0 NOISE=0 TIMEOFFSET=0",
      { FXK_SHAKE_FIRST_A, FXK_SHAKE_LAST, FXK_SHAKE_PHASE0 }, { FXK_SHAKE_FIRST_B, FXK_SHAKE_PHASEMAP } },
    { "shake", "shake", FALSE, TRUE, "AUDIOPROCESSING=0 NOISE=1 TIMEOFFSET=0",
      { FXK_SHAKE_FIRST_B, FXK_SHAKE_LAST, FXK_SHAKE_PHASE0 }, { FXK_SHAKE_FIRST_A, FXK_SHAKE_PHASEMAP } },
    { "shake", "shake", FALSE, TRUE, "AUDIOPROCESSING=0 NOISE=0 TIMEOFFSET=1", { FXK_SHAKE_PHASEMAP }, { FXK_NONE } },
    { "shake", "shake", TRUE, TRUE, "AUDIOPROCESSING=0", { FXK_SHAKE_VBOUNDS }, { FXK_NONE } },
    { "shake", "shake", FALSE, FALSE, "AUDIOPROCESSING=0", { FXK_RG_FLOW_READ, FXK_RG_FLOW_MASK }, { FXK_NONE } },
    { "blur", "blur_gaussian", FALSE, TRUE, "KERNEL=0", { FXK_BLUR13 }, { FXK_BLUR7, FXK_BLUR3 } },
    { "blur", "blur_gaussian", FALSE, TRUE, "KERNEL=1", { FXK_BLUR7 }, { FXK_BLUR13, FXK_BLUR3 } },
    { "blur", "blur_gaussian", FALSE, TRUE, "KERNEL=2", { FXK_BLUR3 }, { FXK_BLUR13, FXK_BLUR7 } },
    { "godrays", "godrays_cast", FALSE, TRUE, "SAMPLES=1", { FXK_CAST_LOOP, FXK_CAST_N50 }, { FXK_CAST_GATHER } },
    { "godrays", "godrays_cast", FALSE, TRUE, "SAMPLES=0", { FXK_CAST_LOOP, FXK_CAST_N30 },
      { FXK_CAST_GATHER, FXK_CAST_N50 } },
    { "shine", "shine_cast", FALSE, TRUE, "SAMPLES=4", { FXK_CAST_LOOP, FXK_CAST_GATHER, FXK_CAST_N50 }, { FXK_NONE } },
    { "shine", "shine_cast", FALSE, TRUE, "SAMPLES=3", { FXK_CAST_LOOP, FXK_CAST_GATHER, FXK_CAST_N30 },
      { FXK_CAST_N50 } },
    { "shine", "shine_cast", FALSE, TRUE, "SAMPLES=2", { FXK_CAST_LOOP, FXK_CAST_GATHER, FXK_CAST_N15 },
      { FXK_CAST_N50, FXK_CAST_N30 } },
    { "opacity", "opacity", FALSE, TRUE, "MASK=0", { FXK_OPACITY_MAIN }, { FXK_NONE } },
    { "opacity", "opacity", FALSE, TRUE, "MASK=1", { FXK_NONE }, { FXK_OPACITY_MAIN } },
    { "transform", "transform", TRUE, FALSE, "MODE=1",
      { FXK_POS_LOCAL, FXK_MVP_LOCAL, FXK_XF_APPLY, FXK_XF_RETURN, FXK_XF_ROT_NEG }, { FXK_NONE } },
    { "transform", "transform", TRUE, FALSE, "MODE=0", { FXK_POS_LOCAL, FXK_MVP_LOCAL }, { FXK_XF_APPLY } },
    { "waterwaves", "waterwaves", FALSE, FALSE, "MASK=1", { FXK_RG_MASK_READ }, { FXK_RG_MASK_ONE } },
    { "waterwaves", "waterwaves", FALSE, FALSE, "MASK=0", { FXK_RG_MASK_ONE }, { FXK_RG_MASK_READ } },
    /* (the mask texture's combo: with it the mask is read, without it the mask stays one) */
    { "iris", "iris", FALSE, FALSE, "MASK=1", { FXK_RG_IRIS_MASK }, { FXK_NONE } },
    { "iris", "iris", FALSE, FALSE, "MASK=0", { FXK_NONE }, { FXK_RG_IRIS_MASK } },
};

/* the taps of a kernel statement found by key: every "v_TexCoord[i]) * w"
 * in its n characters gives w[i]. Number of taps, -1 when it does not read
 * like that */
static int kernel_taps (const char *hit, gsize n, double *w, int max)
{
    char *s = g_strndup (hit, n);
    int taps = 0;
    for (const char *p = s; taps >= 0 && (p = strstr (p, "v_TexCoord[")) != NULL;) {
        char *end = NULL;
        p += 11;
        guint64 i = g_ascii_strtoull (p, &end, 10);
        if (end == p || strncmp (end, "])*", 3) != 0 || i >= (guint64) max) {
            taps = -1;
        } else {
            w[i] = g_ascii_strtod (end + 3, NULL);
            taps++;
            p = end;
        }
    }
    g_free (s);
    return taps;
}

static gboolean about (double a, double b, double eps)
{
    return fabs (a - b) < eps;
}

/* the reads fxs_rw_blur () puts in place of a kernel are bilinear reads
 * between two taps each: the weight is the sum of the two, the place their
 * weighted mean, in tap steps from the tap the rewrite counts from */
static void stock_blur (void)
{
    static const FxKeyId slot[3] = { FXK_BLUR13, FXK_BLUR7, FXK_BLUR3 };
    static const int ntaps[3] = { 13, 7, 3 };
    for (int k = 0; k < 3; k++) {
        char spec[32], *v = NULL, *f = NULL;
        snprintf (spec, sizeof (spec), "KERNEL=%d", k);
        if (!stock_build ("blur", "blur_gaussian", spec, &v, &f))
            return;
        fx_prepass (&v, &f, "blur_gaussian");
        char *fact = fx_active_code (f);
        gsize n = 0;
        const char *hit = fxk_find (fact, slot[k], &n);
        double w[13] = { 0 };
        char *what = g_strdup_printf ("blur_gaussian, %d taps: the bilinear reads are the taps they merge", ntaps[k]);
        if (hit && kernel_taps (hit, n, w, 13) == ntaps[k]) {
            gboolean ok = TRUE;
            double sum = 0;
            for (int i = 0; i < ntaps[k]; i++) {
                sum += w[i];
                ok = ok && w[i] > 0 && w[i] == w[ntaps[k] - 1 - i];
            }
            ok = ok && about (sum, 1.0, 2e-5);
            if (k == 0)
                /* from the middle tap 6: pairs (5,4) (3,2) (1,0) and their mirrors */
                ok = ok && about (w[6], 0.171834, 1e-9) &&
                     about (w[5] + w[4], 0.275763, 1e-9) && about ((1 * w[5] + 2 * w[4]) / (w[5] + w[4]), 1.43155536, 1e-6) &&
                     about (w[3] + w[2], 0.114722, 1e-9) && about ((3 * w[3] + 4 * w[2]) / (w[3] + w[2]), 3.34459824, 1e-6) &&
                     about (w[1] + w[0], 0.023597, 1e-9) && about ((5 * w[1] + 6 * w[0]) / (w[1] + w[0]), 5.26694071, 1e-6);
            else if (k == 1)
                /* from the middle tap 3: (5,6) and (3,4) on one side, (2,1)
                 * and tap 0 on the other. The places agree to 2e-6 of a
                 * tap step */
                ok = ok && about (w[5] + w[6], 0.202817, 1e-9) && about ((2 * w[5] + 3 * w[6]) / (w[5] + w[6]), 2.35156440, 2e-6) &&
                     about (w[3] + w[4], 0.404486, 1e-9) && about (w[4] / (w[3] + w[4]), 0.46943378, 2e-6) &&
                     about (w[2] + w[1], 0.321393, 1e-9) && about ((1 * w[2] + 2 * w[1]) / (w[2] + w[1]), 1.40919988, 2e-6) &&
                     about (w[0], 0.071303, 1e-9);
            else
                /* from the middle tap 1: each side tap with half of the middle one */
                ok = ok && about (w[0] + w[1] / 2, 0.5, 1e-9) && about (w[0] / (w[0] + w[1] / 2), 0.5, 1e-9);
            /* ... and those are the numbers of the statement that replaces it */
            static const char *const in_place[3][3] = { { "_wd * 1.43155536", "* 0.275763", "_wd * 5.26694071" },
                                                         { "_wd * 2.35156440", "* 0.404486", "_wd * 1.40919988" },
                                                         { "_wd * 0.5", "* 0.5;", "v_TexCoord[2] - v_TexCoord[1]" } };
            ok = ok && fxs_rw_blur (&f, fact);
            for (int i = 0; i < 3; i++)
                ok = ok && strstr (f, in_place[k][i]) != NULL;
            result (ok, what, NULL);
        } else {
            soft (FALSE, what, hit ? "the statement does not list its taps as index and weight"
                                   : "the kernel statement of this install is another one");
        }
        g_free (what);
        g_free (fact);
        g_free (v);
        g_free (f);
    }
}

static void stock_cast (void)
{
    static const struct { const char *effect, *shader; int from, to; } c[] = {
        { "godrays", "godrays_cast", 1, 0 }, { "godrays", "godrays_cast", 0, -1 },
        { "shine", "shine_cast", 4, 3 }, { "shine", "shine_cast", 3, 2 }, { "shine", "shine_cast", 2, 1 },
        { "shine", "shine_cast", 1, -1 }, { "shine", "shine_cast", 0, -1 },
    };
    for (guint i = 0; i < G_N_ELEMENTS (c); i++) {
        char spec[32], *v = NULL, *f = NULL;
        snprintf (spec, sizeof (spec), "SAMPLES=%d", c[i].from);
        if (!stock_build (c[i].effect, c[i].shader, spec, &v, &f))
            continue;
        fx_prepass (&v, &f, c[i].shader);
        char *fact = fx_active_code (f);
        gboolean went = fxs_rw_cast (&v, &f, fact);
        char *want = g_strdup_printf ("#define SAMPLES %d\n", c[i].to);
        char *what = c[i].to >= 0 ? g_strdup_printf ("%s: the sample count goes from combo value %d to %d", c[i].shader,
                                                     c[i].from, c[i].to)
                                  : g_strdup_printf ("%s: the sample count of combo value %d is kept", c[i].shader,
                                                     c[i].from);
        soft (c[i].to >= 0 ? went && strstr (f, want) != NULL && strstr (v, want) != NULL : !went, what, NULL);
        g_free (what);
        g_free (want);
        g_free (fact);
        g_free (v);
        g_free (f);
    }
}

static void stock_transform (void)
{
    char *v = NULL, *f = NULL;
    EffPass p;
    if (stock_build ("transform", "transform", "MODE=1", &v, &f)) {
        memset (&p, 0, sizeof (p));
        fx_vertex_traits (&p, v);
        soft (p.xf_sign != 0, "transform in Vertex mode is known as the transform effect",
              p.xf_sign < 0 ? "negative direction" : p.xf_sign > 0 ? "positive direction" : NULL);
        g_free (v);
        g_free (f);
    }
    if (stock_build ("transform", "transform", "MODE=0", &v, &f)) {
        memset (&p, 0, sizeof (p));
        fx_vertex_traits (&p, v);
        soft (p.xf_sign == 0 && !p.moves_verts && !p.partial,
              "transform in UV mode: the local copy of the position is the plain quad", NULL);
        g_free (v);
        g_free (f);
    }
}

static void stock_hoist (void)
{
    static const struct { const char *effect, *shader, *combos; guint bit; gboolean moves; } r[] = {
        { "pulse", "pulse", "AUDIOPROCESSING=0", FXS_RW_PULSE, TRUE },
        { "pulse", "pulse", "AUDIOPROCESSING=1", FXS_RW_PULSE, FALSE },
        { "shake", "shake", "AUDIOPROCESSING=0 NOISE=0 TIMEOFFSET=0", FXS_RW_SHAKE, TRUE },
        { "shake", "shake", "AUDIOPROCESSING=0 NOISE=1 TIMEOFFSET=0", FXS_RW_SHAKE, TRUE },
        { "shake", "shake", "AUDIOPROCESSING=0 NOISE=0 TIMEOFFSET=1", FXS_RW_SHAKE, FALSE },
    };
    for (guint i = 0; i < G_N_ELEMENTS (r); i++) {
        char *v = NULL, *f = NULL;
        if (!stock_build (r[i].effect, r[i].shader, r[i].combos, &v, &f))
            continue;
        fx_prepass (&v, &f, r[i].shader);
        char *fact = fx_active_code (f), *vact = fx_active_code (v);
        gboolean moved = FALSE;
        for (guint k = 0; k < G_N_ELEMENTS (fxs_hoists) && !moved; k++)
            if (fxs_hoists[k].bit == r[i].bit)
                moved = fxs_rw_hoist (&fxs_hoists[k], &v, &f, fact, vact);
        char *what = g_strdup_printf ("%s [%s]: the frame-constant run %s", r[i].shader, r[i].combos,
                                      r[i].moves ? "moves to the vertex shader" : "stays (it is not frame-constant here)");
        soft (moved == r[i].moves && (!moved || (strstr (f, "_we_hoist0") && strstr (v, "_we_hoist0"))), what, NULL);
        g_free (what);
        g_free (fact);
        g_free (vact);
        g_free (v);
        g_free (f);
    }
}

static void stock_checks (void)
{
    const char *env = g_getenv ("WALPE_WE_ASSETS");
    if (!env || !*env) {
        printf ("skip  the checks on Wallpaper Engine's own shaders (WALPE_WE_ASSETS is not set)\n");
        return;
    }
    if (!walpe_we_assets_dir () || g_strcmp0 (walpe_we_assets_source (), "env")) {
        printf ("skip  the checks on Wallpaper Engine's own shaders (WALPE_WE_ASSETS names no install)\n");
        return;
    }
    gboolean seen[FXK_N] = { FALSE };
    for (guint i = 0; i < G_N_ELEMENTS (stock); i++) {
        const Stock *r = &stock[i];
        char *v = NULL, *f = NULL;
        char *what = g_strdup_printf ("%s.%s [%s]", r->shader, r->vertex ? "vert" : "frag", r->combos);
        if (!stock_build (r->effect, r->shader, r->combos, &v, &f)) {
            soft (FALSE, what, "the install has no such shader");
            g_free (what);
            continue;
        }
        if (r->rewrite)
            fx_prepass (&v, &f, r->shader);
        char *act = fx_active_code (r->vertex ? v : f);
        GString *miss = g_string_new (NULL);
        int n = 0;
        for (guint k = 0; k < G_N_ELEMENTS (r->has) && r->has[k] != FXK_NONE; k++) {
            if (fxk_has (act, r->has[k])) {
                seen[r->has[k]] = TRUE;
                n++;
            } else {
                g_string_append_printf (miss, "%snot found: %s", miss->len ? "; " : "", slot_name[r->has[k]]);
            }
        }
        for (guint k = 0; k < G_N_ELEMENTS (r->hasnt) && r->hasnt[k] != FXK_NONE; k++)
            if (fxk_has (act, r->hasnt[k]))
                g_string_append_printf (miss, "%sfound where it does not belong: %s", miss->len ? "; " : "",
                                        slot_name[r->hasnt[k]]);
        char *label = g_strdup_printf ("%s: %d statement(s) found by key, none where it does not belong", what, n);
        soft (miss->len == 0, label, miss->len ? miss->str : NULL);
        g_string_free (miss, TRUE);
        g_free (label);
        g_free (what);
        g_free (act);
        g_free (v);
        g_free (f);
    }
    stock_blur ();
    stock_cast ();
    stock_transform ();
    stock_hoist ();

    /* the slots no shader of this install answered: statements of other
     * versions of the files (the transform effect's rotation has two, the
     * matrix product of a local copy has two), or a version this table
     * does not know yet */
    GString *rest = g_string_new (NULL);
    int n_seen = 0;
    for (int i = FXK_NONE + 1; i < FXK_N; i++) {
        if (seen[i])
            n_seen++;
        else
            g_string_append_printf (rest, "%s%s", rest->len ? "; " : "", slot_name[i]);
    }
    printf ("info  %d of %d stock statements are in this install's shaders%s%s\n", n_seen, FXK_N - 1,
            rest->len ? "; not in it: " : "", rest->str);
    g_string_free (rest, TRUE);
}

int main (int argc, char **argv)
{
    strict = argc > 1 && !strcmp (argv[1], "--strict");
    FxKey before[FXK_N];
    for (int i = 0; i < FXK_N; i++)
        before[i] = fxk_table[i][0];
    key_checks ();
    vertex_checks ();
    rewrite_checks ();
    gboolean same = TRUE;
    for (int i = 0; i < FXK_N; i++)
        same = same && before[i].len == fxk_table[i][0].len && before[i].roll == fxk_table[i][0].roll &&
               before[i].hash == fxk_table[i][0].hash && fxk_table[i][FXK_ALTS - 1].len == 0;
    result (same, "the stock keys are what they were, and no slot keeps a statement of this test", NULL);
    stock_checks ();
    printf ("%s (%d check(s), %d failure(s), %d note(s))\n", fails ? "FAILED" : "PASSED", checks, fails, notes);
    return fails ? 1 : 0;
}
