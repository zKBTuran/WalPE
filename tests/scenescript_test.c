/* scenescript_test - limits and semantics of the SceneScript runtime
 * (src/engine/scenescript.c), without GL: a host with a few text layers.
 *
 *  1. a script that loops forever is stopped after its time budget, once,
 *     and the scripts after it keep running;
 *  2. a script that never comes back from a built-in (a regular expression
 *     that backtracks without end) does not hang the thread: the runtime
 *     is given up and later calls return at once; what it held is given
 *     back when it is freed;
 *  3. engine.setTimeout(fn, 0) re-armed from its own callback runs once
 *     per frame;
 *  4. promise reactions / async functions run;
 *  5. a script that throws for its first frames works again afterwards,
 *     and its event handlers stay alive;
 *  6. a script cannot reach or replace what C calls (the host bridge, the
 *     per-frame entry points): the cursor layer list stays the scene's.
 *  7. members of the official API (lib.sceneScript.d.ts) that scripts call
 *     exist: one missing member used to throw and take the script down;
 *  8. dates and numbers are formatted by locale and options, Intl exists,
 *     and the date strings V8 reads are read.
 *  9. a module written on one line (minified) is rewritten like one with
 *     its exports at line starts;
 * 10. a returned vector with components that are not numbers keeps the
 *     layer's values for those components.
 * 11. a keyframe animation with Bezier handles gives the values of the
 *     formula, bit for bit (its bisection runs in C: __host.bezu), and
 *     costs a fraction of the same bisection run by a script;
 * 12. registerAudioBuffers arrays hold the host's spectrum (written
 *     straight into the Float32Arrays), also when a script put an array
 *     of its own in their place.
 * 13. a scene's scripts get 40 lines in the log.
 * 14. clocks in other time zones (offsets, daylight saving changes), the
 *     12-hour clock at midnight and noon, numeric dates and numbers.
 * 15. a mirrored keyframe animation fires the events at its ends at every
 *     turn and stays on its track after a step longer than the track.
 * `meson test -C build scenescript` */
#include "walpe-build-config.h"
#include <glib.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "engine/scenescript.h"

#define NLAYERS 4

#ifdef WALPE_QJS_MALLOC_STATE
/* bytes the C library has handed out (0 where it cannot say: under
 * AddressSanitizer, whose leak check covers the same ground). Asked by the
 * one check that needs a QuickJS with a replaceable allocator: a build
 * without it (-Dscenescript=disabled, an older libquickjs) has no caller. */
static gsize heap_in_use (void)
{
#if defined (__GLIBC__) && defined (__GLIBC_PREREQ) && __GLIBC_PREREQ (2, 33)
    struct mallinfo2 mi = mallinfo2 ();
    return mi.uordblks + mi.hblkhd;
#else
    return 0;
#endif
}
#endif
static char *layer_text[NLAYERS];
static int layer_sets[NLAYERS];
static double layer_scale[NLAYERS][3], layer_alpha[NLAYERS];
static int alpha_sets[NLAYERS];
/* particle instance overrides of a layer: count, rate, size; colorn, controlpoint1 */
static double inst_num[NLAYERS][3], inst_vec[NLAYERS][2][3];
static int inst_sets[NLAYERS];
static const char *const inst_num_names[3] = { "inst.count", "inst.rate", "inst.size" };
static const char *const inst_vec_names[2] = { "inst.colorn", "inst.controlpoint1" };
static const char *layer_names[NLAYERS] = { "a", "b", "c", "d" };
static int fails;

#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf ("FAIL: " __VA_ARGS__); printf ("\n"); } \
                              else { printf ("ok: " __VA_ARGS__); printf ("\n"); } } while (0)

static int h_find (gpointer u, const char *name)
{
    (void) u;
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

static WalpeSsType h_get (gpointer u, int idx, const char *prop, double *v, int *n, const char **s)
{
    (void) u;
    *n = 0;
    if (idx < 0 || idx >= NLAYERS)
        return WSS_NONE;
    if (!strcmp (prop, "scale")) {
        for (int k = 0; k < 3; k++)
            v[k] = layer_scale[idx][k];
        *n = 3;
        return WSS_VEC;
    }
    if (!strcmp (prop, "alpha")) {
        v[0] = layer_alpha[idx];
        return WSS_NUM;
    }
    for (int k = 0; k < 3; k++)
        if (!strcmp (prop, inst_num_names[k])) {
            v[0] = inst_num[idx][k];
            return WSS_NUM;
        }
    for (int k = 0; k < 2; k++)
        if (!strcmp (prop, inst_vec_names[k])) {
            memcpy (v, inst_vec[idx][k], sizeof (double) * 3);
            *n = 3;
            return WSS_VEC;
        }
    if (strcmp (prop, "text"))
        return WSS_NONE;
    *s = layer_text[idx] ? layer_text[idx] : "";
    return WSS_STR;
}

static void h_set (gpointer u, int idx, const char *prop, WalpeSsType t, const double *v, int n,
                   const char *s)
{
    (void) u;
    if (idx < 0 || idx >= NLAYERS)
        return;
    if (!strcmp (prop, "scale") && t == WSS_VEC) {
        for (int k = 0; k < 3 && k < n; k++)
            layer_scale[idx][k] = v[k];
        return;
    }
    if (!strcmp (prop, "alpha") && t == WSS_NUM) {
        layer_alpha[idx] = v[0];
        alpha_sets[idx]++;
        return;
    }
    for (int k = 0; k < 3; k++)
        if (!strcmp (prop, inst_num_names[k]) && t == WSS_NUM) {
            inst_num[idx][k] = v[0];
            inst_sets[idx]++;
            return;
        }
    for (int k = 0; k < 2; k++)
        if (!strcmp (prop, inst_vec_names[k]) && t == WSS_VEC && n >= 3) {
            memcpy (inst_vec[idx][k], v, sizeof (double) * 3);
            return;
        }
    if (strcmp (prop, "text") || t != WSS_STR)
        return;
    g_free (layer_text[idx]);
    layer_text[idx] = g_strdup (s);
    layer_sets[idx]++;
}

static WalpeSceneScript *make (void)
{
    static const WalpeSsHost host = { NULL, h_find, h_name, h_get, h_set, NULL,
                                      NULL, NULL, NULL, NULL, NULL };
    for (int i = 0; i < NLAYERS; i++) {
        g_clear_pointer (&layer_text[i], g_free);
        layer_sets[i] = 0;
        layer_scale[i][0] = 2;
        layer_scale[i][1] = 3;
        layer_scale[i][2] = 1;
        layer_alpha[i] = 0.5;
        alpha_sets[i] = 0;
        inst_sets[i] = 0;
        for (int k = 0; k < 3; k++) {
            inst_num[i][k] = 1;
            inst_vec[i][0][k] = 1;
            inst_vec[i][1][k] = 0;
        }
    }
    JsonObject *up = json_object_new ();
    WalpeSceneScript *ss = walpe_ss_new (&host, 1920, 1080, up);
    json_object_unref (up);
    return ss;
}

static void start (WalpeSceneScript *ss)
{
    JsonObject *up = json_object_new ();
    walpe_ss_start (ss, up);
    json_object_unref (up);
}

static void add (WalpeSceneScript *ss, int idx, const char *src)
{
    JsonObject *up = json_object_new ();
    if (!walpe_ss_add (ss, idx, "text", src, NULL, up))
        printf ("note: script %d not added\n", idx);
    json_object_unref (up);
}

static void add_prop (WalpeSceneScript *ss, int idx, const char *prop, const char *src)
{
    JsonObject *up = json_object_new ();
    if (!walpe_ss_add (ss, idx, prop, src, NULL, up))
        printf ("note: script %d (%s) not added\n", idx, prop);
    json_object_unref (up);
}

static double tick (WalpeSceneScript *ss, int frame)
{
    gint64 t0 = g_get_monotonic_time ();
    walpe_ss_tick (ss, 1.0 / 30, frame / 30.0, 0, 0, FALSE);
    return (g_get_monotonic_time () - t0) / 1e6;
}

/* ---- 11 / 12 */
static double thread_cpu_s (void)
{
    struct timespec ts;
    clock_gettime (CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* the prelude's __bez, as it is written there (doubles, left to right) */
typedef struct { double frame, value; gboolean fe, be; double fx, fy, bx, by; } Key;
static double ref_bez (const Key *k0, const Key *k1, double f)
{
    double f0 = k0->frame, f1 = k1->frame, d = f1 - f0;
    if (d <= 0)
        return k1->value;
    if (!k0->fe && !k1->be)
        return k0->value + (k1->value - k0->value) * (f - f0) / d;
    double h = d / 3, x1 = f0 + (k0->fe ? k0->fx * h : h), y1 = k0->value + (k0->fe ? k0->fy * h : 0),
           x2 = f1 + (k1->be ? k1->bx * h : -h), y2 = k1->value + (k1->be ? k1->by * h : 0);
    double lo = 0, hi = 1, u = 0.5;
    for (int it = 0; it < 30; it++) {
        u = (lo + hi) / 2;
        double m = 1 - u;
        double x = m * m * m * f0 + 3 * m * m * u * x1 + 3 * m * u * u * x2 + u * u * u * f1;
        if (x < f)
            lo = u;
        else
            hi = u;
    }
    double m = 1 - u;
    return m * m * m * k0->value + 3 * m * m * u * y1 + 3 * m * u * u * y2 + u * u * u * k1->value;
}

static void audio_fill (gpointer u, int res, float *l, float *r)
{
    (void) u;
    for (int i = 0; i < res; i++) {
        l[i] = 0.013f * (float) (i + 1) + 0.1f / (float) res;
        r[i] = 0.9f - 0.011f * (float) i;
    }
}

static const char *COUNT_JS =
    "'use strict';\nlet n = 0;\nexport function update(value) { return 'n' + (++n); }\n";

/* ---- 13: lines a script wrote with console.log that reached the log */
static int log_lines;
static char log_last[512];      /* the last of those lines, as it was handed to the log */
static GLogWriterOutput count_writer (GLogLevelFlags level, const GLogField *fields, gsize n, gpointer u)
{
    for (gsize i = 0; i < n; i++)
        if (!strcmp (fields[i].key, "MESSAGE") && fields[i].value &&
            strstr ((const char *) fields[i].value, "LOGLINE")) {
            log_lines++;
            g_strlcpy (log_last, fields[i].value, sizeof log_last);
            return G_LOG_WRITER_HANDLED;
        }
    return g_log_writer_default (level, fields, n, u);
}

int main (void)
{
    g_log_set_writer_func (count_writer, NULL, NULL);
    WalpeSceneScript *ss = make ();
    if (!ss) {
        printf ("skip: built without QuickJS\n");
        return 77;
    }

    /* 1. runaway loop: stopped once, the others go on */
    add (ss, 0, COUNT_JS);
    add (ss, 1, "'use strict';\nexport function update(value) { while (true) {} }\n");
    add (ss, 2, COUNT_JS);
    start (ss);
    double t1 = tick (ss, 1), t2 = 0;
    for (int f = 2; f <= 6; f++)
        t2 += tick (ss, f);
    CHECK (t1 < 5.0, "runaway loop: first frame came back (%.2f s)", t1);
    CHECK (t2 < 0.2, "runaway loop: the next 5 frames do not pay the budget again (%.3f s)", t2);
    CHECK (layer_sets[0] == 6, "runaway loop: script before it ran every frame (%d of 6)", layer_sets[0]);
    CHECK (layer_sets[2] == 5, "runaway loop: script after it ran in the 5 later frames (%d)", layer_sets[2]);
    walpe_ss_free (ss);

    /* 3. zero-delay timer re-armed from its callback: once per frame */
    ss = make ();
    add (ss, 0, "'use strict';\nlet ticks = 0;\n"
                "function loop() { ticks++; if (ticks < 100000) engine.setTimeout(loop, 0); }\n"
                "export function init(value) { engine.setTimeout(loop, 0); return value; }\n"
                "export function update(value) { return 't' + ticks; }\n");
    start (ss);
    double t3 = 0;
    for (int f = 1; f <= 10; f++)
        t3 += tick (ss, f);
    CHECK (!g_strcmp0 (layer_text[0], "t10"), "setTimeout(fn, 0) re-armed: 10 runs in 10 frames (%s, %.3f s)",
           layer_text[0] ? layer_text[0] : "?", t3);
    walpe_ss_free (ss);

    /* 4. promises and async functions */
    ss = make ();
    add (ss, 0, "'use strict';\nlet a = 'pending', b = 'pending';\n"
                "async function work() { await null; a = 'resolved'; }\n"
                "export function init(value) { work(); Promise.resolve().then(() => { b = 'ran'; }); return value; }\n"
                "export function update(value) { return a + ' ' + b; }\n");
    start (ss);
    tick (ss, 1);
    CHECK (!g_strcmp0 (layer_text[0], "resolved ran"), "promise reactions run (%s)",
           layer_text[0] ? layer_text[0] : "?");
    walpe_ss_free (ss);

    /* 5. a script that throws for its first calls recovers */
    ss = make ();
    add (ss, 0, "'use strict';\nlet n = 0;\n"
                "export function update(value) { n++; if (n <= 5) return shared.notyet.value; return 'ok' + n; }\n");
    start (ss);
    for (int f = 1; f <= 40; f++)
        tick (ss, f);
    CHECK (layer_text[0] && g_str_has_prefix (layer_text[0], "ok"),
           "a script that threw 5 times works again (%s)", layer_text[0] ? layer_text[0] : "?");
    walpe_ss_free (ss);

    /* 6. the prelude's internals are out of reach */
    ss = make ();
    add (ss, 0, COUNT_JS);
    add (ss, 2, "'use strict';\n"
                "globalThis.__cursorLayers = function () { return { length: 2147483647 }; };\n"
                "globalThis.cursorLayers = globalThis.__cursorLayers;\n"
                "globalThis.__tick = globalThis.tick = function () {};\n"
                "globalThis.__interrupted = function () { return true; };\n"
                "const seen = typeof __host + ' ' + typeof __scripts + ' ' + typeof __run;\n"
                "export function cursorClick(event) {}\n"
                "export function update(value) { return seen; }\n");
    start (ss);
    int *cl = NULL;
    int ncl = walpe_ss_cursor_layers (ss, &cl);
    CHECK (ncl == 1 && cl && cl[0] == 2, "cursor layers come from the scene (%d, first %d)", ncl,
           ncl > 0 && cl ? cl[0] : -1);
    g_free (cl);
    tick (ss, 1);
    tick (ss, 2);
    CHECK (layer_sets[0] == 2, "frames still run after a script overwrote the entry names (%d of 2)",
           layer_sets[0]);
    CHECK (!g_strcmp0 (layer_text[2], "undefined undefined undefined"),
           "host bridge and internals are not visible to scripts (%s)",
           layer_text[2] ? layer_text[2] : "?");
    walpe_ss_free (ss);

    /* 7. + 8. API members, date and number formatting */
    ss = make ();
    add (ss, 0, "'use strict';\n"
                "export function update(value) { const bad = [];\n"
                " const t = (n, f) => { try { if (f() !== true) bad.push(n); } catch (e) { bad.push(n + ': ' + e); } };\n"
                " t('console.error', () => { console.error('probe'); return true; });\n"
                " t('Vec3', () => new Vec3(1, -2, 3).abs().max(new Vec3(2, 2, 2)).toString() === '2 2 3' && new Vec3(new Vec2(1, 2), 3).z === 3);\n"
                " t('Vec2', () => Math.abs(new Vec2(1, 0).rotate(90).y - 1) < 1e-9 && new Vec2(3, 4).lengthSqr() === 25);\n"
                " t('Vec4', () => new Vec4(1, 2, 3, 4).add(new Vec4(1, 1, 1, 1)).w === 5);\n"
                " t('Mat4', () => { const m = Mat4.compose(new Vec3(1, 2, 3), new Vec3(10, 20, 30), new Vec3(2, 2, 2)); return m.multiply(m.inverse()).equals(new Mat4()); });\n"
                " t('Mat3', () => Math.abs(Mat3.fromRotation(30).angle() - 30) < 1e-9);\n"
                " t('layer', () => thisLayer.getParent() === undefined && thisLayer.getChildren().length === 0 && thisLayer.size.x === 0 &&\n"
                "   thisLayer.getAnimationLayerCount() === 0 && typeof thisLayer.instance.rate === 'number' && thisLayer.getVideoTexture().getCurrentTime() === 0);\n"
                " t('scene', () => thisScene.getLayerCount() === 4 && thisScene.createLayer({}).origin instanceof Vec3 && thisScene.getCameraTransforms().zoom === 1);\n"
                " t('engine', () => engine.isLandscape() === true && engine.isPortrait() === false && typeof engine.registerAsset('a', false) === 'object');\n"
                " t('storage', () => { localStorage.set('k', 1, localStorage.LOCATION_GLOBAL); return localStorage.get('k', 'global') === 1; });\n"
                " return bad.length ? bad.join('; ') : 'ok'; }\n");
    add (ss, 1, "'use strict';\n"
                "export function update(value) { const d = new Date(2026, 10, 10, 9, 5, 7);\n"
                " return [d.toLocaleTimeString('en-GB', { hour: '2-digit', minute: '2-digit' }), d.toLocaleTimeString('en-US'), d.toLocaleDateString('de-DE'),\n"
                "  d.toLocaleDateString('en-US', { weekday: 'long' }), (1234.5).toLocaleString('en-US'),\n"
                "  new Intl.DateTimeFormat('en-US', { month: 'short', day: 'numeric' }).format(d),\n"
                "  new Date('12/25/2026 08:30').getHours(), new Date('December 25, 2026').getMonth()].join('|'); }\n");
    start (ss);
    tick (ss, 1);
    CHECK (!g_strcmp0 (layer_text[0], "ok"), "API members exist and compute (%s)",
           layer_text[0] ? layer_text[0] : "?");
    CHECK (!g_strcmp0 (layer_text[1], "09:05|9:05:07 AM|10.11.2026|Tuesday|1,234.5|Nov 10|8|11"),
           "dates and numbers follow locale and options (%s)", layer_text[1] ? layer_text[1] : "?");
    walpe_ss_free (ss);

    /* 9. a minified module: "export" and "import" in the middle of a line,
     * the word inside strings, comments and a regular expression */
    ss = make ();
    add (ss, 0, "'use strict';import * as M from 'WEMath';export var scriptProperties=createScriptProperties()"
                ".addText({name:'t',value:'export function'}).finish();let re=/export 'x/;"
                "/* export function no() {} */export function update(value){"
                "return scriptProperties.t+'|'+M.mix(0,10,0.5)+'|'+re.test(\"export 'x\")+'|'+(6/2/3);}");
    start (ss);
    tick (ss, 1);
    CHECK (!g_strcmp0 (layer_text[0], "export function|5|true|1"), "minified module compiles and runs (%s)",
           layer_text[0] ? layer_text[0] : "?");
    walpe_ss_free (ss);
    /* blanks that are not ASCII between the keywords (3436033033:
     * "export<U+00A0>var<U+00A0>scriptProperties", written in a CJK editor) */
    ss = make ();
#define NB "\xc2\xa0"       /* U+00A0 */
#define IS "\xe3\x80\x80"   /* U+3000 */
    add (ss, 0, "// \xe4\xbd\xbf\xe7\x94\xa8" "\n'use strict';\n"
                "export" NB "var" NB "scriptProperties" NB "=" NB "createScriptProperties" NB "()\n"
                "  .addSlider" NB "( { name: 'x', value: 0.5 })  // \xe5\xb1\x9e\xe6\x80\xa7" "\n  .finish" NB "();\n"
                "export" IS "function" NB "update" NB "(value)" NB "{ return 'x=' + scriptProperties.x; }\n");
#undef NB
#undef IS
    start (ss);
    tick (ss, 1);
    CHECK (!g_strcmp0 (layer_text[0], "x=0.5"), "export followed by a no-break space (%s)",
           layer_text[0] ? layer_text[0] : "?");
    walpe_ss_free (ss);

    /* 10. what a script returns is applied as far as it is a value: a vector
     * component that is not a number keeps the layer's (workshop 2901582518
     * scales the parent of its date widget with
     * `value.x = scriptProperties.posX` where no such property exists: the
     * scale became 0 0 0 and the widget was gone), nothing returned or NaN
     * leaves a number alone */
    ss = make ();
    add_prop (ss, 1, "scale", "'use strict';\n"
                              "export var scriptProperties = createScriptProperties().addSlider({ name: 'ScaleX', value: 1 }).finish();\n"
                              "export function update(value) { value.x = scriptProperties.posX; value.y = scriptProperties.posY;\n"
                              "  value.z = scriptProperties.posZ; return value; }\n");
    add_prop (ss, 2, "scale", "'use strict';\nexport function update(value) { value.x = 5; value.y = NaN; return value; }\n");
    add_prop (ss, 3, "alpha", "'use strict';\nlet n = 0;\nexport function update(value) { return ++n == 1 ? undefined : NaN; }\n");
    add_prop (ss, 0, "alpha", "'use strict';\nexport function update(value) { return 0.25; }\n");
    start (ss);
    tick (ss, 1);
    tick (ss, 2);
    CHECK (layer_scale[1][0] == 2 && layer_scale[1][1] == 3 && layer_scale[1][2] == 1,
           "a vector of undefined leaves the scale alone (%g %g %g)", layer_scale[1][0], layer_scale[1][1],
           layer_scale[1][2]);
    CHECK (layer_scale[2][0] == 5 && layer_scale[2][1] == 3, "a NaN component keeps its value, the others are set (%g %g)",
           layer_scale[2][0], layer_scale[2][1]);
    CHECK (layer_alpha[3] == 0.5 && alpha_sets[3] == 0 && layer_alpha[0] == 0.25,
           "undefined / NaN leave a number alone, a number is set (%g, %d sets, %g)", layer_alpha[3], alpha_sets[3],
           layer_alpha[0]);
    walpe_ss_free (ss);

    /* 11. particle instance overrides (IParticleSystemInstance): a script
     * bound to one returns its value, 0 included (3151551777 switches its
     * night birds off with count 0); thisObject of such a script is the
     * instance (3163060610 sets `thisObject.size` in applyUserProperties);
     * thisLayer.instance reads and writes the same values */
    CHECK (walpe_ss_supported_prop ("inst.count") && walpe_ss_supported_prop ("inst.colorn") &&
           walpe_ss_supported_prop ("inst.controlpoint7") && !walpe_ss_supported_prop ("inst.controlpoint8") &&
           !walpe_ss_supported_prop ("inst.id"), "instance override properties a script can be bound to");
    ss = make ();
    add_prop (ss, 1, "inst.count", "'use strict';\nexport function update(value) { return 0; }\n");
    add_prop (ss, 2, "inst.size", "'use strict';\n"
                                  "export function applyUserProperties(p) { thisObject.size = 2.5; }\n"
                                  "export function update(value) { thisLayer.instance.colorn = new Vec3(0.5, 0.25, 1);\n"
                                  "  const c = thisLayer.instance.controlpoint1; c.x = 10; c.y = undefined;\n"
                                  "  thisLayer.instance.controlpoint1 = c; }\n");
    add_prop (ss, 3, "inst.rate", "'use strict';\nexport function update(value) { return value.nothing; }\n");
    add_prop (ss, 0, "inst.rate", "'use strict';\nexport function update(value) {\n"
                                  "  return thisScene.getLayer('c').instance.size + thisObject.count; }\n");
    inst_vec[2][1][1] = 7;
    start (ss);
    tick (ss, 1);
    tick (ss, 2);
    CHECK (inst_num[1][0] == 0 && inst_sets[1] > 0, "a count script that returns 0 sets 0 (%g, %d sets)", inst_num[1][0],
           inst_sets[1]);
    CHECK (inst_num[2][2] == 2.5, "thisObject of an instance override script is the instance (size %g)", inst_num[2][2]);
    CHECK (inst_vec[2][0][0] == 0.5 && inst_vec[2][0][1] == 0.25 && inst_vec[2][0][2] == 1 && inst_vec[2][1][0] == 10 &&
           inst_vec[2][1][1] == 7, "thisLayer.instance vectors (colorn %g %g %g, controlpoint1 %g %g)", inst_vec[2][0][0],
           inst_vec[2][0][1], inst_vec[2][0][2], inst_vec[2][1][0], inst_vec[2][1][1]);
    CHECK (inst_num[3][1] == 1 && inst_sets[3] == 0 && inst_num[0][1] == 3.5,
           "nothing returned leaves the rate alone, instance values are read (%g, %d sets, %g)", inst_num[3][1],
           inst_sets[3], inst_num[0][1]);
    walpe_ss_free (ss);

    /* 2. a built-in that does not return: the thread comes back */
    ss = make ();
    add (ss, 0, COUNT_JS);
    add (ss, 1, "'use strict';\nlet n = 0;\n"
                "export function update(value) { if (++n == 3) /(a+)+$/.test('a'.repeat(64) + 'b'); return 'r' + n; }\n");
    start (ss);
    tick (ss, 1);
    tick (ss, 2);
    int before = layer_sets[0];
    double t5 = tick (ss, 3);
    double t6 = tick (ss, 4) + tick (ss, 5);
    CHECK (t5 < 30.0, "regular expression without end: the frame came back (%.2f s)", t5);
    CHECK (t6 < 0.2, "regular expression without end: later frames return at once (%.3f s)", t6);
    CHECK (before == 2, "scripts ran before it (%d)", before);
    walpe_ss_free (ss);
    /* ... and what the runtime held comes back when it is freed: two more
     * wallpapers whose script holds 16 MiB when it gets stuck leave no
     * more on the heap than the first did */
#ifdef WALPE_QJS_MALLOC_STATE
    {
        gsize first = 0;
        for (int round = 0; round < 3; round++) {
            ss = make ();
            add (ss, 1, "'use strict';\nlet n = 0, keep = [];\n"
                        "export function update(value) {\n"
                        "  if (++n == 1) for (let i = 0; i < 16; i++) keep.push('x'.repeat(1 << 20) + i);\n"
                        "  if (n == 2) /(a+)+$/.test('a'.repeat(64) + 'b');\n  return 'r' + n; }\n");
            start (ss);
            tick (ss, 1);
            tick (ss, 2);
            walpe_ss_free (ss);
            if (round == 0)
                first = heap_in_use ();
        }
        gsize last = heap_in_use ();
        CHECK (last <= first + 4 * 1024 * 1024,
               "a runtime that was given up returns its memory (two more left %.1f MiB)",
               last > first ? (double) (last - first) / 1048576.0 : 0.0);
    }
#endif

    /* 11. Bezier keyframes: the formula's values, bit for bit; cheap */
    {
        static const Key K[3] = {
            { 0, 0.1, TRUE, FALSE, 0.8, 0.5, 0, 0 },
            { 20, 0.9, TRUE, TRUE, 0.35, -0.7, -0.3, -1.2 },
            { 40, 0.2, FALSE, TRUE, 0, 0, -0.6, 0.25 },
        };
        static const char *ANIM =
            "{\"c0\":[{\"frame\":0,\"value\":0.1,\"front\":{\"enabled\":true,\"x\":0.8,\"y\":0.5}},"
            "{\"frame\":20,\"value\":0.9,\"front\":{\"enabled\":true,\"x\":0.35,\"y\":-0.7},"
            "\"back\":{\"enabled\":true,\"x\":-0.3,\"y\":-1.2}},"
            "{\"frame\":40,\"value\":0.2,\"back\":{\"enabled\":true,\"x\":-0.6,\"y\":0.25}}],"
            "\"options\":{\"fps\":30,\"length\":40,\"mode\":\"loop\"}}";
        ss = make ();
        gboolean added = walpe_ss_add_anim (ss, 0, -1, -1, "alpha", ANIM, "0.5");
        start (ss);
        int bad = 0, n = 0;
        double frame = 0, first_bad = -1, dt = 1.0 / 30;
        for (int k = 1; k <= 95 && added; k++) {
            walpe_ss_tick (ss, dt, k * dt, 0, 0, FALSE);
            /* __animTick: b = a + dt * fps * rate; a loop wraps at its length */
            double b = frame + dt * 30.0 * 1.0;
            frame = b >= 40 ? fmod (fmod (b, 40.0) + 40.0, 40.0) : b;
            const Key *k0 = frame <= 20 ? &K[0] : &K[1];
            double want = frame <= K[0].frame ? K[0].value : frame >= K[2].frame ? K[2].value
                        : ref_bez (k0, k0 + 1, frame);
            n++;
            if (layer_alpha[0] != want) {
                if (!bad)
                    first_bad = frame;
                bad++;
            }
        }
        CHECK (added && bad == 0 && alpha_sets[0] >= 95,
               "Bezier keyframes: %d of %d values are the formula's, bit for bit (first other one "
               "at frame %g; %d sets)", n - bad, n, first_bad, alpha_sets[0]);
        walpe_ss_free (ss);

        /* cost: 24 animated channels for 300 frames against a script that
         * runs the same 24 * 300 bisections itself (what the animation
         * engine did before: both in the interpreter, the engine's the
         * slower one). Thread CPU time, and the best of three runs each:
         * on a busy machine one run alone varies by a third even so. */
        double t_anim = 1e9, t_js = 1e9;
        for (int rep = 0; rep < 3; rep++) {
            ss = make ();
            for (int k = 0; k < 24; k++)
                walpe_ss_add_anim (ss, k % NLAYERS, -1, -1, "alpha", ANIM, "0.5");
            start (ss);
            double t0 = thread_cpu_s ();
            for (int k = 1; k <= 300; k++)
                walpe_ss_tick (ss, dt, k * dt, 0, 0, FALSE);
            t_anim = MIN (t_anim, thread_cpu_s () - t0);
            walpe_ss_free (ss);
            ss = make ();
            add (ss, 0, "'use strict';\nlet q = 0;\n"
                        "export function update(value) { let s = 0; q += 1;\n"
                        "  for (let c = 0; c < 24; c++) { const f0 = 0, x1 = 5.33, x2 = 18, f1 = 20, f = (q + c) % 20;\n"
                        "    let lo = 0, hi = 1, u = 0.5; for (let it = 0; it < 30; it++) { u = (lo + hi) / 2; const m = 1 - u;\n"
                        "      const x = m * m * m * f0 + 3 * m * m * u * x1 + 3 * m * u * u * x2 + u * u * u * f1; if (x < f) lo = u; else hi = u; }\n"
                        "    s += u; }\n  return 's' + s; }\n");
            start (ss);
            t0 = thread_cpu_s ();
            for (int k = 1; k <= 300; k++)
                walpe_ss_tick (ss, dt, k * dt, 0, 0, FALSE);
            t_js = MIN (t_js, thread_cpu_s () - t0);
            walpe_ss_free (ss);
        }
        CHECK (t_anim < 0.75 * t_js,
               "24 Bezier channels, 300 frames: %.1f ms of CPU; the bisections alone in a script: "
               "%.1f ms (wanted: under 75 %% of that)", t_anim * 1e3, t_js * 1e3);
    }

    /* 12. audio buffers */
    {
        static const WalpeSsHost host = { NULL, h_find, h_name, h_get, h_set, audio_fill,
                                          NULL, NULL, NULL, NULL, NULL };
        walpe_ss_free (make ());   /* resets the layers */
        JsonObject *up = json_object_new ();
        WalpeSceneScript *as = walpe_ss_new (&host, 1920, 1080, up);
        json_object_unref (up);
        add_prop (as, 0, "alpha", "'use strict';\nconst b = engine.registerAudioBuffers(64);\n"
                                  "export function update(v) { return b.left[5] + b.right[63] + b.average[7]; }\n");
        add_prop (as, 1, "alpha", "'use strict';\nconst b = engine.registerAudioBuffers(16);\n"
                                  "b.left = [9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9];\n"
                                  "export function update(v) { return b.left[3] + b.right[15] + b.average[2] + b.left[16]; }\n");
        add_prop (as, 2, "alpha", "'use strict';\nconst b = engine.registerAudioBuffers(32);\n"
                                  "b.right = new Int32Array(32);\n"
                                  "export function update(v) { return b.left[31] + b.right[4] * 100 + b.average[0]; }\n");
        start (as);
        walpe_ss_tick (as, 1.0 / 30, 1.0 / 30, 0, 0, FALSE);
        float l[64], r[64];
        audio_fill (NULL, 64, l, r);
        double want0 = (double) l[5] + (double) r[63] + (double) (float) (0.5 * (l[7] + r[7]));
        audio_fill (NULL, 16, l, r);
        double want1 = (double) l[3] + (double) r[15] + (double) (float) (0.5 * (l[2] + r[2])) + 9;
        audio_fill (NULL, 32, l, r);
        /* an Int32Array takes ToInt32 of each value: 0 for these */
        double want2 = (double) l[31] + 0.0 * 100 + (double) (float) (0.5 * (l[0] + r[0]));
        CHECK (layer_alpha[0] == want0, "audio buffers: Float32Arrays hold the spectrum (%.9g, want %.9g)",
               layer_alpha[0], want0);
        CHECK (layer_alpha[1] == want1, "audio buffers: a plain array in place of one is filled, its "
               "17th element left alone (%.9g, want %.9g)", layer_alpha[1], want1);
        CHECK (layer_alpha[2] == want2, "audio buffers: another typed array in place of one is "
               "filled by value, not by bytes (%.9g, want %.9g)", layer_alpha[2], want2);
        walpe_ss_free (as);
    }

    /* 13. a scene's scripts get 40 lines in the log, however many they write */
    {
        WalpeSceneScript *ls = make ();
        add (ls, 0, "'use strict';\nfor (let i = 0; i < 300; i++) console.log('LOGLINE ' + i);\n"
                    "export function update(v) { for (let i = 0; i < 10; i++) console.log('LOGLINE again'); return v; }\n");
        start (ls);
        int at_load = log_lines;
        for (int f = 1; f <= 20; f++)
            tick (ls, f);
        CHECK (at_load == 40 && log_lines == 40,
               "console.log: 40 lines of a scene reach the log (%d of 300 at load, %d after 200 more)", at_load,
               log_lines);
        walpe_ss_free (ls);
    }

    /* 13b. what a script logs, and the text of an error it throws, reach
     * the log with their control characters as visible text: a line break
     * in it began a log line of the script's own wording, an escape
     * sequence went to whatever shows the log */
    {
        WalpeSceneScript *ls = make ();
        add (ls, 0, "'use strict';\nconsole.log('LOGLINE one\\nFORGED: two\\x1b[2J\\ttab\\r');\n"
                    "export function update(v) { return v; }\n");
        start (ls);
        CHECK (strstr (log_last, "LOGLINE one\\x0aFORGED: two\\x1b[2J\\x09tab\\x0d") != NULL &&
               !strchr (log_last, '\n') && !strchr (log_last, 0x1b) && !strchr (log_last, '\t'),
               "console.log: control characters of a script's text are visible in the log (%s)", log_last);
        walpe_ss_free (ls);
        log_last[0] = 0;
        ls = make ();
        add (ls, 0, "'use strict';\nexport function update(v) { throw new Error('LOGLINE thrown\\nFORGED: too'); }\n");
        start (ls);
        tick (ls, 1);
        CHECK (strstr (log_last, "LOGLINE thrown\\x0aFORGED: too") != NULL && !strchr (log_last, '\n'),
               "an error's text: its line break is visible in the log (%s)", log_last);
        walpe_ss_free (ls);
    }

    /* 14. clocks in other time zones: the offset of the moment, daylight
     * saving time and its two changes, zones off the hour, the date across
     * midnight; the 12-hour clock at midnight and noon. Needs the system's
     * time zone files (skipped without). */
    {
        GTimeZone *tz = g_time_zone_new_identifier ("Australia/Lord_Howe");
        if (!tz) {
            printf ("note: no time zone data on this machine, case 14 skipped\n");
        } else {
            g_time_zone_unref (tz);
            WalpeSceneScript *zs = make ();
            add (zs, 0, "'use strict';\n"
                "const at = (y, mo, d, h, mi, s) => new Date(Date.UTC(y, mo - 1, d, h, mi, s || 0));\n"
                "const tm = (d, z) => d.toLocaleTimeString('en-GB', { timeZone: z });\n"
                "export function update(value) { return [\n"
                "  tm(at(2026, 1, 15, 12, 0), 'Asia/Tokyo'), tm(at(2026, 1, 15, 12, 0), 'UTC'),\n"
                "  tm(at(2026, 1, 15, 12, 0), 'America/New_York'), tm(at(2026, 7, 15, 12, 0), 'America/New_York'),\n"
                "  tm(at(2026, 3, 8, 6, 59, 59), 'America/New_York'), tm(at(2026, 3, 8, 7, 0), 'America/New_York'),\n"
                "  tm(at(2026, 11, 1, 5, 59, 59), 'America/New_York'), tm(at(2026, 11, 1, 6, 0), 'America/New_York'),\n"
                "  tm(at(2026, 3, 29, 0, 59, 59), 'Europe/Berlin'), tm(at(2026, 3, 29, 1, 0), 'Europe/Berlin'),\n"
                "  tm(at(2026, 10, 25, 0, 59, 59), 'Europe/Berlin'), tm(at(2026, 10, 25, 1, 0), 'Europe/Berlin'),\n"
                "  tm(at(2026, 1, 15, 12, 0), 'Asia/Kolkata'), tm(at(2026, 1, 15, 12, 0), 'Asia/Kathmandu'),\n"
                "  tm(at(2026, 1, 15, 12, 0), 'Australia/Lord_Howe'), tm(at(2026, 7, 15, 12, 0), 'Australia/Lord_Howe'),\n"
                "  at(2026, 12, 31, 20, 0).toLocaleDateString('en-GB', { timeZone: 'Asia/Tokyo' }),\n"
                "  at(2026, 12, 31, 20, 0).toLocaleDateString('en-GB', { timeZone: 'America/New_York' })].join('|'); }\n");
            add (zs, 1, "'use strict';\n"
                "const at = (h, mi, s) => new Date(Date.UTC(2026, 10, 10, h, mi, s));\n"
                "const us = d => d.toLocaleTimeString('en-US', { timeZone: 'UTC' });\n"
                "export function update(value) { const d = at(9, 5, 7); return [\n"
                "  us(at(0, 5, 0)), us(at(12, 5, 0)), us(at(13, 5, 0)), us(at(23, 59, 59)),\n"
                "  d.toLocaleDateString('en-US', { weekday: 'long', timeZone: 'UTC' }),\n"
                "  d.toLocaleDateString('en-US', { month: 'long', day: 'numeric', year: 'numeric', timeZone: 'UTC' }),\n"
                "  d.toLocaleDateString('de-DE', { timeZone: 'UTC' }), d.toLocaleTimeString('de-DE', { timeZone: 'UTC' }),\n"
                "  d.toLocaleDateString('ja-JP', { timeZone: 'UTC' }),\n"
                "  d.toLocaleTimeString('en-US', { hour: '2-digit', minute: '2-digit', hour12: false, timeZone: 'UTC' }),\n"
                "  (1234567.891).toLocaleString('en-US'), (1234567.891).toLocaleString('de-DE'), (0.5).toLocaleString('en-US', { style: 'percent' })].join('|'); }\n");
            start (zs);
            tick (zs, 1);
            CHECK (!g_strcmp0 (layer_text[0],
                               "21:00:00|12:00:00|07:00:00|08:00:00|01:59:59|03:00:00|01:59:59|01:00:00|"
                               "01:59:59|03:00:00|02:59:59|02:00:00|17:30:00|17:45:00|23:00:00|22:30:00|"
                               "01/01/2027|31/12/2026"),
                   "time zones: offsets, both daylight saving changes, zones off the hour, the date (%s)",
                   layer_text[0] ? layer_text[0] : "?");
            CHECK (!g_strcmp0 (layer_text[1],
                               "12:05:00 AM|12:05:00 PM|1:05:00 PM|11:59:59 PM|Tuesday|November 10, 2026|"
                               "10.11.2026|09:05:07|2026/11/10|09:05|1,234,567.891|1.234.567,891|50%"),
                   "12-hour clock at midnight and noon, names, numeric dates, numbers (%s)",
                   layer_text[1] ? layer_text[1] : "?");
            walpe_ss_free (zs);
        }
    }

    /* 15. a mirrored animation: the events at its two ends fire at every
     * turn, the ones between on every pass; after a step longer than the
     * track the playhead is where a back-and-forth run of that length ends */
    {
        static const char *LONG =
            "{\"c0\":[{\"frame\":0,\"value\":0},{\"frame\":30,\"value\":1}],"
            "\"options\":{\"fps\":30,\"length\":30,\"mode\":\"mirror\",\"events\":["
            "{\"frame\":0,\"name\":\"first\"},{\"frame\":15,\"name\":\"middle\"},{\"frame\":30,\"name\":\"last\"}]}}";
        static const char *SHORT =
            "{\"c0\":[{\"frame\":0,\"value\":0},{\"frame\":10,\"value\":1}],"
            "\"options\":{\"fps\":30,\"length\":10,\"mode\":\"mirror\"}}";
        WalpeSceneScript *ms = make ();
        add (ms, 0, "'use strict';\nlet first = 0, middle = 0, last = 0;\n"
                    "export function animationEvent(event, value) { if (event.name === 'first') first++;\n"
                    "  else if (event.name === 'middle') middle++; else if (event.name === 'last') last++; }\n"
                    "export function update(value) { return first + ' ' + middle + ' ' + last; }\n");
        gboolean added = walpe_ss_add_anim (ms, 0, -1, -1, "alpha", LONG, "0.5") &&
                         walpe_ss_add_anim (ms, 1, -1, -1, "alpha", SHORT, "0.5");
        start (ms);
        /* 0.9 frames a tick: no tick lands on an event's frame. 390 ticks =
         * 351 frames = 11 passes of the long track and 21 frames of the 12th */
        const double dt = 0.03;
        double run = 0, travelled = 0, worst = 0;
        int k = 0;
        for (; k < 390 && added; k++) {
            run += dt;
            walpe_ss_tick (ms, dt, run, 0, 0, FALSE);
        }
        travelled = 390 * dt * 30;
        CHECK (added && !g_strcmp0 (layer_text[0], "6 12 6"),
               "mirrored animation: events at the first frame, the middle and the last frame fired "
               "\"%s\" times in 11 passes and a bit (want \"6 12 6\")", layer_text[0] ? layer_text[0] : "?");
        /* one step of a second = three lengths of the short track, then
         * ordinary frames: the value follows the triangle all the time */
        for (k = 0; k < 12 && added; k++) {
            const double step = k == 0 ? 1.0 : dt;
            run += step;
            travelled += step * 30;
            walpe_ss_tick (ms, step, run, 0, 0, FALSE);
            double x = fmod (travelled, 20.0), want = (x <= 10 ? x : 20 - x) / 10;
            worst = MAX (worst, fabs (layer_alpha[1] - want));
        }
        CHECK (added && worst < 1e-6,
               "mirrored animation after a step longer than the track: the value is at most %.6f off "
               "the back-and-forth run (0 and 1 in turns when the playhead stands outside the track)", worst);
        walpe_ss_free (ms);
    }

    /* 16. a script and a one-shot animation on one property: update () is
     * given the animation's value, also after the animation has ended */
    {
        static const char *ONCE =
            "{\"c0\":[{\"frame\":0,\"value\":0},{\"frame\":30,\"value\":1}],"
            "\"options\":{\"fps\":30,\"length\":30,\"mode\":\"single\"}}";
        WalpeSceneScript *os = make ();
        add_prop (os, 0, "alpha", "'use strict';\nexport function update(value) { return value - 0.25; }\n");
        gboolean added = walpe_ss_add_anim (os, 0, -1, -1, "alpha", ONCE, "0.5");
        start (os);
        double playing = 0, ended = 0;
        for (int k = 1; k <= 90 && added; k++) {
            walpe_ss_tick (os, 1.0 / 30, k / 30.0, 0, 0, FALSE);
            double want = MIN (k / 30.0, 1.0) - 0.25, off = fabs (layer_alpha[0] - want);
            if (k < 30)
                playing = MAX (playing, off);
            else
                ended = MAX (ended, off);
        }
        CHECK (added && playing < 1e-9 && ended < 1e-9,
               "script on an animated property: its result is at most %.6f off the animated value - 0.25 "
               "while the one-shot plays and %.6f in the two seconds after its end", playing, ended);
        walpe_ss_free (os);
    }

    printf (fails ? "%d check(s) failed\n" : "all checks passed\n", fails);
    return fails ? 1 : 0;
}
