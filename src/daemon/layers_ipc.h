/* layers_ipc.h - the daemon's "Adjust layers" requests.
 *
 * Not a public header: main.c includes it once, after its IPC helpers
 * (jb_*, ipc_target_path) - the handlers live here so that main.c only
 * carries the hooks. Data model and rules: common/layeradj.h.
 *
 * Every request names a wallpaper the way "props" does: {"monitor": N} =
 * what monitor N shows, {"path": P} = that wallpaper, neither = the
 * default one. Adjustments are saved per wallpaper path (two monitors
 * showing the same wallpaper share them, like its options).
 *
 * {"cmd":"layers"}
 *     -> {"ok":true, "path", "scene":bool, "running":bool, "loading":bool,
 *         "fresh":bool,
 *         "size":[w,h], "layers":[{key,id,name,type,parent,visible,shown,
 *         pos:[x,y], size:[w,h], bounds:[x0,y0,x1,y1], text|file,
 *         adjust:{key,dx,dy,scale,visible}}], "adjustments":[...]}
 *     The list comes from the running scene (this frame's positions, in
 *     scene pixels, y up; "parent" = index into "layers" or -1).
 * {"cmd":"layeradj","op":...}
 *     "get"                                    the saved adjustments
 *     "set",  "layers":[{"key":K, "dx":, "dy":, "scale":, "visible":}]
 *                                              absolute; members left out keep
 *                                              their saved value
 *     "move", "keys":[K..], "dx":, "dy":       add an offset to each (a layer
 *                                              whose parent is also named moves
 *                                              with the parent, not twice)
 *     "scale", "keys":[K..], "factor":         scale the group about the centre
 *                                              of its bounds
 *     "reset", "keys":[K..]                    drop them; no "keys" = all
 *     "highlight", "keys":[K..], "seconds":    outline them on the wallpaper
 *                                              for a while (not saved); no
 *                                              keys = off
 *     "children": true with "keys"            also every layer below them
 *     -> {"ok":true, "changed":n, "adjustments":[{key,dx,dy,scale,visible,
 *         missing}]}
 *   K: a key from "layers", or - while the scene runs - a bare object id
 *   or a layer name. Numbers must be finite and inside layeradj.h's
 *   ranges; a request with one bad value changes nothing.
 *   Changes are applied by the running scene at its next frame (no
 *   reload) and saved in config.json. */
#pragma once
#include "common/layeradj.h"

/* how long a request waits for the running scene's next frame (which
 * carries the layer list). A frame is a few ms on a GPU; a software
 * renderer drawing a 4K scene needs most of a second. */
#define LAYERS_WAIT_MS 1500
#define LAYERS_HL_DEFAULT_S 3.0
#define LAYERS_HL_MAX_S 60.0
#define LAYERS_MAX_KEYS 4096

/* the wallpaper a request is about, spelled the way the monitor showing
 * it has it (the scene looks its adjustments up under the path it was
 * opened with: "~/x", a symlinked library and the like must not make a
 * second list). Own copy: g_free. NULL = none. */
static char *layers_target (JsonObject *o)
{
    const char *tp = ipc_target_path (o);
    if (!tp)
        return NULL;
    for (GList *l = D.mons; l; l = l->next) {
        Mon *m = l->data;
        if (m->want_path && walpe_path_same (m->want_path, tp))
            return g_strdup (m->want_path);
    }
    return g_strdup (tp);
}

/* some monitor shows tp as a scene */
static gboolean layers_is_shown (const char *tp)
{
    for (GList *l = D.mons; l; l = l->next) {
        Mon *m = l->data;
        if (!g_strcmp0 (m->want_path, tp) &&
            g_atomic_int_get (&mon_route (m)->content_type) == WALPE_TYPE_SCENE)
            return TRUE;
    }
    return FALSE;
}

/* the monitors showing tp draw one frame now, also while paused or
 * covered: the user asked for a change he wants to see (or for the layer
 * list, which a scene publishes with a frame) */
static void layers_redraw (const char *tp)
{
    gint64 until = g_get_monotonic_time () + G_USEC_PER_SEC;
    for (GList *l = D.mons; l; l = l->next) {
        Mon *m = l->data;
        if (g_strcmp0 (m->want_path, tp))
            continue;
        Mon *r = mon_route (m);
        g_mutex_lock (&r->lock);
        r->force_frame = TRUE;
        r->render_req = TRUE;
        g_cond_signal (&r->cond);
        g_mutex_unlock (&r->lock);
        if (r != m) {
            /* a follower presents what its leader publishes next */
            g_mutex_lock (&m->lock);
            m->force_until_us = until;
            g_mutex_unlock (&m->lock);
        }
    }
}

/* ------------------------------------------------ a scene's layer list */

typedef struct {
    JsonNode *root;        /* the published {"size", "layers"} */
    JsonArray *arr;
    guint n;
    WalpeLayerRef *refs;   /* names borrowed from root */
    int *parent;
    const char **keys;     /* borrowed from root */
    gboolean fresh;        /* from a frame drawn for this request */
} LayerList;

static void layer_list_free (LayerList *ll)
{
    if (!ll)
        return;
    json_node_unref (ll->root);
    g_free (ll->refs);
    g_free (ll->parent);
    g_free (ll->keys);
    g_free (ll);
}

/* NULL = no scene shows tp (or it did not answer in time). fresh = FALSE
 * accepts the list the scene published last (enough to resolve keys and
 * parents: they do not change while a scene is loaded). */
static LayerList *layer_list_get (const char *tp, gboolean fresh)
{
    if (!tp || !layers_is_shown (tp))
        return NULL;
    gboolean got = FALSE;
    char *json = fresh ? NULL : walpe_layers_published (tp, 0, 0, NULL);
    if (!json) {
        guint64 serial = walpe_layers_request_list ();
        layers_redraw (tp);
        json = walpe_layers_published (tp, serial, LAYERS_WAIT_MS, &got);
        if (json && !got && fresh)
            g_debug ("layers: %s did not draw a frame within %d ms; its last list is used", tp,
                     LAYERS_WAIT_MS);
    }
    JsonNode *root = json ? wpj_parse_data (json, -1) : NULL;
    g_free (json);
    JsonArray *arr = root && JSON_NODE_HOLDS_OBJECT (root)
        ? wpj_arr (json_node_get_object (root), "layers") : NULL;
    if (!arr) {
        if (root)
            json_node_unref (root);
        return NULL;
    }
    LayerList *ll = g_new0 (LayerList, 1);
    ll->root = root;
    ll->arr = arr;
    ll->n = json_array_get_length (arr);
    ll->fresh = got;
    ll->refs = g_new0 (WalpeLayerRef, ll->n ? ll->n : 1);
    ll->parent = g_new0 (int, ll->n ? ll->n : 1);
    ll->keys = g_new0 (const char *, ll->n ? ll->n : 1);
    for (guint i = 0; i < ll->n; i++) {
        JsonObject *lo = wpj_arr_obj (arr, i);
        ll->refs[i].id = (int) wpj_num (lo, "id", -1);
        ll->refs[i].name = wpj_str (lo, "name", "");
        ll->parent[i] = (int) wpj_num (lo, "parent", -1);
        ll->keys[i] = wpj_str (lo, "key", "");
    }
    return ll;
}

/* the saved adjustments of tp and which layer each one means */
typedef struct {
    GPtrArray *saved;      /* WalpeLayerAdj* (copies) */
    int *of_layer;         /* per layer: index into saved, -1 = none */
    gboolean *matched;     /* per saved entry: some layer of the list has it */
} SavedMap;

static void saved_map_free (SavedMap *sm)
{
    g_ptr_array_unref (sm->saved);
    g_free (sm->of_layer);
    g_free (sm->matched);
}

static void saved_map_init (SavedMap *sm, const char *tp, LayerList *ll)
{
    sm->saved = walpe_layers_get (D.cfg, tp);
    sm->of_layer = g_new (int, ll && ll->n ? ll->n : 1);
    sm->matched = g_new0 (gboolean, sm->saved->len ? sm->saved->len : 1);
    for (guint i = 0; ll && i < ll->n; i++)
        sm->of_layer[i] = -1;
    for (guint k = 0; ll && k < sm->saved->len; k++) {
        int i = walpe_layer_find (ll->refs, ll->n, ((WalpeLayerAdj *) sm->saved->pdata[k])->key);
        if (i >= 0) {
            sm->of_layer[i] = (int) k;   /* as in the scene: the later entry counts */
            sm->matched[k] = TRUE;
        }
    }
}

static void layers_jb_adjustments (JsonBuilder *b, const char *tp, LayerList *ll)
{
    SavedMap sm;
    saved_map_init (&sm, tp, ll);
    json_builder_set_member_name (b, "adjustments");
    json_builder_begin_array (b);
    for (guint k = 0; k < sm.saved->len; k++) {
        const WalpeLayerAdj *a = sm.saved->pdata[k];
        json_builder_begin_object (b);
        jb_str (b, "key", a->key);
        json_builder_set_member_name (b, "dx");
        json_builder_add_double_value (b, a->dx);
        json_builder_set_member_name (b, "dy");
        json_builder_add_double_value (b, a->dy);
        json_builder_set_member_name (b, "scale");
        json_builder_add_double_value (b, a->scale);
        jb_str (b, "visible", walpe_layer_vis_name (a->vis));
        if (ll)   /* no layer of the running scene has this key: ignored, kept */
            jb_bool (b, "missing", !sm.matched[k]);
        json_builder_end_object (b);
    }
    json_builder_end_array (b);
    saved_map_free (&sm);
}

/* {"cmd":"layers"} */
static void ipc_layers (JsonBuilder *b, JsonObject *o)
{
    char *tp = layers_target (o);
    if (!tp) {
        jb_fail (b, "no active wallpaper (or no such monitor)");
        return;
    }
    gboolean shown = layers_is_shown (tp);
    LayerList *ll = layer_list_get (tp, TRUE);
    jb_bool (b, "ok", TRUE);
    jb_str (b, "path", tp);
    jb_bool (b, "scene", shown || walpe_detect_type (tp, NULL) == WALPE_TYPE_SCENE);
    jb_bool (b, "running", shown);
    /* a monitor is to show it and its scene has not handed a list over yet
     * (being loaded, or its first frame is not out): ask again */
    gboolean wanted = FALSE;
    for (GList *l = D.mons; l && !wanted; l = l->next)
        wanted = !g_strcmp0 (((Mon *) l->data)->want_path, tp);
    jb_bool (b, "loading", !ll && wanted && walpe_detect_type (tp, NULL) == WALPE_TYPE_SCENE);
    jb_bool (b, "fresh", ll && ll->fresh);
    if (ll) {
        JsonObject *ro = json_node_get_object (ll->root);
        JsonNode *sz = json_object_get_member (ro, "size");
        if (sz) {
            json_builder_set_member_name (b, "size");
            json_builder_add_value (b, json_node_copy (sz));
        }
        json_builder_set_member_name (b, "layers");
        json_builder_add_value (b, json_node_copy (json_object_get_member (ro, "layers")));
    } else {
        json_builder_set_member_name (b, "layers");
        json_builder_begin_array (b);
        json_builder_end_array (b);
    }
    layers_jb_adjustments (b, tp, ll);
    layer_list_free (ll);
    g_free (tp);
}

/* ------------------------------------------------------ "layeradj" */

/* 1 = a finite number, 0 = no such member, -1 = something else */
static int layers_num (JsonObject *o, const char *k, double *out)
{
    JsonNode *n = json_object_get_member (o, k);
    if (!n)
        return 0;
    if (!JSON_NODE_HOLDS_VALUE (n))
        return -1;
    GType t = json_node_get_value_type (n);
    if (t != G_TYPE_INT64 && t != G_TYPE_DOUBLE)
        return -1;
    double d = json_node_get_double (n);
    if (!isfinite (d))
        return -1;
    *out = d;
    return 1;
}

typedef struct { WalpeLayerAdj *a; char *old_key; } LayerEdit;

static void layer_edit_free (gpointer p)
{
    LayerEdit *e = p;
    walpe_layer_adj_free (e->a);
    g_free (e->old_key);
    g_free (e);
}

/* the adjustment to change for layer i of the list (i < 0: for `key`,
 * taken as it is): a copy of the saved one, or a fresh one */
static LayerEdit *layer_edit_for (GPtrArray *edits, LayerList *ll, SavedMap *sm, int i,
                                  const char *key)
{
    const char *canon = i >= 0 ? ll->keys[i] : key;
    for (guint k = 0; k < edits->len; k++) {
        LayerEdit *e = edits->pdata[k];
        if (!strcmp (e->a->key, canon))
            return e;   /* named twice in one request: one record */
    }
    const WalpeLayerAdj *have = NULL;
    if (i >= 0 && sm->of_layer[i] >= 0)
        have = sm->saved->pdata[sm->of_layer[i]];
    for (guint k = 0; i < 0 && !have && k < sm->saved->len; k++)
        if (!strcmp (((WalpeLayerAdj *) sm->saved->pdata[k])->key, key))
            have = sm->saved->pdata[k];
    LayerEdit *e = g_new0 (LayerEdit, 1);
    e->a = have ? walpe_layer_adj_copy (have) : walpe_layer_adj_new (canon);
    if (have && strcmp (have->key, canon)) {
        e->old_key = g_strdup (have->key);   /* saved under an older spelling */
        g_free (e->a->key);
        e->a->key = g_strdup (canon);
    }
    g_ptr_array_add (edits, e);
    return e;
}

/* one selector -> layer indices (list) or the key itself (no list).
 * FALSE + the reply's error. */
static gboolean layers_resolve (JsonBuilder *b, LayerList *ll, const char *sel, GArray *idx)
{
    if (ll) {
        char *err = NULL;
        if (!walpe_layer_select (ll->refs, ll->n, sel, idx, &err)) {
            jb_fail (b, err ? err : "no such layer");
            g_free (err);
            return FALSE;
        }
        return TRUE;
    }
    if (!sel || !walpe_layer_key_parse (sel, NULL, NULL, NULL)) {
        jb_fail (b, "this wallpaper is not shown on a monitor: only layer keys (\"<id>:<name>\") "
                    "can be used, not ids or names");
        return FALSE;
    }
    return TRUE;
}

/* "keys": [..] (+ "children") -> sel[] over the list, or (no list / a
 * saved key that matches no layer, when unmatched is given) the raw keys.
 * FALSE + the reply's error. */
static gboolean layers_selection (JsonBuilder *b, JsonObject *o, LayerList *ll, SavedMap *sm,
                                  gboolean **sel_out, GPtrArray *raw, gboolean allow_unmatched,
                                  guint *count)
{
    JsonArray *ka = wpj_arr (o, "keys");
    guint nk = wpj_arr_len (ka);
    *count = 0;
    if (json_object_has_member (o, "keys") && !ka) {
        jb_fail (b, "\"keys\" must be an array of layer keys");
        return FALSE;
    }
    if (nk > LAYERS_MAX_KEYS) {
        jb_fail (b, "too many keys");
        return FALSE;
    }
    gboolean *sel = g_new0 (gboolean, ll && ll->n ? ll->n : 1);
    GArray *idx = g_array_new (FALSE, FALSE, sizeof (int));
    for (guint k = 0; k < nk; k++) {
        JsonNode *kn = json_array_get_element (ka, k);
        const char *ks = kn && JSON_NODE_HOLDS_VALUE (kn) &&
                         json_node_get_value_type (kn) == G_TYPE_STRING
            ? json_node_get_string (kn) : NULL;
        if (!ks) {
            jb_fail (b, "\"keys\" must be an array of layer keys");
            goto fail;
        }
        gboolean saved_only = FALSE;
        for (guint q = 0; ll && allow_unmatched && q < sm->saved->len; q++)
            saved_only = saved_only || (!sm->matched[q] &&
                         !strcmp (((WalpeLayerAdj *) sm->saved->pdata[q])->key, ks));
        if (saved_only) {
            g_ptr_array_add (raw, g_strdup (ks));
            (*count)++;
            continue;
        }
        g_array_set_size (idx, 0);
        if (!layers_resolve (b, ll, ks, idx))
            goto fail;
        if (!ll) {
            g_ptr_array_add (raw, g_strdup (ks));
            (*count)++;
        }
        for (guint q = 0; q < idx->len; q++)
            sel[g_array_index (idx, int, q)] = TRUE;
    }
    if (ll && wpj_bool (o, "children", FALSE))
        walpe_layer_sel_children (ll->parent, ll->n, sel);
    for (guint i = 0; ll && i < ll->n; i++)
        *count += sel[i];
    g_array_unref (idx);
    *sel_out = sel;
    return TRUE;
fail:
    g_array_unref (idx);
    g_free (sel);
    return FALSE;
}

/* outlines end by themselves: a GUI that died must not leave them */
static GHashTable *hl_until;   /* path -> gint64* (monotonic us) */
static guint hl_timer;

static gboolean hl_tick (gpointer u)
{
    (void) u;
    gint64 now = g_get_monotonic_time ();
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init (&it, hl_until);
    while (g_hash_table_iter_next (&it, &k, &v))
        if (*(gint64 *) v <= now) {
            walpe_layers_set_highlight (k, NULL, 0);
            layers_redraw (k);
            g_hash_table_iter_remove (&it);
        }
    if (g_hash_table_size (hl_until))
        return G_SOURCE_CONTINUE;
    hl_timer = 0;
    return G_SOURCE_REMOVE;
}

static void layers_highlight (const char *tp, const char *const *keys, guint n, double seconds)
{
    if (!hl_until)
        hl_until = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
    walpe_layers_set_highlight (tp, keys, n);
    if (n) {
        gint64 *t = g_new (gint64, 1);
        *t = g_get_monotonic_time () + (gint64) (seconds * 1e6);
        g_hash_table_insert (hl_until, g_strdup (tp), t);
        if (!hl_timer)
            hl_timer = g_timeout_add (200, hl_tick, NULL);
    } else {
        g_hash_table_remove (hl_until, tp);
    }
    layers_redraw (tp);
}

/* {"cmd":"layeradj"} */
static void ipc_layeradj (JsonBuilder *b, JsonObject *o)
{
    const char *op = wpj_str (o, "op", "get");
    char *tp_own = layers_target (o);
    const char *tp = tp_own;
    if (!tp) {
        jb_fail (b, "no active wallpaper (or no such monitor)");
        return;
    }
    gboolean is_get = !strcmp (op, "get"), is_set = !strcmp (op, "set");
    gboolean is_move = !strcmp (op, "move"), is_scale = !strcmp (op, "scale");
    gboolean is_reset = !strcmp (op, "reset"), is_hl = !strcmp (op, "highlight");
    if (!is_get && !is_set && !is_move && !is_scale && !is_reset && !is_hl) {
        jb_fail (b, "unknown op (get, set, move, scale, reset, highlight)");
        g_free (tp_own);
        return;
    }
    /* a group is scaled about where its layers are now: this frame's list */
    LayerList *ll = is_get ? NULL : layer_list_get (tp, is_scale);
    SavedMap sm;
    saved_map_init (&sm, tp, ll);
    GPtrArray *edits = g_ptr_array_new_with_free_func (layer_edit_free);
    GPtrArray *raw = g_ptr_array_new_with_free_func (g_free);
    gboolean *sel = NULL;
    guint nsel = 0, changed = 0;
    gboolean ok = TRUE;

    if (is_set) {
        JsonArray *la = wpj_arr (o, "layers");
        guint nl = wpj_arr_len (la);
        if (!nl || nl > LAYERS_MAX_KEYS) {
            jb_fail (b, "\"set\" needs \"layers\": [{\"key\": K, \"dx\": .., \"dy\": .., "
                        "\"scale\": .., \"visible\": ..}]");
            ok = FALSE;
        }
        GArray *idx = g_array_new (FALSE, FALSE, sizeof (int));
        for (guint k = 0; ok && k < nl; k++) {
            JsonObject *lo = wpj_arr_obj (la, k);
            const char *ks = lo ? wpj_str (lo, "key", NULL) : NULL;
            double dx = 0, dy = 0, sc = 1;
            int hx = lo ? layers_num (lo, "dx", &dx) : 0, hy = lo ? layers_num (lo, "dy", &dy) : 0;
            int hs = lo ? layers_num (lo, "scale", &sc) : 0, vis = 0;
            gboolean hv = lo && json_object_has_member (lo, "visible");
            if (!ks) {
                jb_fail (b, "every entry of \"layers\" needs a \"key\"");
                ok = FALSE;
            } else if (hx < 0 || hy < 0 || hs < 0) {
                jb_fail (b, "dx, dy and scale must be finite numbers");
                ok = FALSE;
            } else if (hv && !walpe_layer_vis_parse (wpj_str (lo, "visible", NULL), &vis)) {
                jb_fail (b, "visible must be \"default\", \"hidden\" or \"shown\"");
                ok = FALSE;
            }
            if (!ok)
                break;
            g_array_set_size (idx, 0);
            if (!layers_resolve (b, ll, ks, idx)) {
                ok = FALSE;
                break;
            }
            for (guint q = 0; q < (ll ? idx->len : 1); q++) {
                LayerEdit *e = layer_edit_for (edits, ll, &sm, ll ? g_array_index (idx, int, q) : -1,
                                               ks);
                if (hx > 0)
                    e->a->dx = dx;
                if (hy > 0)
                    e->a->dy = dy;
                if (hs > 0)
                    e->a->scale = sc;
                if (hv)
                    e->a->vis = vis;
            }
        }
        g_array_unref (idx);
    } else if (!is_get) {
        ok = layers_selection (b, o, ll, &sm, &sel, raw, is_reset, &nsel);
        if (ok && !nsel && (is_move || is_scale)) {
            jb_fail (b, "no layers named (\"keys\": [..])");
            ok = FALSE;
        }
    }

    if (ok && (is_move || is_scale)) {
        double dx = 0, dy = 0, f = 1;
        int hx = layers_num (o, "dx", &dx), hy = layers_num (o, "dy", &dy);
        int hf = layers_num (o, "factor", &f);
        if (hx < 0 || hy < 0 || hf < 0 || (is_move && hx + hy == 0) || (is_scale && hf != 1)) {
            jb_fail (b, is_move ? "\"move\" needs finite numbers \"dx\" and / or \"dy\""
                                : "\"scale\" needs a finite number \"factor\"");
            ok = FALSE;
        } else if (is_scale && (f < WALPE_LAYER_SCALE_MIN || f > WALPE_LAYER_SCALE_MAX)) {
            jb_fail (b, "factor must be within 0.01 .. 100");
            ok = FALSE;
        }
        if (ok && ll) {
            /* the group's roots; scaling: about the centre of their bounds */
            gboolean *roots = g_new0 (gboolean, ll->n ? ll->n : 1);
            walpe_layer_sel_roots (ll->parent, ll->n, sel, roots);
            double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
            guint nr = 0;
            for (guint i = 0; i < ll->n; i++) {
                if (!roots[i])
                    continue;
                JsonObject *lo = wpj_arr_obj (ll->arr, i);
                JsonArray *bb = wpj_arr (lo, "bounds"), *ps = wpj_arr (lo, "pos");
                double bx0, by0, bx1, by1;
                if (wpj_arr_len (bb) >= 4) {
                    bx0 = json_array_get_double_element (bb, 0);
                    by0 = json_array_get_double_element (bb, 1);
                    bx1 = json_array_get_double_element (bb, 2);
                    by1 = json_array_get_double_element (bb, 3);
                } else {
                    bx0 = bx1 = wpj_arr_len (ps) >= 2 ? json_array_get_double_element (ps, 0) : 0;
                    by0 = by1 = wpj_arr_len (ps) >= 2 ? json_array_get_double_element (ps, 1) : 0;
                }
                x0 = MIN (x0, bx0); y0 = MIN (y0, by0);
                x1 = MAX (x1, bx1); y1 = MAX (y1, by1);
                nr++;
            }
            double cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
            for (guint i = 0; i < ll->n; i++) {
                if (!roots[i])
                    continue;
                LayerEdit *e = layer_edit_for (edits, ll, &sm, (int) i, NULL);
                if (is_move) {
                    e->a->dx += dx;
                    e->a->dy += dy;
                } else {
                    JsonArray *ps = wpj_arr (wpj_arr_obj (ll->arr, i), "pos");
                    double px = wpj_arr_len (ps) >= 2 ? json_array_get_double_element (ps, 0) : cx;
                    double py = wpj_arr_len (ps) >= 2 ? json_array_get_double_element (ps, 1) : cy;
                    if (nr > 1) {
                        e->a->dx += (f - 1) * (px - cx);
                        e->a->dy += (f - 1) * (py - cy);
                    }
                    e->a->scale *= f;
                }
            }
            g_free (roots);
        } else if (ok) {
            for (guint k = 0; k < raw->len; k++) {
                LayerEdit *e = layer_edit_for (edits, NULL, &sm, -1, raw->pdata[k]);
                if (is_move) {
                    e->a->dx += dx;
                    e->a->dy += dy;
                } else {
                    e->a->scale *= f;   /* each about its own origin: no scene to ask */
                }
            }
        }
    }

    /* everything is checked before anything is changed */
    for (guint k = 0; ok && k < edits->len; k++) {
        LayerEdit *e = edits->pdata[k];
        const char *bad = walpe_layer_adj_check (e->a);
        if (bad) {
            char *msg = g_strdup_printf ("%s: %s", e->a->key ? e->a->key : "?", bad);
            jb_fail (b, msg);
            g_free (msg);
            ok = FALSE;
        }
    }
    if (ok && (is_set || is_move || is_scale)) {
        for (guint k = 0; k < edits->len; k++) {
            LayerEdit *e = edits->pdata[k];
            const char *why = NULL;
            if (!walpe_layers_put (D.cfg, tp, e->a, e->old_key, &why)) {
                /* only "too many": the rest was checked above */
                jb_fail (b, why ? why : "not saved");
                ok = FALSE;
                break;
            }
            changed++;
        }
    } else if (ok && is_reset) {
        if (!json_object_has_member (o, "keys")) {
            changed = walpe_layers_remove (D.cfg, tp, NULL);
        } else {
            for (guint i = 0; ll && i < ll->n; i++)
                if (sel[i] && sm.of_layer[i] >= 0)
                    changed += walpe_layers_remove (
                        D.cfg, tp, ((WalpeLayerAdj *) sm.saved->pdata[sm.of_layer[i]])->key);
            for (guint k = 0; k < raw->len; k++)
                changed += walpe_layers_remove (D.cfg, tp, raw->pdata[k]);
        }
    } else if (ok && is_hl) {
        double secs = LAYERS_HL_DEFAULT_S;
        int hs = layers_num (o, "seconds", &secs);
        if (hs < 0 || secs <= 0 || secs > LAYERS_HL_MAX_S) {
            jb_fail (b, "seconds must be within 0 .. 60");
            ok = FALSE;
        } else {
            GPtrArray *keys = g_ptr_array_new ();
            for (guint i = 0; ll && i < ll->n; i++)
                if (sel[i])
                    g_ptr_array_add (keys, (gpointer) ll->keys[i]);
            for (guint k = 0; k < raw->len; k++)
                g_ptr_array_add (keys, raw->pdata[k]);
            layers_highlight (tp, (const char *const *) keys->pdata, keys->len, secs);
            changed = keys->len;
            g_ptr_array_unref (keys);
        }
    }
    if (changed && !is_hl) {
        walpe_config_save (D.cfg);
        layers_redraw (tp);
    }
    if (ok) {
        jb_bool (b, "ok", TRUE);
        jb_str (b, "path", tp);
        jb_bool (b, "running", ll != NULL);
        jb_int (b, "changed", changed);
        layers_jb_adjustments (b, tp, ll);
    }
    g_free (sel);
    g_ptr_array_unref (edits);
    g_ptr_array_unref (raw);
    saved_map_free (&sm);
    layer_list_free (ll);
    g_free (tp_own);
}
