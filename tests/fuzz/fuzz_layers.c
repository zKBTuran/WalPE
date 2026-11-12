/* fuzz_layers - the keys of "Adjust layers" (src/common/layeradj.c): a
 * scene's layers get keys made of the id and the name its scene.json gives
 * them ("<id>[~n]:<name>"), the daemon saves adjustments under those keys
 * and finds the layers again by them, and a request on the control socket
 * names layers by key, id or name.
 *
 * The input is text. Lines up to the first empty one are a scene's layers,
 * "<id> <parent> <name>" (numbers as a scene.json may hold them: any int);
 * the lines after it are what a request names (and, after a tab, the
 * numbers of an adjustment). Checked: the key of a layer
 * finds that layer and no other, whatever ids and names the scene has; a
 * selection stays inside the list; the walks over the layer tree end on a
 * tree with cycles. */
#include "fuzz_common.h"
#include "common/layeradj.h"
#include <stdlib.h>

#define LAYERS_MAX 48

int LLVMFuzzerInitialize (int *argc, char ***argv)
{
    (void) argc; (void) argv;
    fuzz_quiet ();
    return 0;
}

static int num (const char **p)
{
    char *end = NULL;
    long v = strtol (*p, &end, 10);
    *p = end;
    return (int) CLAMP (v, (long) G_MININT, (long) G_MAXINT);
}

int LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
    char *text = g_strndup ((const char *) data, size);
    char **lines = g_strsplit (text, "\n", -1);
    WalpeLayerRef refs[LAYERS_MAX];
    int parent[LAYERS_MAX];
    guint n = 0, at = 0;
    for (; lines[at] && *lines[at]; at++) {
        if (n == LAYERS_MAX)
            continue;
        const char *p = lines[at];
        refs[n].id = num (&p);
        parent[n] = num (&p);
        refs[n].name = *p == ' ' ? p + 1 : *p ? p : NULL;
        n++;
    }

    /* the key of a layer means that layer */
    for (guint i = 0; i < n; i++) {
        char *key = walpe_layer_key_for (refs, n, i);
        FUZZ_CHECK (key);
        const char *name = refs[i].name ? refs[i].name : "";
        int id = 0, nth = 0;
        const char *kn = NULL;
        gboolean ok = walpe_layer_key_parse (key, &id, &nth, &kn);
        if (g_utf8_validate (name, -1, NULL) && strlen (name) <= WALPE_LAYER_KEY_MAX - 32) {
            FUZZ_CHECK (ok);
            FUZZ_CHECK (!strcmp (kn, name) && nth >= 0);
            FUZZ_CHECK (walpe_layer_find (refs, n, key) == (int) i);
        }
        for (guint j = 0; j < i; j++) {     /* and no two layers share a key */
            char *other = walpe_layer_key_for (refs, n, j);
            FUZZ_CHECK (strcmp (other, key));
            g_free (other);
        }
        g_free (key);
    }
    FUZZ_CHECK (!walpe_layer_key_for (refs, n, n));

    /* what a request names */
    gboolean sel[LAYERS_MAX + 1] = { 0 }, roots[LAYERS_MAX + 1], before[LAYERS_MAX + 1];
    for (guint k = at; lines[k]; k++) {
        /* "<key, id or name>[<tab><dx> <dy> <scale> <visible>]" */
        char *tab = strchr (lines[k], '\t');
        if (tab)
            *tab = 0;
        const char *s = lines[k];
        int f = walpe_layer_find (refs, n, s);
        FUZZ_CHECK (f >= -1 && f < (int) n);
        GArray *out = g_array_new (FALSE, FALSE, sizeof (int));
        char *err = NULL;
        gboolean got = walpe_layer_select (refs, n, s, out, &err);
        FUZZ_CHECK (got ? !err && out->len >= 1 : err && !out->len);
        for (guint i = 0; i < out->len; i++) {
            int v = g_array_index (out, int, i);
            FUZZ_CHECK (v >= 0 && v < (int) n);
            sel[v] = TRUE;
        }
        g_free (err);
        g_array_unref (out);

        int vis = -1;
        if (walpe_layer_vis_parse (s, &vis))
            FUZZ_CHECK (walpe_layer_vis_name (vis) && vis >= 0 && vis <= 2);
        WalpeLayerAdj *a = walpe_layer_adj_new (s);
        if (tab) {
            const char *p = tab + 1;
            a->dx = g_ascii_strtod (p, (char **) &p);
            a->dy = g_ascii_strtod (p, (char **) &p);
            a->scale = g_ascii_strtod (p, (char **) &p);
            a->vis = num (&p);
        }
        const char *why = walpe_layer_adj_check (a);
        if (!why) {
            FUZZ_CHECK (a->dx >= -WALPE_LAYER_OFFSET_MAX && a->dx <= WALPE_LAYER_OFFSET_MAX);
            FUZZ_CHECK (a->scale >= WALPE_LAYER_SCALE_MIN && a->scale <= WALPE_LAYER_SCALE_MAX);
            FUZZ_CHECK (a->vis >= 0 && a->vis <= 2 && walpe_layer_key_parse (s, NULL, NULL, NULL));
            WalpeLayerAdj *c = walpe_layer_adj_copy (a);
            walpe_layer_adj_is_noop (c);
            walpe_layer_adj_free (c);
        }
        walpe_layer_adj_free (a);
    }
    memcpy (before, sel, sizeof before);
    walpe_layer_sel_children (parent, n, sel);
    for (guint i = 0; i < n; i++)
        FUZZ_CHECK (!before[i] || sel[i]);
    walpe_layer_sel_roots (parent, n, sel, roots);
    for (guint i = 0; i < n; i++)
        FUZZ_CHECK (!roots[i] || sel[i]);
    g_strfreev (lines);
    g_free (text);
    return 0;
}
