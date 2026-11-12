/* fuzz_steam - what comes back from Steam (src/common/workshop_query.c):
 * the Web API's JSON replies (GetPublishedFileDetails, QueryFiles), the
 * Workshop browse page (HTML with its embedded data), an item's
 * description (BBCode), the address a reply redirects to. Network input:
 * the replies travel over TLS, but nothing about them is trusted.
 * The input is one reply; every parser gets it. */
#include "fuzz_common.h"
#include "common/workshop_query.h"

static void items (GPtrArray *a)
{
    for (guint i = 0; a && i < a->len; i++) {
        WalpeWsItem *it = a->pdata[i];
        walpe_ws_item_has_tag (it, "Anime");
        walpe_ws_item_unref (walpe_ws_item_ref (it));
    }
    if (a)
        g_ptr_array_unref (a);
}

int LLVMFuzzerInitialize (int *argc, char ***argv)
{
    (void) argc; (void) argv;
    fuzz_quiet ();
    return 0;
}

int LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
    GError *err = NULL;
    guint64 total = 0;
    guint pages = 0;
    items (walpe_ws_parse_details ((const char *) data, (gssize) size, &err));
    g_clear_error (&err);
    items (walpe_ws_parse_queryfiles ((const char *) data, (gssize) size, &total, &err));
    g_clear_error (&err);
    GPtrArray *ids = g_ptr_array_new_with_free_func (g_free);
    GHashTable *by_id = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                               (GDestroyNotify) walpe_ws_item_unref);
    /* (a reply is a string to its reader: the bytes with a 0 behind them) */
    char *text = g_strndup ((const char *) data, size);
    walpe_ws_parse_browse_page (text, strlen (text), ids, by_id, &total, &pages, &err);
    g_clear_error (&err);
    g_ptr_array_unref (ids);
    g_hash_table_unref (by_id);
    g_free (walpe_ws_bbcode_to_text (text));
    for (int cls = 0; cls < 3; cls++)
        walpe_ws_redirect_allowed (cls, text);
    walpe_ws_valid_api_key (text);
    walpe_ws_tag_canonical (text);
    g_free (text);
    return 0;
}
