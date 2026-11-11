/* scenescript.c - minimal WE SceneScript on QuickJS (see scenescript.h).
 *
 * Each property script (an ES module in WE) is rewritten into a function
 * body: "export " is dropped from function/var/let/const/class
 * declarations and `import * as X from 'Y'` becomes a lookup in the
 * prelude's module table; the function receives thisLayer/thisObject and
 * returns its init/update/applyUserProperties + scriptProperties. All the
 * WE API surface lives in the JS prelude below; C only provides layer
 * property get/set by index (__host). */
#include "walpe-build-config.h"
#include "scenescript.h"
#include "propprobe.h"
#include <string.h>
#include <math.h>

gboolean walpe_ss_supported_prop (const char *prop)
{
    static const char *const ok[] = { "text", "visible", "alpha", "origin", "scale",
                                      "angles", "color", "pointsize", "maxwidth",
                                      "padding",
                                      /* particle instance overrides */
                                      "inst.rate", "inst.count", "inst.size", "inst.lifetime",
                                      "inst.alpha", "inst.speed", "inst.colorn", NULL };
    for (int i = 0; ok[i]; i++)
        if (!g_strcmp0 (prop, ok[i]))
            return TRUE;
    if (prop && !strncmp (prop, "inst.controlpoint", 17) && prop[17] >= '0' && prop[17] <= '7' &&
        !prop[18])
        return TRUE;
    return FALSE;
}

#ifndef HAVE_QUICKJS

WalpeSceneScript *walpe_ss_new (const WalpeSsHost *host, float w, float h, JsonObject *up)
{
    (void) host; (void) w; (void) h; (void) up;
    return NULL;
}
gboolean walpe_ss_add (WalpeSceneScript *ss, int idx, const char *prop, const char *src,
                       JsonObject *sp, JsonObject *up)
{
    (void) ss; (void) idx; (void) prop; (void) src; (void) sp; (void) up;
    return FALSE;
}
void walpe_ss_start (WalpeSceneScript *ss, JsonObject *up) { (void) ss; (void) up; }
void walpe_ss_set_prop_types (WalpeSceneScript *ss, JsonObject *t) { (void) ss; (void) t; }
void walpe_ss_tick (WalpeSceneScript *ss, double ft, double rt, float cx, float cy, gboolean d)
{
    (void) ss; (void) ft; (void) rt; (void) cx; (void) cy; (void) d;
}
int walpe_ss_cursor_layers (WalpeSceneScript *ss, int **out) { (void) ss; *out = NULL; return 0; }
void walpe_ss_cursor_event (WalpeSceneScript *ss, const char *kind, int idx, float wx, float wy,
                            float lx, float ly)
{
    (void) ss; (void) kind; (void) idx; (void) wx; (void) wy; (void) lx; (void) ly;
}
int walpe_ss_count (WalpeSceneScript *ss) { (void) ss; return 0; }
gboolean walpe_ss_uses_audio (WalpeSceneScript *ss) { (void) ss; return FALSE; }
void walpe_ss_free (WalpeSceneScript *ss) { (void) ss; }
gboolean walpe_ss_add_fx (WalpeSceneScript *ss, int idx, int effect, int pass, const char *prop,
                          const char *src, JsonObject *sp, JsonObject *up, const char *iv)
{
    (void) ss; (void) idx; (void) effect; (void) pass; (void) prop; (void) src; (void) sp;
    (void) up; (void) iv;
    return FALSE;
}
gboolean walpe_ss_add_anim (WalpeSceneScript *ss, int idx, int effect, int pass, const char *prop,
                            const char *anim_json, const char *base_json)
{
    (void) ss; (void) idx; (void) effect; (void) pass; (void) prop; (void) anim_json;
    (void) base_json;
    return FALSE;
}
gboolean walpe_ss_add_inert (WalpeSceneScript *ss, int idx, const char *prop, const char *src,
                             JsonObject *sp, JsonObject *up, const char *iv)
{
    (void) ss; (void) idx; (void) prop; (void) src; (void) sp; (void) up; (void) iv;
    return FALSE;
}
void walpe_ss_media_event (WalpeSceneScript *ss, const char *kind, const char *ev)
{
    (void) ss; (void) kind; (void) ev;
}
gboolean walpe_ss_uses_media (WalpeSceneScript *ss) { (void) ss; return FALSE; }
gboolean walpe_ss_uses_cursor (WalpeSceneScript *ss) { (void) ss; return FALSE; }

#else

#include <quickjs/quickjs.h>
#include <langinfo.h>
#include <locale.h>
#include <setjmp.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <ucontext.h>

struct WalpeSceneScript {
    JSRuntime *rt;
    JSContext *ctx;
    WalpeSsHost host;
    int count;
    gboolean uses_audio;
    gboolean uses_media;
    gboolean uses_cursor;     /* a script reads input.cursor* every frame */
    int errors_logged;
    gboolean dead;            /* scripts stopped: JS is not entered any more */
    gboolean lost;            /* the watchdog took the thread out of a built-in:
                               * the runtime is in an undefined state and is
                               * neither used nor freed again */
    int unblamed;             /* entries in a row that ran out of time with no
                               * script or timer to blame */
    JSValue fn[12];           /* the prelude's entry points (SsFn) */
    JSValue f32_proto;        /* Float32Array.prototype (h_audio), JS_UNDEFINED = not looked up */
    GArray *collect;          /* walpe_ss_cursor_layers: layers reported so far */
    int n_layers;             /* layers of the scene, -1 = not counted yet */
};

/* The prelude is one function: what it declares is out of the reach of
 * wallpaper scripts, which see the SceneScript API (exported at its end)
 * and nothing else - not the host bridge, not the functions C calls. C
 * keeps references to those, taken before the first script is compiled:
 * a script cannot put its own function in their place. */
typedef enum { SS_FN_ADD, SS_FN_ADD_ANIM, SS_FN_START, SS_FN_TICK, SS_FN_MEDIA, SS_FN_CURSOR,
               SS_FN_CURSOR_LAYERS, SS_FN_INTERRUPTED, SS_FN_TYPES, SS_FN_N } SsFn;
static const char *const ss_fn_names[SS_FN_N] = { "add", "addAnim", "start", "tick", "media",
                                                  "cursor", "cursorLayers", "interrupted",
                                                  "types" };
G_STATIC_ASSERT (SS_FN_N <= 12);

static const char *PRELUDE =
"(function (__host, __canvas, __probe, __traceAnim, __deflocale, __deftz, __lang, __prof) { 'use strict';\n"
/* Vec2 / Vec3 / Vec4 / Mat3 / Mat4 as lib.sceneScript.d.ts declares them. The
 * frequent operations are written out; the per-component ones come from
 * __vecExt. Angles are degrees, matrices column-major (m[12..14] / m[6..7]
 * = translation), Euler order Z * Y * X. */
"const __D2R = Math.PI / 180, __R2D = 180 / Math.PI;\n"
"function __nums(s) { return s.trim().split(/\\s+/).map(Number); }\n"
"class Vec2 { constructor(x, y) { if (typeof x === 'string') { const p = __nums(x); x = p[0]; y = p[1]; }\n"
"  else if (x && typeof x === 'object') { y = x.y; x = x.x; }\n"
"  this.x = +x || 0; this.y = y === undefined ? this.x : (+y || 0); }\n"
"  copy() { return new Vec2(this.x, this.y); }\n"
"  add(o) { return typeof o === 'number' ? new Vec2(this.x + o, this.y + o) : new Vec2(this.x + o.x, this.y + o.y); }\n"
"  subtract(o) { return typeof o === 'number' ? new Vec2(this.x - o, this.y - o) : new Vec2(this.x - o.x, this.y - o.y); }\n"
"  multiply(o) { return typeof o === 'number' ? new Vec2(this.x * o, this.y * o) : new Vec2(this.x * o.x, this.y * o.y); }\n"
"  divide(o) { return typeof o === 'number' ? new Vec2(this.x / o, this.y / o) : new Vec2(this.x / o.x, this.y / o.y); }\n"
"  length() { return Math.hypot(this.x, this.y); }\n"
"  normalize() { const l = this.length() || 1; return new Vec2(this.x / l, this.y / l); }\n"
"  dot(o) { return this.x * o.x + this.y * o.y; }\n"
"  perpendicular() { return new Vec2(-this.y, this.x); }\n"
"  angle() { return Math.atan2(this.y, this.x) * __R2D; }\n"
"  angleBetween(o) { return Math.atan2(this.x * o.y - this.y * o.x, this.x * o.x + this.y * o.y) * __R2D; }\n"
"  rotate(a) { const r = a * __D2R, c = Math.cos(r), s = Math.sin(r); return new Vec2(this.x * c - this.y * s, this.x * s + this.y * c); }\n"
"  toString() { return this.x + ' ' + this.y; } }\n"
"class Vec3 { constructor(x, y, z) { if (typeof x === 'string') { const p = __nums(x); x = p[0]; y = p[1]; z = p[2]; }\n"
"  else if (x && typeof x === 'object') { z = x.z !== undefined ? x.z : y; y = x.y; x = x.x; }\n"
"  this.x = +x || 0; this.y = y === undefined ? this.x : (+y || 0); this.z = z === undefined ? (y === undefined ? this.x : 0) : (+z || 0); }\n"
"  static fromSpherical(r, theta, phi) { const t = theta * __D2R, p = phi * __D2R, s = Math.sin(t); return new Vec3(r * s * Math.sin(p), r * Math.cos(t), r * s * Math.cos(p)); }\n"
"  copy() { return new Vec3(this.x, this.y, this.z); }\n"
"  add(o) { return typeof o === 'number' ? new Vec3(this.x + o, this.y + o, this.z + o) : new Vec3(this.x + o.x, this.y + o.y, this.z + (o.z || 0)); }\n"
"  subtract(o) { return typeof o === 'number' ? new Vec3(this.x - o, this.y - o, this.z - o) : new Vec3(this.x - o.x, this.y - o.y, this.z - (o.z || 0)); }\n"
"  multiply(o) { return typeof o === 'number' ? new Vec3(this.x * o, this.y * o, this.z * o) : new Vec3(this.x * o.x, this.y * o.y, this.z * (o.z === undefined ? 1 : o.z)); }\n"
"  divide(o) { return typeof o === 'number' ? new Vec3(this.x / o, this.y / o, this.z / o) : new Vec3(this.x / o.x, this.y / o.y, this.z / (o.z === undefined ? 1 : o.z)); }\n"
"  length() { return Math.hypot(this.x, this.y, this.z); }\n"
"  normalize() { const l = this.length() || 1; return new Vec3(this.x / l, this.y / l, this.z / l); }\n"
"  dot(o) { return this.x * o.x + this.y * o.y + this.z * (o.z || 0); }\n"
"  cross(o) { return new Vec3(this.y * o.z - this.z * o.y, this.z * o.x - this.x * o.z, this.x * o.y - this.y * o.x); }\n"
"  refract(n, eta) { const d = this.dot(n), k = 1 - eta * eta * (1 - d * d); if (k < 0) return new Vec3(0, 0, 0);\n"
"    const f = eta * d + Math.sqrt(k); return new Vec3(eta * this.x - f * n.x, eta * this.y - f * n.y, eta * this.z - f * n.z); }\n"
"  angleBetween(o) { const l = this.length() * o.length(); return l ? Math.acos(Math.min(Math.max(this.dot(o) / l, -1), 1)) * __R2D : 0; }\n"
"  toSpherical() { const r = this.length(); if (!r) return new Vec3(0, 0, 0);\n"
"    return new Vec3(r, Math.acos(Math.min(Math.max(this.y / r, -1), 1)) * __R2D, Math.atan2(this.x, this.z) * __R2D); }\n"
"  toString() { return this.x + ' ' + this.y + ' ' + this.z; } }\n"
"class Vec4 { constructor(x, y, z, w) { if (typeof x === 'string') { const p = __nums(x); x = p[0]; y = p[1]; z = p[2]; w = p[3]; if (y !== undefined && w === undefined) w = 0; }\n"
"  else if (x && typeof x === 'object') { const o = x; if (o.w !== undefined) { w = o.w; z = o.z; } else if (o.z !== undefined) { w = y; z = o.z; } else { w = z; z = y; }\n"
"    x = o.x; y = o.y === undefined ? 0 : o.y; if (z === undefined) z = 0; if (w === undefined) w = 0; }\n"
"  this.x = +x || 0; this.y = y === undefined ? this.x : (+y || 0); this.z = z === undefined ? (y === undefined ? this.x : 0) : (+z || 0);\n"
"  this.w = w === undefined ? (y === undefined && z === undefined ? this.x : 0) : (+w || 0); }\n"
"  copy() { return new Vec4(this.x, this.y, this.z, this.w); }\n"
"  add(o) { return typeof o === 'number' ? new Vec4(this.x + o, this.y + o, this.z + o, this.w + o) : new Vec4(this.x + o.x, this.y + o.y, this.z + (o.z || 0), this.w + (o.w || 0)); }\n"
"  subtract(o) { return typeof o === 'number' ? new Vec4(this.x - o, this.y - o, this.z - o, this.w - o) : new Vec4(this.x - o.x, this.y - o.y, this.z - (o.z || 0), this.w - (o.w || 0)); }\n"
"  multiply(o) { return typeof o === 'number' ? new Vec4(this.x * o, this.y * o, this.z * o, this.w * o) : new Vec4(this.x * o.x, this.y * o.y, this.z * (o.z === undefined ? 1 : o.z), this.w * (o.w === undefined ? 1 : o.w)); }\n"
"  divide(o) { return typeof o === 'number' ? new Vec4(this.x / o, this.y / o, this.z / o, this.w / o) : new Vec4(this.x / o.x, this.y / o.y, this.z / (o.z === undefined ? 1 : o.z), this.w / (o.w === undefined ? 1 : o.w)); }\n"
"  length() { return Math.hypot(this.x, this.y, this.z, this.w); }\n"
"  normalize() { const l = this.length() || 1; return new Vec4(this.x / l, this.y / l, this.z / l, this.w / l); }\n"
"  dot(o) { return this.x * o.x + this.y * o.y + this.z * (o.z || 0) + this.w * (o.w || 0); }\n"
"  toString() { return this.x + ' ' + this.y + ' ' + this.z + ' ' + this.w; } }\n"
"function __vecExt(C, K) { const n = K.length;\n"
"  const c = (o, k) => typeof o === 'number' ? o : (o && o[k] !== undefined ? +o[k] : 0);\n"
"  const mk = (v, f) => { const a = [0, 0, 0, 0]; for (let i = 0; i < n; i++) a[i] = f(v[K[i]], K[i]); return new C(a[0], a[1], a[2], a[3]); };\n"
"  const ext = {\n"
"    lengthSqr() { let s = 0; for (let i = 0; i < n; i++) s += this[K[i]] * this[K[i]]; return s; },\n"
"    distanceSqr(o) { let s = 0; for (let i = 0; i < n; i++) { const d = this[K[i]] - c(o, K[i]); s += d * d; } return s; },\n"
"    distance(o) { return Math.sqrt(this.distanceSqr(o)); },\n"
"    isFinite() { for (let i = 0; i < n; i++) if (!Number.isFinite(this[K[i]])) return false; return true; },\n"
"    equals(o) { if (!o) return false; for (let i = 0; i < n; i++) if (!(Math.abs(this[K[i]] - o[K[i]]) <= 1e-6)) return false; return true; },\n"
"    negate() { return mk(this, x => -x); },\n"
"    reflect(nrm) { const d = 2 * this.dot(nrm); return mk(this, (x, k) => x - d * c(nrm, k)); },\n"
"    project(o) { const l = o.dot(o), f = l ? this.dot(o) / l : 0; return mk(this, (x, k) => c(o, k) * f); },\n"
"    mix(o, t) { return mk(this, (x, k) => x + (c(o, k) - x) * c(t, k)); },\n"
"    min(o) { return mk(this, (x, k) => Math.min(x, c(o, k))); },\n"
"    max(o) { return mk(this, (x, k) => Math.max(x, c(o, k))); },\n"
"    clamp(lo, hi) { return mk(this, (x, k) => Math.min(Math.max(x, c(lo, k)), c(hi, k))); },\n"
"    abs() { return mk(this, Math.abs); }, sign() { return mk(this, Math.sign); }, round() { return mk(this, Math.round); },\n"
"    floor() { return mk(this, Math.floor); }, ceil() { return mk(this, Math.ceil); },\n"
"    fract() { return mk(this, x => x - Math.floor(x)); },\n"
"    mod(o) { return mk(this, (x, k) => { const y = c(o, k); return x - y * Math.floor(x / y); }); },\n"
"    step(e) { return mk(this, (x, k) => x < c(e, k) ? 0 : 1); },\n"
"    smoothStep(lo, hi) { return mk(this, (x, k) => { const a = c(lo, k), t = Math.min(Math.max((x - a) / (c(hi, k) - a), 0), 1); return t * t * (3 - 2 * t); }); } };\n"
"  for (const m in ext) Object.defineProperty(C.prototype, m, { value: ext[m], writable: true, configurable: true }); }\n"
"__vecExt(Vec2, ['x', 'y']); __vecExt(Vec3, ['x', 'y', 'z']); __vecExt(Vec4, ['x', 'y', 'z', 'w']);\n"
"const __AX = new Vec3(1, 0, 0), __AY = new Vec3(0, 1, 0), __AZ = new Vec3(0, 0, 1);\n"
"function __meq(a, b, n) { if (!b) return false; for (let i = 0; i < n; i++) if (!(Math.abs(a[i] - b[i]) <= 1e-6)) return false; return true; }\n"
"class Mat3 { constructor() { this.m = [1, 0, 0, 0, 1, 0, 0, 0, 1]; }\n"
"  static __of(a) { const r = new Mat3(); r.m = a; return r; }\n"
"  static identity() { return new Mat3(); }\n"
"  static fromTranslation(v) { const r = new Mat3(); r.m[6] = +v.x || 0; r.m[7] = +v.y || 0; return r; }\n"
"  static fromScale(v) { const r = new Mat3(), s = typeof v === 'number'; r.m[0] = s ? v : +v.x; r.m[4] = s ? v : +v.y; return r; }\n"
"  static fromRotation(a) { const r = a * __D2R, c = Math.cos(r), s = Math.sin(r); return Mat3.__of([c, s, 0, -s, c, 0, 0, 0, 1]); }\n"
"  static fromBasis(r, u) { return Mat3.__of([r.x, r.y, 0, u.x, u.y, 0, 0, 0, 1]); }\n"
"  static fromMat4(m) { const a = m.m; return Mat3.__of([a[0], a[1], a[2], a[4], a[5], a[6], a[8], a[9], a[10]]); }\n"
"  static compose(t, r, s) { return Mat3.fromTranslation(t).multiply(Mat3.fromRotation(+r || 0)).multiply(Mat3.fromScale(s === undefined ? 1 : s)); }\n"
"  translation(p) { if (p !== undefined) { this.m[6] = +p.x || 0; this.m[7] = +p.y || 0; } return new Vec2(this.m[6], this.m[7]); }\n"
"  angle() { return Math.atan2(this.m[1], this.m[0]) * __R2D; }\n"
"  add(o) { return Mat3.__of(this.m.map((v, i) => v + o.m[i])); }\n"
"  subtract(o) { return Mat3.__of(this.m.map((v, i) => v - o.m[i])); }\n"
"  multiply(v) { const a = this.m; if (typeof v === 'number') return Mat3.__of(a.map(x => x * v));\n"
"    if (!v.m) { const x = +v.x || 0, y = +v.y || 0, z = v.z === undefined ? 1 : +v.z;\n"
"      return new Vec3(a[0] * x + a[3] * y + a[6] * z, a[1] * x + a[4] * y + a[7] * z, a[2] * x + a[5] * y + a[8] * z); }\n"
"    const b = v.m, r = [0, 0, 0, 0, 0, 0, 0, 0, 0];\n"
"    for (let c = 0; c < 3; c++) for (let k = 0; k < 3; k++) r[c * 3 + k] = a[k] * b[c * 3] + a[3 + k] * b[c * 3 + 1] + a[6 + k] * b[c * 3 + 2];\n"
"    return Mat3.__of(r); }\n"
"  translate(v) { return this.multiply(Mat3.fromTranslation(v)); }\n"
"  rotate(a) { return this.multiply(Mat3.fromRotation(a)); }\n"
"  scale(v) { return this.multiply(Mat3.fromScale(v)); }\n"
"  transformPoint(v) { const a = this.m; return new Vec2(a[0] * v.x + a[3] * v.y + a[6], a[1] * v.x + a[4] * v.y + a[7]); }\n"
"  transformDirection(v) { const a = this.m; return new Vec2(a[0] * v.x + a[3] * v.y, a[1] * v.x + a[4] * v.y); }\n"
"  transpose() { const a = this.m; return Mat3.__of([a[0], a[3], a[6], a[1], a[4], a[7], a[2], a[5], a[8]]); }\n"
"  determinant() { const a = this.m; return a[0] * (a[4] * a[8] - a[7] * a[5]) - a[3] * (a[1] * a[8] - a[7] * a[2]) + a[6] * (a[1] * a[5] - a[4] * a[2]); }\n"
"  inverse() { const a = this.m, d = this.determinant(); if (!d) return new Mat3(); const i = 1 / d;\n"
"    return Mat3.__of([(a[4] * a[8] - a[5] * a[7]) * i, (a[2] * a[7] - a[1] * a[8]) * i, (a[1] * a[5] - a[2] * a[4]) * i,\n"
"      (a[5] * a[6] - a[3] * a[8]) * i, (a[0] * a[8] - a[2] * a[6]) * i, (a[2] * a[3] - a[0] * a[5]) * i,\n"
"      (a[3] * a[7] - a[4] * a[6]) * i, (a[1] * a[6] - a[0] * a[7]) * i, (a[0] * a[4] - a[1] * a[3]) * i]); }\n"
"  decompose() { const a = this.m; return { translation: this.translation(), rotation: this.angle(), scale: new Vec2(Math.hypot(a[0], a[1]), Math.hypot(a[3], a[4])) }; }\n"
"  copy() { return Mat3.__of(this.m.slice()); }\n"
"  equals(o) { return __meq(this.m, o && o.m, 9); }\n"
"  toString() { return this.m.join(' '); } }\n"
"class Mat4 { constructor() { this.m = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]; }\n"
"  static __of(a) { const r = new Mat4(); r.m = a; return r; }\n"
"  static identity() { return new Mat4(); }\n"
"  static fromTranslation(v) { const r = new Mat4(); r.m[12] = +v.x || 0; r.m[13] = +v.y || 0; r.m[14] = +v.z || 0; return r; }\n"
"  static fromScale(v) { const r = new Mat4(), s = typeof v === 'number'; r.m[0] = s ? v : +v.x; r.m[5] = s ? v : +v.y; r.m[10] = s ? v : (v.z === undefined ? 1 : +v.z); return r; }\n"
"  static fromRotation(angle, axis) { const a = angle * __D2R, c = Math.cos(a), s = Math.sin(a), t = 1 - c;\n"
"    const l = Math.hypot(axis.x, axis.y, axis.z) || 1, x = axis.x / l, y = axis.y / l, z = axis.z / l;\n"
"    return Mat4.__of([t * x * x + c, t * x * y + s * z, t * x * z - s * y, 0, t * x * y - s * z, t * y * y + c, t * y * z + s * x, 0,\n"
"      t * x * z + s * y, t * y * z - s * x, t * z * z + c, 0, 0, 0, 0, 1]); }\n"
"  static fromEuler(x, y, z) { if (x && typeof x === 'object') { z = x.z; y = x.y; x = x.x; }\n"
"    return Mat4.fromRotation(+z || 0, __AZ).multiply(Mat4.fromRotation(+y || 0, __AY)).multiply(Mat4.fromRotation(+x || 0, __AX)); }\n"
"  static fromBasis(r, u, f) { return Mat4.__of([r.x, r.y, r.z, 0, u.x, u.y, u.z, 0, f.x, f.y, f.z, 0, 0, 0, 0, 1]); }\n"
"  static lookAt(eye, center, up) { const f = center.subtract(eye).normalize(), s = f.cross(up || __AY).normalize(), u = s.cross(f);\n"
"    return Mat4.__of([s.x, u.x, -f.x, 0, s.y, u.y, -f.y, 0, s.z, u.z, -f.z, 0, -s.dot(eye), -u.dot(eye), f.dot(eye), 1]); }\n"
"  static compose(t, r, s) { return Mat4.fromTranslation(t).multiply(Mat4.fromEuler(r)).multiply(Mat4.fromScale(s === undefined ? 1 : s)); }\n"
"  translation(p) { if (p !== undefined) { this.m[12] = +p.x || 0; this.m[13] = +p.y || 0; this.m[14] = +p.z || 0; } return new Vec3(this.m[12], this.m[13], this.m[14]); }\n"
"  right() { return new Vec3(this.m[0], this.m[1], this.m[2]); }\n"
"  up() { return new Vec3(this.m[4], this.m[5], this.m[6]); }\n"
"  forward() { return new Vec3(this.m[8], this.m[9], this.m[10]); }\n"
"  add(o) { return Mat4.__of(this.m.map((v, i) => v + o.m[i])); }\n"
"  subtract(o) { return Mat4.__of(this.m.map((v, i) => v - o.m[i])); }\n"
"  multiply(v) { const a = this.m; if (typeof v === 'number') return Mat4.__of(a.map(x => x * v));\n"
"    if (!v.m) { const x = +v.x || 0, y = +v.y || 0, z = +v.z || 0, w = v.w === undefined ? 1 : +v.w;\n"
"      return new Vec4(a[0] * x + a[4] * y + a[8] * z + a[12] * w, a[1] * x + a[5] * y + a[9] * z + a[13] * w,\n"
"        a[2] * x + a[6] * y + a[10] * z + a[14] * w, a[3] * x + a[7] * y + a[11] * z + a[15] * w); }\n"
"    const b = v.m, r = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];\n"
"    for (let c = 0; c < 4; c++) for (let k = 0; k < 4; k++)\n"
"      r[c * 4 + k] = a[k] * b[c * 4] + a[4 + k] * b[c * 4 + 1] + a[8 + k] * b[c * 4 + 2] + a[12 + k] * b[c * 4 + 3];\n"
"    return Mat4.__of(r); }\n"
"  translate(v) { return this.multiply(Mat4.fromTranslation(v)); }\n"
"  rotate(angle, axis) { return this.multiply(Mat4.fromRotation(angle, axis)); }\n"
"  scale(v) { return this.multiply(Mat4.fromScale(v)); }\n"
"  transformPoint(v) { const r = this.multiply(new Vec4(v.x, v.y, v.z || 0, 1)); return new Vec3(r.x, r.y, r.z); }\n"
"  transformDirection(v) { const r = this.multiply(new Vec4(v.x, v.y, v.z || 0, 0)); return new Vec3(r.x, r.y, r.z); }\n"
"  transpose() { const a = this.m; return Mat4.__of([a[0], a[4], a[8], a[12], a[1], a[5], a[9], a[13], a[2], a[6], a[10], a[14], a[3], a[7], a[11], a[15]]); }\n"
"  __sub() { const a = this.m; return [a[0] * a[5] - a[1] * a[4], a[0] * a[6] - a[2] * a[4], a[0] * a[7] - a[3] * a[4], a[1] * a[6] - a[2] * a[5],\n"
"    a[1] * a[7] - a[3] * a[5], a[2] * a[7] - a[3] * a[6], a[8] * a[13] - a[9] * a[12], a[8] * a[14] - a[10] * a[12],\n"
"    a[8] * a[15] - a[11] * a[12], a[9] * a[14] - a[10] * a[13], a[9] * a[15] - a[11] * a[13], a[10] * a[15] - a[11] * a[14]]; }\n"
"  determinant() { const b = this.__sub(); return b[0] * b[11] - b[1] * b[10] + b[2] * b[9] + b[3] * b[8] - b[4] * b[7] + b[5] * b[6]; }\n"
"  inverse() { const a = this.m, b = this.__sub(); let d = b[0] * b[11] - b[1] * b[10] + b[2] * b[9] + b[3] * b[8] - b[4] * b[7] + b[5] * b[6];\n"
"    if (!d) return new Mat4(); d = 1 / d;\n"
"    return Mat4.__of([(a[5] * b[11] - a[6] * b[10] + a[7] * b[9]) * d, (a[2] * b[10] - a[1] * b[11] - a[3] * b[9]) * d,\n"
"      (a[13] * b[5] - a[14] * b[4] + a[15] * b[3]) * d, (a[10] * b[4] - a[9] * b[5] - a[11] * b[3]) * d,\n"
"      (a[6] * b[8] - a[4] * b[11] - a[7] * b[7]) * d, (a[0] * b[11] - a[2] * b[8] + a[3] * b[7]) * d,\n"
"      (a[14] * b[2] - a[12] * b[5] - a[15] * b[1]) * d, (a[8] * b[5] - a[10] * b[2] + a[11] * b[1]) * d,\n"
"      (a[4] * b[10] - a[5] * b[8] + a[7] * b[6]) * d, (a[1] * b[8] - a[0] * b[10] - a[3] * b[6]) * d,\n"
"      (a[12] * b[4] - a[13] * b[2] + a[15] * b[0]) * d, (a[9] * b[2] - a[8] * b[4] - a[11] * b[0]) * d,\n"
"      (a[5] * b[7] - a[4] * b[9] - a[6] * b[6]) * d, (a[0] * b[9] - a[1] * b[7] + a[2] * b[6]) * d,\n"
"      (a[13] * b[1] - a[12] * b[3] - a[14] * b[0]) * d, (a[8] * b[3] - a[9] * b[1] + a[10] * b[0]) * d]); }\n"
"  extractEuler() { const a = this.m, sx = Math.hypot(a[0], a[1], a[2]) || 1, sy = Math.hypot(a[4], a[5], a[6]) || 1, sz = Math.hypot(a[8], a[9], a[10]) || 1;\n"
"    const r20 = a[2] / sx, y = Math.asin(Math.min(Math.max(-r20, -1), 1));\n"
"    if (Math.abs(r20) < 0.999999) return new Vec3(Math.atan2(a[6] / sy, a[10] / sz) * __R2D, y * __R2D, Math.atan2(a[1] / sx, a[0] / sx) * __R2D);\n"
"    return new Vec3(Math.atan2(-a[9] / sz, a[5] / sy) * __R2D, y * __R2D, 0); }\n"
"  normalMatrix() { return Mat3.fromMat4(this).inverse().transpose(); }\n"
"  decompose() { const a = this.m; return { translation: this.translation(), rotation: this.extractEuler(),\n"
"    scale: new Vec3(Math.hypot(a[0], a[1], a[2]), Math.hypot(a[4], a[5], a[6]), Math.hypot(a[8], a[9], a[10])) }; }\n"
"  copy() { return Mat4.__of(this.m.slice()); }\n"
"  equals(o) { return __meq(this.m, o && o.m, 16); }\n"
"  toString() { return this.m.join(' '); } }\n"
"const WEMath = { PI: Math.PI, deg2rad: Math.PI / 180, rad2deg: 180 / Math.PI,\n"
"  mix(a, b, t) { return a + (b - a) * t; }, clamp(v, a, b) { return Math.min(Math.max(v, a), b); },\n"
"  smoothStep(e0, e1, x) { const t = Math.min(Math.max((x - e0) / (e1 - e0), 0), 1); return t * t * (3 - 2 * t); },\n"
"  step(e, x) { return x < e ? 0 : 1; }, fract(x) { return x - Math.floor(x); } };\n"
"const WEColor = {\n"
"  rgb2hsv(c) { const r = c.x, g = c.y, b = c.z, mx = Math.max(r, g, b), mn = Math.min(r, g, b), d = mx - mn;\n"
"    let h = 0; if (d) { if (mx === r) h = ((g - b) / d) % 6; else if (mx === g) h = (b - r) / d + 2; else h = (r - g) / d + 4; h /= 6; if (h < 0) h += 1; }\n"
"    return new Vec3(h, mx ? d / mx : 0, mx); },\n"
"  hsv2rgb(c) { const h = c.x * 6, s = c.y, v = c.z, i = Math.floor(h) % 6, f = h - Math.floor(h), p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);\n"
"    return [new Vec3(v, t, p), new Vec3(q, v, p), new Vec3(p, v, t), new Vec3(p, q, v), new Vec3(t, p, v), new Vec3(v, p, q)][(i + 6) % 6]; },\n"
"  normalizeColor(c) { return new Vec3(c.x / 255, c.y / 255, c.z / 255); },\n"
"  expandColor(c) { return new Vec3(c.x * 255, c.y * 255, c.z * 255); } };\n"
"const WEVector = { angleVector2(a) { return new Vec2(Math.cos(a * WEMath.deg2rad), Math.sin(a * WEMath.deg2rad)); },\n"
"  vectorAngle2(v) { return Math.atan2(v.y, v.x) * WEMath.rad2deg; } };\n"
"const __modules = { WEMath, WEColor, WEVector };\n"
"function createScriptProperties() { const p = {}; const b = {\n"
"  __put(o, v) { if (o && o.name !== undefined) p[o.name] = v; return b; },\n"
"  addSlider(o) { return b.__put(o, o.value !== undefined ? +o.value : (o.min || 0)); },\n"
"  addCheckbox(o) { return b.__put(o, !!o.value); },\n"
"  addCombo(o) { return b.__put(o, o.value !== undefined ? o.value : (o.options && o.options[0] ? o.options[0].value : undefined)); },\n"
"  addText(o) { return b.__put(o, o.value !== undefined ? '' + o.value : ''); },\n"
"  addTextInput(o) { return b.addText(o); },\n"
"  addColor(o) { return b.__put(o, typeof o.value === 'string' ? new Vec3(o.value) : (o.value || new Vec3(1, 1, 1))); },\n"
"  addFile(o) { return b.__put(o, o.value); }, addDirectory(o) { return b.__put(o, o.value); },\n"
"  addGroup() { return b; }, finish() { return p; } }; return b; }\n"
"const __vecprops = { origin: 3, scale: 3, angles: 3, color: 3 };\n"
/* particle instance overrides (host properties "inst.*"): colorn and the
 * control points are vectors, the rest multipliers */
"function __instvec(p) { return p.length > 6 && p[4] === '.' && p[5] === 'c' && p !== 'inst.count'; }\n"
"function __get(i, prop) { const v = __host.get(i, prop); if (Array.isArray(v)) return v.length === 2 ? new Vec2(v[0], v[1]) : new Vec3(v[0], v[1], v[2]); return v; }\n"
"const __strprops = { text: 1, font: 1, horizontalalign: 1, verticalalign: 1 }, __boolprops = { visible: 1, solid: 1, limitwidth: 1, limitrows: 1 };\n"
"function __set(i, prop, v) { if (v === undefined || v === null) return;\n"
"  if (__vecprops[prop] || __instvec(prop)) { if (typeof v === 'string') { if (!isFinite(__nums(v)[0])) return; v = new Vec3(v); }\n"
"    else if (typeof v === 'number') { if (!isFinite(v)) return; v = new Vec3(v, v, v); } else if (typeof v !== 'object') return;\n"
/* a component that is not a number keeps the layer's value. Scripts
 * return such vectors: 2901582518 scales the parent of its date widget
 * with `value.x = scriptProperties.posX` where no such property exists
 * (undefined). That became scale 0 0 0 here and the widget was gone;
 * Wallpaper Engine leaves the scale alone. */
"    let x = +v.x, y = +v.y, z = v.z === undefined ? NaN : +v.z;\n"
"    if (!(isFinite(x) && isFinite(y) && isFinite(z))) { const c = __host.get(i, prop) || [0, 0, 0];\n"
"      if (!isFinite(x)) x = +c[0] || 0; if (!isFinite(y)) y = +c[1] || 0; if (!isFinite(z)) z = +c[2] || 0; }\n"
"    __host.set(i, prop, [x, y, z]); }\n"
"  else if (__boolprops[prop]) __host.set(i, prop, !!v);\n"
"  else if (__strprops[prop]) __host.set(i, prop, '' + v);\n"
"  else __host.set(i, prop, +v); }\n"
"const __anim = { play() {}, stop() {}, pause() {}, isPlaying() { return false; }, setFrame() {}, getFrame() { return 0; }, frameCount: 0, duration: 0, rate: 1 };\n"
/* effect access: values cross to C as arrays (__fxarr) and come back by
 * size (__fxval: 1 = Number, 2..4 = Vec2..Vec4) */
"function __fxarr(v) { if (typeof v === 'boolean') return [v ? 1 : 0]; if (typeof v === 'number') return [v];\n"
"  if (typeof v === 'string') return v.trim().split(/\\s+/).map(Number);\n"
"  if (v && typeof v === 'object') { const a = [+v.x || 0]; if (v.y !== undefined) a.push(+v.y || 0); if (v.z !== undefined) a.push(+v.z || 0); if (v.w !== undefined) a.push(+v.w || 0); return a; }\n"
"  return null; }\n"
/* values scripts write: a component that is not a number keeps what the
 * constant holds (as for layer vectors in __set), it does not become 0 */
"function __fxarrS(v) { if (!v || typeof v !== 'object') return __fxarr(v);\n"
"  const a = [+v.x]; if ('y' in v) a.push(+v.y); if ('z' in v) a.push(+v.z); if ('w' in v) a.push(+v.w); return a; }\n"
"function __fxput(i, e, p, k, a, fb) { for (let n = 0; n < a.length; n++) if (!isFinite(a[n])) { let c = __host.fxget(i, e, p, k); if (!Array.isArray(c)) c = fb;\n"
"    for (let m = n; m < a.length; m++) if (!isFinite(a[m])) a[m] = Array.isArray(c) && isFinite(c[m]) ? c[m] : 0; break; }\n"
"  __host.fxset(i, e, p, k, a); }\n"
"function __fxval(a) { if (!Array.isArray(a)) return a; if (a.length === 1) return a[0]; if (a.length === 2) return new Vec2(a[0], a[1]);\n"
"  if (a.length === 3) return new Vec3(a[0], a[1], a[2]); const v = new Vec4(a[0], a[1], a[2], a[3]); return v; }\n"
"function __material(i, e, p) { const t = { __i: i, __e: e, __p: p, getAnimation(n) { return __animFind(i, e, p, n); } };\n"
"  return new Proxy(t, { get(o, k) { if (k in o || typeof k !== 'string') return o[k]; return __fxval(__host.fxget(i, e, p, k)); },\n"
"    set(o, k, v) { if (typeof k !== 'string' || k.startsWith('__')) { o[k] = v; return true; } const a = __fxarrS(v); if (a) __fxput(i, e, p, k, a); return true; } }); }\n"
"class Effect { constructor(i, e) { this.__i = i; this.__e = e; }\n"
"  get visible() { const v = __host.fxget(this.__i, this.__e, -1, 'visible'); return v === undefined ? false : !!v; }\n"
"  set visible(v) { __host.fxset(this.__i, this.__e, -1, 'visible', !!v); }\n"
"  get name() { return __host.fxname(this.__i, this.__e) || ''; }\n"
"  getMaterial(k) { return __material(this.__i, this.__e, +k || 0); } getMaterialCount() { return 1; }\n"
"  setMaterialProperty(k, v) { const a = __fxarrS(v); if (a) __fxput(this.__i, this.__e, -1, '' + k, a); }\n"
"  executeMaterialFunction() {} getAnimation(n) { return __animFind(this.__i, this.__e, -2, n); } }\n"
"const __effects = {};\n"
"function __effect(i, e) { if (e < 0) return null; const k = i + ':' + e; return __effects[k] || (__effects[k] = new Effect(i, e)); }\n"
/* ILayer (lib.sceneScript.d.ts). What the renderer has is read from and
 * written to the scene; members of features it does not have (bones,
 * animation layers, attachments, particle instance overrides, created
 * layers) exist and do nothing, so a script that touches one keeps running
 * instead of throwing "not a function". Angles: the scene keeps radians,
 * scripts see degrees (ILayer.angles), like every other angle of the API. */
"function __lget(i, prop) { const v = __get(i, prop); return prop === 'angles' && v ? new Vec3(v.x * __R2D, v.y * __R2D, v.z * __R2D) : v; }\n"
"function __lset(i, prop, v) { if (prop === 'angles' && v !== undefined && v !== null) { if (typeof v !== 'object') v = new Vec3(v);\n"
"    v = { x: +v.x * __D2R, y: +v.y * __D2R, z: v.z === undefined ? undefined : +v.z * __D2R }; } __set(i, prop, v); }\n"
"function __parentOf(i) { const p = __host.get(i, 'parent'); return typeof p === 'number' && p >= 0 ? p : -1; }\n"
"function __local(i) { const o = __get(i, 'origin'), s = __get(i, 'scale'), a = __get(i, 'angles');\n"
"  return Mat4.fromTranslation(o || __AZ.multiply(0)).multiply(Mat4.fromRotation((a ? a.z : 0) * __R2D, __AZ)).multiply(Mat4.fromScale(s || 1)); }\n"
"function __world(i, hops) { const m = __local(i), p = __parentOf(i); return p >= 0 && hops < 64 ? __world(p, hops + 1).multiply(m) : m; }\n"
"function __lidx(l) { return l === undefined || l === null ? -1 : typeof l === 'number' ? l : typeof l === 'string' ? __host.find(l) : (typeof l.__i === 'number' ? l.__i : -1); }\n"
"const __texAnim = { play() {}, stop() {}, pause() {}, isPlaying() { return false; }, setFrame() {}, getFrame() { return 0; }, join() {},\n"
"  getCurrentTime() { return 0; }, setCurrentTime() {}, addEndedCallback() {}, frameCount: 0, duration: 0, rate: 1, loop: true };\n"
"function __animLayer() { return { fps: 30, frameCount: 0, duration: 0, name: '', rate: 1, blend: 1, visible: true, play() {}, pause() {}, stop() {},\n"
"  isPlaying() { return false; }, getFrame() { return 0; }, setFrame() {}, addEndedCallback() {}, getAnimation() { return __anim; } }; }\n"
/* IParticleSystemInstance: the object's instance overrides, live. On a
 * layer that is no particle system the values are kept and do nothing. */
"function __instance(i) { const o = {}, keep = {};\n"
"  const def = (k, vec) => Object.defineProperty(o, k, { enumerable: true,\n"
"    get() { const v = __get(i, 'inst.' + k); return v !== undefined ? v : k in keep ? keep[k] : vec ? new Vec3(k === 'colorn' ? 1 : 0) : 1; },\n"
"    set(v) { if (vec && typeof v === 'number') v = new Vec3(v); keep[k] = v; __set(i, 'inst.' + k, v); } });\n"
"  for (const k of ['alpha', 'size', 'count', 'speed', 'lifetime', 'rate']) def(k, false);\n"
"  def('colorn', true); for (let k = 0; k < 8; k++) def('controlpoint' + k, true); return o; }\n"
"class Layer { constructor(i) { this.__i = i; this.parallaxDepth = new Vec2(1, 1); }\n"
"  get name() { return __host.name(this.__i); }\n"
"  get angles() { return __lget(this.__i, 'angles'); } set angles(v) { __lset(this.__i, 'angles', v); }\n"
"  get size() { return __get(this.__i, 'size') || new Vec2(0, 0); } set size(v) {}\n"
"  get backgroundcolor() { return this.__bg || (this.__bg = new Vec3(0, 0, 0)); } set backgroundcolor(v) { this.__bg = new Vec3(v); }\n"
"  get instance() { return this.__inst || (this.__inst = __instance(this.__i)); }\n"
"  getAnimation(n) { return __animFind(this.__i, -1, -1, n); } getTextureAnimation() { return __texAnim; } getVideoTexture() { return __texAnim; }\n"
"  getEffect(n) { return __effect(this.__i, typeof n === 'number' ? n : __host.fxfind(this.__i, '' + n)); }\n"
"  getEffectCount() { return __host.fxcount(this.__i); }\n"
"  getParent() { const p = __parentOf(this.__i); return p >= 0 ? __layer(p) : undefined; }\n"
"  getChildren() { const a = [], n = this.__i < 0 ? 0 : __host.count(); for (let k = 0; k < n; k++) if (__parentOf(k) === this.__i) a.push(__layer(k)); return a; }\n"
/* setParent(parent, adjustTransforms) / (parent, attachment, adjustTransforms) */
"  setParent(p, a, b) { const adjust = typeof a === 'boolean' ? a : !!b, pi = __lidx(p); if (pi === this.__i || this.__i < 0) return;\n"
"    const w = adjust ? __world(this.__i, 0) : null; __host.set(this.__i, 'parent', pi);\n"
"    if (w) { const d = (pi >= 0 ? __world(pi, 0).inverse().multiply(w) : w).decompose();\n"
"      this.origin = d.translation; this.angles = new Vec3(0, 0, d.rotation.z); this.scale = new Vec3(d.scale.x, d.scale.y, 1); } }\n"
"  getTransformMatrix() { return __world(this.__i, 0); }\n"
"  rotateObjectSpace(a) { const v = this.angles; if (v && a) this.angles = new Vec3(v.x + (+a.x || 0), v.y + (+a.y || 0), v.z + (+a.z || 0)); }\n"
"  lookAt() {} lookAtYaw() {}\n"
"  getAttachmentIndex() { return -1; } getAttachmentMatrix() { return this.getTransformMatrix(); }\n"
"  getAttachmentOrigin() { return this.origin || new Vec3(0, 0, 0); } getAttachmentAngles() { return this.angles || new Vec3(0, 0, 0); }\n"
"  transformAttachmentToTexture() { return new Mat3(); }\n"
"  getAnimationLayerCount() { return 0; } getAnimationLayer() { return __animLayer(); } createAnimationLayer() { return __animLayer(); }\n"
"  playSingleAnimation() { return __animLayer(); } destroyAnimationLayer() { return false; }\n"
"  getBoneCount() { return 0; } getBoneIndex() { return -1; } getBoneParentIndex() { return -1; }\n"
"  getBoneTransform() { return new Mat4(); } setBoneTransform() {} getLocalBoneTransform() { return new Mat4(); } setLocalBoneTransform() {}\n"
"  getLocalBoneAngles() { return new Vec3(0, 0, 0); } setLocalBoneAngles() {} getLocalBoneOrigin() { return new Vec3(0, 0, 0); } setLocalBoneOrigin() {}\n"
"  applyBonePhysicsImpulse() {} resetBonePhysicsSimulation() {}\n"
"  getBlendShapeIndex() { return -1; } getBlendShapeWeight() { return 0; } setBlendShapeWeight() {}\n"
/* IParticleSystem / ISoundLayer: play() emits again, pause() stops emitting
 * (live particles keep simulating), stop() also clears them; a no-op on other
 * layers. 2901582518's cursor trails pause() in init and play() only while
 * the button is held: without these the init threw and the trail emitted
 * forever at the screen centre. */
"  play() { __host.set(this.__i, 'playing', 1); } pause() { __host.set(this.__i, 'playing', 0); }\n"
"  stop() { __host.set(this.__i, 'playing', -1); } isPlaying() { return !!__host.get(this.__i, 'playing'); }\n"
"  emitParticles() {} }\n"
"for (const p of ['origin', 'scale', 'color', 'alpha', 'visible', 'text', 'pointsize', 'maxwidth', 'padding', 'font', 'horizontalalign',\n"
"  'verticalalign', 'solid', 'limitwidth', 'limitrows', 'maxrows'])\n"
"  Object.defineProperty(Layer.prototype, p, { get() { return __get(this.__i, p); }, set(v) { __set(this.__i, p, v); }, configurable: true });\n"
/* what the renderer does not have: plain values a script can read and write */
"Object.assign(Layer.prototype, { perspective: false, alignment: 'center', opaquebackground: false, anchor: 'none', fov: 50, zoom: 1,\n"
"  volume: 1, rootmotion: false });\n"
"const __layers = [];\n"
"function __layer(i) { if (i < 0) return null; return __layers[i] || (__layers[i] = new Layer(i)); }\n"
/* thisScene.createLayer(): layers cannot be added to a loaded scene. The
 * script gets an object with the layer interface that is never drawn. */
"let __ghostLogged = false;\n"
"function __ghost(cfg) { const g = Object.create(Layer.prototype); g.__i = -1;\n"
"  const d = { name: '', origin: new Vec3(0, 0, 0), scale: new Vec3(1, 1, 1), angles: new Vec3(0, 0, 0), color: new Vec3(1, 1, 1), alpha: 1,\n"
"    visible: true, text: '', pointsize: 0, size: new Vec2(0, 0), parallaxDepth: new Vec2(1, 1) };\n"
"  if (cfg && typeof cfg === 'object') for (const k in cfg) d[k] = cfg[k];\n"
"  for (const k in d) Object.defineProperty(g, k, { value: d[k], writable: true, enumerable: true, configurable: true });\n"
"  if (!__ghostLogged) { __ghostLogged = true; __host.log('thisScene.createLayer(): not supported, the new layer is not drawn'); }\n"
"  return g; }\n"
"const thisScene = { getLayer(n) { return __layer(typeof n === 'number' ? n : __host.find('' + n)); },\n"
"  getLayerByID(id) { const n = __host.count(); id = +id; for (let i = 0; i < n; i++) if (__host.get(i, 'id') === id) return __layer(i); return null; },\n"
"  getLayerIndex(l) { return __lidx(l); }, getLayerCount() { return __host.count(); },\n"
"  enumerateLayers() { const a = [], n = __host.count(); for (let i = 0; i < n; i++) a.push(__layer(i)); return a; },\n"
"  sortLayer() { return false; }, createLayer(cfg) { return __ghost(cfg); }, destroyLayer() { return false; },\n"
"  getInitialLayerConfig(l) { const i = __lidx(l); if (i < 0 || !__host.name(i) && __host.name(i) !== '') return {};\n"
"    const L = __layer(i), c = { name: L.name, origin: L.origin, scale: L.scale, angles: L.angles, visible: L.visible, alpha: L.alpha, color: L.color };\n"
"    if (L.text !== undefined) c.text = L.text; return c; },\n"
"  getCameraTransforms() { return { eye: new Vec3(0, 0, 1), center: new Vec3(0, 0, 0), up: new Vec3(0, 1, 0), zoom: 1 }; }, setCameraTransforms() {},\n"
"  getAnimation(n) { const t = n !== undefined && n !== null ? __tracks.find(u => !u.leader && u.name === '' + n) : __tracks.find(u => !u.leader);\n"
"    return t ? (t.api || (t.api = new __Anim(t))) : __anim; },\n"
"  createModelData() { return { applyData() {}, replaceData() {}, getAnimation() { return __anim; },\n"
"    POSITION: 'position', NORMAL: 'normal', TANGENT_SIGNED: 'tangentSigned', UV: 'uv', COLOR: 'color' }; }, destroyModelData() {},\n"
"  bloom: false, bloomstrength: 0, bloomthreshold: 0, clearenabled: true, clearcolor: new Vec3(0, 0, 0), ambientcolor: new Vec3(0, 0, 0),\n"
"  skylightcolor: new Vec3(0, 0, 0), fov: 50, nearz: 0.01, farz: 10000, camerafade: false, camerashake: false, camerashakespeed: 0,\n"
"  camerashakeamplitude: 0, camerashakeroughness: 0, cameraparallax: false, cameraparallaxamount: 0, cameraparallaxdelay: 0,\n"
"  cameraparallaxmouseinfluence: 0 };\n"
"thisScene.clearColor = thisScene.clearcolor;\n"
"const __timers = []; let __timerId = 1;\n"
/* what runs right now (the script or timer to stop when the time budget
 * runs out: that error cannot be caught, so try / finally never see it) */
"let __cur = null, __curTimer = null, __frame = 0;\n"
"const __audioBufs = [];\n"
"const engine = { frametime: 0, runtime: 0, timeOfDay: 0, canvasSize: new Vec2(__canvas[0], __canvas[1]),\n"
"  screenResolution: new Vec2(__canvas[0], __canvas[1]), userProperties: {},\n"
"  AUDIO_RESOLUTION_16: 16, AUDIO_RESOLUTION_32: 32, AUDIO_RESOLUTION_64: 64,\n"
"  registerAudioBuffers(res) { res = res === 16 || res === 32 ? res : 64;\n"
"    const b = { left: new Float32Array(res), right: new Float32Array(res), average: new Float32Array(res), resolution: res };\n"
"    __audioBufs.push(b); __host.wantAudio(); return b; },\n"
/* WE returns a callback that cancels the timer (lib.sceneScript.d.ts) */
"  setTimeout(fn, ms) { const id = __timerId++; __timers.push({ id, fn, due: engine.runtime + (+ms || 0) / 1000, every: 0, L: thisLayer, T: thisObject }); return __cancel(id); },\n"
"  setInterval(fn, ms) { const id = __timerId++; const e = Math.max((+ms || 0) / 1000, 0.001); __timers.push({ id, fn, due: engine.runtime + e, every: e, L: thisLayer, T: thisObject }); return __cancel(id); },\n"
"  clearTimeout(h) { const id = typeof h === 'function' ? h.__id : h; const k = __timers.findIndex(t => t.id === id); if (k >= 0) __timers.splice(k, 1); },\n"
"  isDesktopDevice() { return true; }, isMobileDevice() { return false; }, isWallpaper() { return true; },\n"
"  isScreensaver() { return false; }, isRunningInEditor() { return false; }, openUserShortcut() { return false; },\n"
"  isPortrait() { return engine.screenResolution.y > engine.screenResolution.x; }, isLandscape() { return !engine.isPortrait(); },\n"
"  registerAsset(file) { return { file: '' + file }; } };\n"
"engine.clearInterval = engine.clearTimeout;\n"
"function __cancel(id) { const c = function () { engine.clearTimeout(id); return true; }; c.__id = id; return c; }\n"
"const input = { cursorWorldPosition: new Vec3(0, 0, 0), cursorScreenPosition: new Vec2(0, 0), cursorLeftDown: false };\n"
"const shared = {};\n"
"const MediaPlaybackEvent = { PLAYBACK_STOPPED: 0, PLAYBACK_PLAYING: 1, PLAYBACK_PAUSED: 2 };\n"
/* legacy RegExp.$1..$9 (used by a date formatter; QuickJS lacks them) */
"(function () { const ex = RegExp.prototype.exec; RegExp.prototype.exec = function (s) {\n"
"  const m = ex.call(this, s); if (m) for (let i = 1; i < 10; i++) RegExp['$' + i] = m[i] === undefined ? '' : m[i];\n"
"  return m; }; })();\n"
/* localStorage lives as long as the scene (nothing is written to disk) */
"const __store = { screen: Object.create(null), global: Object.create(null) };\n"
"function __sto(l) { return l === 'global' ? __store.global : __store.screen; }\n"
"const localStorage = { LOCATION_GLOBAL: 'global', LOCATION_SCREEN: 'screen', get(k, l) { return __sto(l)[k]; }, set(k, v, l) { __sto(l)[k] = v; },\n"
"  delete(k, l) { const s = __sto(l), had = k in s; delete s[k]; return had; }, clear(l) { const s = __sto(l); for (const k in s) delete s[k]; } };\n"
"function __logstr(a) { let r = ''; for (let i = 0; i < a.length; i++) { let s; try { s = typeof a[i] === 'object' && a[i] !== null && a[i].toString === Object.prototype.toString ? JSON.stringify(a[i]) : String(a[i]); } catch (e) { s = '?'; } r += (i ? ' ' : '') + s; } return r; }\n"
"const console = { log(...a) { __host.log(__logstr(a)); }, error(...a) { __host.log('error: ' + __logstr(a)); } };\n"
"console.info = console.debug = console.log; console.warn = console.error;\n"
"const renderContext = {};\n"
/* new Date(string) / Date.parse: QuickJS reads ISO 8601 and its own
 * toString format only. V8 also takes the legacy forms scripts are full of
 * ("12/25/2026", "December 25, 2026 08:00:00", "2026/12/25 08:00", the
 * result of toLocaleString('en-US', { timeZone })): those are read here
 * when the engine's own parser gives up. Without a zone = local time. */
"const __ND = Date, __MON = { jan: 0, feb: 1, mar: 2, apr: 3, may: 4, jun: 5, jul: 6, aug: 7, sep: 8, oct: 9, nov: 10, dec: 11 };\n"
"function __legacyDate(s) { s = s.trim().replace(/,/g, ' ').replace(/\\s+/g, ' '); let h = 0, mi = 0, sec = 0, ms = 0, off = null;\n"
"  const m = /(\\d{1,2}):(\\d{2})(?::(\\d{2})(?:\\.(\\d{1,3}))?)? ?([ap]\\.?m\\.?)?/i.exec(s);\n"
"  if (m) { h = +m[1]; mi = +m[2]; sec = +m[3] || 0; ms = m[4] ? +(m[4] + '00').slice(0, 3) : 0;\n"
"    if (m[5]) { const pm = /^p/i.test(m[5]); if (h > 12) return NaN; if (h === 12) h = pm ? 12 : 0; else if (pm) h += 12; }\n"
"    const z = /^(z|utc|gmt)? ?(?:([+-])(\\d\\d):?(\\d\\d)?)?(?: ?\\(.*\\))?$/i.exec(s.slice(m.index + m[0].length).trim());\n"
"    if (!z) return NaN; if (z[1] || z[2]) off = z[2] ? (z[2] === '-' ? -1 : 1) * (+z[3] * 60 + (+z[4] || 0)) : 0;\n"
"    s = s.slice(0, m.index); }\n"
"  const tk = s.trim().split(/[ \\/.\\-]+/), nums = []; let mon = -1;\n"
"  for (let i = 0; i < tk.length; i++) { const t = tk[i]; if (!t) continue; if (/^\\d+$/.test(t)) nums.push(t);\n"
"    else if (/^[A-Za-z]+$/.test(t)) { const k = __MON[t.slice(0, 3).toLowerCase()]; if (k !== undefined && mon < 0) mon = k; } else return NaN; }\n"
"  let Y, M, D;\n"
"  if (mon >= 0) { M = mon; if (nums.length === 2) { const yf = nums[0].length > 2 || +nums[0] > 31; Y = +nums[yf ? 0 : 1]; D = +nums[yf ? 1 : 0]; }\n"
"    else if (nums.length === 1 && nums[0].length === 4) { Y = +nums[0]; D = 1; } else return NaN; }\n"
"  else if (nums.length === 3) { if (nums[0].length === 4) { Y = +nums[0]; M = +nums[1] - 1; D = +nums[2]; } else { M = +nums[0] - 1; D = +nums[1]; Y = +nums[2]; if (nums[2].length <= 2) Y += Y < 50 ? 2000 : 1900; } }\n"
"  else return NaN;\n"
"  if (!(M >= 0 && M < 12 && D >= 1 && D <= 31 && h <= 24 && mi < 60 && sec < 60)) return NaN;\n"
"  return off === null ? new __ND(Y, M, D, h, mi, sec, ms).getTime() : __ND.UTC(Y, M, D, h, mi, sec, ms) - off * 60000; }\n"
"function __dparse(s) { const t = __ND.parse(s); return isNaN(t) && typeof s === 'string' ? __legacyDate(s) : t; }\n"
"const __Date = function Date(...a) { if (!new.target) return __ND();\n"
"  return Reflect.construct(__ND, a.length === 1 && typeof a[0] === 'string' ? [__dparse(a[0])] : a, new.target === __Date ? __ND : new.target); };\n"
"__Date.prototype = __ND.prototype; __Date.now = __ND.now; __Date.UTC = __ND.UTC; __Date.parse = s => __dparse('' + s);\n"
"Object.defineProperty(__ND.prototype, 'constructor', { value: __Date, writable: true, configurable: true });\n"
"globalThis.Date = __Date;\n"
/* ---- dates and numbers as text. Wallpaper Engine runs scripts on V8 with
 * ICU; QuickJS has no ECMA-402, its toLocale*String ignore locale and
 * options (one fixed US format) and Intl does not exist. This is the part
 * clock and date layers use: toLocaleDateString / toLocaleTimeString /
 * toLocaleString with the usual options (weekday, year, month, day, hour,
 * minute, second, hour12 / hourCycle, dateStyle / timeStyle, timeZone),
 * Number.prototype.toLocaleString and Intl.DateTimeFormat / NumberFormat on
 * top of them. Month and weekday names come from the system's locale data
 * when that locale is installed (__host.locale), else English (CJK: built
 * in); number and date patterns follow CLDR for the languages in __LP.
 * o = field order, s = separator of numeric dates, z = zero-padded numeric
 * dates, h = 12-hour clock, h1 = hour not padded, t = time separator,
 * j = between date and time, g / c = group and decimal separators,
 * g2 = no grouping below 10000, wc = comma after the weekday, dot = "10.",
 * de = "10 de noviembre de 2026", we = weekday last, k = CJK suffixes. */
"const __LP = { und: { o: 'DMY', s: '/' },\n"
"  en: { o: 'MDY', s: '/', h: 1, wc: 1 }, 'en-GB': { o: 'DMY', s: '/', z: 1 }, 'en-IE': { o: 'DMY', s: '/', z: 1 }, 'en-ZA': { o: 'YMD', s: '/', z: 1 },\n"
"  'en-AU': { o: 'DMY', s: '/', z: 1, h: 1, lp: 1 }, 'en-NZ': { o: 'DMY', s: '/', z: 1, h: 1, lp: 1 }, 'en-IN': { o: 'DMY', s: '/', h: 1, lp: 1 },\n"
"  'en-CA': { o: 'YMD', s: '-', z: 1, h: 1, to: 'MDY', wc: 1, lp: 1 },\n"
"  de: { o: 'DMY', s: '.', g: '.', c: ',', wc: 1, dot: 1, ps: 1 }, fr: { o: 'DMY', s: '/', z: 1, g: ' ', c: ',', j: ' ', ps: 1 },\n"
"  es: { o: 'DMY', s: '/', g: '.', c: ',', g2: 1, h1: 1, wc: 1, de: 1, ps: 1 }, it: { o: 'DMY', s: '/', g: '.', c: ',' },\n"
"  pt: { o: 'DMY', s: '/', z: 1, g: '.', c: ',', wc: 1, de: 1 }, nl: { o: 'DMY', s: '-', g: '.', c: ',' },\n"
"  ru: { o: 'DMY', s: '.', z: 1, g: ' ', c: ',', wc: 1, ps: 1 }, uk: { o: 'DMY', s: '.', z: 1, g: ' ', c: ',', wc: 1 },\n"
"  pl: { o: 'DMY', s: '.', z: 1, g: ' ', c: ',', g2: 1, wc: 1 }, tr: { o: 'DMY', s: '.', z: 1, g: '.', c: ',', j: ' ', we: 1 },\n"
"  cs: { o: 'DMY', s: '. ', g: ' ', c: ',', h1: 1, dot: 1, j: ' ' }, sv: { o: 'YMD', s: '-', z: 1, g: ' ', c: ',', to: 'DMY', j: ' ', ps: 1 },\n"
"  da: { o: 'DMY', s: '.', g: '.', c: ',', t: '.', dot: 1, j: ' ', ps: 1 }, nb: { o: 'DMY', s: '.', g: ' ', c: ',', dot: 1, ps: 1 },\n"
"  fi: { o: 'DMY', s: '.', g: ' ', c: ',', t: '.', h1: 1, dot: 1, j: ' ', ps: 1 }, hu: { o: 'YMD', s: '. ', z: 1, g: ' ', c: ',', h1: 1 },\n"
"  ja: { o: 'YMD', s: '/', h1: 1, j: ' ', k: ['年', '月', '日', ''] }, zh: { o: 'YMD', s: '/', j: ' ', k: ['年', '月', '日', ''] },\n"
"  ko: { o: 'YMD', s: '. ', h: 1, j: ' ', k: ['년 ', '월 ', '일', ' '] }, id: { o: 'DMY', s: '/', g: '.', c: ',', t: '.' },\n"
"  vi: { o: 'DMY', s: '/', g: '.', c: ',' }, th: { o: 'DMY', s: '/' } };\n"
"__LP.no = __LP.nb;\n"
"const __DR = { en: 'US', de: 'DE', fr: 'FR', es: 'ES', it: 'IT', pt: 'BR', ru: 'RU', ja: 'JP', zh: 'CN', ko: 'KR', tr: 'TR', pl: 'PL', nl: 'NL',\n"
"  sv: 'SE', da: 'DK', nb: 'NO', no: 'NO', fi: 'FI', cs: 'CZ', uk: 'UA', hu: 'HU', el: 'GR', id: 'ID', vi: 'VN', th: 'TH', he: 'IL', hi: 'IN', ar: 'SA' };\n"
"const __EN = { M: ['January', 'February', 'March', 'April', 'May', 'June', 'July', 'August', 'September', 'October', 'November', 'December'],\n"
"  m: ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'],\n"
"  D: ['Sunday', 'Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday'], d: ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'], am: 'AM', pm: 'PM' };\n"
"function __cjk(days, pre, suf, am, pm, sh) { const M = []; for (let i = 1; i <= 12; i++) M.push(i + '月');\n"
"  const D = days.split('').map(c => pre + c + suf); return { M, m: M, D, d: days.split('').map(c => sh + c), am, pm }; }\n"
"const __CJK = { ja: __cjk('日月火水木金土', '', '曜日', '午前', '午後', ''),\n"
"  zh: __cjk('日一二三四五六', '星期', '', '上午', '下午', '周'),\n"
"  ko: __cjk('일월화수목금토', '', '요일', '오전', '오후', '') };\n"
"__CJK.ko.M = __CJK.ko.m = __CJK.ko.M.map((s, i) => (i + 1) + '월');\n"
"const __locs = Object.create(null);\n"
"function __locTag(l) { if (Array.isArray(l)) l = l[0]; if (l && typeof l === 'object') l = '' + l; return typeof l === 'string' && l && l !== 'default' ? l : __deflocale; }\n"
"function __loc(tag) { let L = __locs[tag]; if (L) return L;\n"
"  const p = ('' + tag).replace(/_/g, '-').split('-'), lang = (p[0] || 'en').toLowerCase().replace(/[^a-z]/g, '').slice(0, 3) || 'en'; let reg = '';\n"
"  for (let k = 1; k < p.length; k++) if (/^[A-Za-z]{2}$/.test(p[k])) { reg = p[k].toUpperCase(); break; }\n"
"  const P = __LP[lang + '-' + reg] || __LP[lang] || __LP.und, a = __host.locale(lang, reg || __DR[lang] || lang.toUpperCase()), N = __CJK[lang] || __EN;\n"
"  L = a && a.length >= 40 ? { M: a.slice(0, 12), m: a.slice(12, 24), D: a.slice(24, 31), d: a.slice(31, 38), am: a[38] || N.am, pm: a[39] || N.pm } : Object.assign({}, N);\n"
"  L.P = P; L.lang = lang; L.g = P.g || ','; L.c = P.c || '.'; __locs[tag] = L; return L; }\n"
"const __tzc = Object.create(null);\n"
"function __tzoff(tz, ms) { const q = Math.floor(ms / 900000); let e = __tzc[tz]; if (!e || e.q !== q) { e = __tzc[tz] = { q, off: __host.tzoff(tz, Math.floor(ms / 1000)) }; } return e.off; }\n"
"function __p2(n) { return (n < 10 ? '0' : '') + n; }\n"
"function __first(s) { return Array.from('' + s)[0] || ''; }\n"
/* ToDateTimeOptions (ECMA-402): req = what the caller needs ('date',
 * 'time', 'any'), def = what is added when the options name none of it */
"function __dtParts(d, tag, o, req, def) { const L = __loc(tag), P = L.P, out = []; o = o || {};\n"
"  const put = (type, value) => { out.push({ type, value: '' + value }); }, lit = v => { if (v) out.push({ type: 'literal', value: v }); };\n"
"  let ms = d && typeof d.getTime === 'function' ? d.getTime() : NaN; if (isNaN(ms)) { lit('Invalid Date'); return out; }\n"
"  let wd = o.weekday, y = o.year, mo = o.month, da = o.day, h = o.hour, mi = o.minute, s = o.second; const ds = o.dateStyle, ts = o.timeStyle;\n"
"  if (ds || ts) { wd = y = mo = da = h = mi = s = undefined;\n"
"    if (ds === 'full') { wd = 'long'; mo = 'long'; da = y = 'numeric'; } else if (ds === 'long') { mo = 'long'; da = y = 'numeric'; }\n"
"    else if (ds === 'medium') { mo = 'short'; da = y = 'numeric'; } else if (ds) { mo = da = 'numeric'; y = '2-digit'; }\n"
"    if (ts) { h = 'numeric'; mi = '2-digit'; if (ts !== 'short') s = '2-digit'; } }\n"
"  else { const hasD = wd || y || mo || da, hasT = h || mi || s;\n"
"    if (!((req !== 'time' && hasD) || (req !== 'date' && hasT))) { if (def !== 'time') y = mo = da = 'numeric'; if (def !== 'date') h = mi = s = 'numeric'; } }\n"
"  let utc = false, zone = ''; const tz = o.timeZone;\n"
"  if (tz) { if (/^(utc|gmt|etc\\/utc|etc\\/gmt|z)$/i.test(tz)) { utc = true; zone = 'UTC'; }\n"
"    else { const off = __tzoff('' + tz, ms); if (typeof off === 'number') { ms += off * 1000; utc = true;\n"
"      const m = Math.abs(off) / 60; zone = 'GMT' + (off < 0 ? '-' : '+') + Math.floor(m / 60) + (m % 60 ? ':' + __p2(m % 60) : ''); } } }\n"
"  const t = utc ? new Date(ms) : d, Y = utc ? t.getUTCFullYear() : t.getFullYear(), Mo = utc ? t.getUTCMonth() : t.getMonth(), D = utc ? t.getUTCDate() : t.getDate(),\n"
"    W = utc ? t.getUTCDay() : t.getDay(), H = utc ? t.getUTCHours() : t.getHours(), Mi = utc ? t.getUTCMinutes() : t.getMinutes(), S = utc ? t.getUTCSeconds() : t.getSeconds();\n"
"  const txt = mo === 'long' || mo === 'short' || mo === 'narrow', num = !txt && (mo || da || y);\n"
"  const wS = wd ? (wd === 'long' ? L.D[W] : wd === 'short' ? L.d[W] : __first(L.D[W])) : '';\n"
"  const yS = y ? (y === '2-digit' ? __p2(Y % 100) : '' + Y) : '';\n"
"  const dS = da ? (da === '2-digit' || (P.z && num && mo) ? __p2(D) : '' + D) : '';\n"
"  const mS = mo ? (mo === 'long' ? L.M[Mo] : mo === 'short' ? L.m[Mo] : mo === 'narrow' ? __first(L.M[Mo]) : (mo === '2-digit' || (P.z && da) ? __p2(Mo + 1) : '' + (Mo + 1))) : '';\n"
"  if (P.k && (txt || (wd && (mo || da || y)))) { const k = P.k;\n"
"    if (y) { put('year', Y); lit(k[0]); } if (mo) { put('month', Mo + 1); lit(k[1]); } if (da) { put('day', D); lit(k[2]); } if (wd) { lit(k[3]); put('weekday', wS); } }\n"
"  else if (txt) { const to = P.to || P.o;\n"
"    if (wd && !P.we) { put('weekday', wS); if (mo || da || y) lit(P.wc ? ', ' : ' '); }\n"
"    if (to === 'MDY') { put('month', mS); if (da) { lit(' '); put('day', dS); } if (y) { lit(da ? ', ' : ' '); put('year', yS); } }\n"
"    else if (to === 'YMD') { if (y) { put('year', yS); lit('. '); } put('month', mS); if (da) { lit(' '); put('day', dS); lit('.'); } }\n"
"    else { if (da) { put('day', dS); lit((P.dot ? '.' : '') + (P.de ? ' de ' : ' ')); } put('month', mS); if (y) { lit(P.de ? ' de ' : ' '); put('year', yS); } }\n"
"    if (wd && P.we) { lit(' '); put('weekday', wS); } }\n"
"  else { if (wd) { put('weekday', wS); if (num) lit(', '); }\n"
"    const f = { Y: y ? ['year', yS] : null, M: mo ? ['month', mS] : null, D: da ? ['day', dS] : null }; let first = true;\n"
"    for (let i = 0; i < 3; i++) { const e = f[P.o[i]]; if (!e) continue; if (!first) lit(P.s); first = false; put(e[0], e[1]); }\n"
"    if (!first && P.s === '. ' && P.o === 'YMD' && y && mo && da) lit('.'); }\n"
"  if (h || mi || s) { if (out.length) lit(P.j || ', ');\n"
"    const h12 = o.hour12 !== undefined ? !!o.hour12 : o.hourCycle ? /^h1[12]$/.test(o.hourCycle) : !!P.h, sep = P.t || ':';\n"
"    let hh = H, per = ''; if (h && h12) { per = H < 12 ? L.am : L.pm; if (P.lp) per = per.toLowerCase(); hh = H % 12; if (!hh && o.hourCycle !== 'h11') hh = 12; }\n"
"    else if (o.hourCycle === 'h24' && !hh) hh = 24;\n"
"    if (per && P.k) { put('dayPeriod', per); lit(P.k[3]); }\n"
"    if (h) put('hour', h === '2-digit' || (!h12 && !P.h1) ? __p2(hh) : hh);\n"
"    if (mi) { if (h) lit(sep); put('minute', h || mi === '2-digit' ? __p2(Mi) : Mi); }\n"
"    if (s) { if (h || mi) lit(sep); put('second', h || mi || s === '2-digit' ? __p2(S) : S); }\n"
"    if (per && !P.k) { lit(' '); put('dayPeriod', per); }\n"
"    if (o.timeZoneName) { lit(' '); const lo = -d.getTimezoneOffset(), m = Math.abs(lo);\n"
"      put('timeZoneName', zone || (lo ? 'GMT' + (lo < 0 ? '-' : '+') + Math.floor(m / 60) + (m % 60 ? ':' + __p2(m % 60) : '') : 'UTC')); } }\n"
"  return out; }\n"
"function __dtf(d, l, o, req, def) { const a = __dtParts(d, __locTag(l), o, req, def); let r = ''; for (let i = 0; i < a.length; i++) r += a[i].value; return r; }\n"
"const __CUR = { USD: '$', EUR: '€', GBP: '£', JPY: '¥', CNY: 'CN¥', KRW: '₩', RUB: '₽', TRY: '₺', INR: '₹', BRL: 'R$' };\n"
"function __numFmt(n, tag, o) { const L = __loc(tag), P = L.P; o = o || {}; n = +n;\n"
"  if (!isFinite(n)) return isNaN(n) ? 'NaN' : (n < 0 ? '-' : '') + '∞';\n"
"  const st = o.style, cur = st === 'currency' ? ('' + (o.currency || 'USD')).toUpperCase() : '', dF = cur ? (cur === 'JPY' || cur === 'KRW' ? 0 : 2) : 0;\n"
"  if (st === 'percent') n *= 100;\n"
"  let minF = o.minimumFractionDigits, maxF = o.maximumFractionDigits;\n"
"  minF = minF === undefined ? (maxF === undefined ? dF : Math.min(dF, +maxF || 0)) : Math.min(Math.max(+minF || 0, 0), 20);\n"
"  maxF = maxF === undefined ? Math.max(minF, cur ? dF : st === 'percent' ? 0 : 3) : Math.min(Math.max(+maxF || 0, minF), 20);\n"
"  let a = Math.abs(n); if (o.maximumSignificantDigits) a = +a.toPrecision(Math.min(Math.max(+o.maximumSignificantDigits || 1, 1), 21));\n"
"  if (a >= 1e21) return (n < 0 ? '-' : '') + a;\n"
"  const q = a.toFixed(maxF).split('.'); let ip = q[0], fp = q[1] || '';\n"
"  while (fp.length > minF && fp[fp.length - 1] === '0') fp = fp.slice(0, -1);\n"
"  const minI = Math.min(+o.minimumIntegerDigits || 1, 21); while (ip.length < minI) ip = '0' + ip;\n"
"  if (o.useGrouping !== false && o.useGrouping !== 'false' && ip.length > (P.g2 ? 4 : 3)) { let r = '';\n"
"    for (let k = ip.length; k > 0; k -= 3) r = ip.slice(Math.max(k - 3, 0), k) + (r ? L.g + r : ''); ip = r; }\n"
"  let r = ip + (fp ? L.c + fp : '');\n"
"  if (st === 'percent') r += (P.ps ? ' %' : '%');\n"
"  if (cur) { const sym = o.currencyDisplay === 'code' ? cur + ' ' : o.currencyDisplay === 'name' ? cur + ' ' : (__CUR[cur] || cur + ' ');\n"
"    r = P.c === ',' ? r + ' ' + sym.trim() : sym + r; }\n"
"  return (n < 0 && /[1-9]/.test(r) ? '-' : '') + r; }\n"
"function __asDate(d) { return d === undefined ? new Date() : (d && typeof d.getTime === 'function' ? d : new Date(d)); }\n"
"function __DTF(l, o) { if (!(this instanceof __DTF)) return new __DTF(l, o); const tag = __locTag(l), op = Object.assign({}, o);\n"
"  this.format = d => __dtf(__asDate(d), tag, op, 'any', 'date'); this.formatToParts = d => __dtParts(__asDate(d), tag, op, 'any', 'date');\n"
"  this.resolvedOptions = () => Object.assign({ locale: tag, calendar: 'gregory', numberingSystem: 'latn', timeZone: __deftz }, op); }\n"
"function __NF(l, o) { if (!(this instanceof __NF)) return new __NF(l, o); const tag = __locTag(l), op = Object.assign({}, o);\n"
"  this.format = n => __numFmt(n, tag, op); this.formatToParts = n => [{ type: 'integer', value: __numFmt(n, tag, op) }];\n"
"  this.resolvedOptions = () => Object.assign({ locale: tag, numberingSystem: 'latn', style: 'decimal' }, op); }\n"
"__DTF.supportedLocalesOf = __NF.supportedLocalesOf = l => (l === undefined ? [] : [].concat(l));\n"
"const Intl = { DateTimeFormat: __DTF, NumberFormat: __NF, getCanonicalLocales(l) { return l === undefined ? [] : [].concat(l); } };\n"
"Date.prototype.toLocaleString = function (l, o) { return __dtf(this, l, o, 'any', 'all'); };\n"
"Date.prototype.toLocaleDateString = function (l, o) { return __dtf(this, l, o, 'date', 'date'); };\n"
"Date.prototype.toLocaleTimeString = function (l, o) { return __dtf(this, l, o, 'time', 'time'); };\n"
"Number.prototype.toLocaleString = function (l, o) { return __numFmt(this, __locTag(l), o); };\n"
/* ---- dynamic thisLayer / thisObject (WE sets these globals before it runs
 * any function of a script: classes defined in one script and instantiated
 * from another see the caller's layer, which shared event dispatchers rely
 * on) and WE property keyframe animations: tracks on layer properties and
 * effect constants, grouped by parent/children keys, played by time or by
 * scripts (getAnimation), with animationEvent dispatch at event frames */
"const __tracks = [];\n"
/* every animation plays from load unless "startpaused" (WE); one-shots
 * ("single") play once and hold their last frame: 2901582518's opening
 * blur ("bokehblur", no script plays it), 3219908811's 5 s notice,
 * 3465215190's intro. Scripts drive the paused ones (getAnimation). */
"function __addAnim(i, e, p, prop, a, base) { const o = a.options || {}; const ch = [];\n"
"  for (let c = 0; c < 4; c++) { const k = a['c' + c]; ch.push(Array.isArray(k) && k.length ? k.slice().sort((x, y) => x.frame - y.frame) : null); }\n"
"  const b = __fxarr(base); __tracks.push({ i, e, p, prop, ch, rel: !!a.relative, base: b || [0, 0, 0, 0],\n"
"    fps: +o.fps > 0 ? +o.fps : 30, length: +o.length > 0 ? +o.length : 1, mode: o.mode || 'loop', name: o.name || '', wrap: !!o.wraploop,\n"
"    events: Array.isArray(o.events) ? o.events : [], parentKey: o.parent && o.parent.key, frame: 0, rate: 1,\n"
"    playing: !o.startpaused, fresh: true, leader: null, followers: [], api: null }); }\n"
"function __animResolve() { for (const t of __tracks) { if (!t.parentKey) continue;\n"
"    const l = __tracks.find(u => u !== t && u.i === t.i && u.e === t.e && u.p === t.p && u.prop === t.parentKey && !u.parentKey);\n"
"    if (l) { t.leader = l; l.followers.push(t); } }\n"
"  for (const t of __tracks) if (!t.leader) __animApply(t); }\n"
"function __bez(k0, k1, f) { const f0 = k0.frame, f1 = k1.frame, d = f1 - f0; if (d <= 0) return k1.value;\n"
"  const fo = k0.front || {}, bk = k1.back || {};\n"
"  if (!fo.enabled && !bk.enabled) return k0.value + (k1.value - k0.value) * (f - f0) / d;\n"
"  const h = d / 3, x1 = f0 + (fo.enabled ? (+fo.x || 0) * h : h), y1 = k0.value + (fo.enabled ? (+fo.y || 0) * h : 0),\n"
"    x2 = f1 + (bk.enabled ? (+bk.x || 0) * h : -h), y2 = k1.value + (bk.enabled ? (+bk.y || 0) * h : 0);\n"
/* u with x(u) = f, by 30 halvings: __host.bezu runs the loop that stood
 * here - "let lo = 0, hi = 1, u = 0.5; for (let it = 0; it < 30; it++) {
 * u = (lo + hi) / 2; const m = 1 - u; const x = m * m * m * f0 + 3 * m * m *
 * u * x1 + 3 * m * u * u * x2 + u * u * u * f1; if (x < f) lo = u; else
 * hi = u; }" - in C, with the same double operations in the same order
 * (h_bezu), so u is the same number. In the interpreter it was 30 rounds
 * of some 25 operations per animated channel and frame: most of the script
 * time of scenes with keyframe animations. */
"  const u = __host.bezu(f0, x1, x2, f1, f);\n"
"  const m = 1 - u; return m * m * m * k0.value + 3 * m * m * u * y1 + 3 * m * u * u * y2 + u * u * u * k1.value; }\n"
"function __chan(keys, f, W) { const n = keys.length, k0 = keys[0], kn = keys[n - 1];\n"
"  if (W > 0 && k0.frame + W > kn.frame && (f > kn.frame || f < k0.frame)) {\n"
"    if (f > kn.frame) return __bez(kn, { frame: k0.frame + W, value: k0.value, back: k0.back }, f);\n"
"    return __bez({ frame: kn.frame - W, value: kn.value, front: kn.front }, k0, f); }\n"
"  if (f <= k0.frame) return k0.value;\n"
"  if (f >= kn.frame) return kn.value;\n"
"  for (let k = 0; k + 1 < n; k++) if (f <= keys[k + 1].frame) return __bez(keys[k], keys[k + 1], f); return keys[n - 1].value; }\n"
"function __trackSet(t, f) { const v = [];\n"
"  for (let c = 0; c < 4; c++) { if (!t.ch[c]) { if (c < t.base.length) v.push(t.base[c]); continue; }\n"
"    const x = +__chan(t.ch[c], f, t.wrap && t.mode === 'loop' ? t.length : 0) || 0; v.push(t.rel ? (t.base[c] || 0) + x : x); }\n"
"  while (v.length > Math.max(t.base.length, 1)) v.pop();\n"
"  if (t.e < 0) { if (t.prop === 'alpha' || v.length === 1) __host.set(t.i, t.prop, v[0]); else __host.set(t.i, t.prop, [v[0], v[1] || 0, v[2] || 0]); }\n"
"  else __host.fxset(t.i, t.e, t.p, t.prop, v); }\n"
"function __animApply(t) { __trackSet(t, t.frame); for (const c of t.followers) __trackSet(c, t.frame); }\n"
"function __animEvents(t, a, b) { if (a === b) return; const inc = t.fresh; t.fresh = false; if (!t.events.length) return; const fwd = b > a;\n"
"  for (const ev of t.events) { const f = +ev.frame; if (fwd ? ((f > a || (inc && f === a)) && f <= b) : ((f < a || (inc && f === a)) && f >= b)) __animEvent(t, ev); } }\n"
"function __animEvent(t, ev) { const e = { name: ev.name, frame: +ev.frame }; if (__traceAnim) __host.log('anim event ' + ev.name + ' (' + __host.name(t.i) + '/' + t.e + '/' + t.prop + ' rt ' + engine.runtime.toFixed(2) + ')');\n"
"  for (const s of __scripts) { const f = s.handlers && s.handlers.animationEvent; if (!f || s.i !== t.i) continue;\n"
"    const r = __run(s, f, e, __sget(s)); if (r !== undefined) __sset(s, r); } }\n"
/* events fire when the playhead passes their frame, and also on the frame
 * it starts from (after load, play() from a stop, setFrame()): WE fires a
 * frame-0 event when a one-shot starts forward from 0. 3163060610 sets its
 * song title / cover / text colour on such events ("mTitleUpdate" at frame
 * 0 of the title's show animation).
 * dt is capped: the first frames after a load can be seconds apart, which
 * would skip opening animations entirely. A one-shot stops at its end in
 * the direction it plays (the old "b <= 0" stop ended every forward
 * one-shot on its first, zero-length tick) */
"function __animTick(dt) { dt = Math.min(Math.max(+dt || 0, 0), 1.0);\n"
"  for (const t of __tracks) { if (t.leader || !t.playing) continue;\n"
"    const a = t.frame, L = t.length; let b = a + dt * t.fps * t.rate;\n"
"    if (t.mode === 'loop') { if (b >= L || b < 0) { const fw = b >= L; __animEvents(t, a, fw ? L : 0); t.frame = ((b % L) + L) % L; __animApply(t);\n"
/* ... and the part of the step after the wrap, the loop's first frame
 * included: an event at frame 0 fired once at load and never again */
"        t.fresh = true; __animEvents(t, fw ? 0 : L, t.frame); continue; } }\n"
"    else if (t.mode === 'mirror') { if (b > L) { b = 2 * L - b; t.rate = -t.rate; } else if (b < 0) { b = -b; t.rate = -t.rate; } }\n"
"    else if (b >= L && t.rate > 0) { b = L; t.playing = false; } else if (b <= 0 && t.rate < 0) { b = 0; t.playing = false; }\n"
"    else b = Math.min(Math.max(b, 0), L);\n"
"    t.frame = b; __animApply(t); __animEvents(t, a, b); } }\n"
"class __Anim { constructor(t) { this.__t = t; }\n"
"  get name() { return this.__t.name; } get fps() { return this.__t.fps; } get frameCount() { return this.__t.length; }\n"
"  get duration() { return this.__t.length / this.__t.fps; } get rate() { return this.__t.rate; } set rate(v) { this.__t.rate = +v || 0; }\n"
"  play() { const t = this.__t; if (t.mode === 'single') { if (t.rate > 0 && t.frame >= t.length) { t.frame = 0; t.fresh = true; } else if (t.rate < 0 && t.frame <= 0) { t.frame = t.length; t.fresh = true; } }\n"
"    if (__traceAnim) __host.log('anim play ' + t.name + ' (' + __host.name(t.i) + '/' + t.e + '/' + t.prop + ') frame ' + t.frame.toFixed(2) + ' rate ' + t.rate);\n"
"    if (!t.playing) t.fresh = true; t.playing = true; } pause() { this.__t.playing = false; }\n"
"  stop() { this.__t.playing = false; this.__t.frame = 0; this.__t.fresh = true; __animApply(this.__t); }\n"
"  isPlaying() { return this.__t.playing; } getFrame() { return this.__t.frame; }\n"
"  setFrame(f) { const t = this.__t; t.frame = Math.min(Math.max(+f || 0, 0), t.length); t.fresh = true; __animApply(t); } }\n"
"function __animFind(i, e, p, n) { const ok = t => !t.leader && t.i === i && (e === -1 ? t.e < 0 : t.e === e) && (p === -2 || e === -1 || t.p === p);\n"
"  let t = n !== undefined && n !== null ? __tracks.find(u => ok(u) && u.name === '' + n) : null;\n"
"  if (!t) t = __tracks.find(ok); if (!t) return __anim; return t.api || (t.api = new __Anim(t)); }\n"
"const __scripts = [];\n"
/* e = -3: a script on a property the renderer does not drive (parallaxDepth,
 * size, volume, particle instance overrides, scene settings). It runs like
 * the others - WE authors keep shared set-up and event handlers on such
 * properties - and its value lives here: "1 2 3" as a vector, a number or
 * boolean as it is, other text as text. */
"function __inertval(iv) { if (typeof iv !== 'string') return iv; const a = __nums(iv);\n"
"  for (let k = 0; k < a.length; k++) if (!isFinite(a[k])) return iv;\n"
"  return a.length < 1 || a.length > 4 || !iv.trim() ? iv : a.length === 1 ? a[0] : __fxval(a); }\n"
"function __add(i, prop, factory, sp, e, p, iv) { const L = __layer(i);\n"
"  e = e === undefined ? -1 : e; p = p === undefined ? -1 : p;\n"
/* thisObject of a script on an instance override is the instance (the
 * dust of 3163060610 sets `thisObject.size` from a user option) */
"  const T = e < 0 ? (e === -1 && prop.length > 5 && prop.slice(0, 5) === 'inst.' && L ? L.instance : L) : p < 0 ? __effect(i, e) : __material(i, e, p);\n"
"  thisLayer = L; thisObject = T; const s = factory(L, T); s.L = L; s.T = T; s.e = e; s.p = p;\n"
"  s.val = iv === undefined ? undefined : e === -3 ? __inertval(iv) : __fxval(__fxarr(iv));\n"
"  if (s.props && sp) for (const k in sp) { const v = sp[k]; const cur = s.props[k];\n"
"    s.props[k] = typeof v !== 'string' ? v : cur instanceof Vec3 ? new Vec3(v) : cur instanceof Vec4 ? new Vec4(v) : cur instanceof Vec2 ? new Vec2(v) : v; }\n"
"  s.i = i; s.prop = prop; s.errors = 0; s.retry = 0; s.dead = false; __scripts.push(s); }\n"
/* bound property of a script: layer property, effect visibility or a
 * material constant (cached in s.val when the uniform is not active) */
"function __sget(s) { if (s.e === -3) return s.val; if (s.e < 0) return __lget(s.i, s.prop);\n"
"  if (s.p < 0) { const v = __host.fxget(s.i, s.e, -1, 'visible'); return v === undefined ? !!s.val : !!v; }\n"
"  const v = __host.fxget(s.i, s.e, s.p, s.prop); return v === undefined ? s.val : __fxval(v); }\n"
"function __sset(s, v) { if (v === undefined || v === null) return; if (s.e === -3) { s.val = v; return; } if (s.e < 0) return __lset(s.i, s.prop, v);\n"
"  if (s.p < 0) { s.val = !!v; __host.fxset(s.i, s.e, -1, 'visible', !!v); return; }\n"
"  const a = __fxarrS(v); if (!a) return; __fxput(s.i, s.e, s.p, s.prop, a, __fxarr(s.val)); s.val = __fxval(a); }\n"
"function __media(kind, ev) { for (const k in ev) if (/Color$/.test(k) && Array.isArray(ev[k])) ev[k] = new Vec3(ev[k][0], ev[k][1], ev[k][2]);\n"
"  for (const s of __scripts) { const f = s.handlers && s.handlers[kind]; if (f) __run(s, f, ev); } }\n"
/* cursor events (IComponent cursor*): to every script of the layer that
 * was hit, CursorEvent = { worldPosition, localPosition } */
"function __cursorLayers() { for (const s of __scripts) { const h = s.handlers;\n"
"    if (h && (h.cursorClick || h.cursorDown || h.cursorUp || h.cursorEnter || h.cursorLeave || h.cursorMove)) __host.cursorLayer(s.i); } }\n"
"function __cursor(kind, i, wx, wy, lx, ly) { const ev = { worldPosition: new Vec3(wx, wy, 0), localPosition: new Vec3(lx, ly, 0) };\n"
"  for (const s of __scripts) { if (s.i !== i) continue; const f = s.handlers && s.handlers[kind]; if (f) __run(s, f, ev); } }\n"
/* A script that throws is not switched off for good (it used to be, after
 * its 4th exception, with its event handlers): its update() is tried again
 * after 4, 8, 16 ... 300 frames while it keeps failing, any success clears
 * the count, and events are always delivered. s.dead = it ran out of time
 * (__interrupted): never called again. */
"function __skip(s) { return s.dead || (s.errors > 3 && __frame < s.retry); }\n"
"function __run(s, fn, arg, arg2) { if (s.dead) return undefined; thisLayer = s.L; thisObject = s.T; __cur = s;\n"
"  try { const r = fn(arg, arg2); __cur = null; s.errors = 0; return r; } catch (e) { s.errors++;\n"
"    if (s.errors > 3) s.retry = __frame + Math.min(300, 1 << Math.min(s.errors - 2, 9));\n"
"    if (s.errors === 1 || s.errors === 4) __host.log('script error (layer ' + __host.name(s.i) + ', ' + s.prop + '): ' + e + (e && e.stack ? ' @ ' + ('' + e.stack).split('\\n').slice(0, 3).join(' | ') : '') + (s.errors === 4 ? ' [4 in a row: tried again less often until it works]' : '')); __cur = null; return undefined; } }\n"
/* called from C after an entry ran out of time: stop what was running */
"function __interrupted() { const s = __cur, t = __curTimer; __cur = null; __curTimer = null;\n"
"  if (t) { const k = __timers.indexOf(t); if (k >= 0) __timers.splice(k, 1); __host.log('a timer callback ran out of time: timer removed (layer ' + (t.L ? t.L.name : '?') + ')'); return true; }\n"
"  if (s) { s.dead = true; __host.log('script stopped: it ran out of time (layer ' + __host.name(s.i) + ', ' + s.prop + ')'); return true; }\n"
"  return false; }\n"
/* WE hands color properties to scripts as Vec3 ("r g b" strings here)
 * by the option's declared type when the scene knows it (__uptypes, from
 * project.json): only "color" options. A text option holding "10 20 30"
 * stays text. Without declarations: by the look of the value, as before. */
"let __uptypes = null;\n"
"function __types(t) { __uptypes = t && typeof t === 'object' ? t : null; }\n"
"function __upconv(up) { for (const k in up) { const v = up[k]; if (typeof v !== 'string') continue;\n"
"    if (__uptypes ? __uptypes[k] === 'color' && isFinite(__nums(v)[0]) : /^\\s*-?[0-9.]+(\\s+-?[0-9.]+){2}\\s*$/.test(v)) up[k] = new Vec3(v); } return up; }\n"
/* WALPE_PROP_PROBE: which user properties scripts actually read */
"function __upwrap(up) { if (!__probe) return up; const u = k => { if (typeof k === 'string' && Object.prototype.hasOwnProperty.call(up, k)) __host.use(k); };\n"
"  return new Proxy(up, { get(t, k) { u(k); return t[k]; }, has(t, k) { u(k); return k in t; },\n"
"    getOwnPropertyDescriptor(t, k) { u(k); return Object.getOwnPropertyDescriptor(t, k); } }); }\n"
"function __start(up) { up = __upwrap(__upconv(up)); engine.userProperties = up; __animResolve();\n"
"  for (const s of __scripts) { if (s.init) __sset(s, __run(s, s.init, __sget(s))); }\n"
"  for (const s of __scripts) { if (s.applyUserProperties) __run(s, s.applyUserProperties, up); }\n"
"  for (const s of __scripts) { const f = s.handlers && s.handlers.applyGeneralSettings; if (f) __run(s, f, { language: __lang }); } }\n"
"function __tick(ft, rt, cx, cy, down) { const p0 = __prof ? __host.cpu() : 0; engine.frametime = ft; engine.runtime = rt; input.cursorLeftDown = !!down;\n"
"  const d = new Date(); engine.timeOfDay = (d.getHours() * 3600 + d.getMinutes() * 60 + d.getSeconds()) / 86400;\n"
"  input.cursorWorldPosition = new Vec3(cx, cy, 0); input.cursorScreenPosition = new Vec2(cx, cy);\n"
"  for (const b of __audioBufs) __host.audio(b.resolution, b.left, b.right, b.average);\n"
/* only the timers that are due when the frame starts: one created by a
 * callback (setTimeout(loop, 0) from inside loop) waits for the next frame
 * instead of running again at once until the time budget is gone */
"  __frame++; const p1 = __prof ? __host.cpu() : 0;\n"
"  if (__timers.length) { const due = __timers.filter(t => t.due <= rt);\n"
"    for (const t of due) { const k = __timers.indexOf(t); if (k < 0) continue;\n"
"      if (t.every) t.due = rt + t.every; else __timers.splice(k, 1);\n"
"      thisLayer = t.L; thisObject = t.T; __curTimer = t; try { t.fn(); } catch (e) { __host.log('timer error: ' + e); } __curTimer = null; } }\n"
"  const p2 = __prof ? __host.cpu() : 0;\n"
"  __animTick(ft);\n"
"  if (__prof) return __profTick(p0, p1, p2);\n"
"  for (const s of __scripts) { if (s.update && !__skip(s)) __sset(s, __run(s, s.update, __sget(s))); } }\n"
/* WALPE_SS_PROFILE=1: the same loop with the thread's CPU time taken around
 * each script's update () and around the two property bridge calls that
 * frame it; every 300 frames the ten most expensive scripts are logged
 * (layer, property, microseconds per frame, share of the bridge) */
"let __profN = 0, __profAll = 0, __profPre = 0, __profTim = 0, __profAnim = 0;\n"
"function __profTick(p0, p1, p2) { const t0 = __host.cpu(); __profPre += p1 - p0; __profTim += p2 - p1; __profAnim += t0 - p2;\n"
"  for (const s of __scripts) { if (!s.update || __skip(s)) continue;\n"
"    const a = __host.cpu(), v = __sget(s), b = __host.cpu(), r = __run(s, s.update, v), c = __host.cpu(); __sset(s, r); const d = __host.cpu();\n"
"    s.pu = (s.pu || 0) + (c - b); s.pb = (s.pb || 0) + (b - a) + (d - c); }\n"
"  __profAll += __host.cpu() - t0; if (++__profN < 300) return;\n"
"  const top = __scripts.filter(s => s.pu || s.pb).sort((x, y) => (y.pu + y.pb) - (x.pu + x.pb)).slice(0, 10);\n"
"  __host.log('script profile, 300 frames, us/frame: set-up + ' + __audioBufs.length + ' audio buffers ' + (__profPre / __profN).toFixed(1) + ', ' + __timers.length + ' timers ' + (__profTim / __profN).toFixed(1) + ', property animations ' + (__profAnim / __profN).toFixed(1) + ' (' + __tracks.filter(t => !t.leader && t.playing).length + ' of ' + __tracks.length + ' tracks playing), update() calls ' + (__profAll / __profN).toFixed(1) + ' (' + __scripts.filter(s => s.update).length + ' of ' + __scripts.length + ' scripts have one; each measured call adds about 1 us of clock reads)');\n"
"  for (const s of top) __host.log('  ' + ((s.pu + s.pb) / __profN).toFixed(1) + ' us/frame (bridge ' + (s.pb / __profN).toFixed(1) + '): layer ' + __host.name(s.i) + ', ' + s.prop + (s.e >= 0 ? ' [effect ' + s.e + ']' : ''));\n"
"  for (const s of __scripts) { s.pu = 0; s.pb = 0; } __profN = 0; __profAll = 0; __profPre = 0; __profTim = 0; __profAnim = 0; }\n"
/* WALPE_PROP_PROBE: scripts see a fixed date that advances with the
 * (virtual) scene clock and a seeded Math.random, so the state digests of
 * two loads match */
"if (__probe) { const RD = Date, T0 = 1750000000000; let sd = 12345;\n"
"  globalThis.Date = class extends RD { constructor(...a) { if (a.length) super(...a); else super(T0 + Math.floor(engine.runtime * 1000)); }\n"
"    static now() { return T0 + Math.floor(engine.runtime * 1000); } };\n"
"  Math.random = function () { sd = (sd * 1103515245 + 12345) & 0x7fffffff; return sd / 0x80000000; }; }\n"
/* the API wallpaper scripts see (thisLayer / thisObject are defined by C) */
"Object.freeze(__modules);\n"
"const __api = { Vec2, Vec3, Vec4, Mat3, Mat4, WEMath, WEColor, WEVector, __modules, createScriptProperties, thisScene, engine, input,\n"
"  shared, MediaPlaybackEvent, localStorage, console, renderContext, Intl };\n"
"for (const k in __api) Object.defineProperty(globalThis, k, { value: __api[k], writable: false, enumerable: false, configurable: false });\n"
"return { add: __add, addAnim: __addAnim, start: __start, tick: __tick, media: __media, cursor: __cursor,\n"
"  cursorLayers: __cursorLayers, interrupted: __interrupted, types: __types }; })\n";

static WalpeSceneScript *ss_of (JSContext *ctx) { return JS_GetContextOpaque (ctx); }

/* Host callbacks and the script watchdog (ss_wd_handler, below). While the
 * thread is in the scene's code or in GLib it may hold a lock, so the
 * watchdog must not take it out of there: that stretch is bracketed with
 * ss_host_enter () / ss_host_leave (). Everything else a host function
 * does is QuickJS again and stays under the watchdog - in particular the
 * conversion of its arguments: JS_ToInt32 / JS_ToFloat64 / JS_ToCString on
 * an object run that object's valueOf / toString, i.e. script code, and
 * with it any built-in that never returns (join () of an array of length
 * 2^32 - 1, a regular expression). The whole callback used to count as
 * host code: such an argument hung the render thread for good. */
static void ss_host_enter (void);
static void ss_host_leave (void);

/* what scripts hand over as a layer coordinate / effect constant, in the
 * range the scene's float arithmetic keeps finite */
#define SS_VEC_MAX 1e9
#define SS_FX_MAX 1e30
#define SS_STR_MAX (64 * 1024)   /* bytes of text handed to the scene per call */
static gboolean ss_clamp (double *v, int n, double max)
{
    for (int i = 0; i < n; i++) {
        if (!isfinite (v[i]))
            return FALSE;
        v[i] = CLAMP (v[i], -max, max);
    }
    return TRUE;
}

/* a script's log line: at most SS_LOG_MAX bytes of it, cut between two
 * characters (a script could log megabytes per line) */
#define SS_LOG_MAX 1024
static int ss_log_len (const char *s)
{
    gsize n = strlen (s);
    if (n <= SS_LOG_MAX)
        return (int) n;
    n = SS_LOG_MAX;
    while (n > 0 && ((guchar) s[n] & 0xc0) == 0x80)
        n--;
    return (int) n;
}

static JSValue h_find (JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void) t;
    WalpeSceneScript *ss = ss_of (ctx);
    const char *n = argc > 0 ? JS_ToCString (ctx, argv[0]) : NULL;
    ss_host_enter ();
    int idx = n ? ss->host.find (ss->host.user, n) : -1;
    ss_host_leave ();
    if (n)
        JS_FreeCString (ctx, n);
    return JS_NewInt32 (ctx, idx);
}

static JSValue h_name (JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void) t;
    WalpeSceneScript *ss = ss_of (ctx);
    int idx = -1;
    if (argc > 0 && JS_ToInt32 (ctx, &idx, argv[0]))
        return JS_EXCEPTION;
    ss_host_enter ();
    const char *n = ss->host.name (ss->host.user, idx);
    ss_host_leave ();
    return n ? JS_NewString (ctx, n) : JS_NULL;
}

static JSValue h_count (JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void) t; (void) argc; (void) argv;
    WalpeSceneScript *ss = ss_of (ctx);
    /* the scene's layers are fixed once it is loaded: counted once (a
     * script's `for (i < thisScene.getLayerCount())` asked every round) */
    if (ss->n_layers < 0) {
        int n = 0;
        ss_host_enter ();
        while (ss->host.name (ss->host.user, n))
            n++;
        ss_host_leave ();
        ss->n_layers = n;
    }
    return JS_NewInt32 (ctx, ss->n_layers);
}

static JSValue h_get (JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void) t;
    WalpeSceneScript *ss = ss_of (ctx);
    if (argc < 2)
        return JS_UNDEFINED;
    int idx = -1;
    if (JS_ToInt32 (ctx, &idx, argv[0]))
        return JS_EXCEPTION;
    const char *prop = JS_ToCString (ctx, argv[1]);
    if (!prop)
        return JS_EXCEPTION;
    double v[4] = { 0 };
    int n = 0;
    const char *s = NULL;
    ss_host_enter ();
    WalpeSsType ty = ss->host.get (ss->host.user, idx, prop, v, &n, &s);
    ss_host_leave ();
    JS_FreeCString (ctx, prop);
    switch (ty) {
    case WSS_NUM: return JS_NewFloat64 (ctx, v[0]);
    case WSS_BOOL: return JS_NewBool (ctx, v[0] != 0);
    case WSS_STR: return JS_NewString (ctx, s ? s : "");
    case WSS_VEC: {
        JSValue a = JS_NewArray (ctx);
        for (int i = 0; i < n && i < 4; i++)
            JS_SetPropertyUint32 (ctx, a, (uint32_t) i, JS_NewFloat64 (ctx, v[i]));
        return a;
    }
    default: return JS_UNDEFINED;
    }
}

static JSValue h_set (JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void) t;
    WalpeSceneScript *ss = ss_of (ctx);
    if (argc < 3)
        return JS_UNDEFINED;
    int idx = -1;
    if (JS_ToInt32 (ctx, &idx, argv[0]))
        return JS_EXCEPTION;
    const char *prop = JS_ToCString (ctx, argv[1]);
    if (!prop)
        return JS_EXCEPTION;
    /* the value is converted first (that may run script code, see
     * ss_host_enter), the scene is called last and once */
    JSValueConst val = argv[2];
    WalpeSsType ty = WSS_NONE;
    double v[4] = { 0 };
    int n = 0;
    const char *str = NULL;
    if (JS_IsBool (val)) {
        v[0] = JS_ToBool (ctx, val) > 0;
        n = 1;
        ty = WSS_BOOL;
    } else if (JS_IsString (val)) {
        str = JS_ToCString (ctx, val);
        if (str)
            ty = WSS_STR;
    } else if (JS_IsArray (ctx, val) > 0) {
        gboolean ok = TRUE;
        for (; ok && n < 4; n++) {
            JSValue e = JS_GetPropertyUint32 (ctx, val, (uint32_t) n);
            if (JS_IsUndefined (e))
                break;
            ok = !JS_IsException (e) && JS_ToFloat64 (ctx, &v[n], e) == 0;
            JS_FreeValue (ctx, e);
        }
        /* a component that is not a finite number: the vector is not
         * applied (the prelude fills such components from the layer);
         * numbers no scene has are brought into range */
        if (ok && n > 0 && ss_clamp (v, n, SS_VEC_MAX))
            ty = WSS_VEC;
    } else {
        if (JS_ToFloat64 (ctx, &v[0], val) == 0 && ss_clamp (v, 1, SS_VEC_MAX)) {
            n = 1;
            ty = WSS_NUM;
        }
    }
    if (ty != WSS_NONE) {
        ss_host_enter ();
        /* text for the scene: at most SS_STR_MAX bytes of it (the scene
         * shows far less; a script's string may be tens of megabytes, per
         * frame), and valid UTF-8 - a lone surrogate ('\ud800') leaves
         * QuickJS as three bytes that are not (Pango, the log) */
        char *valid = NULL;
        if (str && strnlen (str, SS_STR_MAX + 1) > SS_STR_MAX) {
            gsize cut = SS_STR_MAX;
            while (cut > 0 && ((guchar) str[cut] & 0xc0) == 0x80)
                cut--;
            valid = g_strndup (str, cut);
        }
        if (str && !g_utf8_validate (valid ? valid : str, -1, NULL)) {
            char *made = g_utf8_make_valid (valid ? valid : str, -1);
            g_free (valid);
            valid = made;
        }
        ss->host.set (ss->host.user, idx, prop, ty, ty == WSS_STR ? NULL : v, n,
                      valid ? valid : str);
        g_free (valid);
        ss_host_leave ();
    }
    if (str)
        JS_FreeCString (ctx, str);
    JS_FreeCString (ctx, prop);
    return JS_UNDEFINED;
}

static JSValue h_log (JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void) t;
    WalpeSceneScript *ss = ss_of (ctx);
    const char *s = argc > 0 ? JS_ToCString (ctx, argv[0]) : NULL;
    if (s) {
        ss_host_enter ();
        if (ss->errors_logged++ < 40 || g_getenv ("WALPE_TRACE_ANIM")) {
            int n = ss_log_len (s);
            g_message ("scenescript: %.*s%s", n, s, s[n] ? " [...]" : "");
        }
        ss_host_leave ();
        JS_FreeCString (ctx, s);
    }
    return JS_UNDEFINED;
}

/* WALPE_PROP_PROBE: a script read user property argv[0] */
/* WALPE_SS_PROFILE: this thread's CPU time, microseconds */
static gint64 ss_cpu_ns (void);
static JSValue h_cpu (JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void) t; (void) argc; (void) argv;
    return JS_NewFloat64 (ctx, (double) ss_cpu_ns () / 1000.0);
}

static JSValue h_use (JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void) t;
    const char *s = argc > 0 ? JS_ToCString (ctx, argv[0]) : NULL;
    if (s) {
        ss_host_enter ();
        walpe_probe_use (s, "script.read");
        ss_host_leave ();
        JS_FreeCString (ctx, s);
    }
    return JS_UNDEFINED;
}

static JSValue h_want_audio (JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void) t; (void) argc; (void) argv;
    ss_of (ctx)->uses_audio = TRUE;
    return JS_UNDEFINED;
}

/* v is a Float32Array (and not another typed array with 4-byte elements) */
static gboolean ss_is_f32 (JSContext *ctx, JSValueConst v)
{
    WalpeSceneScript *ss = ss_of (ctx);
    if (JS_IsUndefined (ss->f32_proto)) {
        JSValue g = JS_GetGlobalObject (ctx);
        JSValue ctor = JS_GetPropertyStr (ctx, g, "Float32Array");
        ss->f32_proto = JS_GetPropertyStr (ctx, ctor, "prototype");
        JS_FreeValue (ctx, ctor);
        JS_FreeValue (ctx, g);
        if (JS_IsException (ss->f32_proto)) {
            JS_FreeValue (ctx, JS_GetException (ctx));
            ss->f32_proto = JS_NULL;
        }
    }
    JSValue p = JS_GetPrototype (ctx, v);
    gboolean same = JS_IsObject (p) && JS_IsObject (ss->f32_proto) &&
                    JS_VALUE_GET_PTR (p) == JS_VALUE_GET_PTR (ss->f32_proto);
    JS_FreeValue (ctx, p);
    return same;
}

/* __host.bezu(f0, x1, x2, f1, f): the parameter u in 0..1 at which the
 * cubic Bezier with the x control values f0, x1, x2, f1 reaches f - the
 * bisection of the prelude's __bez (see there), operation for operation:
 * JS numbers are doubles, and this file is compiled without contraction
 * of a * b + c (meson.build: -ffp-contract=off). */
static JSValue h_bezu (JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void) t;
    double a[5] = { 0, 0, 0, 0, 0 };
    for (int i = 0; i < 5 && i < argc; i++)
        if (JS_ToFloat64 (ctx, &a[i], argv[i]))
            return JS_EXCEPTION;
    const double f0 = a[0], x1 = a[1], x2 = a[2], f1 = a[3], f = a[4];
    double lo = 0, hi = 1, u = 0.5;
    for (int it = 0; it < 30; it++) {
        u = (lo + hi) / 2;
        const double m = 1 - u;
        const double x = m * m * m * f0 + 3 * m * m * u * x1 + 3 * m * u * u * x2 + u * u * u * f1;
        if (x < f)
            lo = u;
        else
            hi = u;
    }
    return JS_NewFloat64 (ctx, u);
}

/* fill registerAudioBuffers arrays (Float32Array left/right/average) */
static JSValue h_audio (JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void) t;
    WalpeSceneScript *ss = ss_of (ctx);
    int res = 64;
    if (argc < 4 || JS_ToInt32 (ctx, &res, argv[0]) || !ss->host.audio)
        return JS_UNDEFINED;
    res = res == 16 || res == 32 ? res : 64;
    float l[64], r[64];
    ss_host_enter ();
    ss->host.audio (ss->host.user, res, l, r);
    ss_host_leave ();
    /* the three arrays are the Float32Arrays registerAudioBuffers made:
     * write their storage (192 property writes through the interpreter's
     * generic path per buffer and frame before). The values are the ones
     * the element writes below store: (float) of the double. Anything
     * else in their place (a script put its own array there) is filled
     * element by element as before. */
    float av[64];
    for (int i = 0; i < res; i++)
        av[i] = (float) (0.5 * (l[i] + r[i]));
    const float *src[3] = { l, r, av };
    gboolean direct = TRUE;
    guint8 *dst[3] = { NULL, NULL, NULL };
    for (int k = 0; k < 3 && direct; k++) {
        size_t off = 0, len = 0, bpe = 0, size = 0;
        JSValue ab = JS_GetTypedArrayBuffer (ctx, argv[1 + k], &off, &len, &bpe);
        if (JS_IsException (ab)) {
            JS_FreeValue (ctx, JS_GetException (ctx));   /* not a typed array */
            direct = FALSE;
            break;
        }
        guint8 *base = JS_GetArrayBuffer (ctx, &size, ab);
        JS_FreeValue (ctx, ab);
        if (!base) {
            JS_FreeValue (ctx, JS_GetException (ctx));   /* detached */
            direct = FALSE;
        } else if (bpe != sizeof (float) || len < (size_t) res * sizeof (float) ||
                   off + len > size || !ss_is_f32 (ctx, argv[1 + k])) {
            direct = FALSE;
        } else {
            dst[k] = base + off;
        }
    }
    if (direct) {
        for (int k = 0; k < 3; k++)
            memcpy (dst[k], src[k], (size_t) res * sizeof (float));
        return JS_UNDEFINED;
    }
    for (int i = 0; i < res; i++) {
        JS_SetPropertyUint32 (ctx, argv[1], (uint32_t) i, JS_NewFloat64 (ctx, l[i]));
        JS_SetPropertyUint32 (ctx, argv[2], (uint32_t) i, JS_NewFloat64 (ctx, r[i]));
        JS_SetPropertyUint32 (ctx, argv[3], (uint32_t) i, JS_NewFloat64 (ctx, 0.5 * (l[i] + r[i])));
    }
    return JS_UNDEFINED;
}

/* __host.fxget(i, e, p, key) -> bool | [n floats] | undefined */
static JSValue h_fxget (JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void) t;
    WalpeSceneScript *ss = ss_of (ctx);
    int i = -1, e = -1, p = -1;
    if (argc < 4 || !ss->host.fx_get || JS_ToInt32 (ctx, &i, argv[0]) ||
        JS_ToInt32 (ctx, &e, argv[1]) || JS_ToInt32 (ctx, &p, argv[2]))
        return JS_UNDEFINED;
    const char *key = JS_ToCString (ctx, argv[3]);
    if (!key)
        return JS_EXCEPTION;
    double v[4] = { 0 };
    int n = 0;
    ss_host_enter ();
    WalpeSsType ty = ss->host.fx_get (ss->host.user, i, e, p, key, v, &n);
    ss_host_leave ();
    JS_FreeCString (ctx, key);
    if (ty == WSS_BOOL)
        return JS_NewBool (ctx, v[0] != 0);
    if (ty != WSS_VEC && ty != WSS_NUM)
        return JS_UNDEFINED;
    JSValue a = JS_NewArray (ctx);
    for (int k = 0; k < n && k < 4; k++)
        JS_SetPropertyUint32 (ctx, a, (uint32_t) k, JS_NewFloat64 (ctx, v[k]));
    return a;
}

/* __host.fxset(i, e, p, key, bool | [floats]) */
static JSValue h_fxset (JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void) t;
    WalpeSceneScript *ss = ss_of (ctx);
    int i = -1, e = -1, p = -1;
    if (argc < 5 || !ss->host.fx_set || JS_ToInt32 (ctx, &i, argv[0]) ||
        JS_ToInt32 (ctx, &e, argv[1]) || JS_ToInt32 (ctx, &p, argv[2]))
        return JS_UNDEFINED;
    const char *key = JS_ToCString (ctx, argv[3]);
    if (!key)
        return JS_EXCEPTION;
    if (JS_IsBool (argv[4])) {
        double d = JS_ToBool (ctx, argv[4]) > 0;
        ss_host_enter ();
        ss->host.fx_set (ss->host.user, i, e, p, key, WSS_BOOL, &d, 1);
        ss_host_leave ();
    } else if (JS_IsArray (ctx, argv[4]) > 0) {
        double v[4] = { 0 };
        int n = 0;
        gboolean ok = TRUE;
        for (; ok && n < 4; n++) {
            JSValue el = JS_GetPropertyUint32 (ctx, argv[4], (uint32_t) n);
            if (JS_IsUndefined (el))
                break;
            ok = !JS_IsException (el) && JS_ToFloat64 (ctx, &v[n], el) == 0;
            JS_FreeValue (ctx, el);
            if (!isfinite (v[n]))
                v[n] = 0;
        }
        if (ok && n > 0 && ss_clamp (v, n, SS_FX_MAX)) {
            ss_host_enter ();
            ss->host.fx_set (ss->host.user, i, e, p, key, WSS_VEC, v, n);
            ss_host_leave ();
        }
    }
    JS_FreeCString (ctx, key);
    return JS_UNDEFINED;
}

static JSValue h_fxfind (JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void) t;
    WalpeSceneScript *ss = ss_of (ctx);
    int i = -1;
    if (argc < 2 || !ss->host.fx_find || JS_ToInt32 (ctx, &i, argv[0]))
        return JS_NewInt32 (ctx, -1);
    const char *n = JS_ToCString (ctx, argv[1]);
    ss_host_enter ();
    int r = n ? ss->host.fx_find (ss->host.user, i, n) : -1;
    ss_host_leave ();
    if (n)
        JS_FreeCString (ctx, n);
    return JS_NewInt32 (ctx, r);
}

static JSValue h_fxname (JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void) t;
    WalpeSceneScript *ss = ss_of (ctx);
    int i = -1, e = -1;
    if (argc < 2 || !ss->host.fx_name || JS_ToInt32 (ctx, &i, argv[0]) || JS_ToInt32 (ctx, &e, argv[1]))
        return JS_NULL;
    ss_host_enter ();
    const char *n = ss->host.fx_name (ss->host.user, i, e);
    ss_host_leave ();
    return n ? JS_NewString (ctx, n) : JS_NULL;
}

static JSValue h_fxcount (JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void) t;
    WalpeSceneScript *ss = ss_of (ctx);
    int i = -1;
    if (argc < 1 || !ss->host.fx_count || JS_ToInt32 (ctx, &i, argv[0]))
        return JS_NewInt32 (ctx, 0);
    ss_host_enter ();
    int c = ss->host.fx_count (ss->host.user, i);
    ss_host_leave ();
    return JS_NewInt32 (ctx, c);
}

/* __host.cursorLayer(i): layer i has a script with a cursor* function.
 * Only what the scene knows as a layer is taken, each once: the list C
 * builds is bounded by the scene, whatever the JS side does. */
static JSValue h_cursor_layer (JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void) t;
    WalpeSceneScript *ss = ss_of (ctx);
    int idx = -1;
    if (argc < 1 || !ss->collect || JS_ToInt32 (ctx, &idx, argv[0]) || idx < 0)
        return JS_UNDEFINED;
    ss_host_enter ();
    gboolean have = !ss->host.name (ss->host.user, idx);
    for (guint i = 0; !have && i < ss->collect->len; i++)
        have = g_array_index (ss->collect, int, i) == idx;
    if (!have)
        g_array_append_val (ss->collect, idx);
    ss_host_leave ();
    return JS_UNDEFINED;
}

/* __host.locale(lang, region) -> 12 month names, 12 short ones, 7 weekday
 * names from Sunday, 7 short ones, AM, PM - from the system's locale data
 * for lang_REGION.UTF-8, or undefined when that locale is not installed
 * (the prelude then uses its English tables). The process locale is not
 * touched. */
static gboolean ss_letters (const char *s, gsize min, gsize max)
{
    gsize n = 0;
    for (; s[n]; n++)
        if (!g_ascii_isalpha (s[n]))
            return FALSE;
    return n >= min && n <= max;
}

static JSValue h_locale (JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void) t;
    static const nl_item items[] = {
        MON_1, MON_2, MON_3, MON_4, MON_5, MON_6, MON_7, MON_8, MON_9, MON_10, MON_11, MON_12,
        ABMON_1, ABMON_2, ABMON_3, ABMON_4, ABMON_5, ABMON_6, ABMON_7, ABMON_8, ABMON_9,
        ABMON_10, ABMON_11, ABMON_12, DAY_1, DAY_2, DAY_3, DAY_4, DAY_5, DAY_6, DAY_7,
        ABDAY_1, ABDAY_2, ABDAY_3, ABDAY_4, ABDAY_5, ABDAY_6, ABDAY_7, AM_STR, PM_STR };
    if (argc < 2)
        return JS_UNDEFINED;
    const char *lang = JS_ToCString (ctx, argv[0]);
    const char *reg = JS_ToCString (ctx, argv[1]);
    JSValue r = JS_UNDEFINED;
    /* the names are copied out of libc first; the array is filled outside
     * the host bracket (storing an element runs a setter a script may
     * have put on Array.prototype: script code, see ss_host_enter) */
    char *names[G_N_ELEMENTS (items)] = { NULL };
    gboolean ok = FALSE;
    ss_host_enter ();
    if (lang && reg && ss_letters (lang, 2, 3) && ss_letters (reg, 2, 2)) {
        locale_t loc = (locale_t) 0;
        static const char *const enc[] = { ".UTF-8", ".utf8", NULL };
        for (int k = 0; enc[k] && !loc; k++) {
            char name[32];
            g_snprintf (name, sizeof name, "%s_%s%s", lang, reg, enc[k]);
            loc = newlocale (LC_TIME_MASK | LC_CTYPE_MASK, name, (locale_t) 0);
        }
        if (loc) {
            ok = TRUE;
            for (guint i = 0; ok && i < G_N_ELEMENTS (items); i++) {
                const char *v = nl_langinfo_l (items[i], loc);
                ok = v && g_utf8_validate (v, -1, NULL);
                if (ok)
                    names[i] = g_strndup (v, 256);
            }
            freelocale (loc);
        }
    }
    ss_host_leave ();
    if (ok) {
        r = JS_NewArray (ctx);
        for (guint i = 0; i < G_N_ELEMENTS (items); i++)
            JS_DefinePropertyValueUint32 (ctx, r, i, JS_NewString (ctx, names[i]), JS_PROP_C_W_E);
    }
    ss_host_enter ();
    for (guint i = 0; i < G_N_ELEMENTS (items); i++)
        g_free (names[i]);
    ss_host_leave ();
    if (lang)
        JS_FreeCString (ctx, lang);
    if (reg)
        JS_FreeCString (ctx, reg);
    return r;
}

/* __host.tzoff(zone, unix seconds) -> seconds east of UTC at that moment,
 * undefined for a zone the system does not know. Zone names only
 * ("Asia/Tokyo"): no paths, no POSIX rule strings. */
static JSValue h_tzoff (JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void) t;
    double secs = 0;
    if (argc < 2 || JS_ToFloat64 (ctx, &secs, argv[1]) || !isfinite (secs) || fabs (secs) > 1e11)
        return JS_UNDEFINED;
    const char *zone = JS_ToCString (ctx, argv[0]);
    if (!zone)
        return JS_EXCEPTION;
    JSValue r = JS_UNDEFINED;
    gboolean ok = g_ascii_isalpha (zone[0]) && strlen (zone) < 64 && !strstr (zone, "..");
    for (const char *p = zone; ok && *p; p++)
        ok = g_ascii_isalnum (*p) || strchr ("/_+-", *p) != NULL;
    if (ok) {
        ss_host_enter ();
#if GLIB_CHECK_VERSION (2, 68, 0)
        GTimeZone *tz = g_time_zone_new_identifier (zone);
#else
        GTimeZone *tz = g_time_zone_new (zone);
#endif
        if (tz) {
            gint iv = g_time_zone_find_interval (tz, G_TIME_TYPE_UNIVERSAL, (gint64) secs);
            if (iv >= 0)
                r = JS_NewInt32 (ctx, g_time_zone_get_offset (tz, iv));
            g_time_zone_unref (tz);
        }
        ss_host_leave ();
    }
    JS_FreeCString (ctx, zone);
    return r;
}

/* the locale scripts get when they name none (toLocaleTimeString()), as a
 * BCP 47 tag from the environment's LC_TIME ("en_US.UTF-8" -> "en-US"), or
 * Wallpaper Engine's language code for applyGeneralSettings ("en-us") */
static char *ss_env_locale (const char *category, gboolean we_code)
{
    const char *v = g_getenv ("LC_ALL");
    if (!v || !*v)
        v = g_getenv (category);
    if (!v || !*v)
        v = g_getenv ("LANG");
    char lang[4] = "", reg[3] = "";
    if (v) {
        gsize n = 0;
        while (g_ascii_isalpha (v[n]))
            n++;
        if (n >= 2 && n <= 3) {
            for (gsize i = 0; i < n; i++)
                lang[i] = g_ascii_tolower (v[i]);
            if (v[n] == '_' && g_ascii_isalpha (v[n + 1]) && g_ascii_isalpha (v[n + 2]) &&
                !g_ascii_isalpha (v[n + 3])) {
                reg[0] = g_ascii_toupper (v[n + 1]);
                reg[1] = g_ascii_toupper (v[n + 2]);
            }
        }
    }
    if (!*lang)
        return g_strdup (we_code ? "en-us" : "en-US");
    char *tag = *reg ? g_strdup_printf ("%s-%s", lang, reg) : g_strdup (lang);
    if (we_code) {
        char *low = g_ascii_strdown (tag, -1);
        g_free (tag);
        tag = low;
    }
    return tag;
}

/* ---- time limits for scripts ------------------------------------------
 * Scripts come with workshop wallpapers and run on the render thread, so
 * every entry into JS is bounded, in CPU time of this thread (wall time
 * would stop a healthy script whenever the machine is busy or the process
 * was stopped):
 *  - SS_SOFT_NS: QuickJS polls interrupt_cb in loops and calls; past the
 *    budget it throws an error no script can catch. The script (or timer)
 *    that was running is then stopped for good (__interrupted), so a
 *    runaway loop costs its budget once, not once per frame.
 *  - SS_HARD_NS: work inside one built-in is never polled (the regular
 *    expression engine of older QuickJS backtracks for hours on a
 *    pattern like /(a+)+$/). A per-thread CPU timer, armed only while JS
 *    runs, signals this thread; the handler jumps back to ss_run. The
 *    runtime is abandoned after that (never entered, never freed): the
 *    wallpaper goes on without its scripts instead of freezing its
 *    monitor - and the daemon's quit, which waits for the thread. */
#define SS_SOFT_NS (250 * G_GINT64_CONSTANT (1000000))
#define SS_BLAME_NS (50 * G_GINT64_CONSTANT (1000000))
#define SS_HARD_NS (1500 * G_GINT64_CONSTANT (1000000))
#define SS_HARD_RETRY_NS (50 * G_GINT64_CONSTANT (1000000))
#define SS_MAX_JOBS 10000    /* promise reactions run per entry */

static __thread struct {
    sigjmp_buf jmp;
    volatile sig_atomic_t armed;   /* inside JS (ss_run) */
    volatile sig_atomic_t busy;    /* inside a host callback: not here */
    gboolean interrupted;          /* the soft limit fired in this entry */
    gboolean timer_tried, timer_ok;
    int timer;                     /* kernel timer id */
    gint64 deadline;               /* thread CPU ns, 0 = none */
} ss_tls;

static void ss_host_enter (void) { ss_tls.busy++; }
static void ss_host_leave (void) { ss_tls.busy--; }

static gint64 ss_cpu_ns (void)
{
    struct timespec ts;
    if (clock_gettime (CLOCK_THREAD_CPUTIME_ID, &ts) != 0)
        return g_get_monotonic_time () * 1000;
    return (gint64) ts.tv_sec * G_GINT64_CONSTANT (1000000000) + ts.tv_nsec;
}

static int interrupt_cb (JSRuntime *rt, void *opaque)
{
    (void) rt; (void) opaque;
    if (ss_tls.deadline && ss_cpu_ns () > ss_tls.deadline) {
        ss_tls.interrupted = TRUE;
        return 1;
    }
    return 0;
}

static void arm_deadline (gint64 budget_ns) { ss_tls.deadline = ss_cpu_ns () + budget_ns; }

/* the hard limit: Linux per-thread CPU timers through the raw system
 * calls (no librt needed with an old glibc), on the two ABIs whose
 * kernel and libc timer structures are the same */
#if defined (__linux__) && (defined (__x86_64__) || defined (__aarch64__))
#ifndef sigev_notify_thread_id
#define sigev_notify_thread_id _sigev_un._tid
#endif
/* text of the program itself (QuickJS is linked statically into it) */
extern char __executable_start[] __attribute__ ((weak));
extern char etext[] __attribute__ ((weak));

static int ss_wd_sig;   /* signal of the timers, 0 = none free */

/* Leaving from a signal handler is only safe where no lock can be held:
 * in QuickJS's own code, i.e. the interrupted instruction is in the
 * program text and no host callback is running. Inside libc (malloc), the
 * loader or a host function the timer simply fires again 50 ms later. */
static void ss_wd_handler (int sig, siginfo_t *si, void *uctx)
{
    (void) sig; (void) si;
    if (!ss_tls.armed || ss_tls.busy)
        return;
    ucontext_t *uc = uctx;
#if defined (__x86_64__)
    const char *pc = (const char *) uc->uc_mcontext.gregs[REG_RIP];
#else
    const char *pc = (const char *) uc->uc_mcontext.pc;
#endif
    if (!__executable_start || !etext || pc < __executable_start || pc >= etext)
        return;
    ss_tls.armed = 0;
    siglongjmp (ss_tls.jmp, 1);
}

static void ss_wd_timer_free (gpointer p)
{
    syscall (SYS_timer_delete, GPOINTER_TO_INT (p) - 1);
}
static GPrivate ss_wd_key = G_PRIVATE_INIT (ss_wd_timer_free);

static void ss_wd_thread_init (void)
{
    static gsize once;
    if (ss_tls.timer_tried)
        return;
    ss_tls.timer_tried = TRUE;
    if (g_once_init_enter (&once)) {
        /* a real-time signal nobody handles yet */
        int found = 0;
        for (int sg = SIGRTMIN + 9; sg <= SIGRTMAX - 1 && !found; sg++) {
            struct sigaction old, sa;
            if (sigaction (sg, NULL, &old) != 0 || (old.sa_flags & SA_SIGINFO) ||
                old.sa_handler != SIG_DFL)
                continue;
            memset (&sa, 0, sizeof (sa));
            sa.sa_sigaction = ss_wd_handler;
            sa.sa_flags = SA_SIGINFO | SA_RESTART;
            sigemptyset (&sa.sa_mask);
            if (sigaction (sg, &sa, NULL) == 0)
                found = sg;
        }
        ss_wd_sig = found;
        if (!found)
            g_message ("scenescript: no free signal for the script watchdog (built-ins that "
                       "never return are not stopped)");
        g_once_init_leave (&once, 1);
    }
    if (!ss_wd_sig)
        return;
    sigset_t set;
    sigemptyset (&set);
    sigaddset (&set, ss_wd_sig);
    pthread_sigmask (SIG_UNBLOCK, &set, NULL);
    struct sigevent sev;
    memset (&sev, 0, sizeof (sev));
    sev.sigev_notify = SIGEV_THREAD_ID;
    sev.sigev_signo = ss_wd_sig;
    sev.sigev_notify_thread_id = (pid_t) syscall (SYS_gettid);
    int id = 0;
    if (syscall (SYS_timer_create, CLOCK_THREAD_CPUTIME_ID, &sev, &id) == 0) {
        ss_tls.timer = id;
        ss_tls.timer_ok = TRUE;
        g_private_set (&ss_wd_key, GINT_TO_POINTER (id + 1));   /* deleted with the thread */
    }
}

static void ss_wd_set (gint64 first_ns, gint64 again_ns)
{
    if (!ss_tls.timer_ok)
        return;
    struct itimerspec its;
    its.it_value.tv_sec = (time_t) (first_ns / G_GINT64_CONSTANT (1000000000));
    its.it_value.tv_nsec = (long) (first_ns % G_GINT64_CONSTANT (1000000000));
    its.it_interval.tv_sec = (time_t) (again_ns / G_GINT64_CONSTANT (1000000000));
    its.it_interval.tv_nsec = (long) (again_ns % G_GINT64_CONSTANT (1000000000));
    syscall (SYS_timer_settime, ss_tls.timer, 0, &its, NULL);
}
#else
static void ss_wd_thread_init (void) { }
static void ss_wd_set (gint64 first_ns, gint64 again_ns) { (void) first_ns; (void) again_ns; }
#endif

/* log the pending exception of ctx */
static void ss_log_exception (WalpeSceneScript *ss, JSContext *ctx, const char *what)
{
    JSValue e = JS_GetException (ctx);
    /* the text of the error may be the thrown object's own toString:
     * script code, only run with the limits armed (ss_run) */
    const char *m = ss_tls.armed ? JS_ToCString (ctx, e) : NULL;
    if (!m)
        JS_FreeValue (ctx, JS_GetException (ctx));   /* ... which threw in turn */
    ss_host_enter ();
    if (ss->errors_logged++ < 40) {
        int n = m ? ss_log_len (m) : 1;
        g_message ("scenescript: %s: %.*s%s", what, n, m ? m : "?", m && m[n] ? " [...]" : "");
    }
    ss_host_leave ();
    if (m)
        JS_FreeCString (ctx, m);
    JS_FreeValue (ctx, e);
}

/* JSON text from the scene (script properties, user options, a static
 * value, an animation, a media event) as a value. This runs outside
 * ss_run, with no time limit armed, so nothing here may run script code:
 * a text the engine does not take is reported without asking the error
 * object for its message (Error.prototype.toString and the "message"
 * getter are the wallpaper's to replace - with a loop there the load
 * never came back). json-glib writes a number beyond the double range
 * ("1e999" in a scene file) as inf.0, which is no JSON: such numbers are
 * handed over as the largest double. FALSE = not parsed (*out undefined). */
static gboolean ss_json (WalpeSceneScript *ss, const char *what, const char *text, const char *name,
                         JSValue *out)
{
    char *fixed = NULL;
    if (strstr (text, "inf") || strstr (text, "nan")) {
        GString *o = g_string_sized_new (strlen (text) + 16);
        gboolean in_str = FALSE;
        for (const char *p = text; *p; p++) {
            if (in_str) {
                if (*p == '\\' && p[1])
                    g_string_append_c (o, *p++);
                else if (*p == '"')
                    in_str = FALSE;
            } else if (*p == '"') {
                in_str = TRUE;
            } else if (!strncmp (p, "inf", 3) || !strncmp (p, "nan", 3)) {
                g_string_append (o, *p == 'i' ? "1.7976931348623157e308" : "null");
                p += 2;
                if (p[1] == '.' && p[2] == '0')
                    p += 2;
                continue;
            }
            g_string_append_c (o, *p);
        }
        text = fixed = g_string_free (o, FALSE);
    }
    JSValue v = JS_ParseJSON (ss->ctx, text, strlen (text), name);
    g_free (fixed);
    if (!JS_IsException (v)) {
        *out = v;
        return TRUE;
    }
    JS_FreeValue (ss->ctx, JS_GetException (ss->ctx));
    if (ss->errors_logged++ < 40)
        g_message ("scenescript: %s: %s is not JSON the script engine reads", what, name);
    *out = JS_UNDEFINED;
    return FALSE;
}

/* the part of ss_run that runs with the limits armed. Kept apart so the
 * function holding the sigsetjmp has no local that changes after it. */
static gboolean __attribute__ ((noinline))
ss_run_armed (WalpeSceneScript *ss, const char *what, JSValueConst fn, int argc, JSValue *argv,
              const char *code, const char *name, JSValue *ret)
{
    JSValue r;
    if (code)
        r = JS_Eval (ss->ctx, code, strlen (code), name, JS_EVAL_TYPE_GLOBAL);
    else
        r = JS_Call (ss->ctx, fn, JS_UNDEFINED, argc, argv);
    gboolean ok = !JS_IsException (r);
    if (!ok)
        ss_log_exception (ss, ss->ctx, what);
    else if (ret)
        *ret = r;
    else
        JS_FreeValue (ss->ctx, r);
    gboolean timed_out = ss_tls.interrupted, blamed = FALSE;
    if (timed_out) {
        /* out of time: stop the script or timer that was running */
        ss_tls.interrupted = FALSE;
        arm_deadline (SS_BLAME_NS);
        JSValueConst f = ss->fn[SS_FN_INTERRUPTED];
        JSValue b = JS_IsFunction (ss->ctx, f) ? JS_Call (ss->ctx, f, JS_UNDEFINED, 0, NULL)
                                               : JS_UNDEFINED;
        blamed = !JS_IsException (b) && JS_ToBool (ss->ctx, b) > 0;
        if (JS_IsException (b))
            JS_FreeValue (ss->ctx, JS_GetException (ss->ctx));
        JS_FreeValue (ss->ctx, b);
        ss_tls.interrupted = FALSE;
    }
    /* promise reactions / async functions queued by the scripts: QuickJS
     * leaves running them to the embedder (they never ran, and piled up
     * until the JS heap was full). They run on what is left of the entry's
     * budget; one that runs out of time has no script to its name (the
     * script that queued it returned long ago), so it counts as an entry
     * with nobody to blame - it used to count as nothing, and a reaction
     * that never returned cost the full budget in every frame, for good. */
    JSContext *jc = NULL;
    int n = 0, rc = 0;
    while (!ss->dead && n++ < SS_MAX_JOBS && (rc = JS_ExecutePendingJob (ss->rt, &jc)) > 0)
        ;
    if (rc < 0 && jc)
        ss_log_exception (ss, jc, "promise job");
    if (ss_tls.interrupted) {
        timed_out = TRUE;
        blamed = FALSE;
        ss_tls.interrupted = FALSE;
    }
    /* With nobody to blame three times in a row every script stops. */
    if (timed_out && !blamed) {
        if (++ss->unblamed >= 3 && !ss->dead) {
            ss->dead = TRUE;
            ss_host_enter ();
            g_warning ("scenescript: scripts ran out of time three times in a row: all scripts "
                       "of this wallpaper are stopped");
            ss_host_leave ();
        }
    } else if (timed_out || ok) {
        ss->unblamed = 0;
    }
    return ok;
}

/* Every entry into JS: evaluates `code` (named `name`) or calls the
 * function `fn`. TRUE = no exception; *ret (optional) receives the result,
 * which the caller frees. Exceptions are logged as "what: message". */
static gboolean ss_run (WalpeSceneScript *ss, const char *what, JSValueConst fn, int argc,
                        JSValue *argv, const char *code, const char *name, JSValue *ret)
{
    if (ss->dead)
        return FALSE;
    ss_wd_thread_init ();
    ss_tls.interrupted = FALSE;
    arm_deadline (SS_SOFT_NS);
    if (sigsetjmp (ss_tls.jmp, 1) != 0) {
        /* from ss_wd_handler: a built-in did not return */
        ss_wd_set (0, 0);
        ss_tls.deadline = 0;
        ss->dead = ss->lost = TRUE;
        g_warning ("scenescript: %s: a script did not come back from a built-in function "
                   "(a regular expression that backtracks without end?) within %.1f s of CPU "
                   "time. All scripts of this wallpaper are stopped; the wallpaper keeps running",
                   what, SS_HARD_NS / 1e9);
        return FALSE;
    }
    ss_wd_set (SS_HARD_NS, SS_HARD_RETRY_NS);
    ss_tls.armed = 1;
    gboolean ok = ss_run_armed (ss, what, fn, argc, argv, code, name, ret);
    ss_tls.armed = 0;
    ss_wd_set (0, 0);
    ss_tls.deadline = 0;
    return ok;
}

/* values made for a call are only given back to a runtime that is intact */
static void ss_free_value (WalpeSceneScript *ss, JSValue v)
{
    if (!ss->lost)
        JS_FreeValue (ss->ctx, v);
}

/* host callbacks go through one trampoline */
typedef JSValue SsHostFn (JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv);
static const struct { const char *name; SsHostFn *fn; int len; } ss_host_fns[] = {
    { "find", h_find, 1 }, { "name", h_name, 1 }, { "count", h_count, 0 },
    { "get", h_get, 2 }, { "set", h_set, 3 }, { "log", h_log, 1 }, { "use", h_use, 1 },
    { "audio", h_audio, 4 }, { "wantAudio", h_want_audio, 0 }, { "fxget", h_fxget, 4 },
    { "fxset", h_fxset, 5 }, { "fxfind", h_fxfind, 2 }, { "fxname", h_fxname, 2 },
    { "fxcount", h_fxcount, 1 }, { "cursorLayer", h_cursor_layer, 1 },
    { "locale", h_locale, 2 }, { "tzoff", h_tzoff, 2 }, { "cpu", h_cpu, 0 },
    { "bezu", h_bezu, 5 },
};

static JSValue ss_host_call (JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv,
                             int magic)
{
    /* not marked as host code here: each function brackets the stretch
     * in which it really is (ss_host_enter) */
    return ss_host_fns[magic].fn (ctx, t, argc, argv);
}

WalpeSceneScript *walpe_ss_new (const WalpeSsHost *host, float canvas_w, float canvas_h,
                                JsonObject *user_props)
{
    (void) user_props;
#ifdef WALPE_QUICKJS_OLD
    {
        /* see meson.build: once per process */
        static gsize said;
        if (g_once_init_enter (&said)) {
            g_message ("scenescript: built with a QuickJS release older than 2024-01-13 (fixes for "
                       "memory errors that wallpaper scripts can reach may be missing): update "
                       "QuickJS and rebuild");
            g_once_init_leave (&said, 1);
        }
    }
#endif
    WalpeSceneScript *ss = g_new0 (WalpeSceneScript, 1);
    ss->host = *host;
    ss->rt = JS_NewRuntime ();
    JS_SetMemoryLimit (ss->rt, 64 * 1024 * 1024);
    JS_SetMaxStackSize (ss->rt, 512 * 1024);
    JS_SetInterruptHandler (ss->rt, interrupt_cb, NULL);
    ss->ctx = JS_NewContext (ss->rt);
    JS_SetContextOpaque (ss->ctx, ss);
    ss->n_layers = -1;
    ss->f32_proto = JS_UNDEFINED;
    for (int i = 0; i < SS_FN_N; i++)
        ss->fn[i] = JS_UNDEFINED;
    JSValue g = JS_GetGlobalObject (ss->ctx);
    /* WE sets these before it runs any function of a script */
    JS_DefinePropertyValueStr (ss->ctx, g, "thisLayer", JS_NULL,
                               JS_PROP_WRITABLE | JS_PROP_ENUMERABLE);
    JS_DefinePropertyValueStr (ss->ctx, g, "thisObject", JS_NULL,
                               JS_PROP_WRITABLE | JS_PROP_ENUMERABLE);
    JS_FreeValue (ss->ctx, g);
    JSValue a[8];
    a[0] = JS_NewObject (ss->ctx);
    for (guint i = 0; i < G_N_ELEMENTS (ss_host_fns); i++)
        JS_SetPropertyStr (ss->ctx, a[0], ss_host_fns[i].name,
                           JS_NewCFunctionMagic (ss->ctx, ss_host_call, ss_host_fns[i].name,
                                                 ss_host_fns[i].len, JS_CFUNC_generic_magic,
                                                 (int) i));
    a[1] = JS_NewArray (ss->ctx);
    JS_SetPropertyUint32 (ss->ctx, a[1], 0, JS_NewFloat64 (ss->ctx, canvas_w));
    JS_SetPropertyUint32 (ss->ctx, a[1], 1, JS_NewFloat64 (ss->ctx, canvas_h));
    a[2] = JS_NewBool (ss->ctx, walpe_probe_on ());
    /* WALPE_TRACE_ANIM=1: log animation events and play/pause/setFrame */
    a[3] = JS_NewBool (ss->ctx, g_getenv ("WALPE_TRACE_ANIM") != NULL);
    /* default locale, local time zone name, WE language code */
    char *deflocale = ss_env_locale ("LC_TIME", FALSE), *lang = ss_env_locale ("LC_MESSAGES", TRUE);
    a[4] = JS_NewString (ss->ctx, deflocale);
    a[5] = JS_NewString (ss->ctx, "UTC");
#if GLIB_CHECK_VERSION (2, 58, 0)
    {
        GTimeZone *tz = g_time_zone_new_local ();
        const char *id = tz ? g_time_zone_get_identifier (tz) : NULL;
        if (id && g_ascii_isalpha (id[0]) && strchr (id, '/') && g_utf8_validate (id, -1, NULL)) {
            JS_FreeValue (ss->ctx, a[5]);
            a[5] = JS_NewString (ss->ctx, id);
        }
        if (tz)
            g_time_zone_unref (tz);
    }
#endif
    a[6] = JS_NewString (ss->ctx, lang);
    /* WALPE_SS_PROFILE=1: which script costs what (__profTick) */
    a[7] = JS_NewBool (ss->ctx, g_getenv ("WALPE_SS_PROFILE") != NULL);
    g_free (deflocale);
    g_free (lang);
    JSValue pf = JS_UNDEFINED, api = JS_UNDEFINED;
    gboolean ok = ss_run (ss, "prelude", JS_UNDEFINED, 0, NULL, PRELUDE, "<walpe-prelude>", &pf) &&
                  ss_run (ss, "prelude", pf, 8, a, NULL, NULL, &api);
    for (int i = 0; ok && i < SS_FN_N; i++) {
        ss->fn[i] = JS_GetPropertyStr (ss->ctx, api, ss_fn_names[i]);
        ok = JS_IsFunction (ss->ctx, ss->fn[i]);
    }
    if (!ss->lost) {
        JS_FreeValue (ss->ctx, api);
        JS_FreeValue (ss->ctx, pf);
        for (int i = 0; i < 8; i++)
            JS_FreeValue (ss->ctx, a[i]);
    }
    if (!ok) {
        walpe_ss_free (ss);
        return NULL;
    }
    return ss;
}

/* ES module -> function body (see header comment). The source is scanned
 * as tokens, not as lines: scripts come minified too ("'use strict';export
 * var scriptProperties=...;export function update(v){...}" on one line
 * left its "export" in place and the script did not compile). Strings,
 * template literals, comments and regular expression literals are copied
 * untouched. */
/* bytes of a Unicode space at p (JS white space outside ASCII), else 0.
 * Scripts edited in CJK tools carry them: 3436033033 has
 * "export<U+00A0>var<U+00A0>scriptProperties". */
static int rw_uspace (const char *p)
{
    const guchar *u = (const guchar *) p;
    if (u[0] == 0xc2 && u[1] == 0xa0)
        return 2;                                   /* U+00A0 */
    if (u[0] == 0xe3 && u[1] == 0x80 && u[2] == 0x80)
        return 3;                                   /* U+3000 */
    if (u[0] == 0xef && u[1] == 0xbb && u[2] == 0xbf)
        return 3;                                   /* U+FEFF */
    if (u[0] == 0xe2 && u[1] == 0x80 && (u[2] <= 0x8a || u[2] == 0xa8 || u[2] == 0xa9 || u[2] == 0xaf) && u[2] >= 0x80)
        return 3;                                   /* U+2000..200A, 2028, 2029, 202F */
    return 0;
}

/* p is inside an identifier or number (any other non-ASCII character counts
 * as a letter) */
static gboolean rw_id_char (const char *p)
{
    char c = *p;
    return g_ascii_isalnum (c) || c == '_' || c == '$' || (((guchar) c & 0x80) && !rw_uspace (p));
}

/* p at an opening quote: the end of the literal (after the closing quote).
 * A template literal holds ${ expressions }, which hold braces, strings
 * and templates again: the open ones are kept on a small heap stack - a
 * quote character for a literal, minus the brace depth for an expression -
 * not on the C stack (`${`${`${ ... a few hundred thousand deep in a
 * wallpaper's script was a stack overflow of the render thread). */
static const char *rw_string_end (const char *p)
{
    int local[32];
    int *st = local;
    gsize cap = G_N_ELEMENTS (local), len = 0;
    st[len++] = *p++;
    while (*p && len > 0) {
        int top = st[len - 1];
        int push = 0;
        if (top > 0) {                       /* in a literal opened by `top` */
            if (*p == top) {
                len--;
            } else if (*p == '\\' && p[1]) {
                p++;
            } else if (*p == '\n' && top != '`') {
                len--;                       /* unterminated: it ends before the line end */
                continue;
            } else if (top == '`' && *p == '$' && p[1] == '{') {
                p++;
                push = -1;
            }
        } else if (*p == '\'' || *p == '"' || *p == '`') {
            push = *p;                       /* in ${ }: a nested literal */
        } else if (*p == '{') {
            st[len - 1]--;
        } else if (*p == '}' && ++st[len - 1] == 0) {
            len--;
        }
        p++;
        if (push) {
            if (len == cap) {
                cap *= 2;
                if (st == local) {
                    st = g_new (int, cap);
                    memcpy (st, local, sizeof (local));
                } else {
                    st = g_renew (int, st, cap);
                }
            }
            st[len++] = push;
        }
    }
    if (st != local)
        g_free (st);
    return p;
}

static const char *rw_skip_blanks (const char *p)
{
    for (;;) {
        int u = rw_uspace (p);
        if (u)
            p += u;
        else if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            p++;
        else
            return p;
    }
}

static gboolean rw_word (const char *p, const char *w)
{
    gsize n = strlen (w);
    return !strncmp (p, w, n) && !rw_id_char (p + n);
}

static char *rewrite_module (const char *src)
{
    GString *o = g_string_sized_new (strlen (src) + 64);
    const char *p = src;
    char prev = 0;            /* last significant character: 0 = start, 'a' = a word or number */
    gboolean prev_kw = FALSE; /* that word was a keyword an expression follows */
    while (*p) {
        char c = *p;
        const char *e = NULL;
        if (c == '/' && p[1] == '/') {
            e = strchr (p, '\n');
            if (!e)
                e = p + strlen (p);
            g_string_append_len (o, p, e - p);
            p = e;
            continue;
        }
        if (c == '/' && p[1] == '*') {
            e = strstr (p + 2, "*/");
            e = e ? e + 2 : p + strlen (p);
            g_string_append_len (o, p, e - p);
            p = e;
            continue;
        }
        if (c == '\'' || c == '"' || c == '`') {
            e = rw_string_end (p);
            g_string_append_len (o, p, e - p);
            p = e;
            prev = 'a';
            prev_kw = FALSE;
            continue;
        }
        if (c == '/' && (prev == 0 || strchr ("(,=:[!&|?{};+-*%<>~^}", prev) || (prev == 'a' && prev_kw))) {
            /* regular expression literal: to the closing '/' outside a class */
            gboolean cls = FALSE;
            e = p + 1;
            while (*e && *e != '\n' && (cls || *e != '/')) {
                if (*e == '\\' && e[1] && e[1] != '\n')
                    e++;
                else if (*e == '[')
                    cls = TRUE;
                else if (*e == ']')
                    cls = FALSE;
                e++;
            }
            if (*e == '/')
                e++;
            g_string_append_len (o, p, e - p);
            p = e;
            prev = 'a';
            prev_kw = FALSE;
            continue;
        }
        if (rw_id_char (p)) {
            e = p;
            while (rw_id_char (e))
                e++;
            gsize len = (gsize) (e - p);
            if (len == 6 && !strncmp (p, "export", 6) && prev != '.') {
                const char *k = rw_skip_blanks (e);
                if (k > e && (rw_word (k, "function") || rw_word (k, "var") || rw_word (k, "let") ||
                              rw_word (k, "const") || rw_word (k, "async") || rw_word (k, "class"))) {
                    p = k;            /* the declaration without its "export" */
                    continue;
                }
            } else if (len == 6 && !strncmp (p, "import", 6) && (prev == 0 || strchr (";{}", prev) || prev == 'a')) {
                const char *k = rw_skip_blanks (e);
                /* a statement, not import(...), import.meta or a property named import */
                if (*k == '*' || *k == '{' || *k == '\'' || *k == '"' || rw_id_char (k)) {
                    char mod[64], alias[64], q1 = 0, q2 = 0;
                    int used = 0;
                    if (sscanf (e, " * as %63[A-Za-z0-9_$] from %c%63[A-Za-z0-9_]%c%n", alias, &q1, mod,
                                &q2, &used) == 4 && (q1 == '\'' || q1 == '"') && q1 == q2) {
                        g_string_append_printf (o, "const %s = __modules['%s'] || {};", alias, mod);
                        p = e + used;
                    } else {
                        g_string_append (o, "/* unsupported import */");
                        p = e;
                        while (*p && *p != ';' && *p != '\n')
                            p = *p == '\'' || *p == '"' || *p == '`' ? rw_string_end (p) : p + 1;
                    }
                    if (*p == ';')
                        p++;
                    prev = ';';
                    prev_kw = FALSE;
                    continue;
                }
            }
            g_string_append_len (o, p, (gssize) len);
            prev_kw = (len == 6 && (!strncmp (p, "return", 6) || !strncmp (p, "typeof", 6) ||
                                    !strncmp (p, "delete", 6))) ||
                      (len == 4 && (!strncmp (p, "case", 4) || !strncmp (p, "void", 4) ||
                                    !strncmp (p, "else", 4))) ||
                      (len == 2 && (!strncmp (p, "in", 2) || !strncmp (p, "of", 2) ||
                                    !strncmp (p, "do", 2))) ||
                      (len == 3 && !strncmp (p, "new", 3)) || (len == 5 && (!strncmp (p, "throw", 5) ||
                                    !strncmp (p, "yield", 5) || !strncmp (p, "await", 5))) ||
                      (len == 10 && !strncmp (p, "instanceof", 10));
            prev = 'a';
            p = e;
            continue;
        }
        int us = rw_uspace (p);
        if (us) {                 /* a blank like any other */
            g_string_append_len (o, p, us);
            p += us;
            continue;
        }
        g_string_append_c (o, c);
        if (!g_ascii_isspace (c)) {
            prev = c;
            prev_kw = FALSE;
        }
        p++;
    }
    return g_string_free (o, FALSE);
}

/* scriptproperties value: plain, {"user": prop, "value": fallback} or a
 * combo condition {"user": {"name": prop, "condition": "2"}, "value": b}
 * (a checkbox that is true while the combo's value equals the condition:
 * 3163060610's theme1..4 / pausedIsOk). Returns a new node. */
static JsonNode *sp_value (JsonNode *n, JsonObject *user_props)
{
    if (n && json_node_get_node_type (n) == JSON_NODE_OBJECT) {
        JsonObject *b = json_node_get_object (n);
        JsonNode *u = json_object_get_member (b, "user");
        JsonNode *fb = json_object_get_member (b, "value");
        if (u && json_node_get_node_type (u) == JSON_NODE_OBJECT) {
            JsonObject *c = json_node_get_object (u);
            const char *name = json_object_has_member (c, "name")
                ? json_object_get_string_member (c, "name") : NULL;
            JsonNode *cn = json_object_get_member (c, "condition");
            JsonNode *pv = name && user_props ? json_object_get_member (user_props, name) : NULL;
            if (!cn || !pv || json_node_get_node_type (pv) != JSON_NODE_VALUE ||
                json_node_get_node_type (cn) != JSON_NODE_VALUE)
                return fb ? json_node_copy (fb) : NULL;
            walpe_probe_use (name, "scriptprop");
            char b1[G_ASCII_DTOSTR_BUF_SIZE], b2[G_ASCII_DTOSTR_BUF_SIZE];
            const char *have = NULL, *want = NULL;
            JsonNode *pair[2] = { pv, cn };
            char *bufs[2] = { b1, b2 };
            const char **outs[2] = { &have, &want };
            for (int k = 0; k < 2; k++) {
                GType vt = json_node_get_value_type (pair[k]);
                if (vt == G_TYPE_STRING)
                    *outs[k] = json_node_get_string (pair[k]);
                else if (vt == G_TYPE_BOOLEAN)
                    *outs[k] = json_node_get_boolean (pair[k]) ? "1" : "0";
                else if (vt == G_TYPE_INT64)
                    *outs[k] = (g_snprintf (bufs[k], G_ASCII_DTOSTR_BUF_SIZE, "%" G_GINT64_FORMAT,
                                            json_node_get_int (pair[k])), bufs[k]);
                else
                    *outs[k] = (g_snprintf (bufs[k], G_ASCII_DTOSTR_BUF_SIZE, "%g",
                                            json_node_get_double (pair[k])), bufs[k]);
            }
            gboolean eq = !g_strcmp0 (have, want);
            if (!eq && have && want) {
                char *e1 = NULL, *e2 = NULL;
                double x = g_ascii_strtod (have, &e1), y = g_ascii_strtod (want, &e2);
                eq = e1 && !*e1 && e2 && !*e2 && e1 != have && e2 != want && x == y;
            }
            JsonNode *r = json_node_new (JSON_NODE_VALUE);
            json_node_set_boolean (r, eq);
            return r;
        }
        const char *up = u && json_node_get_node_type (u) == JSON_NODE_VALUE &&
                         json_node_get_value_type (u) == G_TYPE_STRING
            ? json_node_get_string (u) : NULL;
        JsonNode *pv = up && user_props ? json_object_get_member (user_props, up) : NULL;
        if (pv)
            walpe_probe_use (up, "scriptprop");
        JsonNode *r = pv ? pv : fb;
        return r ? json_node_copy (r) : NULL;
    }
    return n ? json_node_copy (n) : NULL;
}

/* the event functions a WE script may export (IComponent) */
static const char *const HANDLERS[] = {
    "applyUserProperties", "applyGeneralSettings", "destroy", "resizeScreen",
    "cursorEnter", "cursorLeave", "cursorMove", "cursorDown", "cursorUp", "cursorClick",
    "mediaStatusChanged", "mediaPlaybackChanged", "mediaPropertiesChanged",
    "mediaThumbnailChanged", "mediaTimelineChanged", "animationEvent", NULL
};

static gboolean ss_add_common (WalpeSceneScript *ss, int idx, int effect, int pass,
                               const char *prop, const char *src, JsonObject *scriptprops,
                               JsonObject *user_props, const char *init_value)
{
    /* scriptproperties -> plain JSON object (bindings resolved) */
    JsonBuilder *b = json_builder_new ();
    json_builder_begin_object (b);
    if (scriptprops) {
        GList *ks = json_object_get_members (scriptprops);
        for (GList *k = ks; k; k = k->next) {
            JsonNode *v = sp_value (json_object_get_member (scriptprops, k->data), user_props);
            if (!v)
                continue;
            json_builder_set_member_name (b, k->data);
            json_builder_add_value (b, v);
        }
        g_list_free (ks);
    }
    json_builder_end_object (b);
    JsonGenerator *gen = json_generator_new ();
    JsonNode *root = json_builder_get_root (b);
    json_generator_set_root (gen, root);
    char *spjson = json_generator_to_data (gen, NULL);
    json_node_unref (root);
    g_object_unref (gen);
    g_object_unref (b);

    GString *hs = g_string_new (NULL);
    for (int k = 0; HANDLERS[k]; k++)
        g_string_append_printf (hs, " %s: typeof %s === 'function' ? %s : null,", HANDLERS[k],
                                HANDLERS[k], HANDLERS[k]);
    char *body = rewrite_module (src);
    /* the script as a function; the prelude's add() runs it */
    char *code = g_strdup_printf (
        "(function (__wL, __wT) {\n%s\n;const __h = {%s }; return {"
        " init: typeof init === 'function' ? init : null,"
        " update: typeof update === 'function' ? update : null,"
        " applyUserProperties: __h.applyUserProperties, handlers: __h,"
        " props: typeof scriptProperties !== 'undefined' ? scriptProperties : null }; })",
        body, hs->str);
    g_string_free (hs, TRUE);
    g_free (body);
    if (strstr (src, "registerAudioBuffers"))
        ss->uses_audio = TRUE;
    if (strstr (src, "cursorWorldPosition") || strstr (src, "cursorScreenPosition") ||
        strstr (src, "cursorLeftDown"))
        ss->uses_cursor = TRUE;
    if (strstr (src, "mediaStatusChanged") || strstr (src, "mediaPlaybackChanged") ||
        strstr (src, "mediaPropertiesChanged") || strstr (src, "mediaThumbnailChanged") ||
        strstr (src, "mediaTimelineChanged"))
        ss->uses_media = TRUE;
    JSValue a[7];
    for (int k = 0; k < 7; k++)
        a[k] = JS_UNDEFINED;
    gboolean ok = ss_run (ss, prop, JS_UNDEFINED, 0, NULL, code, "<scenescript>", &a[2]);
    g_free (code);
    if (ok && !JS_IsFunction (ss->ctx, a[2])) {
        g_message ("scenescript: %s: the script is not one module", prop);
        ok = FALSE;
    }
    if (ok) {
        a[0] = JS_NewInt32 (ss->ctx, idx);
        a[1] = JS_NewString (ss->ctx, prop);
        a[4] = JS_NewInt32 (ss->ctx, effect);
        a[5] = JS_NewInt32 (ss->ctx, pass);
        ok = ss_json (ss, prop, spjson, "<scriptproperties>", &a[3]) &&
             (!(init_value && *init_value) || ss_json (ss, prop, init_value, "<value>", &a[6])) &&
             ss_run (ss, prop, ss->fn[SS_FN_ADD], 7, a, NULL, NULL, NULL);
    }
    for (int k = 0; k < 7; k++)
        ss_free_value (ss, a[k]);
    g_free (spjson);
    if (ok)
        ss->count++;
    return ok;
}

gboolean walpe_ss_add (WalpeSceneScript *ss, int idx, const char *prop, const char *src,
                       JsonObject *scriptprops, JsonObject *user_props)
{
    if (!ss || ss->dead || !src || !walpe_ss_supported_prop (prop))
        return FALSE;
    return ss_add_common (ss, idx, -1, -1, prop, src, scriptprops, user_props, NULL);
}

gboolean walpe_ss_add_fx (WalpeSceneScript *ss, int idx, int effect, int pass, const char *prop,
                          const char *src, JsonObject *scriptprops, JsonObject *user_props,
                          const char *init_value)
{
    if (!ss || ss->dead || !src || !prop || effect < 0 || !ss->host.fx_get)
        return FALSE;
    return ss_add_common (ss, idx, effect, pass, prop, src, scriptprops, user_props, init_value);
}

/* effect -3 = the prelude's mark for a script whose property lives in JS only */
gboolean walpe_ss_add_inert (WalpeSceneScript *ss, int idx, const char *prop, const char *src,
                             JsonObject *scriptprops, JsonObject *user_props,
                             const char *init_value)
{
    if (!ss || ss->dead || !src || !prop)
        return FALSE;
    return ss_add_common (ss, idx, -3, -1, prop, src, scriptprops, user_props, init_value);
}

gboolean walpe_ss_add_anim (WalpeSceneScript *ss, int idx, int effect, int pass, const char *prop,
                            const char *anim_json, const char *base_json)
{
    if (!ss || ss->dead || !prop || !anim_json)
        return FALSE;
    JSValue a[6] = { JS_NewInt32 (ss->ctx, idx), JS_NewInt32 (ss->ctx, effect),
                     JS_NewInt32 (ss->ctx, pass), JS_NewString (ss->ctx, prop),
                     JS_UNDEFINED, JS_UNDEFINED };
    gboolean ok = ss_json (ss, prop, anim_json, "<animation>", &a[4]) &&
                  (!(base_json && *base_json) || ss_json (ss, prop, base_json, "<value>", &a[5])) &&
                  ss_run (ss, prop, ss->fn[SS_FN_ADD_ANIM], 6, a, NULL, NULL, NULL);
    for (int k = 0; k < 6; k++)
        ss_free_value (ss, a[k]);
    if (ok)
        ss->count++;
    return ok;
}

void walpe_ss_set_prop_types (WalpeSceneScript *ss, JsonObject *types)
{
    if (!ss || ss->dead || !types)
        return;
    JsonNode *n = json_node_new (JSON_NODE_OBJECT);
    json_node_set_object (n, types);
    char *js = json_to_string (n, FALSE);
    json_node_unref (n);
    JSValue t;
    gboolean parsed = ss_json (ss, "option types", js, "<option types>", &t);
    g_free (js);
    if (!parsed)
        return;
    ss_run (ss, "option types", ss->fn[SS_FN_TYPES], 1, &t, NULL, NULL, NULL);
    ss_free_value (ss, t);
}

void walpe_ss_start (WalpeSceneScript *ss, JsonObject *user_props)
{
    if (!ss || !ss->count || ss->dead)
        return;
    JsonNode *n = json_node_new (JSON_NODE_OBJECT);
    json_node_set_object (n, user_props);
    char *js = json_to_string (n, FALSE);
    json_node_unref (n);
    JSValue up;
    gboolean parsed = ss_json (ss, "user properties", js, "<userProperties>", &up);
    g_free (js);
    if (!parsed)
        return;
    ss_run (ss, "init", ss->fn[SS_FN_START], 1, &up, NULL, NULL, NULL);
    ss_free_value (ss, up);
}

void walpe_ss_tick (WalpeSceneScript *ss, double frametime, double runtime,
                    float cursor_x, float cursor_y, gboolean left_down)
{
    if (!ss || !ss->count || ss->dead)
        return;
    JSValue a[5] = { JS_NewFloat64 (ss->ctx, frametime), JS_NewFloat64 (ss->ctx, runtime),
                     JS_NewFloat64 (ss->ctx, cursor_x), JS_NewFloat64 (ss->ctx, cursor_y),
                     JS_NewBool (ss->ctx, left_down) };
    ss_run (ss, "update", ss->fn[SS_FN_TICK], 5, a, NULL, NULL, NULL);
    /* collect cycles from per-frame Vec garbage now and then */
    static __thread guint n;
    if (++n % 120 == 0 && !ss->lost)
        JS_RunGC (ss->rt);
}

int walpe_ss_count (WalpeSceneScript *ss) { return ss ? ss->count : 0; }
gboolean walpe_ss_uses_audio (WalpeSceneScript *ss) { return ss && ss->uses_audio; }
gboolean walpe_ss_uses_media (WalpeSceneScript *ss) { return ss && ss->uses_media; }
gboolean walpe_ss_uses_cursor (WalpeSceneScript *ss) { return ss && ss->uses_cursor; }

void walpe_ss_media_event (WalpeSceneScript *ss, const char *kind, const char *event_json)
{
    if (!ss || !ss->count || !ss->uses_media || ss->dead)
        return;
    JSValue a[2];
    a[0] = JS_NewString (ss->ctx, kind);
    if (!ss_json (ss, kind, event_json, "<media>", &a[1])) {
        JS_FreeValue (ss->ctx, a[0]);
        return;
    }
    ss_run (ss, kind, ss->fn[SS_FN_MEDIA], 2, a, NULL, NULL, NULL);
    ss_free_value (ss, a[0]);
    ss_free_value (ss, a[1]);
}

int walpe_ss_cursor_layers (WalpeSceneScript *ss, int **out)
{
    *out = NULL;
    if (!ss || !ss->count || ss->dead)
        return 0;
    /* the prelude reports each layer through __host.cursorLayer: nothing
     * a script returns is read here (a script that replaced the function
     * C called used to hand back { length: 2147483647 }) */
    ss->collect = g_array_new (FALSE, FALSE, sizeof (int));
    ss_run (ss, "cursor", ss->fn[SS_FN_CURSOR_LAYERS], 0, NULL, NULL, NULL, NULL);
    int n = (int) ss->collect->len;
    GArray *c = ss->collect;
    ss->collect = NULL;
    if (n > 0)
        *out = (int *) g_array_free (c, FALSE);
    else
        g_array_free (c, TRUE);
    return n;
}

void walpe_ss_cursor_event (WalpeSceneScript *ss, const char *kind, int idx, float wx, float wy,
                            float lx, float ly)
{
    if (!ss || !ss->count || ss->dead)
        return;
    JSValue a[6] = { JS_NewString (ss->ctx, kind), JS_NewInt32 (ss->ctx, idx),
                     JS_NewFloat64 (ss->ctx, wx), JS_NewFloat64 (ss->ctx, wy),
                     JS_NewFloat64 (ss->ctx, lx), JS_NewFloat64 (ss->ctx, ly) };
    ss_run (ss, kind, ss->fn[SS_FN_CURSOR], 6, a, NULL, NULL, NULL);
    ss_free_value (ss, a[0]);
}

void walpe_ss_free (WalpeSceneScript *ss)
{
    if (!ss)
        return;
    if (ss->lost) {
        /* abandoned in the middle of a built-in: freeing it would walk
         * data that is half updated. Its heap (64 MiB at most) stays. */
        g_free (ss);
        return;
    }
    for (int i = 0; i < SS_FN_N; i++)
        JS_FreeValue (ss->ctx, ss->fn[i]);
    if (ss->ctx)
        JS_FreeValue (ss->ctx, ss->f32_proto);
    if (ss->ctx)
        JS_FreeContext (ss->ctx);
    if (ss->rt)
        JS_FreeRuntime (ss->rt);
    g_free (ss);
}

#endif
