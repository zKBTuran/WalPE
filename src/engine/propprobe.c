/* propprobe.c - see propprobe.h */
#include "propprobe.h"
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>

static GMutex probe_lock;
static GHashTable *probe_uses;   /* prop -> GHashTable(site) */
static int probe_seq;

static const char *probe_dir (void)
{
    static const char *dir;
    static gsize once;
    if (g_once_init_enter (&once)) {
        const char *e = g_getenv ("WALPE_PROP_PROBE");
        dir = e && *e ? g_strdup (e) : NULL;
        g_once_init_leave (&once, 1);
    }
    return dir;
}

gboolean walpe_probe_on (void)
{
    return probe_dir () != NULL;
}

static int probe_frame;   /* this load's digest frame (0 = default) */

int walpe_probe_frame (void)
{
    int n = g_atomic_int_get (&probe_frame);
    if (n <= 0) {
        const char *e = g_getenv ("WALPE_PROP_PROBE_FRAME");
        n = e ? atoi (e) : 0;
    }
    return n > 0 ? n : 6;
}

void walpe_probe_begin (void)
{
    if (!walpe_probe_on ())
        return;
    /* <dir>/frame (a number), read at every load: the harness asks for a
     * later digest when an option only shows after scripts have run for a
     * while (clocks refresh once a second) */
    char *f = g_build_filename (probe_dir (), "frame", NULL);
    char *txt = NULL;
    int n = 0;
    if (g_file_get_contents (f, &txt, NULL, NULL))
        n = atoi (txt);
    g_free (txt);
    g_free (f);
    g_atomic_int_set (&probe_frame, n > 0 && n < 100000 ? n : 0);
    g_mutex_lock (&probe_lock);
    g_clear_pointer (&probe_uses, g_hash_table_unref);
    g_mutex_unlock (&probe_lock);
}

void walpe_probe_use (const char *prop, const char *site)
{
    if (!prop || !*prop || !walpe_probe_on ())
        return;
    g_mutex_lock (&probe_lock);
    if (!probe_uses)
        probe_uses = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                            (GDestroyNotify) g_hash_table_unref);
    GHashTable *sites = g_hash_table_lookup (probe_uses, prop);
    if (!sites) {
        sites = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
        g_hash_table_insert (probe_uses, g_strdup (prop), sites);
    }
    if (!g_hash_table_contains (sites, site ? site : "?"))
        g_hash_table_add (sites, g_strdup (site ? site : "?"));
    g_mutex_unlock (&probe_lock);
}

static int str_cmp (gconstpointer a, gconstpointer b)
{
    return strcmp (a, b);
}

void walpe_probe_write (JsonBuilder *b, const char *origin)
{
    const char *dir = probe_dir ();
    if (!dir || !b)
        return;
    json_builder_set_member_name (b, "uses");
    json_builder_begin_object (b);
    g_mutex_lock (&probe_lock);
    GList *ks = probe_uses ? g_list_sort (g_hash_table_get_keys (probe_uses), str_cmp) : NULL;
    for (GList *k = ks; k; k = k->next) {
        json_builder_set_member_name (b, k->data);
        json_builder_begin_array (b);
        GList *ss = g_list_sort (g_hash_table_get_keys (g_hash_table_lookup (probe_uses, k->data)),
                                 str_cmp);
        for (GList *s = ss; s; s = s->next)
            json_builder_add_string_value (b, s->data);
        g_list_free (ss);
        json_builder_end_array (b);
    }
    g_list_free (ks);
    int seq = ++probe_seq;
    g_mutex_unlock (&probe_lock);
    json_builder_end_object (b);
    json_builder_end_object (b);   /* root */
    JsonNode *root = json_builder_get_root (b);
    char *txt = json_to_string (root, TRUE);
    json_node_unref (root);
    char *bn = g_path_get_basename (origin ? origin : "wallpaper");
    char *tmp = g_strdup_printf ("%s/.%s.%d.tmp", dir, bn, seq);
    char *fin = g_strdup_printf ("%s/%s.%d.json", dir, bn, seq);
    g_mkdir_with_parents (dir, 0700);
    if (g_file_set_contents (tmp, txt, -1, NULL))
        g_rename (tmp, fin);
    g_message ("probe: state digest %s", fin);
    g_free (tmp);
    g_free (fin);
    g_free (bn);
    g_free (txt);
}
