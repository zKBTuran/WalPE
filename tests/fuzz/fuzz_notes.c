/* fuzz_notes - the second reader of a .pkg archive: the scan of a packaged
 * scene's JSON entries that the options window makes on the GUI thread
 * (src/engine/propnotes.c, scan_pkg: the entry table read in place, each
 * .json entry parsed and walked for the options it binds). The input is
 * the archive. propnotes.c is included for the scan. */
#include "../../src/engine/propnotes.c"
#include "fuzz_common.h"

static JsonNode *slider;     /* an option's definition, as project.json has them */

int LLVMFuzzerInitialize (int *argc, char ***argv)
{
    (void) argc; (void) argv;
    fuzz_quiet ();
    slider = json_from_string ("{\"type\":\"slider\",\"min\":0,\"max\":8,\"value\":1}", NULL);
    return 0;
}

int LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
    /* as walpe_prop_notes () sets the scan up */
    Scan sc = { g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL),
                g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL),
                g_string_new (NULL),
                g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free),
                g_array_new (FALSE, FALSE, sizeof (PtCount)), NULL };
    g_array_set_clear_func (sc.counts, pt_count_clear);
    scan_pkg (&sc, fuzz_file (0, data, size));
    /* the questions asked of the result */
    GHashTableIter it;
    gpointer k;
    g_hash_table_iter_init (&it, sc.ok);
    while (g_hash_table_iter_next (&it, &k, NULL)) {
        count_saturated (&sc, k, json_node_get_object (slider));
        mentions (sc.scripts->str, k, TRUE);
    }
    GHashTable *ids = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
    idents_add (ids, sc.scripts->str, TRUE, 0);
    g_hash_table_unref (ids);
    g_hash_table_unref (sc.ok);
    g_hash_table_unref (sc.lacking);
    g_string_free (sc.scripts, TRUE);
    g_hash_table_unref (sc.ptfiles);
    g_array_free (sc.counts, TRUE);
    return 0;
}
