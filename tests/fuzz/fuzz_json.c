/* fuzz_json - how a wallpaper's JSON text is read (src/common/util.c): the
 * depth limit in front of json-glib, a text with more than one value, a
 * UTF-8 BOM, commas in front of a closing bracket; then the tolerant
 * getters on whatever came out. The input is the file's text. */
#include "fuzz_common.h"
#include "common/util.h"

static void walk (JsonNode *n, int depth)
{
    FUZZ_CHECK (depth < 4096);
    if (JSON_NODE_HOLDS_OBJECT (n)) {
        JsonObject *o = json_node_get_object (n);
        GList *names = json_object_get_members (o);
        for (GList *l = names; l; l = l->next) {
            const char *k = l->data;
            double x, y;
            wpj_str (o, k, NULL);
            wpj_num (o, k, 0);
            wpj_bool (o, k, FALSE);
            wpj_obj (o, k);
            wpj_num2 (o, k, &x, &y);
            JsonArray *a = wpj_arr (o, k);
            for (guint i = 0; i < wpj_arr_len (a) && i < 64; i++)
                wpj_arr_obj (a, i);
            walk (json_object_get_member (o, k), depth + 1);
        }
        g_list_free (names);
    } else if (JSON_NODE_HOLDS_ARRAY (n)) {
        JsonArray *a = json_node_get_array (n);
        for (guint i = 0; i < json_array_get_length (a); i++)
            walk (json_array_get_element (a, i), depth + 1);
    } else if (JSON_NODE_HOLDS_VALUE (n)) {
        walpe_d2i (json_node_get_value_type (n) == G_TYPE_DOUBLE ? json_node_get_double (n) : 0);
    }
}

int LLVMFuzzerInitialize (int *argc, char ***argv)
{
    (void) argc; (void) argv;
    fuzz_quiet ();
    return 0;
}

int LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
    JsonNode *n = wpj_parse_file (fuzz_file (0, data, size));
    if (n) {
        walk (n, 0);
        json_node_unref (n);
    }
    /* the strict reading (control socket lines, shader annotations) */
    n = wpj_parse_data ((const char *) data, (gssize) size);
    if (n) {
        walk (n, 0);
        json_node_unref (n);
    }
    char *esc = NULL, *text = g_strndup ((const char *) data, size);
    esc = walpe_json_escape (text);
    g_free (esc);
    g_free (text);
    return 0;
}
