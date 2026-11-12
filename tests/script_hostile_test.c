/* script_hostile_test - SceneScript against a hostile wallpaper script
 * (src/engine/scenescript.c), without GL. The host here is strict: it
 * fails the test when the runtime hands it something outside the contract
 * of scenescript.h (a number that is not finite, a count out of range, a
 * NULL string, a layer index it did not announce).
 *
 *  1. module rewriter: template literals nested 200 000 deep, unterminated
 *     strings / comments / regular expressions, an "import" at the very
 *     end, bytes of half a UTF-8 sequence at the end;
 *  2. a promise reaction that never returns is stopped like a script that
 *     never returns: the frames after it do not pay the time budget again;
 *  3. vectors with NaN / Infinity / 1e308 never reach the host, and text
 *     of megabytes reaches it cut to 64 KiB;
 *  4. thrown values whose toString / message never return or throw;
 *  5. a seeded driver that calls every member of the SceneScript API it
 *     can reach with hostile arguments (wrong types, NaN, huge numbers,
 *     strings with NULs, proxies, getters that throw, re-entrant setters)
 *     for 12 frames, plus cursor and media events: nothing may crash, hang
 *     or break the host contract (run under ASan + UBSan in sanitizer
 *     builds; `script-hostile-test driver <seeds>` runs more than the 6
 *     of the default run);
 *  6. log lines of a script are cut to a sane length;
 *  7. recursion without end comes back as an error on the render thread;
 *  8. an argument whose conversion to a number / string never returns (in
 *     C, inside a call into the scene) does not hang the thread;
 *  9. JSON the engine refuses (a number beyond the double range, written
 *     as inf.0) is reported without running Error.prototype.toString, and
 *     such numbers arrive as the largest double;
 * 10. the spectrum arrays of registerAudioBuffers: filled in place only
 *     when they are the engine's own Float32Array, whatever a script put
 *     in the global of that name, and without running script code while
 *     the storage of the arrays is held;
 * 11. time zone names: only names of zones reach GLib (a file of the zone
 *     directory that is no zone costs a critical warning per call);
 * 12. the watchdog's signal arrives while a call into the scene holds a
 *     lock: the call runs to its end;
 * 13. the script memory filled to the last block, in every place a script
 *     runs (QuickJS ends the process there, old releases and current
 *     ones): the wallpaper goes on, each case in a child process;
 * 14. many scripts that each use up the time limit of their first entry,
 *     or stay just under it: the load has one budget for all of them;
 * 15. a timer callback that arms its successor and never returns is not
 *     paid for in every frame for good;
 * 16. the time limit striking while a call into the scene converts an
 *     argument ends the script, not (later) all scripts;
 * 17. built-ins that never return, as the script's own statement: stopped
 *     by the watchdog, with QuickJS in the program or as a shared library.
 * `meson test -C build script-hostile` */
#include "walpe-build-config.h"
#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>
#include "engine/scenescript.h"

#define NLAYERS 6
static const char *layer_names[NLAYERS] = { "a", "b", "c", "text", "fx", "inst" };
static int fails, contract_breaks, sets, fx_sets;
static gsize longest_str;
static gsize longest_log;

#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf ("FAIL: " __VA_ARGS__); printf ("\n"); } \
                              else { printf ("ok: " __VA_ARGS__); printf ("\n"); } } while (0)

static void broke (const char *what)
{
    if (contract_breaks++ < 20)
        printf ("contract: %s\n", what);
}

/* 12: the layer "slow" takes longer to find than the hard time limit of a
 * script entry, in CPU time of the thread as the limit counts it, with a
 * lock held */
static GMutex slow_lock;
static int slow_calls;

static double thread_cpu (void)
{
    struct timespec ts;
    if (clock_gettime (CLOCK_THREAD_CPUTIME_ID, &ts) != 0)
        return g_get_monotonic_time () / 1e6;
    return (double) ts.tv_sec + (double) ts.tv_nsec / 1e9;
}

static int h_find (gpointer u, const char *name)
{
    (void) u;
    if (!name) {
        broke ("find (NULL)");
        return -1;
    }
    if (!strcmp (name, "slow")) {
        g_mutex_lock (&slow_lock);
        /* (nearly all of the time in this function itself, not in the
         * clock call: where the signal finds the thread is what counts) */
        volatile guint work = 0;
        for (double until = thread_cpu () + 1.8; thread_cpu () < until; )
            for (int i = 0; i < 200000; i++)
                work += (guint) i;
        slow_calls++;
        g_mutex_unlock (&slow_lock);
        return 1;
    }
    for (int i = 0; i < NLAYERS; i++)
        if (!strcmp (layer_names[i], name))
            return i;
    return -1;
}

static const char *h_name (gpointer u, int idx)
{
    (void) u;
    return idx >= 0 && idx < NLAYERS ? layer_names[idx] : NULL;
}

static double val[NLAYERS][4];
static char *text[NLAYERS];

static WalpeSsType h_get (gpointer u, int idx, const char *prop, double *v, int *n, const char **s)
{
    (void) u;
    if (!prop || !v || !n || !s) {
        broke ("get with a NULL argument");
        return WSS_NONE;
    }
    *n = 0;
    if (idx < 0 || idx >= NLAYERS)
        return WSS_NONE;
    if (!strcmp (prop, "text")) {
        *s = text[idx] ? text[idx] : "";
        return WSS_STR;
    }
    if (!strcmp (prop, "origin") || !strcmp (prop, "scale") || !strcmp (prop, "angles") ||
        !strcmp (prop, "color")) {
        memcpy (v, val[idx], sizeof (double) * 3);
        *n = 3;
        return WSS_VEC;
    }
    if (!strcmp (prop, "size")) {
        v[0] = 64; v[1] = 32;
        *n = 2;
        return WSS_VEC;
    }
    if (!strcmp (prop, "alpha") || !strcmp (prop, "pointsize")) {
        v[0] = 1;
        return WSS_NUM;
    }
    if (!strcmp (prop, "visible")) {
        v[0] = 1;
        return WSS_BOOL;
    }
    if (!strcmp (prop, "parent")) {
        v[0] = idx > 0 ? idx - 1 : -1;
        return WSS_NUM;
    }
    return WSS_NONE;
}

static void h_set (gpointer u, int idx, const char *prop, WalpeSsType t, const double *v, int n,
                   const char *s)
{
    (void) u;
    sets++;
    if (!prop) {
        broke ("set (prop NULL)");
        return;
    }
    if (t == WSS_STR) {
        if (!s) {
            broke ("set string NULL");
            return;
        }
        gsize l = strlen (s);
        if (l > longest_str)
            longest_str = l;
        if (l > 64 * 1024)
            broke ("set string longer than 64 KiB");
        if (!g_utf8_validate (s, -1, NULL))
            broke ("set string that is not UTF-8");
        if (idx >= 0 && idx < NLAYERS && !strcmp (prop, "text")) {
            g_free (text[idx]);
            text[idx] = g_strndup (s, 256);
        }
        return;
    }
    if (!v) {
        broke ("set number NULL");
        return;
    }
    if (n < 1 || n > 4)
        broke ("set with n outside 1..4");
    for (int i = 0; i < n && i < 4; i++)
        if (!isfinite (v[i])) {
            char *m = g_strdup_printf ("set %s[%d] = %g (type %d)", prop, i, v[i], (int) t);
            broke (m);
            g_free (m);
        } else if (fabs (v[i]) > 1e12 && t == WSS_VEC) {
            char *m = g_strdup_printf ("set %s[%d] = %g: beyond any scene", prop, i, v[i]);
            broke (m);
            g_free (m);
        }
    if (idx >= 0 && idx < NLAYERS && t == WSS_VEC && n >= 2 &&
        (!strcmp (prop, "origin") || !strcmp (prop, "scale")))
        memcpy (val[idx], v, sizeof (double) * (gsize) MIN (n, 3));
}

static void h_audio (gpointer u, int res, float *l, float *r)
{
    (void) u;
    if (res != 16 && res != 32 && res != 64) {
        broke ("audio resolution");
        return;
    }
    for (int i = 0; i < res; i++)
        l[i] = r[i] = (float) i / (float) res;
}

static WalpeSsType h_fx_get (gpointer u, int idx, int e, int p, const char *key, double *v, int *n)
{
    (void) u; (void) p;
    if (!key || !v || !n) {
        broke ("fx_get with a NULL argument");
        return WSS_NONE;
    }
    if (idx != 4 || e < 0 || e > 1)
        return WSS_NONE;
    if (!strcmp (key, "visible")) {
        v[0] = 1;
        *n = 1;
        return WSS_BOOL;
    }
    v[0] = 0.25; v[1] = 0.5; v[2] = 0.75;
    *n = 3;
    return WSS_VEC;
}

static void h_fx_set (gpointer u, int idx, int e, int p, const char *key, WalpeSsType t,
                      const double *v, int n)
{
    (void) u; (void) idx; (void) e; (void) p; (void) t;
    fx_sets++;
    if (!key || !v) {
        broke ("fx_set with a NULL argument");
        return;
    }
    if (n < 1 || n > 4)
        broke ("fx_set with n outside 1..4");
    for (int i = 0; i < n && i < 4; i++)
        if (!isfinite (v[i]))
            broke ("fx_set with a value that is not finite");
}

static int h_fx_find (gpointer u, int idx, const char *name)
{
    (void) u;
    if (!name) {
        broke ("fx_find (NULL)");
        return -1;
    }
    return idx == 4 && !strcmp (name, "blur") ? 0 : -1;
}

static const char *h_fx_name (gpointer u, int idx, int e)
{
    (void) u;
    return idx == 4 && e == 0 ? "blur" : idx == 4 && e == 1 ? "shake" : NULL;
}

static int h_fx_count (gpointer u, int idx)
{
    (void) u;
    return idx == 4 ? 2 : 0;
}

static WalpeSceneScript *make (void)
{
    static const WalpeSsHost host = { NULL, h_find, h_name, h_get, h_set, h_audio,
                                      h_fx_get, h_fx_set, h_fx_find, h_fx_name, h_fx_count };
    for (int i = 0; i < NLAYERS; i++) {
        g_clear_pointer (&text[i], g_free);
        val[i][0] = val[i][1] = val[i][2] = 1;
    }
    JsonObject *up = json_object_new ();
    WalpeSceneScript *ss = walpe_ss_new (&host, 1920, 1080, up);
    json_object_unref (up);
    return ss;
}

static gboolean add (WalpeSceneScript *ss, int idx, const char *prop, const char *src)
{
    JsonObject *up = json_object_new ();
    gboolean ok = walpe_ss_add (ss, idx, prop, src, NULL, up);
    json_object_unref (up);
    return ok;
}

static void start (WalpeSceneScript *ss)
{
    JsonObject *up = json_object_new ();
    json_object_set_string_member (up, "opt", "1 0.5 0.25");
    json_object_set_double_member (up, "num", 1e308);
    walpe_ss_start (ss, up);
    json_object_unref (up);
}

static double tick (WalpeSceneScript *ss, int frame)
{
    gint64 t0 = g_get_monotonic_time ();
    walpe_ss_tick (ss, 1.0 / 30, frame / 30.0, 0, 0, FALSE);
    return (g_get_monotonic_time () - t0) / 1e6;
}

/* the log handler counts the longest "scenescript:" line */
static void log_cb (const gchar *dom, GLogLevelFlags lvl, const gchar *msg, gpointer u)
{
    (void) dom; (void) lvl; (void) u;
    gsize l = msg ? strlen (msg) : 0;
    if (l > longest_log)
        longest_log = l;
}

/* ------------------------------------------------------------ 1. rewriter */

static void rewriter (void)
{
    WalpeSceneScript *ss = make ();
    /* `${`${`${ ... 200 000 deep: the rewriter went one C stack frame
     * down per level (SIGSEGV on the render thread) */
    GString *s = g_string_new ("'use strict';\nexport function update(v) { return v; }\nconst t = ");
    for (int i = 0; i < 200000; i++)
        g_string_append (s, "`${");
    g_string_append (s, "1");
    for (int i = 0; i < 200000; i++)
        g_string_append (s, "}`");
    g_string_append (s, ";\n");
    add (ss, 0, "text", s->str);
    CHECK (TRUE, "rewriter: 200 000 nested template literals did not overflow the stack");
    g_string_free (s, TRUE);
    static const char *const odd[] = {
        "export function update(v) { return v; }\nconst a = `${",
        "export function update(v) { return v; }\nconst a = `${`",
        "export function update(v) { return v; }\nconst a = `${'",
        "export function update(v) { return v; }\nconst a = '\\",
        "export function update(v) { return v; }\nconst a = /[/",
        "export function update(v) { return v; }\nconst a = /\\",
        "export function update(v) { return v; }\n/*",
        "export function update(v) { return v; }\n//",
        "export function update(v) { return v; }\nimport",
        "export function update(v) { return v; }\nimport * as",
        "export function update(v) { return v; }\nimport * as X from '",
        "export function update(v) { return v; }\nimport * as X from 'WEMath",
        "export function update(v) { return v; }\nimport {a} from 'x",
        "export function update(v) { return v; }\nexport",
        "export function update(v) { return v; }\nexport \xc2",
        "export function update(v) { return v; }\nexport\xe3\x80",
        "export function update(v) { return v; }\nexport\xe2\x80",
        "export function update(v) { return v; }\nexport\xef\xbb",
        "export\xc2\xa0" "function update(v) { return v; } })(); (function () { return 1; ",
        "`", "'", "\"", "/", "\\", "${", "}", "\xff\xfe", "",
    };
    for (guint i = 0; i < G_N_ELEMENTS (odd); i++)
        add (ss, (int) (i % NLAYERS), "text", odd[i]);
    start (ss);
    tick (ss, 1);
    CHECK (TRUE, "rewriter: %u cut-off sources read to their end and no further",
           (guint) G_N_ELEMENTS (odd));
    walpe_ss_free (ss);
}

/* --------------------------------------------------- 2. promise runaways */

static void promises (void)
{
    static const char *const never[2][2] = {
        { "a promise reaction that never returns",
          "'use strict';\nexport function update(v) {\n"
          "  Promise.resolve().then(() => { for (;;) {} });\n  return v; }\n" },
        { "an async function that never returns",
          "'use strict';\nasync function w() { await null; for (;;) {} }\n"
          "export function update(v) { w(); return v; }\n" },
    };
    for (int k = 0; k < 2; k++) {
        WalpeSceneScript *ss = make ();
        add (ss, 0, "text", "'use strict';\nlet n = 0;\nexport function update(v) { return 'n' + (++n); }\n");
        add (ss, 1, "text", never[k][1]);
        start (ss);
        /* the time budget is paid a few times (nobody to blame: the script
         * that queued the reaction has returned), then never again; it was
         * paid in every frame for as long as the wallpaper was shown */
        double first = 0, later = 0;
        for (int f = 1; f <= 5; f++)
            first += tick (ss, f);
        for (int f = 6; f <= 13; f++)
            later += tick (ss, f);
        CHECK (first < 20.0, "%s: the first frames came back (%.2f s)", never[k][0], first);
        CHECK (later < 0.25, "%s: 8 later frames do not pay the time budget again (%.3f s)",
               never[k][0], later);
        walpe_ss_free (ss);
    }

    /* a chain that queues a new reaction from every reaction: bounded per frame */
    WalpeSceneScript *ss = make ();
    add (ss, 1, "text", "'use strict';\nfunction again() { Promise.resolve().then(again); }\n"
                        "export function init(v) { again(); return v; }\n"
                        "export function update(v) { return v; }\n");
    start (ss);
    double t = 0;
    for (int f = 1; f <= 8; f++)
        t += tick (ss, f);
    CHECK (t < 30.0, "promise chain without end: 8 frames came back (%.2f s)", t);
    walpe_ss_free (ss);
}

/* ----------------------------------------------------- 3. vectors, text */

static void vectors (void)
{
    WalpeSceneScript *ss = make ();
    int before = contract_breaks;
    add (ss, 0, "origin", "'use strict';\nexport function update(v) { return new Vec3(NaN, Infinity, -Infinity); }\n");
    add (ss, 1, "scale", "'use strict';\nexport function update(v) { return new Vec3(1e308, -1e308, 1e300); }\n");
    add (ss, 3, "text", "'use strict';\nconst big = '\\u00e4'.repeat(3000000);\n"
                        "export function update(v) { thisLayer.horizontalalign = big; return big; }\n");
    add (ss, 2, "scale", "'use strict';\nexport function update(v) {\n"
                         "  thisLayer.origin = { x: NaN, y: 1 / 0, z: 0 };\n"
                         "  thisLayer.angles = new Vec3(0, 0, 1e308);\n"
                         "  thisLayer.color = new Vec3(-1 / 0, 0 / 0, 2);\n"
                         "  thisScene.getLayer('a').scale = new Vec3(4e38, 3.5e38, 1);\n"
                         "  return v; }\n");
    start (ss);
    for (int f = 1; f <= 3; f++)
        tick (ss, f);
    CHECK (contract_breaks == before, "vectors: NaN / Infinity / numbers beyond a float do not "
           "reach the scene (%d)", contract_breaks - before);
    CHECK (longest_str > 0 && longest_str <= 64 * 1024, "text: a 6 MB string reaches the scene cut "
           "to a sane length (%" G_GSIZE_FORMAT " bytes)", longest_str);
    walpe_ss_free (ss);
}

/* ------------------------------------------------------ 4. thrown values */

static void thrown (void)
{
    WalpeSceneScript *ss = make ();
    add (ss, 0, "text", "'use strict';\nexport function update(v) { throw { toString() { for (;;) {} } }; }\n");
    add (ss, 1, "text", "'use strict';\nexport function update(v) { throw { get message() { for (;;) {} }, toString() { throw this; } }; }\n");
    add (ss, 2, "text", "'use strict';\nexport function update(v) { throw new Proxy({}, { get() { throw 1; }, has() { throw 2; } }); }\n");
    add (ss, 3, "text", "'use strict';\nexport function init(v) { throw null; }\nexport function update(v) { throw undefined; }\n");
    add (ss, 4, "text", "'use strict';\nexport function update(v) { const e = new Error('x'); "
                        "Object.defineProperty(e, 'stack', { get() { for (;;) {} } }); throw e; }\n");
    /* at the top level of the module too */
    add (ss, 5, "text", "throw { toString() { for (;;) {} } };\n");
    start (ss);
    double t = 0;
    for (int f = 1; f <= 12; f++)
        t += tick (ss, f);
    CHECK (t < 6.0, "thrown values with hostile toString / message: 12 frames in %.2f s", t);
    walpe_ss_free (ss);
}

/* ------------------------------------------------------ 5. the API driver */

/* Calls everything reachable from the SceneScript globals with hostile
 * arguments. xorshift32 with the seed in the source: the same calls on
 * every run. */
static const char *DRIVER_JS =
    "'use strict';\n"
    "let seed = SEED >>> 0 || 1;\n"
    "function rnd(n) { seed ^= seed << 13; seed >>>= 0; seed ^= seed >>> 17; seed ^= seed << 5; seed >>>= 0; return seed % n; }\n"
    "const thrower = { valueOf() { throw new Error('valueOf'); }, toString() { throw new Error('toString'); } };\n"
    "const looper = { get x() { return NaN; }, get y() { return Infinity; }, get z() { return -Infinity; }, get w() { return 1e308; } };\n"
    "function reent() { try { thisLayer.text = 'r' + rnd(9); thisLayer.scale = new Vec3(rnd(3)); thisLayer.visible = !thisLayer.visible; } catch (e) {} return rnd(100); }\n"
    "const hostile = [\n"
    "  () => NaN, () => Infinity, () => -Infinity, () => 1e308, () => -1e308, () => 2147483648, () => -2147483649,\n"
    "  () => 4294967296, () => 9007199254740993, () => 1e-320, () => -0, () => 0, () => 1, () => -1, () => 0.5, () => 7,\n"
    "  () => '', () => 'a\\u0000b', () => '\\ud800', () => 'x'.repeat(70000), () => '1e999 nan inf', () => 'NaN NaN NaN',\n"
    "  () => '-1 -1 -1 -1 -1', () => '%s%n%x', () => '../../../../etc/passwd', () => '__proto__', () => 'constructor',\n"
    "  () => 'a', () => 'text', () => 'blur', () => 'visible', () => 'Asia/Tokyo', () => '../../etc/localtime', () => 'en-US',\n"
    "  () => null, () => undefined, () => true, () => false, () => ({}), () => [], () => [NaN, Infinity, -Infinity, 1e308],\n"
    "  () => [1, 2, 3, 4, 5, 6, 7, 8, 9], () => new Array(1000),\n"
    "  () => new Proxy({}, { get() { throw new Error('proxy get'); } }),\n"
    "  () => new Proxy([], { get(t, k) { return k === 'length' ? 2147483647 : NaN; } }),\n"
    "  () => new Proxy(function () {}, { apply() { throw new Error('apply'); } }),\n"
    "  () => thrower, () => looper, () => ({ valueOf: reent }), () => ({ toString() { return {}; } }),\n"
    "  () => ({ x: { valueOf: reent }, y: thrower, z: 1 }), () => ({ __i: 2147483647 }), () => ({ __i: -1 }), () => ({ __i: NaN }),\n"
    "  () => ({ __i: 0, __e: 99999, __p: -5 }), () => ({ name: thrower, value: looper }),\n"
    "  () => Symbol('s'), () => (typeof BigInt === 'function' ? BigInt(10) : 10), () => function () { return reent(); },\n"
    "  () => new Float32Array(3), () => new Float32Array(0), () => new Uint8Array(1 << 20), () => new Date(NaN), () => new Date(8.64e15),\n"
    "  () => new Vec3(NaN, 1e308, -1e308), () => new Vec2(looper), () => new Vec4('nan inf -inf 1e999'), () => Object.create(null),\n"
    "  () => thisLayer, () => thisScene, () => engine, () => globalThis, () => thisScene.getLayer('fx'), () => thisScene.getLayer('inst'),\n"
    "  () => { const e = thisScene.getLayer('fx').getEffect(rnd(3) - 1); return e; },\n"
    "];\n"
    "function hv() { return hostile[rnd(hostile.length)](); }\n"
    "const roots = [];\n"
    "function root(n, o) { if (o !== null && o !== undefined) roots.push([n, o]); }\n"
    "function collect() { roots.length = 0;\n"
    "  root('thisLayer', thisLayer); root('thisObject', thisObject); root('thisScene', thisScene); root('engine', engine);\n"
    "  root('input', input); root('localStorage', localStorage); root('console', console); root('shared', shared);\n"
    "  root('WEMath', WEMath); root('WEColor', WEColor); root('WEVector', WEVector); root('Intl', Intl);\n"
    "  for (const n of ['a', 'b', 'text', 'fx', 'inst', 'nope']) { let L = null; try { L = thisScene.getLayer(n); } catch (e) {} root('layer:' + n, L);\n"
    "    if (L) { for (let k = -1; k < 3; k++) { let E = null; try { E = L.getEffect(k); } catch (e) {} root('fx:' + n + k, E);\n"
    "      if (E) { let M = null; try { M = E.getMaterial(0); } catch (e) {} root('mat:' + n + k, M); } }\n"
    "      for (const an of [undefined, 0, 'x']) { let A = null; try { A = L.getAnimation(an); } catch (e) {} root('anim:' + n, A); }\n"
    "      try { root('inst:' + n, L.instance); } catch (e) {} try { root('texanim:' + n, L.getTextureAnimation()); } catch (e) {}\n"
    "      try { root('video:' + n, L.getVideoTexture()); } catch (e) {} } }\n"
    "  root('vec2', new Vec2(1, 2)); root('vec3', new Vec3(1, 2, 3)); root('vec4', new Vec4(1, 2, 3, 4));\n"
    "  root('mat3', new Mat3()); root('mat4', new Mat4()); root('Vec3', Vec3); root('Mat4', Mat4); root('Mat3', Mat3);\n"
    "  try { root('ghost', thisScene.createLayer({ image: 'x' })); } catch (e) {}\n"
    "  try { root('dtf', new Intl.DateTimeFormat(hv(), hv())); } catch (e) {} try { root('nf', new Intl.NumberFormat(hv(), hv())); } catch (e) {}\n"
    "  root('date', new Date(0)); }\n"
    "function members(o) { const out = []; let p = o, hops = 0;\n"
    "  while (p && p !== Object.prototype && p !== Function.prototype && hops++ < 6) {\n"
    "    for (const k of Object.getOwnPropertyNames(p)) if (k !== 'constructor' && k !== 'caller' && k !== 'callee' && k !== 'arguments') out.push(k);\n"
    "    p = Object.getPrototypeOf(p); }\n"
    "  return out; }\n"
    "let calls = 0, threw = 0;\n"
    "function one() { const r = roots[rnd(roots.length)], o = r[1], ms = members(o); if (!ms.length) return;\n"
    "  const k = ms[rnd(ms.length)]; let cur; try { cur = o[k]; } catch (e) { threw++; return; }\n"
    "  calls++;\n"
    "  try { if (typeof cur === 'function' && rnd(8)) { const n = rnd(5), a = []; for (let i = 0; i < n; i++) a.push(hv());\n"
    "      const res = cur.apply(rnd(6) ? o : hv(), a); if (res && typeof res === 'object' && roots.length < 400 && !rnd(4)) roots.push([r[0] + '.' + k + '()', res]); }\n"
    "    else { o[k] = hv(); } } catch (e) { threw++; } }\n"
    "export function init(value) { collect(); for (let i = 0; i < 300; i++) one(); return value; }\n"
    "export function update(value) { if (!rnd(4)) collect(); for (let i = 0; i < 400; i++) one();\n"
    "  if (!rnd(3)) engine.setTimeout(() => { for (let i = 0; i < 50; i++) one(); }, rnd(40));\n"
    "  if (!rnd(5)) Promise.resolve().then(() => { for (let i = 0; i < 50; i++) one(); });\n"
    "  return RET; }\n"
    "export function applyUserProperties(p) { for (let i = 0; i < 50; i++) one(); }\n"
    "export function cursorClick(e) { for (let i = 0; i < 50; i++) one(); e.worldPosition = hv(); }\n"
    "export function cursorMove(e) { for (let i = 0; i < 20; i++) one(); }\n"
    "export function cursorEnter(e) { throw hv(); }\n"
    "export function mediaPropertiesChanged(e) { for (let i = 0; i < 50; i++) one(); }\n"
    "export function mediaThumbnailChanged(e) { e.primaryColor = hv(); for (let i = 0; i < 20; i++) one(); }\n"
    "export function destroy() { for (;;) {} }\n";

static void driver (guint32 seed)
{
    static const struct { const char *prop, *ret; } bind[] = {
        { "text", "hv()" }, { "origin", "hv()" }, { "scale", "hv()" }, { "visible", "hv()" },
        { "alpha", "hv()" }, { "color", "hv()" }, { "angles", "hv()" }, { "pointsize", "hv()" },
    };
    WalpeSceneScript *ss = make ();
    if (!ss)
        return;
    for (int i = 0; i < NLAYERS; i++) {
        const char *prop = bind[(seed + (guint32) i) % G_N_ELEMENTS (bind)].prop;
        char *sd = g_strdup_printf ("%u", seed * 2654435761u + (guint32) i * 97u + 1u);
        char **parts = g_strsplit (DRIVER_JS, "SEED", 2);
        char *a = g_strconcat (parts[0], sd, parts[1], NULL);
        g_strfreev (parts);
        parts = g_strsplit (a, "RET", 2);
        char *src = g_strconcat (parts[0], bind[(seed + (guint32) i) % G_N_ELEMENTS (bind)].ret,
                                 parts[1], NULL);
        g_strfreev (parts);
        g_free (a);
        g_free (sd);
        add (ss, i, prop, src);
        g_free (src);
    }
    /* an effect script and one on a property nothing drives */
    {
        JsonObject *up = json_object_new ();
        char **parts = g_strsplit (DRIVER_JS, "SEED", 2);
        char *a = g_strconcat (parts[0], "77", parts[1], NULL);
        g_strfreev (parts);
        parts = g_strsplit (a, "RET", 2);
        char *src = g_strconcat (parts[0], "hv()", parts[1], NULL);
        g_strfreev (parts);
        walpe_ss_add_fx (ss, 4, 0, -1, "visible", src, NULL, up, "true");
        walpe_ss_add_fx (ss, 4, 1, 0, "strength", src, NULL, up, "\"0.25 0.5\"");
        walpe_ss_add_inert (ss, -1, "clearcolor", src, NULL, up, "\"0 0 0\"");
        g_free (src);
        g_free (a);
        json_object_unref (up);
    }
    walpe_ss_add_anim (ss, 0, -1, -1, "alpha",
                       "{\"options\":{\"fps\":1e308,\"length\":-5,\"mode\":\"loop\"},"
                       "\"c0\":[{\"frame\":0,\"value\":0},{\"frame\":1e308,\"value\":1e308}]}", "1");
    walpe_ss_add_anim (ss, 1, -1, -1, "origin", "{\"options\":null,\"c0\":7,\"c1\":[null,{},[]]}",
                       "\"nan inf 1e999\"");
    walpe_ss_add_anim (ss, 2, 0, 0, "x", "[]", "{}");
    start (ss);
    int *cl = NULL;
    int ncl = walpe_ss_cursor_layers (ss, &cl);
    if (ncl < 0 || ncl > NLAYERS)
        broke ("cursor layers: more than the scene has");
    for (int i = 0; i < ncl; i++)
        if (cl[i] < 0 || cl[i] >= NLAYERS)
            broke ("cursor layers: a layer the scene does not have");
    g_free (cl);
    static const char *const kinds[] = { "cursorClick", "cursorMove", "cursorEnter", "cursorLeave",
                                         "cursorDown", "cursorUp", "nonsense" };
    for (int f = 1; f <= 12; f++) {
        walpe_ss_tick (ss, f == 5 ? NAN : f == 6 ? 1e308 : 1.0 / 30, f == 7 ? INFINITY : f / 30.0,
                       f == 8 ? NAN : 100, f == 9 ? -INFINITY : 100, f & 1);
        walpe_ss_cursor_event (ss, kinds[f % G_N_ELEMENTS (kinds)], f == 10 ? 99999 : f % NLAYERS,
                               f == 3 ? NAN : 10, 20, f == 4 ? INFINITY : 1, 2);
        if (f % 3 == 0)
            walpe_ss_media_event (ss, f % 2 ? "mediaPropertiesChanged" : "mediaThumbnailChanged",
                                  "{\"title\":\"t\",\"primaryColor\":[1e308,null,\"x\"],"
                                  "\"textColor\":[],\"state\":99999999999}");
    }
    walpe_ss_free (ss);
}

/* ---------------------------------------------------------- 6. log lines */

static void logs (void)
{
    WalpeSceneScript *ss = make ();
    longest_log = 0;
    guint h = g_log_set_handler (NULL, G_LOG_LEVEL_MASK, log_cb, NULL);
    add (ss, 0, "text", "'use strict';\nexport function update(v) { console.log('y'.repeat(4000000)); "
                        "console.error('z'.repeat(300000), 'w'.repeat(300000)); throw new Error('e'.repeat(2000000)); }\n");
    start (ss);
    for (int f = 1; f <= 3; f++)
        tick (ss, f);
    g_log_remove_handler (NULL, h);
    CHECK (longest_log > 0 && longest_log <= 4096, "log: a script's line is cut (longest %" G_GSIZE_FORMAT
           " bytes)", longest_log);
    walpe_ss_free (ss);
}

/* ----------------------------------------------------------- 7. recursion */

static gpointer recursion_thread (gpointer data)
{
    (void) data;
    WalpeSceneScript *ss = make ();
    add (ss, 0, "text", "'use strict';\nfunction f(n) { return f(n + 1) + 1; }\n"
                        "export function update(v) { try { f(0); } catch (e) { return 'caught'; } return 'no'; }\n");
    add (ss, 1, "text", "'use strict';\nconst deep = '['.repeat(100000) + ']'.repeat(100000);\n"
                        "export function update(v) { try { JSON.parse(deep); } catch (e) { return 'caught'; } return 'parsed'; }\n");
    add (ss, 2, "text", "'use strict';\nexport function update(v) { let o = {}; for (let i = 0; i < 100000; i++) o = { o };\n"
                        "  try { JSON.stringify(o); } catch (e) { return 'caught'; } return 'done'; }\n");
    add (ss, 3, "text", "'use strict';\nexport function update(v) { try { new RegExp('('.repeat(50000) + ')'.repeat(50000)).test('x'); } catch (e) { return 'caught'; } return 'done'; }\n");
    start (ss);
    for (int f = 1; f <= 2; f++)
        tick (ss, f);
    char *r = g_strdup (text[0]);
    walpe_ss_free (ss);
    return r;
}

static void recursion (void)
{
    GThread *t = g_thread_new ("render", recursion_thread, NULL);
    char *r = g_thread_join (t);
    CHECK (!g_strcmp0 (r, "caught"), "recursion without end is an error the script sees (%s)",
           r ? r : "?");
    g_free (r);
}

/* ------------------------------------- 8. conversions inside a host call */

/* The scene functions convert their arguments in C (JS_ToInt32 and
 * friends), which runs the argument's valueOf / toString - script code.
 * With a built-in that never returns in there (join () of an array of
 * length 2^32 - 1; a regular expression that backtracks for hours) the
 * thread used to hang for good: the watchdog stood down for the whole
 * host call. Each case runs on its own thread; one that is not back
 * after 60 s fails the test and ends the process. */
static const char *const HANG_JS[] = {
    /* Layer / Effect objects keep the index they hand to the scene in a
     * plain property */
    "'use strict';\nconst a = []; a.length = 4294967295;\n"
    "export function update(v) { thisLayer.__i = a; return thisLayer.name; }\n",
    "'use strict';\nconst a = []; a.length = 4294967295;\n"
    "export function update(v) { const e = thisScene.getLayer('fx').getEffect(0); e.__e = a; return '' + e.visible; }\n",
    "'use strict';\nconst re = { valueOf() { return /(x+x+)+y/.test('x'.repeat(64)) ? 1 : 0; } };\n"
    "export function update(v) { thisLayer.__i = re; thisLayer.alpha = 0.5; return v; }\n",
    "'use strict';\nconst re = { toString() { return /(x+x+)+y/.test('x'.repeat(64)) ? 'a' : 'b'; } };\n"
    "export function update(v) { const e = thisScene.getLayer('fx').getEffect(0); e.setMaterialProperty(re, 1); return v; }\n",
};

static GMutex hang_lock;
static GCond hang_cond;
static int hang_done;

static gpointer hang_thread (gpointer data)
{
    WalpeSceneScript *ss = make ();
    add (ss, 0, "text", data);
    start (ss);
    for (int f = 1; f <= 3; f++)
        tick (ss, f);
    walpe_ss_free (ss);
    g_mutex_lock (&hang_lock);
    hang_done = 1;
    g_cond_signal (&hang_cond);
    g_mutex_unlock (&hang_lock);
    return NULL;
}

/* fn on its own thread; FALSE (and the process ends) when it is not back in 60 s */
static gboolean back_in_time (GThreadFunc fn, gpointer data, const char *what)
{
    hang_done = 0;
    gint64 t0 = g_get_monotonic_time ();
    GThread *t = g_thread_new ("render", fn, data);
    g_mutex_lock (&hang_lock);
    gint64 until = t0 + 60 * G_TIME_SPAN_SECOND;
    while (!hang_done && g_cond_wait_until (&hang_cond, &hang_lock, until))
        ;
    gboolean back = hang_done;
    g_mutex_unlock (&hang_lock);
    CHECK (back, "%s: the thread came back (%.1f s)", what, (g_get_monotonic_time () - t0) / 1e6);
    if (!back) {
        printf ("FAILED\n");
        fflush (stdout);
        _exit (1);
    }
    g_thread_join (t);
    return TRUE;
}

static void conversions (void)
{
    for (guint i = 0; i < G_N_ELEMENTS (HANG_JS); i++) {
        char *what = g_strdup_printf ("an argument whose conversion never returns (case %u)", i + 1);
        back_in_time (hang_thread, (gpointer) HANG_JS[i], what);
        g_free (what);
    }
}

/* ----------------------------------------- 9. JSON the engine refuses */

/* The scene hands script properties, option values, static values,
 * animations and media events over as JSON text, outside any time limit.
 * A number beyond the double range in a scene file ("1e999") comes out of
 * json-glib as inf.0, which the engine refuses - and the error used to be
 * turned into text through Error.prototype.toString, which an earlier
 * script of the same wallpaper may have replaced. */
static int json_added, json_started;

static gpointer json_thread (gpointer data)
{
    (void) data;
    WalpeSceneScript *ss = make ();
    add (ss, 0, "text", "'use strict';\n"
                        "Error.prototype.toString = function () { for (;;) {} };\n"
                        "Object.defineProperty(Error.prototype, 'message', { get() { for (;;) {} }, configurable: true });\n"
                        "Object.defineProperty(Error.prototype, 'name', { get() { for (;;) {} }, configurable: true });\n"
                        "Object.defineProperty(Error.prototype, 'stack', { get() { for (;;) {} }, configurable: true });\n"
                        "export function update(v) { return v; }\n");
    start (ss);
    tick (ss, 1);
    /* what json-glib writes for 1e999 */
    JsonObject *sp = json_object_new (), *up = json_object_new ();
    json_object_set_double_member (sp, "big", INFINITY);
    json_object_set_double_member (sp, "small", -INFINITY);
    json_object_set_string_member (sp, "text", "inf.0 nan \"inf\" \\");
    json_object_set_double_member (up, "opt", INFINITY);
    json_added = walpe_ss_add (ss, 1, "text",
                               "'use strict';\nexport var scriptProperties = createScriptProperties()"
                               ".addSlider({ name: 'big', value: 1 }).addSlider({ name: 'small', value: 1 })"
                               ".addText({ name: 'text', value: '' }).finish();\n"
                               "export function update(v) { return scriptProperties.big + '|' + scriptProperties.small"
                               " + '|' + scriptProperties.text; }\n", sp, up);
    walpe_ss_add_anim (ss, 2, -1, -1, "alpha", "{\"options\":{\"fps\":inf.0},\"c0\":[{\"frame\":0,\"value\":-inf.0}]}",
                       "inf.0");
    walpe_ss_add_anim (ss, 2, -1, -1, "alpha", "{not json", "also not");
    walpe_ss_add_fx (ss, 4, 0, -1, "visible", "export function update(v) { return v; }", NULL, up, "{{{");
    walpe_ss_add_inert (ss, 3, "volume", "export function update(v) { return v; }", NULL, up, "nan.0");
    walpe_ss_set_prop_types (ss, up);
    walpe_ss_start (ss, up);
    json_started = 1;
    walpe_ss_media_event (ss, "mediaPropertiesChanged", "{\"title\":inf.0");
    walpe_ss_media_event (ss, "mediaPropertiesChanged", "");
    for (int f = 2; f <= 4; f++)
        tick (ss, f);
    json_object_unref (sp);
    json_object_unref (up);
    walpe_ss_free (ss);
    g_mutex_lock (&hang_lock);
    hang_done = 1;
    g_cond_signal (&hang_cond);
    g_mutex_unlock (&hang_lock);
    return NULL;
}

static void json_checks (void)
{
    back_in_time (json_thread, NULL, "JSON the engine refuses, with Error.prototype.toString replaced");
    CHECK (json_added, "a script property beyond the double range does not cost the script");
    CHECK (text[1] && g_str_has_prefix (text[1], "1.7976931348623157e+308|-1.7976931348623157e+308|inf.0 nan \"inf\" \\"),
           "such a number arrives as the largest double, text stays text (%s)", text[1] ? text[1] : "?");
}

/* ------------------------------------------------ 10. audio buffers */

/* h_audio copies the spectrum straight into the storage of the three
 * Float32Arrays. Which arrays qualify was decided by a prototype looked up
 * at the first call from the global "Float32Array" - the scripts' to
 * replace: with a getter there, script code ran between the fetch of the
 * first array's storage pointer and the copy into it, and with another
 * typed array class in its place the copy wrote raw float bits into
 * integer arrays. The host of this test hands out i / resolution. */
static void audio_buffers (void)
{
    WalpeSceneScript *ss = make ();
    add (ss, 0, "text", "'use strict';\n"
                        "const b = engine.registerAudioBuffers(64);\n"
                        "export function update(v) { return b.left[32] + ' ' + b.right[16] + ' ' + b.average[48]; }\n");
    start (ss);
    for (int f = 1; f <= 3; f++)
        tick (ss, f);
    CHECK (text[0] && !strcmp (text[0], "0.5 0.25 0.75"), "audio buffers: Float32Arrays are filled (%s)",
           text[0] ? text[0] : "?");
    walpe_ss_free (ss);

    ss = make ();
    add (ss, 1, "text", "'use strict';\n"
                        "let reads = 0;\n"
                        "Object.defineProperty(globalThis, 'Float32Array', { configurable: true,\n"
                        "  get() { reads++; return Uint32Array; } });\n"
                        "const b = engine.registerAudioBuffers(64);\n"
                        "const made = reads;\n"
                        "export function update(v) { return b.left[32] + ' ' + b.average[48] + ' ' + (reads - made); }\n");
    start (ss);
    for (int f = 1; f <= 3; f++)
        tick (ss, f);
    CHECK (text[1] && !strcmp (text[1], "0 0 0"), "audio buffers: with the global Float32Array replaced, the "
           "host does not read the global and copies no float bits into integer arrays (%s)",
           text[1] ? text[1] : "?");
    walpe_ss_free (ss);

    /* arrays in other shapes: a view at an offset is filled where it
     * stands and nowhere else, one that is too short takes what fits,
     * anything else is written element by element or not at all */
    ss = make ();
    int before = contract_breaks;
    add (ss, 2, "text", "'use strict';\n"
                        "const F = Float32Array, b = engine.registerAudioBuffers(64);\n"
                        "const store = new F(256);\n"
                        "b.left = new F(store.buffer, 64 * 4, 64);\n"
                        "b.right = new F(8);\n"
                        "const c = engine.registerAudioBuffers(16);\n"
                        "c.left = [0]; c.right = new Proxy([], {}); c.average = Object.freeze({});\n"
                        "const d = engine.registerAudioBuffers(32);\n"
                        "d.left = new Float64Array(32); d.right = new DataView(new ArrayBuffer(256)); d.average = null;\n"
                        "const e = engine.registerAudioBuffers(64);\n"
                        "e.resolution = { valueOf() { e.left = e.right = e.average = undefined; return 64; } };\n"
                        "export function update(v) {\n"
                        "  return [store[63], store[64 + 32], store[128], b.right[4], b.right.length, c.left[8], d.left[16]].join(' '); }\n");
    start (ss);
    for (int f = 1; f <= 3; f++)
        tick (ss, f);
    CHECK (text[2] && !strcmp (text[2], "0 0.5 0 0.0625 8 0.5 0.5"), "audio buffers: views, short arrays and "
           "other objects in place of the arrays (%s)", text[2] ? text[2] : "?");
    CHECK (contract_breaks == before, "audio buffers: the host contract held");
    walpe_ss_free (ss);
}

/* ------------------------------------------------------ 11. time zones */

/* toLocaleString (.., { timeZone }) asks the host for a zone's offset, and
 * the host asks GLib, which reads <zone directory>/<name>. Names of files
 * there that are no zones went through: "leapseconds" is a text file, and
 * GLib logs a critical warning for every attempt - one per call, as often
 * as a script calls. */
static int n_glib_logs;

static void glib_log_cb (const gchar *dom, GLogLevelFlags lvl, const gchar *msg, gpointer u)
{
    (void) dom; (void) lvl; (void) msg; (void) u;
    n_glib_logs++;
}

static void time_zones (void)
{
    WalpeSceneScript *ss = make ();
    n_glib_logs = 0;
    guint h = g_log_set_handler ("GLib", G_LOG_LEVEL_MASK, glib_log_cb, NULL);
    add (ss, 0, "text", "'use strict';\n"
                        "const names = ['leapseconds', 'localtime', 'posixrules', 'posix/UTC', 'right/UTC', 'tzdata.zi',\n"
                        "  'zone.tab', '../zoneinfo/UTC', '/etc/localtime', 'Europe', 'europe/berlin', '+VERSION', ''];\n"
                        "export function update(v) {\n"
                        "  let same = 0; const d = new Date(1700000000000);\n"
                        "  for (let k = 0; k < 20; k++) for (const n of names) {\n"
                        "    const t = new Date(1700000000000 + k * 3600000);\n"
                        "    if (t.toLocaleString('en-US', { timeZone: n }) === t.toLocaleString('en-US')) same++; }\n"
                        "  const tokyo = d.toLocaleString('en-GB', { timeZone: 'Asia/Tokyo', hour: '2-digit', minute: '2-digit' });\n"
                        "  const utc = d.toLocaleString('en-GB', { timeZone: 'UTC', hour: '2-digit', minute: '2-digit' });\n"
                        "  return same + ' ' + tokyo + ' ' + utc; }\n");
    start (ss);
    for (int f = 1; f <= 3; f++)
        tick (ss, f);
    g_log_remove_handler ("GLib", h);
    /* 22:13 UTC; Tokyo is nine hours ahead where the zone data is installed */
    gboolean have_tokyo = g_file_test ("/usr/share/zoneinfo/Asia/Tokyo", G_FILE_TEST_EXISTS);
    CHECK (text[0] && g_str_has_prefix (text[0], "260 ") && g_str_has_suffix (text[0], " 22:13") &&
           (!have_tokyo || strstr (text[0], " 07:13 ")),
           "time zones: names that are no zones give local time, zones their own (%s)", text[0] ? text[0] : "?");
    CHECK (n_glib_logs == 0, "time zones: no file that is no zone is handed to GLib (%d warnings from GLib)",
           n_glib_logs);
    walpe_ss_free (ss);
}

/* ------------------- 12. the watchdog's signal and a call into the scene */

/* The hard limit takes the thread out of a built-in function that does not
 * return, from the handler of a timer signal. That is sound only while the
 * thread is in the script engine's own code: in a call into the scene (or
 * in the runtime's allocator, which is marked the same way) it may hold a
 * lock, and whoever takes that lock next would wait for good. Here such a
 * call takes longer than the limit, so the signal arrives in the middle of
 * it several times: it has to be left alone until it returns. */
static gpointer slow_thread (gpointer data)
{
    (void) data;
    WalpeSceneScript *ss = make ();
    add (ss, 0, "text", "'use strict';\n"
                        "export function update(v) { return 'layer ' + thisScene.getLayer('slow').name; }\n");
    start (ss);
    tick (ss, 1);
    walpe_ss_free (ss);
    g_mutex_lock (&hang_lock);
    hang_done = 1;
    g_cond_signal (&hang_cond);
    g_mutex_unlock (&hang_lock);
    return NULL;
}

static void slow_call (void)
{
    slow_calls = 0;
    back_in_time (slow_thread, NULL, "a call into the scene that takes longer than the hard limit");
    gboolean unlocked = g_mutex_trylock (&slow_lock);
    if (unlocked)
        g_mutex_unlock (&slow_lock);
    CHECK (unlocked && slow_calls >= 1, "the watchdog's signal leaves a call into the scene alone: it ran "
           "to its end %d time(s), its lock is %s", slow_calls, unlocked ? "free" : "still held");
}

/* ------------------------------------------- 13. the script memory, full */

/* Scripts that keep what they allocate until the runtime's memory limit
 * refuses, down to the last small block, and then go on. QuickJS takes
 * that badly: the "out of memory" error is built with more allocations,
 * and what the next refusal returns is read or written through - the
 * process (the daemon, with every wallpaper it shows) ended 0.1 s after
 * such a wallpaper opened. Six of these cases did that with QuickJS
 * 2021-03-27, the fifth still does with 2026-06-04. Each case runs in a
 * child (this program, `memory-child <case>`), so a crash is an exit
 * status here. Layer 0 has a script that counts frames. */
#define MEM_FILL \
    "'use strict';\nconst a = [];\n" \
    "function fill() { try { for (;;) a.push(new Array(8000).fill(0)); } catch (e) {}\n" \
    "  try { for (;;) a.push([0, 1, 2, 3, 4, 5, 6, 7]); } catch (e) {} try { for (;;) a.push({}); } catch (e) {} }\n"
static const char *const MEM_JS[][2] = {
    { "update(), which then reads a property of null",
      MEM_FILL "export function update(v) { fill(); null.x; }\n" },
    { "update(), which then runs a regular expression",
      MEM_FILL "export function update(v) { fill(); for (let i = 0; i < 100; i++) { try { /a(b+)c/.exec('abbbc' + i); } catch (e) {} } return v; }\n" },
    { "update(), which then writes a date as text",
      MEM_FILL "export function update(v) { fill(); for (let i = 0; i < 100; i++) { try { new Date().toString(); (1.5).toFixed(3); } catch (e) {} } return v; }\n" },
    { "update(), which then compiles a function",
      MEM_FILL "export function update(v) { fill(); for (let i = 0; i < 100; i++) { try { new Function('return 1')(); } catch (e) {} } return v; }\n" },
    { "the argument objects of calls kept in update()",
      "'use strict';\nconst a = [];\n"
      "export function update(v) { for (;;) a.push(function () { return arguments; }(1, 2, 3)); }\n" },
    { "the top of the module",
      MEM_FILL "fill();\nexport function update(v) { return v + 'x'; }\n" },
    { "init()",
      MEM_FILL "export function init(v) { fill(); return v; }\nexport function update(v) { return v + 'x'; }\n" },
    { "a promise reaction",
      MEM_FILL "export function init(v) { Promise.resolve().then(() => { fill(); null.x; }); return v; }\n"
      "export function update(v) { return v; }\n" },
    { "a timer",
      MEM_FILL "export function init(v) { engine.setTimeout(() => { fill(); null.x; }, 0); return v; }\n"
      "export function update(v) { return v; }\n" },
    { "update(), and left full for the events that follow",
      MEM_FILL "let full = false;\nexport function update(v) { if (!full) { full = true; fill(); } return v; }\n"
      "export function cursorClick(e) { a.push('' + e.worldPosition); }\n"
      "export function mediaPropertiesChanged(e) { a.push(e.title + e.title); }\n" },
};

static int memory_child (guint k)
{
    if (k >= G_N_ELEMENTS (MEM_JS))
        return 2;
    WalpeSceneScript *ss = make ();
    add (ss, 0, "text", "'use strict';\nlet n = 0;\nexport function update(v) { return 'n' + (++n); }\n");
    add (ss, 1, "text", MEM_JS[k][1]);
    start (ss);
    for (int f = 1; f <= 6; f++) {
        tick (ss, f);
        walpe_ss_cursor_event (ss, "cursorClick", 1, 1, 2, 3, 4);
        walpe_ss_media_event (ss, "mediaPropertiesChanged", "{\"title\":\"a title of some length\"}");
    }
    WalpeSsMem m;
    walpe_ss_mem (ss, &m);
    printf ("6 frames, scripts %s, the counting script at %s\n", m.dead ? "stopped" : "running",
            text[0] ? text[0] : "-");
    walpe_ss_free (ss);
    fflush (stdout);
    return 0;
}

static void memory_full (void)
{
    char *exe = g_file_read_link ("/proc/self/exe", NULL);
    for (guint k = 0; exe && k < G_N_ELEMENTS (MEM_JS); k++) {
        char num[16];
        g_snprintf (num, sizeof num, "%u", k);
        char *argv[] = { exe, (char *) "memory-child", num, NULL };
        char *out = NULL;
        int status = -1;
        gint64 t0 = g_get_monotonic_time ();
        gboolean ran = g_spawn_sync (NULL, argv, NULL, G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &out,
                                     NULL, &status, NULL);
        char *nl = out ? strrchr (g_strchomp (out), '\n') : NULL;
        CHECK (ran && WIFEXITED (status) && WEXITSTATUS (status) == 0 && out && strstr (out, "6 frames"),
               "script memory filled in %s: the wallpaper went on (%.1f s; %s%s%s)", MEM_JS[k][0],
               (g_get_monotonic_time () - t0) / 1e6,
               !ran ? "not started" : WIFSIGNALED (status) ? "the process ended with signal " : "",
               ran && WIFSIGNALED (status) ? g_strsignal (WTERMSIG (status)) : "",
               ran && WIFEXITED (status) ? (nl ? nl + 1 : out ? out : "") : "");
        g_free (out);
    }
    CHECK (exe != NULL, "script memory: the test found its own program");
    g_free (exe);
}

/* ------------------------------------------------- 14. the time of a load */

/* Every entry into a script has its own time limit, and a scene has as
 * many entries at load as it has scripts: n scripts that each use theirs
 * up kept the load (and a daemon asked to quit meanwhile) busy for n times
 * the limit - 20 s with 80 layers. A load has one budget for all of them. */
static void load_time (guint n)
{
    WalpeSceneScript *ss = make ();
    double t0 = thread_cpu ();
    guint added = 0;
    for (guint i = 0; i < n; i++)
        added += add (ss, (int) (i % NLAYERS), "text", "'use strict';\nfor (;;) {}\nexport function update(v) { return v; }\n");
    start (ss);
    double load = thread_cpu () - t0;
    CHECK (load < 6.5, "load: %u scripts that never come back from their first line cost %.2f s of "
           "CPU time (5 s for a load; %u were taken)", n, load, added);
    t0 = thread_cpu ();
    for (int f = 1; f <= 5; f++)
        tick (ss, f);
    WalpeSsMem m;
    walpe_ss_mem (ss, &m);
    CHECK (m.dead && thread_cpu () - t0 < 0.1, "load: after that the wallpaper's scripts are stopped "
           "(%s; 5 frames %.3f s)", m.dead ? "stopped" : "running", thread_cpu () - t0);
    walpe_ss_free (ss);

    /* just under the limit of an entry, each: nothing runs out of time,
     * and the load still ends */
    ss = make ();
    t0 = thread_cpu ();
    for (guint i = 0; i < n; i++)
        add (ss, (int) (i % NLAYERS), "text", "'use strict';\nconst t = Date.now();\n"
                                               "while (Date.now() - t < 200) {}\n"
                                               "export function update(v) { return v; }\n");
    start (ss);
    load = thread_cpu () - t0;
    CHECK (load < 6.5, "load: %u scripts that each stay just under an entry's limit cost %.2f s", n, load);
    walpe_ss_free (ss);

    /* a load of many scripts that behave is far from the budget, and the
     * budget is the load's: frames are not counted against it */
    ss = make ();
    added = 0;
    for (guint i = 0; i < 86; i++)
        added += add (ss, (int) (i % NLAYERS), "text",
                      "'use strict';\nlet n = 0; const tab = []; for (let i = 0; i < 2000; i++) tab.push(Math.sin(i));\n"
                      "export function init(v) { return 'i'; }\n"
                      "export function update(v) { return 'n' + (++n); }\n");
    add (ss, 0, "alpha", "'use strict';\nexport function update(v) { const t = Date.now(); while (Date.now() - t < 200) {} return v; }\n");
    start (ss);
    t0 = thread_cpu ();
    for (int f = 1; f <= 28; f++)
        tick (ss, f);
    walpe_ss_mem (ss, &m);
    CHECK (added == 86 && !m.dead && text[5] && !strcmp (text[5], "n28"),
           "load: 86 scripts that behave are all taken, and 28 frames of %.2f s in all leave them "
           "running (%u, %s, %s)", thread_cpu () - t0, added, m.dead ? "stopped" : "running",
           text[5] ? text[5] : "-");
    walpe_ss_free (ss);
}

/* --------------------------------------- 15. timers that arm themselves */

/* A timer callback that runs out of time is removed - but the callback
 * armed its successor first, which did the same in the next frame: the
 * limit of an entry was paid in every frame, for as long as the wallpaper
 * was shown. Entries that run out of time are counted per wallpaper. */
static void timers_rearmed (void)
{
    WalpeSceneScript *ss = make ();
    add (ss, 0, "text", "'use strict';\nlet n = 0;\nexport function update(v) { return 'n' + (++n); }\n");
    add (ss, 1, "text", "'use strict';\nfunction again() { engine.setTimeout(again, 0); for (;;) {} }\n"
                        "export function init(v) { engine.setTimeout(again, 0); return v; }\n"
                        "export function update(v) { return v; }\n");
    start (ss);
    double t0 = thread_cpu ();
    for (int f = 1; f <= 22; f++)
        tick (ss, f);
    double first = thread_cpu () - t0;
    t0 = thread_cpu ();
    for (int f = 23; f <= 30; f++)
        tick (ss, f);
    double later = thread_cpu () - t0;
    WalpeSsMem m;
    walpe_ss_mem (ss, &m);
    CHECK (later < 0.25 && m.dead, "a timer that arms itself again and never returns: 22 frames cost "
           "%.2f s, the 8 after them %.3f s (scripts %s)", first, later, m.dead ? "stopped" : "running");
    walpe_ss_free (ss);

    /* what runs out of time once (a script, a timer) is stopped alone, as before */
    ss = make ();
    add (ss, 0, "text", "'use strict';\nlet n = 0;\nexport function update(v) { return 'n' + (++n); }\n");
    add (ss, 1, "text", "'use strict';\nexport function init(v) { engine.setInterval(() => { for (;;) {} }, 1); return v; }\n"
                        "export function update(v) { return v; }\n");
    add (ss, 2, "text", "'use strict';\nexport function update(v) { for (;;) {} }\n");
    start (ss);
    for (int f = 1; f <= 30; f++)
        tick (ss, f);
    walpe_ss_mem (ss, &m);
    /* (the frame in which the timer ran out of time ended there) */
    CHECK (!m.dead && text[0] && atoi (text[0] + 1) >= 28, "a timer and a script that ran out of time once: "
           "the other scripts run on (%s, %s of 30 frames)", m.dead ? "stopped" : "running",
           text[0] ? text[0] : "-");
    walpe_ss_free (ss);
}

/* -------------------------- 16. the time limit inside a call to the scene */

/* The time limit ends a script with an error no script can catch. Where it
 * struck while a function of the scene converted an argument (an object's
 * valueOf), the function answered "no such effect" and dropped the error:
 * the script ran on, into the next such call, until the 1.5 s watchdog for
 * built-ins took the thread out and every script of the wallpaper was lost. */
static const char *const SWALLOW_JS[][2] = {
    { "reading an effect's visibility",
      "'use strict';\nconst e = thisScene.getLayer('fx').getEffect(0);\n"
      "export function update(v) { e.__e = { valueOf() { for (;;) {} } }; for (;;) { e.visible; } }\n" },
    { "setting an effect's visibility",
      "'use strict';\nconst e = thisScene.getLayer('fx').getEffect(0);\n"
      "export function update(v) { e.__i = { valueOf() { for (;;) {} } }; for (;;) { e.visible = true; } }\n" },
    { "reading an effect's name",
      "'use strict';\nconst e = thisScene.getLayer('fx').getEffect(1);\n"
      "export function update(v) { e.__e = { valueOf() { for (;;) {} } }; for (;;) { try { e.name; } catch (x) {} } }\n" },
};

static void swallowed (void)
{
    for (guint k = 0; k < G_N_ELEMENTS (SWALLOW_JS); k++) {
        WalpeSceneScript *ss = make ();
        add (ss, 0, "text", "'use strict';\nlet n = 0;\nexport function update(v) { return 'n' + (++n); }\n");
        add (ss, 1, "text", SWALLOW_JS[k][1]);
        start (ss);
        double t0 = thread_cpu ();
        tick (ss, 1);
        double first = thread_cpu () - t0;
        for (int f = 2; f <= 6; f++)
            tick (ss, f);
        WalpeSsMem m;
        walpe_ss_mem (ss, &m);
        CHECK (first < 1.0 && !m.dead && !m.lost && text[0] && !strcmp (text[0], "n6"),
               "time limit while %s: the script is stopped after %.2f s, the others run on (%s, %s)",
               SWALLOW_JS[k][0], first, m.lost ? "all lost" : m.dead ? "all stopped" : "running",
               text[0] ? text[0] : "-");
        walpe_ss_free (ss);
    }
}

/* ------------------------------------ 17. built-ins that never come back */

/* The watchdog takes the thread out of a built-in function that does not
 * return (a regular expression, join () of a huge array) - where the
 * thread is in the script engine's code. That was looked for in the
 * program only: with QuickJS as a shared library the watchdog never found
 * the thread there, and such a script kept the render thread for good.
 * The two cases above that reach such a built-in through an argument's
 * conversion (8) stop the same way; these are the plain statements. What
 * they show depends on the build: set one up with a shared libquickjs
 * (-Dquickjs_libdir=<its directory>) to see the library's case. */
static gpointer builtin_thread (gpointer data)
{
    WalpeSceneScript *ss = make ();
    add (ss, 0, "text", data);
    start (ss);
    for (int f = 1; f <= 3; f++)
        tick (ss, f);
    walpe_ss_free (ss);
    g_mutex_lock (&hang_lock);
    hang_done = 1;
    g_cond_signal (&hang_cond);
    g_mutex_unlock (&hang_lock);
    return NULL;
}

static void builtins (void)
{
    static const char *const js[][2] = {
        { "a regular expression that backtracks without end",
          "'use strict';\nexport function update(v) { return /(x+x+)+y/.test('x'.repeat(64)) ? 'a' : 'b'; }\n" },
        { "join () of an array of 4 294 967 295 holes, at the top of the module",
          "'use strict';\nconst a = []; a.length = 4294967295; const s = a.join();\n"
          "export function update(v) { return v; }\n" },
    };
    for (guint k = 0; k < G_N_ELEMENTS (js); k++) {
        gint64 t0 = g_get_monotonic_time ();
        back_in_time (builtin_thread, (gpointer) js[k][1], js[k][0]);
        double t = (g_get_monotonic_time () - t0) / 1e6;
        CHECK (t < 30.0, "%s: ended by the limits of the scripts, not by the test (%.1f s)", js[k][0], t);
    }
}

int main (int argc, char **argv)
{
    if (argc > 2 && !strcmp (argv[1], "memory-child"))
        return memory_child ((guint) atoi (argv[2]));
    WalpeSceneScript *ss = make ();
    if (!ss) {
        printf ("skip: built without QuickJS\n");
        return 77;
    }
    walpe_ss_free (ss);
    const char *only = argc > 1 ? argv[1] : NULL;
    if (!only || !strcmp (only, "rewriter"))
        rewriter ();
    if (!only || !strcmp (only, "promises"))
        promises ();
    if (!only || !strcmp (only, "vectors"))
        vectors ();
    if (!only || !strcmp (only, "thrown"))
        thrown ();
    if (!only || !strcmp (only, "logs"))
        logs ();
    if (!only || !strcmp (only, "recursion"))
        recursion ();
    if (!only || !strcmp (only, "conversions"))
        conversions ();
    if (!only || !strcmp (only, "json"))
        json_checks ();
    if (!only || !strcmp (only, "audio"))
        audio_buffers ();
    if (!only || !strcmp (only, "timezones"))
        time_zones ();
    if (!only || !strcmp (only, "slowcall"))
        slow_call ();
    if (!only || !strcmp (only, "memory"))
        memory_full ();
    if (!only || !strcmp (only, "load"))
        load_time (argc > 2 ? (guint) atoi (argv[2]) : 40);
    if (!only || !strcmp (only, "timers"))
        timers_rearmed ();
    if (!only || !strcmp (only, "swallowed"))
        swallowed ();
    if (!only || !strcmp (only, "builtins"))
        builtins ();
    if (!only || !strcmp (only, "driver")) {
        int before = contract_breaks;
        /* 6 seeds by default (each takes seconds: its scripts run into the
         * time limits on purpose); `script-hostile-test driver 200` for more */
        guint n = argc > 2 ? (guint) atoi (argv[2]) : 6;
        gint64 t0 = g_get_monotonic_time ();
        for (guint32 seed = 1; seed <= n; seed++)
            driver (seed);
        CHECK (contract_breaks == before, "API driver: %u seeds, %d host calls, %d effect calls, "
               "longest string %" G_GSIZE_FORMAT " bytes, %.1f s: the host contract held (%d breaks)",
               n, sets, fx_sets, longest_str, (g_get_monotonic_time () - t0) / 1e6,
               contract_breaks - before);
    }
    CHECK (contract_breaks == 0, "host contract: %d break(s) in all", contract_breaks);
    printf ("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 0;
}
