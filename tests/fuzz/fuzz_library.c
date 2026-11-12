/* fuzz_library - the project.json of a wallpaper folder as the library
 * window reads it (src/ui/store.c): the scan that lists a folder (title,
 * type, description, preview, the count of its options; a preset takes the
 * type of the item it is based on) and the second read for the options
 * pane (walpe_entry_load_props), then what the engine makes of the same
 * file for that pane (walpe_content_property_defs, walpe_detect_type).
 * Every folder a download leaves in the library is read this way without
 * the wallpaper ever being shown. And the delete dialog's plan for another
 * item of the library (src/common/library.c walpe_library_delete_plan),
 * which opens the project.json of every folder to find the presets based on
 * the item (find_dependents) and reads Steam's libraryfolders.vdf and the
 * remembered steamcmd folder on the way (src/common/workshop.c scan_vdf).
 *
 * The library is three folders made once; the project.json of one of them
 * is a link to the memory file that holds the input, so nothing is written
 * for an input. The same bytes stand in for libraryfolders.vdf and for the
 * steamcmd note of a home directory that is made here as well. */
#include "fuzz_common.h"
#include "../../src/ui/store.h"
#include "common/library.h"
#include "common/util.h"
#include "engine/content.h"

static const char *dir;
static char *item;

/* dir/folder/name -> the memory file of the input */
static void link_input (const char *folder, const char *name, const char *input)
{
    char *d = g_build_filename (dir, folder, NULL);
    char *f = g_build_filename (d, name, NULL);
    if (g_mkdir_with_parents (d, 0700) != 0 || (unlink (f), symlink (input, f)) != 0)
        abort ();
    g_free (f);
    g_free (d);
}

static void plan (const char *folder)
{
    GError *err = NULL;
    char *path = g_build_filename (dir, folder, NULL);
    WalpeDelPlan *pl = walpe_library_delete_plan (path, dir, &err);
    FUZZ_CHECK (pl || err);
    if (pl) {
        FUZZ_CHECK (pl->targets && pl->targets->len >= 1 && pl->forget && pl->forget[0]);
        for (int i = 0; pl->dependents && pl->dependents[i]; i++)
            fuzz_touch (pl->dependents[i], strlen (pl->dependents[i]));
        for (int i = 0; pl->forget[i]; i++)
            FUZZ_CHECK (g_path_is_absolute (pl->forget[i]));
        walpe_del_plan_free (pl);
    }
    g_clear_error (&err);
    g_free (path);
}

static void put (const char *folder, const char *name, const char *text)
{
    char *d = g_build_filename (dir, folder, NULL);
    char *f = g_build_filename (d, name, NULL);
    if (g_mkdir_with_parents (d, 0700) != 0 || !g_file_set_contents (f, text, -1, NULL))
        abort ();
    g_free (f);
    g_free (d);
}

int LLVMFuzzerInitialize (int *argc, char ***argv)
{
    (void) argc; (void) argv;
    fuzz_quiet ();
    dir = fuzz_dir ("library");
    /* a home of this process's own: what the delete plan reads of Steam is here, not the machine's */
    char *home = g_build_filename (dir, "home", NULL);
    char *cache = g_build_filename (home, ".cache", NULL), *conf = g_build_filename (home, ".config", NULL);
    char *data = g_build_filename (home, ".local", "share", NULL);
    g_setenv ("HOME", home, TRUE);
    g_setenv ("XDG_CACHE_HOME", cache, TRUE);
    g_setenv ("XDG_CONFIG_HOME", conf, TRUE);
    g_setenv ("XDG_DATA_HOME", data, TRUE);
    g_free (data);
    g_free (conf);
    g_free (cache);
    g_free (home);
    put ("7", "project.json", "{\"type\":\"web\",\"file\":\"index.html\",\"title\":\"base\"}");
    put ("7", "index.html", "<html></html>");
    put ("42", "scene.json", "{}");
    put ("42", "preview.jpg", "x");
    put ("42", "preview.gif", "GIF89a");
    put ("42", "a.mp4", "x");
    put ("loose", "clip.mp4", "x");
    item = g_build_filename (dir, "42", NULL);
    const char *input = fuzz_file (0, (const uint8_t *) "{}", 2);
    link_input ("42", "project.json", input);
    link_input ("home/.local/share/Steam/steamapps", "libraryfolders.vdf", input);
    link_input ("home/.cache/walpe", "steamcmd-root", input);
    return 0;
}

int LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
    fuzz_file (0, data, size);
    GList *l = walpe_store_scan (dir);
    guint seen = 0;
    for (GList *k = l; k; k = k->next) {
        WalpeEntry *e = k->data;
        FUZZ_CHECK (e && e->path && e->title && e->type && g_utf8_validate (e->title, -1, NULL));
        fuzz_touch (e->title, strlen (e->title));
        if (e->description)
            FUZZ_CHECK (strlen (e->description) <= 60000 + 8 && g_utf8_validate (e->description, -1, NULL));
        if (e->preview)
            FUZZ_CHECK (g_str_has_prefix (e->preview, dir));
        if (walpe_entry_load_props (e)) {
            FUZZ_CHECK (e->props && e->props_root);
            GList *names = json_object_get_members (e->props);
            g_list_free (names);
        }
        walpe_entry_unload_props (e);
        FUZZ_CHECK (!e->props && !e->props_root);
        seen++;
    }
    FUZZ_CHECK (seen >= 2);     /* the folders the input does not touch are always listed */
    g_list_free_full (l, (GDestroyNotify) walpe_store_entry_free);

    /* what deleting the base item, and the input's own folder, would take along */
    plan ("7");
    plan ("42");

    WalpeType t = walpe_detect_type (item, NULL);
    FUZZ_CHECK (walpe_type_name (t) != NULL);
    JsonNode *defs = walpe_content_property_defs (item, NULL);
    if (defs)
        json_node_unref (defs);
    return 0;
}
