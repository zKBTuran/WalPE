/* layeradj.h - "Adjust layers": the user's own placement of scene layers.
 *
 * Per wallpaper (keyed by its path, like the saved options) a list of
 * adjustments {layer key, dx, dy, scale, visible}: move a layer by
 * (dx, dy) scene pixels (x right, y UP, the axes of scene.json), scale it
 * about its own origin, hide it or force it shown. The scene applies them
 * as the last transform of the layer, after option binding, animations and
 * SceneScript (scenenode_layers.h); children follow their parent.
 *
 * Nothing here knows a particular wallpaper: the engine only gets
 * "layer K of the scene loaded from path P is moved by ...".
 *
 * Saved in config.json next to "props":
 *   "layers": { "<wallpaper path>": [ { "key": "123:Cover", "dx": 420,
 *               "dy": -310, "scale": 1, "visible": "default" }, ... ] }
 *
 * Layer key: "<id>:<name>" - the scene object's "id" (primary) and its
 * name (fallback when the id is gone, e.g. after a Workshop update that
 * renumbered the objects). "-" stands for "no id". When several objects
 * share id and name the second one is "<id>~2:<name>", the third "~3".
 * A key that matches no layer of the loaded scene is ignored and kept.
 *
 * Threads: the daemon's main thread changes the lists; scene render
 * threads read them. Everything below locks internally. */
#pragma once
#include <glib.h>
#include <json-glib/json-glib.h>
#include "common/config.h"

G_BEGIN_DECLS

typedef enum {
    WALPE_LAYER_VIS_DEFAULT = 0,   /* as the wallpaper decides */
    WALPE_LAYER_VIS_HIDDEN  = 1,
    WALPE_LAYER_VIS_SHOWN   = 2,   /* shown even where the wallpaper hides it */
} WalpeLayerVis;

/* accepted ranges (anything else is refused, never clamped silently) */
#define WALPE_LAYER_OFFSET_MAX 100000.0   /* |dx|, |dy|, scene pixels */
#define WALPE_LAYER_SCALE_MIN  0.01
#define WALPE_LAYER_SCALE_MAX  100.0
#define WALPE_LAYER_KEY_MAX    512        /* bytes */
#define WALPE_LAYER_MAX        4096       /* adjustments per wallpaper */

typedef struct {
    char  *key;
    double dx, dy;    /* scene pixels; +x right, +y up */
    double scale;     /* multiplier, 1 = unchanged */
    int    vis;       /* WalpeLayerVis */
} WalpeLayerAdj;

WalpeLayerAdj *walpe_layer_adj_new (const char *key);   /* dx dy 0, scale 1, default */
WalpeLayerAdj *walpe_layer_adj_copy (const WalpeLayerAdj *a);
void           walpe_layer_adj_free (WalpeLayerAdj *a);
/* changes nothing (offset 0, scale 1, visibility default) */
gboolean       walpe_layer_adj_is_noop (const WalpeLayerAdj *a);
/* NULL = acceptable, else why not (static English text) */
const char    *walpe_layer_adj_check (const WalpeLayerAdj *a);
/* "default" | "hidden" | "shown" (also unset / hide / show / true / false) */
gboolean       walpe_layer_vis_parse (const char *s, int *vis);
const char    *walpe_layer_vis_name (int vis);

/* ---------------------------------------------------------------- keys */

/* what a key is made of: one entry per layer of a loaded scene, in scene
 * order (id < 0 = the object has none, name may be NULL; an id above
 * 999999999, which the nine digits of a key cannot hold, counts as none
 * too) */
typedef struct { int id; const char *name; } WalpeLayerRef;

/* "<id>[~n]:<name>"; nth = 0 for the first layer with this id and name */
char    *walpe_layer_key_make (int id, int nth, const char *name);
/* FALSE = not a key. *name points into key. */
gboolean walpe_layer_key_parse (const char *key, int *id, int *nth, const char **name);
/* the key of layer i */
char    *walpe_layer_key_for (const WalpeLayerRef *l, guint n, guint i);
/* the layer a saved key means, or -1:
 *   1. the nth layer with this id and this name
 *   2. the id exists under another name (the layer was renamed): the nth
 *      layer with this id
 *   3. no layer has the id (or the key has none): the layer with this
 *      name - the nth one for a key without id, the only one for a key
 *      with id (several candidates = ambiguous = no match) */
int      walpe_layer_find (const WalpeLayerRef *l, guint n, const char *key);
/* a selector typed by a person (walpe-ctl): a key, a bare object id
 * ("123") or a bare name. Appends the matching layer indices to out
 * (GArray of int). FALSE + *err (g_free) when nothing or, for a name,
 * more than one layer matches. */
gboolean walpe_layer_select (const WalpeLayerRef *l, guint n, const char *sel, GArray *out,
                             char **err);

/* selections on the layer tree (parent[i] = index or -1) */
/* roots[i] = sel[i] and no ancestor of i is selected: a group is moved by
 * moving its roots, the rest follows */
void walpe_layer_sel_roots (const int *parent, guint n, const gboolean *sel, gboolean *roots);
/* add every descendant of a selected layer */
void walpe_layer_sel_children (const int *parent, guint n, gboolean *sel);

/* -------------------------------------------------------------- config */

/* hooks of config.c: read / write the "layers" member, free */
void walpe_layers_config_read (WalpeConfig *c, JsonObject *root);
void walpe_layers_config_write (WalpeConfig *c, JsonBuilder *b);
void walpe_layers_config_free (WalpeConfig *c);

/* the adjustments saved for a wallpaper: copies (GPtrArray of
 * WalpeLayerAdj*, freed with the array); never NULL */
GPtrArray *walpe_layers_get (WalpeConfig *c, const char *path);
/* save one adjustment under a->key. old_key (may be NULL): the spelling
 * the same layer was saved under before (a key matched by its name
 * fallback) - that entry is replaced. An adjustment that changes nothing
 * removes the entry. FALSE + *why (static) when it is not acceptable. */
gboolean   walpe_layers_put (WalpeConfig *c, const char *path, const WalpeLayerAdj *a,
                             const char *old_key, const char **why);
/* drop one adjustment (key) or all of a wallpaper (key NULL); returns how
 * many went */
guint      walpe_layers_remove (WalpeConfig *c, const char *path, const char *key);
guint      walpe_layers_count (WalpeConfig *c, const char *path);

/* -------------------------------------- scene render threads (engine) */

/* changes whenever any list, a highlight or a list request changed: a
 * scene compares it with the value it last saw (one atomic read per
 * frame) and only then looks again */
guint      walpe_layers_generation (void);
/* adjustments of the config last loaded (the daemon's) for path; NULL
 * when there are none */
GPtrArray *walpe_layers_active (const char *path);
/* keys to outline on the wallpaper (g_strfreev), NULL = none */
char     **walpe_layers_highlight (const char *path);
/* serial of the newest request for layer lists (0 = never asked) */
guint64    walpe_layers_list_serial (void);
/* a scene hands over its layer list (JSON text, taken) for the
 * request `serial`; owner identifies the scene instance. With its first
 * frame a scene calls this with serial 0 and json NULL: "a scene showing
 * path draws now" (before that a request for its list need not wait). */
void       walpe_layers_publish (gpointer owner, const char *path, guint64 serial, char *json);
void       walpe_layers_unpublish (gpointer owner);

/* ------------------------------------------------ daemon main thread */

/* a list changed outside walpe_layers_put/remove (config reloaded) */
void       walpe_layers_changed (void);
/* keys NULL / n 0 = no outline for path */
void       walpe_layers_set_highlight (const char *path, const char *const *keys, guint n);
/* TRUE when some path has an outline */
gboolean   walpe_layers_any_highlight (void);
void       walpe_layers_clear_highlights (void);
/* a scene showing path has drawn a frame (and was not freed since) */
gboolean   walpe_layers_scene_alive (const char *path);
/* ask the running scenes for their layer lists; returns the request's
 * serial for walpe_layers_published */
guint64    walpe_layers_request_list (void);
/* the list a scene showing path published (JSON text, g_free), or
 * NULL when no scene shows it. Waits up to wait_ms for one that answers
 * request `serial`; *fresh = it does (else: the last one published). No
 * wait when no scene showing path has drawn a frame yet (it is loading,
 * or nothing shows it). */
char      *walpe_layers_published (const char *path, guint64 serial, int wait_ms,
                                   gboolean *fresh);

G_END_DECLS
