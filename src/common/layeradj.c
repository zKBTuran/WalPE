/* layeradj.c - per-wallpaper layer adjustments (see layeradj.h) */
#include "layeradj.h"
#include "util.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* guards every WalpeConfig.layers table, the highlight table and the
 * published layer lists */
static GMutex L;
static GCond L_cond;               /* a layer list was published */
static WalpeConfig *active;        /* the config last loaded (daemon singleton) */
static gint generation = 1;        /* atomic */
static gint list_serial;           /* atomic: newest layer-list request */
static GHashTable *highlight;      /* path -> GStrv */

typedef struct {
    gpointer owner;
    char *path;
    guint64 serial;
    char *json;
} Pub;
static GPtrArray *pubs;            /* Pub* */

static void bump (void)
{
    g_atomic_int_inc (&generation);
}

/* ------------------------------------------------------------ records */

WalpeLayerAdj *walpe_layer_adj_new (const char *key)
{
    WalpeLayerAdj *a = g_new0 (WalpeLayerAdj, 1);
    a->key = g_strdup (key ? key : "");
    a->scale = 1.0;
    return a;
}

WalpeLayerAdj *walpe_layer_adj_copy (const WalpeLayerAdj *a)
{
    WalpeLayerAdj *c = g_new0 (WalpeLayerAdj, 1);
    *c = *a;
    c->key = g_strdup (a->key);
    return c;
}

void walpe_layer_adj_free (WalpeLayerAdj *a)
{
    if (!a)
        return;
    g_free (a->key);
    g_free (a);
}

gboolean walpe_layer_adj_is_noop (const WalpeLayerAdj *a)
{
    return a->dx == 0 && a->dy == 0 && a->scale == 1.0 && a->vis == WALPE_LAYER_VIS_DEFAULT;
}

const char *walpe_layer_adj_check (const WalpeLayerAdj *a)
{
    if (!a || !a->key || !walpe_layer_key_parse (a->key, NULL, NULL, NULL))
        return "not a layer key (expected \"<id>:<name>\" as listed by \"layers\")";
    if (!isfinite (a->dx) || !isfinite (a->dy))
        return "dx and dy must be finite numbers";
    if (fabs (a->dx) > WALPE_LAYER_OFFSET_MAX || fabs (a->dy) > WALPE_LAYER_OFFSET_MAX)
        return "dx and dy must be within -100000 .. 100000 scene pixels";
    if (!isfinite (a->scale))
        return "scale must be a finite number";
    if (a->scale < WALPE_LAYER_SCALE_MIN || a->scale > WALPE_LAYER_SCALE_MAX)
        return "scale must be within 0.01 .. 100";
    if (a->vis != WALPE_LAYER_VIS_DEFAULT && a->vis != WALPE_LAYER_VIS_HIDDEN &&
        a->vis != WALPE_LAYER_VIS_SHOWN)
        return "visible must be \"default\", \"hidden\" or \"shown\"";
    return NULL;
}

gboolean walpe_layer_vis_parse (const char *s, int *vis)
{
    static const struct { const char *w; int v; } tab[] = {
        { "default", WALPE_LAYER_VIS_DEFAULT }, { "unset", WALPE_LAYER_VIS_DEFAULT },
        { "", WALPE_LAYER_VIS_DEFAULT },
        { "hidden", WALPE_LAYER_VIS_HIDDEN }, { "hide", WALPE_LAYER_VIS_HIDDEN },
        { "false", WALPE_LAYER_VIS_HIDDEN }, { "off", WALPE_LAYER_VIS_HIDDEN },
        { "shown", WALPE_LAYER_VIS_SHOWN }, { "show", WALPE_LAYER_VIS_SHOWN },
        { "true", WALPE_LAYER_VIS_SHOWN }, { "on", WALPE_LAYER_VIS_SHOWN },
    };
    if (!s)
        return FALSE;
    for (guint i = 0; i < G_N_ELEMENTS (tab); i++)
        if (!g_ascii_strcasecmp (s, tab[i].w)) {
            if (vis)
                *vis = tab[i].v;
            return TRUE;
        }
    return FALSE;
}

const char *walpe_layer_vis_name (int vis)
{
    return vis == WALPE_LAYER_VIS_HIDDEN ? "hidden" : vis == WALPE_LAYER_VIS_SHOWN ? "shown"
                                                                                  : "default";
}

/* --------------------------------------------------------------- keys */

char *walpe_layer_key_make (int id, int nth, const char *name)
{
    GString *s = g_string_new (NULL);
    if (id >= 0)
        g_string_append_printf (s, "%d", id);
    else
        g_string_append_c (s, '-');
    if (nth > 0)
        g_string_append_printf (s, "~%d", nth + 1);
    g_string_append_c (s, ':');
    g_string_append (s, name ? name : "");
    return g_string_free (s, FALSE);
}

/* up to 9 digits at *p -> value, -1 = none / too long */
static int digits (const char **p)
{
    const char *s = *p;
    int n = 0, v = 0;
    while (g_ascii_isdigit (s[n]) && n < 10) {
        v = v * 10 + (s[n] - '0');
        n++;
    }
    if (n < 1 || n > 9)
        return -1;
    *p = s + n;
    return v;
}

gboolean walpe_layer_key_parse (const char *key, int *id, int *nth, const char **name)
{
    if (!key || strlen (key) > WALPE_LAYER_KEY_MAX || !g_utf8_validate (key, -1, NULL))
        return FALSE;
    const char *p = key;
    int i = -1, n = 0;
    if (*p == '-') {
        p++;
    } else if ((i = digits (&p)) < 0) {
        return FALSE;
    }
    if (*p == '~') {
        p++;
        n = digits (&p);
        if (n < 2)
            return FALSE;
        n--;
    }
    if (*p != ':')
        return FALSE;
    if (id)
        *id = i;
    if (nth)
        *nth = n;
    if (name)
        *name = p + 1;
    return TRUE;
}

static gboolean name_is (const WalpeLayerRef *r, const char *name)
{
    return !strcmp (r->name ? r->name : "", name ? name : "");
}

char *walpe_layer_key_for (const WalpeLayerRef *l, guint n, guint i)
{
    if (i >= n)
        return NULL;
    int nth = 0;
    for (guint j = 0; j < i; j++)
        nth += l[j].id == l[i].id && name_is (&l[j], l[i].name);
    return walpe_layer_key_make (l[i].id, nth, l[i].name);
}

int walpe_layer_find (const WalpeLayerRef *l, guint n, const char *key)
{
    int id, nth;
    const char *name;
    if (!walpe_layer_key_parse (key, &id, &nth, &name))
        return -1;
    int c = 0;
    for (guint i = 0; i < n; i++)
        if ((id >= 0 ? l[i].id == id : l[i].id < 0) && name_is (&l[i], name) && c++ == nth)
            return (int) i;
    if (id >= 0) {
        /* renamed: the id decides */
        gboolean any = FALSE;
        c = 0;
        for (guint i = 0; i < n; i++)
            if (l[i].id == id) {
                any = TRUE;
                if (c++ == nth)
                    return (int) i;
            }
        if (any || !*name)
            return -1;
        /* the id is gone: the one layer with this name */
        int found = -1;
        for (guint i = 0; i < n; i++)
            if (name_is (&l[i], name)) {
                if (found >= 0)
                    return -1;
                found = (int) i;
            }
        return found;
    }
    if (!*name)
        return -1;
    /* a key without id whose layer has one now: by name, in order */
    c = 0;
    for (guint i = 0; i < n; i++)
        if (name_is (&l[i], name) && c++ == nth)
            return (int) i;
    return -1;
}

gboolean walpe_layer_select (const WalpeLayerRef *l, guint n, const char *sel, GArray *out,
                             char **err)
{
    if (err)
        *err = NULL;
    if (!sel || !*sel || strlen (sel) > WALPE_LAYER_KEY_MAX || !g_utf8_validate (sel, -1, NULL)) {
        if (err)
            *err = g_strdup ("empty or malformed layer name");
        return FALSE;
    }
    int idx = walpe_layer_find (l, n, sel);
    if (idx >= 0) {
        g_array_append_val (out, idx);
        return TRUE;
    }
    if (strspn (sel, "0123456789") == strlen (sel) && strlen (sel) <= 9) {
        int id = atoi (sel), got = 0;
        for (guint i = 0; i < n; i++)
            if (l[i].id == id) {
                int v = (int) i;
                g_array_append_val (out, v);
                got++;
            }
        if (got)
            return TRUE;
    }
    int found = -1, count = 0;
    for (guint i = 0; i < n; i++)
        if (name_is (&l[i], sel)) {
            if (!count++)
                found = (int) i;
        }
    if (count == 1) {
        g_array_append_val (out, found);
        return TRUE;
    }
    if (err) {
        if (!count) {
            *err = g_strdup_printf ("no layer \"%s\" in this scene (see \"layers\")", sel);
        } else {
            GString *s = g_string_new (NULL);
            g_string_append_printf (s, "%d layers are named \"%s\"; use a key:", count, sel);
            for (guint i = 0; i < n; i++)
                if (name_is (&l[i], sel)) {
                    char *k = walpe_layer_key_for (l, n, i);
                    g_string_append_printf (s, " \"%s\"", k);
                    g_free (k);
                }
            *err = g_string_free (s, FALSE);
        }
    }
    return FALSE;
}

void walpe_layer_sel_roots (const int *parent, guint n, const gboolean *sel, gboolean *roots)
{
    for (guint i = 0; i < n; i++) {
        roots[i] = sel[i];
        /* hops bounded: a broken scene may hold a parent cycle */
        for (int p = parent[i], hops = 0; roots[i] && p >= 0 && (guint) p < n && hops < 256;
             p = parent[p], hops++)
            if (sel[p])
                roots[i] = FALSE;
    }
}

void walpe_layer_sel_children (const int *parent, guint n, gboolean *sel)
{
    gboolean *add = g_new0 (gboolean, n ? n : 1);
    for (guint i = 0; i < n; i++)
        for (int p = parent[i], hops = 0; !sel[i] && !add[i] && p >= 0 && (guint) p < n &&
             hops < 256; p = parent[p], hops++)
            if (sel[p])
                add[i] = TRUE;
    for (guint i = 0; i < n; i++)
        sel[i] = sel[i] || add[i];
    g_free (add);
}

/* -------------------------------------------------------------- config */

static void adj_array_free (gpointer p)
{
    g_ptr_array_unref (p);
}

static GPtrArray *adj_array_new (void)
{
    return g_ptr_array_new_with_free_func ((GDestroyNotify) walpe_layer_adj_free);
}

/* L held */
static GPtrArray *list_of (WalpeConfig *c, const char *path, gboolean create)
{
    if (!c || !path || !*path)
        return NULL;
    if (!c->layers) {
        if (!create)
            return NULL;
        c->layers = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, adj_array_free);
    }
    GPtrArray *a = g_hash_table_lookup (c->layers, path);
    if (!a && create) {
        a = adj_array_new ();
        g_hash_table_insert (c->layers, g_strdup (path), a);
    }
    return a;
}

void walpe_layers_config_read (WalpeConfig *c, JsonObject *root)
{
    JsonObject *lo = root ? wpj_obj (root, "layers") : NULL;
    g_mutex_lock (&L);
    g_clear_pointer (&c->layers, g_hash_table_unref);
    GList *paths = lo ? json_object_get_members (lo) : NULL;
    for (GList *p = paths; p; p = p->next) {
        JsonArray *arr = wpj_arr (lo, p->data);
        guint n = wpj_arr_len (arr), bad = 0;
        if (!*(const char *) p->data)
            continue;
        for (guint i = 0; i < n; i++) {
            JsonObject *o = wpj_arr_obj (arr, i);
            if (!o) {
                bad++;
                continue;
            }
            WalpeLayerAdj a = { .key = (char *) wpj_str (o, "key", NULL),
                                .dx = wpj_num (o, "dx", 0), .dy = wpj_num (o, "dy", 0),
                                .scale = wpj_num (o, "scale", 1.0) };
            gboolean vok = walpe_layer_vis_parse (wpj_str (o, "visible", "default"), &a.vis);
            GPtrArray *list = list_of (c, p->data, TRUE);
            if (!vok || walpe_layer_adj_check (&a) || list->len >= WALPE_LAYER_MAX) {
                bad++;
                continue;
            }
            gboolean dup = FALSE;
            for (guint k = 0; k < list->len && !dup; k++)
                dup = !strcmp (((WalpeLayerAdj *) list->pdata[k])->key, a.key);
            if (dup || walpe_layer_adj_is_noop (&a))
                continue;
            g_ptr_array_add (list, walpe_layer_adj_copy (&a));
        }
        if (bad)
            g_warning ("config: %u layer adjustment(s) of %s are not usable and were dropped",
                       bad, (const char *) p->data);
        GPtrArray *list = list_of (c, p->data, FALSE);
        if (list && !list->len)
            g_hash_table_remove (c->layers, p->data);
    }
    g_list_free (paths);
    active = c;
    g_mutex_unlock (&L);
    bump ();
}

static int path_cmp (gconstpointer a, gconstpointer b)
{
    return strcmp (*(const char *const *) a, *(const char *const *) b);
}

void walpe_layers_config_write (WalpeConfig *c, JsonBuilder *b)
{
    g_mutex_lock (&L);
    if (c->layers && g_hash_table_size (c->layers)) {
        /* sorted: the file does not reshuffle on every save */
        guint np = 0;
        const char **paths = (const char **) g_hash_table_get_keys_as_array (c->layers, &np);
        qsort (paths, np, sizeof (char *), path_cmp);
        json_builder_set_member_name (b, "layers");
        json_builder_begin_object (b);
        for (guint p = 0; p < np; p++) {
            GPtrArray *list = g_hash_table_lookup (c->layers, paths[p]);
            json_builder_set_member_name (b, paths[p]);
            json_builder_begin_array (b);
            for (guint i = 0; list && i < list->len; i++) {
                WalpeLayerAdj *a = list->pdata[i];
                json_builder_begin_object (b);
                json_builder_set_member_name (b, "key");
                json_builder_add_string_value (b, a->key);
                json_builder_set_member_name (b, "dx");
                json_builder_add_double_value (b, a->dx);
                json_builder_set_member_name (b, "dy");
                json_builder_add_double_value (b, a->dy);
                json_builder_set_member_name (b, "scale");
                json_builder_add_double_value (b, a->scale);
                json_builder_set_member_name (b, "visible");
                json_builder_add_string_value (b, walpe_layer_vis_name (a->vis));
                json_builder_end_object (b);
            }
            json_builder_end_array (b);
        }
        json_builder_end_object (b);
        g_free (paths);
    }
    g_mutex_unlock (&L);
}

void walpe_layers_config_free (WalpeConfig *c)
{
    g_mutex_lock (&L);
    g_clear_pointer (&c->layers, g_hash_table_unref);
    if (active == c)
        active = NULL;
    g_mutex_unlock (&L);
    bump ();
}

/* L held */
static GPtrArray *copy_list (GPtrArray *list)
{
    GPtrArray *out = adj_array_new ();
    for (guint i = 0; list && i < list->len; i++)
        g_ptr_array_add (out, walpe_layer_adj_copy (list->pdata[i]));
    return out;
}

GPtrArray *walpe_layers_get (WalpeConfig *c, const char *path)
{
    g_mutex_lock (&L);
    GPtrArray *out = copy_list (list_of (c, path, FALSE));
    g_mutex_unlock (&L);
    return out;
}

guint walpe_layers_count (WalpeConfig *c, const char *path)
{
    g_mutex_lock (&L);
    GPtrArray *list = list_of (c, path, FALSE);
    guint n = list ? list->len : 0;
    g_mutex_unlock (&L);
    return n;
}

gboolean walpe_layers_put (WalpeConfig *c, const char *path, const WalpeLayerAdj *a,
                           const char *old_key, const char **why)
{
    const char *bad = !c || !path || !*path ? "no wallpaper" : walpe_layer_adj_check (a);
    if (bad) {
        if (why)
            *why = bad;
        return FALSE;
    }
    gboolean noop = walpe_layer_adj_is_noop (a);
    g_mutex_lock (&L);
    GPtrArray *list = list_of (c, path, !noop);
    int at = -1;
    for (guint i = 0; list && i < list->len; i++) {
        const char *k = ((WalpeLayerAdj *) list->pdata[i])->key;
        if (!strcmp (k, a->key) || (old_key && !strcmp (k, old_key))) {
            if (at < 0) {
                at = (int) i;
            } else {
                g_ptr_array_remove_index (list, i);   /* both spellings: one entry stays */
                i--;
            }
        }
    }
    gboolean ok = TRUE;
    if (noop) {
        if (at >= 0)
            g_ptr_array_remove_index (list, (guint) at);
    } else if (at >= 0) {
        walpe_layer_adj_free (list->pdata[at]);
        list->pdata[at] = walpe_layer_adj_copy (a);
    } else if (list->len >= WALPE_LAYER_MAX) {
        ok = FALSE;
        if (why)
            *why = "too many layer adjustments for one wallpaper";
    } else {
        g_ptr_array_add (list, walpe_layer_adj_copy (a));
    }
    if (list && !list->len)
        g_hash_table_remove (c->layers, path);
    g_mutex_unlock (&L);
    bump ();
    return ok;
}

guint walpe_layers_remove (WalpeConfig *c, const char *path, const char *key)
{
    guint n = 0;
    g_mutex_lock (&L);
    GPtrArray *list = list_of (c, path, FALSE);
    if (list && !key) {
        n = list->len;
        g_hash_table_remove (c->layers, path);
    } else if (list) {
        for (guint i = 0; i < list->len; i++)
            if (!strcmp (((WalpeLayerAdj *) list->pdata[i])->key, key)) {
                g_ptr_array_remove_index (list, i);
                i--;
                n++;
            }
        if (!list->len)
            g_hash_table_remove (c->layers, path);
    }
    g_mutex_unlock (&L);
    if (n)
        bump ();
    return n;
}

/* -------------------------------------------------------------- engine */

guint walpe_layers_generation (void)
{
    return (guint) g_atomic_int_get (&generation);
}

GPtrArray *walpe_layers_active (const char *path)
{
    GPtrArray *out = NULL;
    g_mutex_lock (&L);
    GPtrArray *list = active ? list_of (active, path, FALSE) : NULL;
    if (list && list->len)
        out = copy_list (list);
    g_mutex_unlock (&L);
    return out;
}

char **walpe_layers_highlight (const char *path)
{
    char **out = NULL;
    g_mutex_lock (&L);
    char **k = highlight && path ? g_hash_table_lookup (highlight, path) : NULL;
    if (k && k[0])
        out = g_strdupv (k);
    g_mutex_unlock (&L);
    return out;
}

guint64 walpe_layers_list_serial (void)
{
    return (guint64) (guint) g_atomic_int_get (&list_serial);
}

static void pub_free (gpointer p)
{
    Pub *pb = p;
    g_free (pb->path);
    g_free (pb->json);
    g_free (pb);
}

void walpe_layers_publish (gpointer owner, const char *path, guint64 serial, char *json)
{
    g_mutex_lock (&L);
    if (!pubs)
        pubs = g_ptr_array_new_with_free_func (pub_free);
    Pub *pb = NULL;
    for (guint i = 0; i < pubs->len && !pb; i++)
        if (((Pub *) pubs->pdata[i])->owner == owner)
            pb = pubs->pdata[i];
    if (!pb) {
        pb = g_new0 (Pub, 1);
        pb->owner = owner;
        g_ptr_array_add (pubs, pb);
    }
    if (!json && pb->json && !g_strcmp0 (pb->path, path)) {
        /* "I draw" from a scene that already handed a list over: keep it */
        g_mutex_unlock (&L);
        return;
    }
    g_free (pb->path);
    g_free (pb->json);
    pb->path = g_strdup (path);
    pb->json = json;
    pb->serial = serial;
    g_cond_broadcast (&L_cond);
    g_mutex_unlock (&L);
}

void walpe_layers_unpublish (gpointer owner)
{
    g_mutex_lock (&L);
    for (guint i = 0; pubs && i < pubs->len; i++)
        if (((Pub *) pubs->pdata[i])->owner == owner) {
            g_ptr_array_remove_index (pubs, i);
            /* a request waiting for this scene's next list
             * (walpe_layers_published) has nothing to wait for now */
            g_cond_broadcast (&L_cond);
            break;
        }
    g_mutex_unlock (&L);
}

/* --------------------------------------------------------------- daemon */

void walpe_layers_changed (void)
{
    bump ();
}

void walpe_layers_set_highlight (const char *path, const char *const *keys, guint n)
{
    if (!path || !*path)
        return;
    g_mutex_lock (&L);
    if (!highlight)
        highlight = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                           (GDestroyNotify) g_strfreev);
    if (!keys || !n) {
        g_hash_table_remove (highlight, path);
    } else {
        char **k = g_new0 (char *, n + 1);
        for (guint i = 0; i < n; i++)
            k[i] = g_strdup (keys[i]);
        g_hash_table_insert (highlight, g_strdup (path), k);
    }
    g_mutex_unlock (&L);
    bump ();
}

gboolean walpe_layers_any_highlight (void)
{
    g_mutex_lock (&L);
    gboolean any = highlight && g_hash_table_size (highlight) > 0;
    g_mutex_unlock (&L);
    return any;
}

void walpe_layers_clear_highlights (void)
{
    g_mutex_lock (&L);
    if (highlight)
        g_hash_table_remove_all (highlight);
    g_mutex_unlock (&L);
    bump ();
}

guint64 walpe_layers_request_list (void)
{
    guint64 s = (guint64) (guint) g_atomic_int_add (&list_serial, 1) + 1;
    bump ();
    return s;
}

gboolean walpe_layers_scene_alive (const char *path)
{
    gboolean alive = FALSE;
    g_mutex_lock (&L);
    for (guint i = 0; pubs && path && i < pubs->len && !alive; i++)
        alive = !strcmp (((Pub *) pubs->pdata[i])->path, path);
    g_mutex_unlock (&L);
    return alive;
}

char *walpe_layers_published (const char *path, guint64 serial, int wait_ms, gboolean *fresh)
{
    char *out = NULL;
    gint64 until = g_get_monotonic_time () + (gint64) MAX (0, wait_ms) * 1000;
    if (fresh)
        *fresh = FALSE;
    g_mutex_lock (&L);
    for (;;) {
        Pub *best = NULL;
        for (guint i = 0; pubs && path && i < pubs->len; i++) {
            Pub *pb = pubs->pdata[i];
            if (!strcmp (pb->path, path) && (!best || pb->serial > best->serial))
                best = pb;
        }
        gboolean ok = best && best->json && best->serial >= serial;
        /* !best: no scene showing path draws yet - nothing to wait for */
        if (ok || !best || wait_ms <= 0 || g_get_monotonic_time () >= until) {
            if (best)
                out = g_strdup (best->json);
            if (fresh)
                *fresh = ok;
            break;
        }
        /* L is released while this waits: the scene may be unloaded
         * meanwhile (walpe_layers_unpublish frees its entry and its list).
         * `best` is therefore looked up again after every wait, also after
         * the one that ran out - the entry found before the wait was read
         * after it was freed when a wallpaper was switched or reloaded
         * while a "layers" request waited for its next frame. */
        g_cond_wait_until (&L_cond, &L, until);
    }
    g_mutex_unlock (&L);
    return out;
}
