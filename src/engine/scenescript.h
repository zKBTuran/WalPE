/* scenescript.h - Wallpaper Engine SceneScript (QuickJS).
 *
 * A scene object's property may carry {"script": "<JS module>",
 * "scriptproperties": {...}, "value": v}. The script exports init(value),
 * update(value) (called every frame; its return value becomes the
 * property), applyUserProperties(changed) and applyGeneralSettings. The
 * API is the one lib.sceneScript.d.ts (2.8) declares; every member exists.
 * Working: layer properties text / visible / alpha / origin / scale /
 * angles (degrees) / color / pointsize / maxwidth / padding / alignment,
 * parent links (getParent, getChildren, setParent, getTransformMatrix),
 * createScriptProperties(), engine.{frametime, runtime, timeOfDay,
 * canvasSize, userProperties, registerAudioBuffers, setTimeout /
 * setInterval}, thisLayer / thisObject, thisScene.getLayer*(), shared,
 * Vec2 / Vec3 / Vec4 / Mat3 / Mat4, WEMath, WEColor, WEVector,
 * localStorage (in memory), console, IEffect / IMaterial
 * (layer.getEffect(), scripts bound to an effect's visibility or to its
 * material constants), the media integration events
 * (walpe_ss_media_event), property keyframe animations with
 * animationEvent, cursor events on hit layers (walpe_ss_cursor_event),
 * and what V8 has and QuickJS lacks: toLocaleString / toLocaleDateString /
 * toLocaleTimeString with locale and options, Intl.DateTimeFormat /
 * NumberFormat, time zones, the legacy date strings Date.parse takes.
 * Present but without effect (the renderer has no such feature): bones,
 * animation layers, attachments, particle instance overrides, camera
 * transforms, created layers and model data, texture animation control.
 *
 * One QuickJS runtime per scene instance, used only on its render thread.
 * Without QuickJS (HAVE_QUICKJS unset) walpe_ss_new() returns NULL and the
 * static "value" is rendered as before. */
#pragma once
#include <glib.h>
#include <json-glib/json-glib.h>

typedef enum { WSS_NONE, WSS_NUM, WSS_BOOL, WSS_VEC, WSS_STR } WalpeSsType;

/* scene callbacks (render thread) for layer property access by index */
typedef struct {
    gpointer user;
    int (*find) (gpointer user, const char *name);            /* -1 = none */
    const char *(*name) (gpointer user, int idx);
    /* fills v (up to 4) / *s (borrowed); returns the type and *n = vec size */
    WalpeSsType (*get) (gpointer user, int idx, const char *prop, double *v, int *n,
                        const char **s);
    void (*set) (gpointer user, int idx, const char *prop, WalpeSsType t,
                 const double *v, int n, const char *s);
    /* audio spectrum for registerAudioBuffers (res = 16/32/64) */
    void (*audio) (gpointer user, int res, float *left, float *right);
    /* effects of layer idx (index = position in its "effects" array):
     * key "visible" with pass -1 = the effect's visibility (WSS_BOOL),
     * else a material constant by its constantshadervalues name (WSS_VEC
     * with n = 1..4); pass -1 = any/all passes. WSS_NONE = unknown. */
    WalpeSsType (*fx_get) (gpointer user, int idx, int effect, int pass, const char *key,
                           double *v, int *n);
    void (*fx_set) (gpointer user, int idx, int effect, int pass, const char *key,
                    WalpeSsType t, const double *v, int n);
    int (*fx_find) (gpointer user, int idx, const char *name);   /* -1 = none */
    const char *(*fx_name) (gpointer user, int idx, int effect);
    int (*fx_count) (gpointer user, int idx);
} WalpeSsHost;

typedef struct WalpeSceneScript WalpeSceneScript;

/* canvas = scene size; user_props = {prop: value} (copied as JSON) */
WalpeSceneScript *walpe_ss_new (const WalpeSsHost *host, float canvas_w, float canvas_h,
                                JsonObject *user_props);
/* is prop a layer property the runtime can drive? */
gboolean walpe_ss_supported_prop (const char *prop);
/* compile one property script of layer idx; FALSE on syntax/runtime error */
gboolean walpe_ss_add (WalpeSceneScript *ss, int idx, const char *prop, const char *src,
                       JsonObject *scriptprops, JsonObject *user_props);
/* a script on an effect of layer idx: pass < 0 and prop "visible" = the
 * effect's visibility (thisObject = IEffect), else the material constant
 * `prop` of that pass (thisObject = IMaterial; init_value = the static
 * "value" as JSON, e.g. "\"0.25 0.76\"" or "1") */
/* WE property keyframe animation ({"animation": {...}} on a layer property
 * (effect = -1) or an effect constant): played by the runtime, scriptable
 * through getAnimation(); base_json = the property's static value (JSON) */
gboolean walpe_ss_add_anim (WalpeSceneScript *ss, int idx, int effect, int pass, const char *prop,
                            const char *anim_json, const char *base_json);
gboolean walpe_ss_add_fx (WalpeSceneScript *ss, int idx, int effect, int pass, const char *prop,
                          const char *src, JsonObject *scriptprops, JsonObject *user_props,
                          const char *init_value);
/* a script on a property the scene does not drive (idx -1 = no layer, the
 * scene's general section): it runs like any other - init, update, events,
 * shared state - on the static value (JSON, may be NULL); what it returns
 * stays in the runtime */
gboolean walpe_ss_add_inert (WalpeSceneScript *ss, int idx, const char *prop, const char *src,
                             JsonObject *scriptprops, JsonObject *user_props,
                             const char *init_value);
/* media integration events (WE IComponent media* functions): kind =
 * "mediaStatusChanged" | "mediaPlaybackChanged" | "mediaPropertiesChanged"
 * | "mediaThumbnailChanged" | "mediaTimelineChanged", event as JSON
 * (arrays named *Color become Vec3) */
void walpe_ss_media_event (WalpeSceneScript *ss, const char *kind, const char *event_json);
/* some script exports a media* event function */
gboolean walpe_ss_uses_media (WalpeSceneScript *ss);
/* some script reads input.cursorWorldPosition/ScreenPosition/LeftDown */
gboolean walpe_ss_uses_cursor (WalpeSceneScript *ss);
/* before walpe_ss_start: the declared type of each user option ({key:
 * "color" | "bool" | "textinput" ...}, project.json). Only colour options
 * then reach scripts as Vec3; a text option holding "10 20 30" stays text.
 * Without it values are typed by their look. */
void walpe_ss_set_prop_types (WalpeSceneScript *ss, JsonObject *types);
/* after all adds: init() + applyUserProperties(user_props) */
void walpe_ss_start (WalpeSceneScript *ss, JsonObject *user_props);
/* per frame: timers, audio buffers, update() of every script; cursor =
 * scene coordinates (input.cursorWorldPosition), left_down =
 * input.cursorLeftDown */
void walpe_ss_tick (WalpeSceneScript *ss, double frametime, double runtime,
                    float cursor_x, float cursor_y, gboolean left_down);
/* layers (indices) with a script exporting a cursor* event function;
 * *out is g_free()d by the caller */
int walpe_ss_cursor_layers (WalpeSceneScript *ss, int **out);
/* kind = "cursorDown" | "cursorUp" | "cursorClick" | "cursorEnter" |
 * "cursorLeave" | "cursorMove": calls that function of every script on
 * layer idx (world = scene coordinates, local = relative to the layer) */
void walpe_ss_cursor_event (WalpeSceneScript *ss, const char *kind, int idx, float wx, float wy,
                            float lx, float ly);
int walpe_ss_count (WalpeSceneScript *ss);
gboolean walpe_ss_uses_audio (WalpeSceneScript *ss);
void walpe_ss_free (WalpeSceneScript *ss);
