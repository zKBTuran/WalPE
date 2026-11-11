/* uitext_test.c - text written by somebody else made fit for a label
 * (src/common/uitext.c), and the library scan's use of it for the titles
 * and descriptions of project.json (src/ui/store.c). No display.
 */
#include "common/uitext.h"
#include "common/util.h"
#include "ui/store.h"
#include <glib/gstdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(c) do { if (!(c)) { g_printerr ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                                 failures++; } } while (0)

/* the longest stretch without a place to break a line */
static glong longest_run (const char *s)
{
    glong run = 0, best = 0;
    for (const char *p = s; *p; p = g_utf8_next_char (p)) {
        gunichar c = g_utf8_get_char (p);
        if (g_unichar_isspace (c) || g_unichar_iswide (c) || c == 0x200b)
            run = 0;
        else if (++run > best)
            best = run;
    }
    return best;
}

static char *repeat (const char *unit, gsize n)
{
    GString *s = g_string_sized_new (strlen (unit) * n + 1);
    for (gsize i = 0; i < n; i++)
        g_string_append (s, unit);
    return g_string_free (s, FALSE);
}

static void test_text (void)
{
    char *t = walpe_ui_text (NULL, 10, WALPE_UITEXT_LINE);
    CHECK (!strcmp (t, ""));
    g_free (t);
    t = walpe_ui_text ("Rainy Day ☔ 4K", 200, WALPE_UITEXT_LINE);
    CHECK (!strcmp (t, "Rainy Day ☔ 4K"));              /* ordinary titles are untouched */
    g_free (t);
    t = walpe_ui_text ("初音ミク - 夜景", 200, WALPE_UITEXT_LINE);
    CHECK (!strcmp (t, "初音ミク - 夜景"));
    g_free (t);
    /* one line: breaks and tabs are spaces, control characters go */
    t = walpe_ui_text ("a\nb\tc\r\nd\x01\x7f" "e\xc2\x85" "f", 200, WALPE_UITEXT_LINE);
    CHECK (!strcmp (t, "a b c def"));
    g_free (t);
    /* direction controls: "Delete <title>?" must not be reordered by the title */
    t = walpe_ui_text ("x\xe2\x80\xae" "gpj.exe\xe2\x81\xa6y\xe2\x80\x8f\xd8\x9cz", 200, WALPE_UITEXT_LINE);
    CHECK (!strcmp (t, "xgpj.exeyz"));
    g_free (t);
    /* a cut ends in the mark and never inside a character */
    t = walpe_ui_text ("ääääääääää", 4, WALPE_UITEXT_LINE);
    CHECK (!strcmp (t, "ääää…") && g_utf8_validate (t, -1, NULL));
    g_free (t);
    /* bytes that are not UTF-8 (a file name) */
    t = walpe_ui_text ("caf\xe9.mp4", 200, WALPE_UITEXT_LINE);
    CHECK (g_utf8_validate (t, -1, NULL) && g_str_has_prefix (t, "caf") && g_str_has_suffix (t, ".mp4"));
    g_free (t);
    /* a megabyte in: the result is bounded and so is the work */
    char *big = repeat ("W", 1024 * 1024);
    gint64 t0 = g_get_monotonic_time ();
    t = walpe_ui_text (big, 200, WALPE_UITEXT_LINE);
    CHECK (g_utf8_strlen (t, -1) == 201 && g_str_has_suffix (t, "…"));
    g_free (t);
    /* paragraphs: line breaks stay; 8000 characters without a space (the
     * Workshop's limit for a description) get a place to break at least
     * every WALPE_UITEXT_RUN characters - unbroken they were 9 s of layout */
    char *w8000 = repeat ("W", 8000);
    t = walpe_ui_text (w8000, 16000, WALPE_UITEXT_PARAGRAPHS);
    CHECK (longest_run (t) <= WALPE_UITEXT_RUN);
    CHECK (longest_run (w8000) == 8000);
    char **parts = g_strsplit (t, "\xe2\x80\x8b", -1);
    char *no_zw = g_strjoinv ("", parts);
    CHECK (!strcmp (no_zw, w8000));                         /* nothing else was changed */
    g_free (no_zw);
    g_strfreev (parts);
    g_free (t);
    g_free (w8000);
    t = walpe_ui_text (big, 16000, WALPE_UITEXT_PARAGRAPHS);
    CHECK (g_utf8_strlen (t, -1) <= 16000 + 16000 / WALPE_UITEXT_RUN + 2);
    CHECK (longest_run (t) <= WALPE_UITEXT_RUN);
    g_free (t);
    CHECK (g_get_monotonic_time () - t0 < 2 * G_USEC_PER_SEC);
    g_free (big);
    t = walpe_ui_text ("line one\r\n\r\nline two\tend\x02", 16000, WALPE_UITEXT_PARAGRAPHS);
    CHECK (!strcmp (t, "line one\n\nline two end"));
    g_free (t);
    /* ordinary prose and links up to the run length are not touched */
    const char *prose = "Thanks for 10k subs! https://steamcommunity.com/sharedfiles/filedetails/"
                        "?id=3219908811 \xe2\x80\x8f\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d";
    t = walpe_ui_text (prose, 16000, WALPE_UITEXT_PARAGRAPHS);
    CHECK (!strcmp (t, prose));
    g_free (t);
    /* text that breaks anywhere (CJK) needs no help */
    char *cjk = repeat ("漢", 3000);
    t = walpe_ui_text (cjk, 16000, WALPE_UITEXT_PARAGRAPHS);
    CHECK (!strcmp (t, cjk));
    g_free (t);
    g_free (cjk);
}

static void put (const char *dir, const char *name, const char *text)
{
    char *p = g_build_filename (dir, name, NULL);
    g_file_set_contents (p, text, -1, NULL);
    g_free (p);
}

/* the scan: what a project.json can put into the library's labels */
static void test_scan (const char *root)
{
    char *lib = g_build_filename (root, "lib", NULL);
    char *a = g_build_filename (lib, "3000000001", NULL);
    char *b = g_build_filename (lib, "3000000002", NULL);
    char *c = g_build_filename (lib, "3000000003", NULL);
    g_mkdir_with_parents (a, 0755);
    g_mkdir_with_parents (b, 0755);
    g_mkdir_with_parents (c, 0755);
    char *w = repeat ("W", 300000);
    char *j = g_strdup_printf ("{\"title\":\"evil\\n\\u202etitle%s\",\"type\":\"scene\","
                               "\"description\":\"%s\"}", w, w);
    put (a, "project.json", j);
    g_free (j);
    put (b, "project.json", "{\"title\":\"Rainy Day\",\"type\":\"video\",\"description\":\"d\"}");
    put (c, "project.json", "{\"title\":\"\\u202e\\n\\t \",\"type\":\"scene\"}");
    GList *l = walpe_store_scan (lib);
    CHECK (g_list_length (l) == 3);
    int seen = 0;
    for (GList *i = l; i; i = i->next) {
        WalpeEntry *e = i->data;
        if (!strcmp (e->id, "3000000001")) {
            seen++;
            CHECK (g_utf8_strlen (e->title, -1) <= 201);
            CHECK (g_str_has_prefix (e->title, "evil titleWWW"));
            CHECK (e->description && strlen (e->description) <= 60000 + 8);
            CHECK (e->description && g_utf8_validate (e->description, -1, NULL));
        } else if (!strcmp (e->id, "3000000002")) {
            seen++;
            CHECK (!strcmp (e->title, "Rainy Day"));
            CHECK (!g_strcmp0 (e->description, "d"));
        } else if (!strcmp (e->id, "3000000003")) {
            seen++;
            CHECK (!strcmp (e->title, "3000000003"));   /* nothing printable: the folder */
        }
    }
    CHECK (seen == 3);
    g_list_free_full (l, (GDestroyNotify) walpe_store_entry_free);
    g_free (w);
    g_free (a);
    g_free (b);
    g_free (c);
    g_free (lib);
}

/* Import of a folder (walpe_store_import_file): what is inside it besides
 * files and folders is not followed. Before: a link to a folder elsewhere
 * was copied with all it reached, and the FIFO below blocked the copy for
 * ever (this test then ends by its time limit). */
static void test_import (const char *root)
{
    char *lib = g_build_filename (root, "lib2", NULL);
    char *out = g_build_filename (root, "outside", NULL);
    char *src = g_build_filename (root, "picked", NULL);
    char *sub = g_build_filename (src, "sub", NULL);
    g_mkdir_with_parents (lib, 0755);
    g_mkdir_with_parents (out, 0755);
    g_mkdir_with_parents (sub, 0755);
    put (out, "secret.txt", "not part of the wallpaper");
    put (src, "video.mp4", "0123456789");
    put (sub, "extra.png", "png");
    char *l1 = g_build_filename (src, "link-to-folder", NULL);
    char *l2 = g_build_filename (sub, "link-to-file", NULL);
    char *l3 = g_build_filename (sub, "loop", NULL);
    char *ff = g_build_filename (src, "pipe", NULL);
    char *sec = g_build_filename (out, "secret.txt", NULL);
    CHECK (symlink (out, l1) == 0);
    CHECK (symlink (sec, l2) == 0);
    CHECK (symlink (src, l3) == 0);
    CHECK (mkfifo (ff, 0600) == 0);
    GError *err = NULL;
    char *dst = walpe_store_import_file (src, lib, &err);
    CHECK (dst != NULL && !err);
    g_clear_error (&err);
    if (dst) {
        const char *want[] = { "video.mp4", "sub/extra.png", "project.json", NULL };
        const char *gone[] = { "link-to-folder", "link-to-folder/secret.txt", "sub/link-to-file",
                               "sub/loop", "pipe", NULL };
        for (int i = 0; want[i]; i++) {
            char *p = g_build_filename (dst, want[i], NULL);
            CHECK (g_file_test (p, G_FILE_TEST_IS_REGULAR));
            g_free (p);
        }
        for (int i = 0; gone[i]; i++) {
            char *p = g_build_filename (dst, gone[i], NULL);
            CHECK (!g_file_test (p, G_FILE_TEST_EXISTS) && !g_file_test (p, G_FILE_TEST_IS_SYMLINK));
            g_free (p);
        }
    }
    g_free (dst);
    /* the chosen file itself may be a link (a video kept on another disk) */
    char *lv = g_build_filename (root, "linked.mp4", NULL);
    char *vid = g_build_filename (src, "video.mp4", NULL);
    CHECK (symlink (vid, lv) == 0);
    dst = walpe_store_import_file (lv, lib, &err);
    CHECK (dst && g_file_test (dst, G_FILE_TEST_IS_REGULAR) && !g_file_test (dst, G_FILE_TEST_IS_SYMLINK));
    g_clear_error (&err);
    g_free (dst);
    /* a file the library does not list (by its name: no video, GIF or
     * picture, or a name that starts with a dot) is refused and not
     * copied; it was copied, reported as imported and never shown */
    char *lib3 = g_build_filename (root, "lib3", NULL);
    static const char *const unlisted[] = { "notes.txt", ".hidden.png", "archive", "movie.mp4.part" };
    for (guint i = 0; i < G_N_ELEMENTS (unlisted); i++) {
        put (root, unlisted[i], "x");
        char *f = g_build_filename (root, unlisted[i], NULL);
        char *in = g_build_filename (lib3, "imported", unlisted[i], NULL);
        dst = walpe_store_import_file (f, lib3, &err);
        CHECK (!dst && err && strstr (err->message, "the library can show"));
        CHECK (!g_file_test (in, G_FILE_TEST_EXISTS));
        g_clear_error (&err);
        g_free (dst);
        g_free (in);
        g_free (f);
    }
    /* what it lists is taken, whatever the case of the extension */
    static const char *const listed[] = { "a.PNG", "b.jpeg", "c.gif", "d.webm", "e.tex" };
    for (guint i = 0; i < G_N_ELEMENTS (listed); i++) {
        put (root, listed[i], "x");
        char *f = g_build_filename (root, listed[i], NULL);
        dst = walpe_store_import_file (f, lib3, &err);
        CHECK (dst && !err && g_file_test (dst, G_FILE_TEST_IS_REGULAR));
        g_clear_error (&err);
        g_free (dst);
        g_free (f);
    }
    GList *shown = walpe_store_scan (lib3);
    CHECK (g_list_length (shown) == G_N_ELEMENTS (listed));     /* everything imported is listed */
    g_list_free_full (shown, g_object_unref);
    g_free (lib3);
    /* a FIFO picked directly: an error, not a wait */
    dst = walpe_store_import_file (ff, lib, &err);
    CHECK (!dst && err);
    g_clear_error (&err);
    g_free (dst);
    g_free (lv);
    g_free (vid);
    g_free (l1);
    g_free (l2);
    g_free (l3);
    g_free (ff);
    g_free (sec);
    g_free (sub);
    g_free (src);
    g_free (out);
    g_free (lib);
}

/* what the window says about a wallpaper the engine could not load: the
 * engine's status is the only place that has it ("open" is answered before
 * the load), and the window did not read it */
static char *load_text (const char *status_json)
{
    JsonNode *n = wpj_parse_data (status_json, -1);
    JsonObject *o = n && JSON_NODE_HOLDS_OBJECT (n) ? json_node_get_object (n) : NULL;
    char *t = walpe_store_load_error_text (o ? wpj_obj (o, "state") : NULL);
    if (n)
        json_node_unref (n);
    return t;
}

static void test_load_error (void)
{
    char *t = load_text ("{\"ok\":true,\"state\":{\"monitors\":[{\"index\":0,\"current\":\"/lib/a\","
                         "\"type\":\"scene\"},{\"index\":1,\"current\":\"/lib/b\"}]}}");
    CHECK (t == NULL);          /* everything loaded */
    g_free (t);
    CHECK (!(t = load_text ("{\"ok\":true,\"state\":{}}")));
    CHECK (!(t = walpe_store_load_error_text (NULL)));
    CHECK (!(t = load_text ("{\"state\":{\"monitors\":3}}")));      /* a daemon before per-monitor status */
    CHECK (!(t = load_text ("{\"state\":{\"monitors\":[7,\"x\",{\"load_error\":\"\"},{\"load_error\":5}]}}")));
    /* one monitor: the reason, and the wallpaper that came back */
    t = load_text ("{\"state\":{\"monitors\":[{\"index\":0,\"current\":\"/lib/good\",\"type\":\"scene\","
                   "\"load_error\":\"/lib/bad: bad scene json: /lib/bad/scene.json\","
                   "\"load_restored\":\"/lib/good\"}]}}");
    CHECK (!g_strcmp0 (t, "bad could not be loaded; good is shown again: bad scene json: /lib/bad/scene.json"));
    g_free (t);
    /* nothing to go back to */
    t = load_text ("{\"state\":{\"monitors\":[{\"index\":0,\"current\":\"/lib/bad.png\",\"type\":\"none\","
                   "\"load_error\":\"/lib/bad.png: failed to load image\"}]}}");
    CHECK (!g_strcmp0 (t, "bad.png could not be loaded: failed to load image"));
    g_free (t);
    /* two monitors, one of them failed: it is named (counted from 1, as the window does) */
    t = load_text ("{\"state\":{\"monitors\":[{\"index\":0,\"current\":\"/lib/a\"},"
                   "{\"index\":1,\"current\":\"/lib/a\",\"load_error\":\"/lib/c: no such file\","
                   "\"load_restored\":\"/lib/a\"}]}}");
    CHECK (!g_strcmp0 (t, "Monitor 2: c could not be loaded; a is shown again: no such file"));
    g_free (t);
    /* both failed the same way: no monitor named */
    t = load_text ("{\"state\":{\"monitors\":[{\"index\":0,\"load_error\":\"/lib/c: x\"},"
                   "{\"index\":1,\"load_error\":\"/lib/c: x\"}]}}");
    CHECK (!g_strcmp0 (t, "c could not be loaded: x"));
    g_free (t);
    /* a sentence of another shape is passed on whole */
    t = load_text ("{\"state\":{\"monitors\":[{\"index\":0,\"load_error\":\"out of memory\"}]}}");
    CHECK (!g_strcmp0 (t, "the wallpaper could not be loaded: out of memory"));
    g_free (t);
}

int main (void)
{
    char *tmp = g_dir_make_tmp ("walpe-uitext-XXXXXX", NULL);
    if (!tmp)
        g_error ("no temp dir");
    char *T = realpath (tmp, NULL);
    g_free (tmp);
    g_setenv ("HOME", T, TRUE);
    test_text ();
    test_scan (T);
    test_import (T);
    test_load_error ();
    char *argv[] = { "/bin/rm", "-rf", "--", T, NULL };
    if (!failures && strstr (T, "walpe-uitext-"))
        g_spawn_sync (NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, NULL, NULL, NULL);
    free (T);
    if (failures) {
        g_printerr ("uitext: %d check(s) FAILED\n", failures);
        return 1;
    }
    g_print ("uitext: ok\n");
    return 0;
}
