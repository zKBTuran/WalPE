/* workshop_test.c - workshop browsing + steamcmd job tests (version 0.11)
 *
 *   offline (always): tag taxonomy, URL building, parsers on Workshop
 *     replies (tests/fixtures/workshop: synthetic, shaped like Steam's), BBCode,
 *     settings file mode 0600, steamcmd job against a fake steamcmd
 *     (success with on-disk progress, "no cached login" message)
 *   live (WALPE_WS_LIVE=1): search/details/preview/author against Steam
 *
 * Run: meson test -C build workshop -v   (WALPE_WS_LIVE=1 for the live part) */
#include "common/workshop.h"
#include "common/util.h"
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>

static char *fixdir;
static int failures;

#define CHECK(c) do { if (!(c)) { g_printerr ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                                 failures++; } } while (0)

static char *fixture (const char *name, gsize *len)
{
    char *p = g_build_filename (fixdir, name, NULL);
    char *d = NULL;
    if (!g_file_get_contents (p, &d, len, NULL))
        g_error ("missing fixture %s", p);
    g_free (p);
    return d;
}

static void test_taxonomy (void)
{
    guint n = 0;
    const WalpeWsTagCategory *c = walpe_ws_tag_categories (&n);
    CHECK (n == 6);
    guint genre = 0, res = 0;
    for (guint i = 0; i < n; i++) {
        guint k = g_strv_length ((char **) c[i].tags);
        if (!strcmp (c[i].name, "Genre"))
            genre = k;
        if (!strcmp (c[i].name, "Resolution"))
            res = k;
    }
    CHECK (genre == 25);
    CHECK (res == 25);
    CHECK (!g_strcmp0 (walpe_ws_tag_canonical ("pixel ART"), "Pixel art"));
    CHECK (!g_strcmp0 (walpe_ws_tag_canonical ("sci-fi"), "Sci-Fi"));
    CHECK (!g_strcmp0 (walpe_ws_tag_canonical ("audio responsive"), "Audio responsive"));
    CHECK (walpe_ws_tag_canonical ("Cats") == NULL);
}

static void test_urls (void)
{
    const char *req[] = { "Scene", "Pixel art", NULL };
    const char *ex[] = { "Mature", NULL };
    WalpeWsQuery q = { "neon city", WALPE_WS_SORT_RECENT, 3, req, ex };
    char *u = walpe_ws_browse_url (&q);
    CHECK (g_str_has_prefix (u, "https://steamcommunity.com/workshop/browse/?appid=431960"));
    CHECK (strstr (u, "&searchtext=neon%20city"));
    CHECK (strstr (u, "&browsesort=mostrecent"));
    CHECK (strstr (u, "&requiredtags%5B%5D=Scene&requiredtags%5B%5D=Pixel%20art"));
    CHECK (strstr (u, "&excludedtags%5B%5D=Mature"));
    CHECK (g_str_has_suffix (u, "&p=3"));
    g_free (u);
    WalpeWsQuery q2 = { NULL, WALPE_WS_SORT_RELEVANCE, 1, NULL, NULL };
    u = walpe_ws_browse_url (&q2);   /* relevance without text = trend */
    CHECK (strstr (u, "browsesort=trend") && strstr (u, "days=7"));
    g_free (u);
    WalpeWsQuery q3 = { NULL, WALPE_WS_SORT_POPULAR, 1, NULL, NULL };
    u = walpe_ws_browse_url (&q3);
    CHECK (strstr (u, "browsesort=trend") && strstr (u, "days=-1"));
    g_free (u);
    u = walpe_ws_queryfiles_url (&q, "0123456789ABCDEF0123456789ABCDEF");
    CHECK (g_str_has_prefix (u, "https://api.steampowered.com/IPublishedFileService/QueryFiles/v1/?key="));
    CHECK (strstr (u, "&query_type=1&page=3&numperpage=30"));
    CHECK (strstr (u, "&requiredtags%5B1%5D=Pixel%20art"));
    CHECK (strstr (u, "&excludedtags%5B0%5D=Mature"));
    CHECK (strstr (u, "&search_text=neon%20city"));
    g_free (u);
}

static void test_browse_parse (void)
{
    gsize len;
    char *h = fixture ("browse_trend_p1.html", &len);
    GPtrArray *ids = g_ptr_array_new_with_free_func (g_free);
    GHashTable *items = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                               (GDestroyNotify) walpe_ws_item_unref);
    guint64 total = 0;
    guint pages = 0;
    GError *err = NULL;
    CHECK (walpe_ws_parse_browse_page (h, len, ids, items, &total, &pages, &err));
    CHECK (ids->len == 30);
    CHECK (total == 1234567 && pages == 1000);
    CHECK (!g_strcmp0 (g_ptr_array_index (ids, 0), "9990000001"));
    for (guint i = 0; i < ids->len; i++)
        CHECK (strcmp (g_ptr_array_index (ids, i), "1"));   /* the nav link is not a result */
    WalpeWsItem *it = g_hash_table_lookup (items, "9990000001");
    CHECK (it && !g_strcmp0 (it->title, "Tide & Lanterns"));
    CHECK (it && it->stars == 4 && it->votes == 83);
    CHECK (it && !g_strcmp0 (it->author, "example_author_1"));
    /* a size beyond 32 bits, sent as text */
    CHECK (it && walpe_ws_item_has_tag (it, "Scene") && it->file_size == G_GUINT64_CONSTANT (4831838208));
    g_free (h);
    g_ptr_array_set_size (ids, 0);
    g_hash_table_remove_all (items);

    h = fixture ("popular_excl.html", &len);
    CHECK (walpe_ws_parse_browse_page (h, len, ids, items, &total, &pages, NULL));
    CHECK (ids->len == 30 && total == 654321);
    GHashTableIter hi;
    gpointer v;
    g_hash_table_iter_init (&hi, items);
    while (g_hash_table_iter_next (&hi, NULL, &v)) {   /* Steam honoured the tags */
        CHECK (walpe_ws_item_has_tag (v, "Anime"));
        CHECK (!walpe_ws_item_has_tag (v, "Mature") && !walpe_ws_item_has_tag (v, "Questionable"));
    }
    g_free (h);
    g_ptr_array_set_size (ids, 0);

    h = fixture ("search_rare.html", &len);
    CHECK (walpe_ws_parse_browse_page (h, len, ids, NULL, &total, &pages, NULL));
    CHECK (ids->len == 3 && total == 3 && pages == 1);
    g_free (h);
    g_ptr_array_set_size (ids, 0);

    h = fixture ("legacy_links.html", &len);
    CHECK (walpe_ws_parse_browse_page (h, len, ids, NULL, &total, &pages, NULL));
    CHECK (ids->len == 2 && !g_strcmp0 (g_ptr_array_index (ids, 0), "9990000401"));
    g_free (h);
    g_ptr_array_set_size (ids, 0);

    h = fixture ("error_page.html", &len);
    CHECK (!walpe_ws_parse_browse_page (h, len, ids, NULL, &total, &pages, &err));
    CHECK (err != NULL);
    g_clear_error (&err);
    g_free (h);

    /* a reply that ends inside the SSR script tag, in a buffer without a
     * terminator (as the HTTP reply is): nothing is read past its end (the
     * sanitizer build of this test reports it) and the page is refused */
    static const char cut[] = "<html><script type=\"application/json\" id=\"valve-ssr-data\"";
    gsize cl = sizeof cut - 1;
    char *exact = g_malloc (cl);
    memcpy (exact, cut, cl);
    CHECK (!walpe_ws_parse_browse_page (exact, cl, ids, NULL, &total, &pages, &err));
    g_clear_error (&err);
    g_free (exact);
    g_ptr_array_unref (ids);
    g_hash_table_unref (items);
}

static void test_details_parse (void)
{
    gsize len;
    char *j = fixture ("details_trend_p1.json", &len);
    GPtrArray *a = walpe_ws_parse_details (j, (gssize) len, NULL);
    CHECK (a && a->len == 30);
    for (guint i = 0; a && i < a->len; i++) {
        WalpeWsItem *it = g_ptr_array_index (a, i);
        CHECK (it->result == 1 && it->title && it->preview_url && it->tags[0]);
        CHECK (it->description != NULL && it->time_updated > 0);
    }
    if (a)
        g_ptr_array_unref (a);
    g_free (j);

    j = fixture ("details_one_missing.json", &len);
    a = walpe_ws_parse_details (j, (gssize) len, NULL);
    CHECK (a && a->len == 2);
    if (a) {
        WalpeWsItem *x = g_ptr_array_index (a, 0), *y = g_ptr_array_index (a, 1);
        CHECK (!g_strcmp0 (x->id, "9990000501") && x->result == 1);
        CHECK (walpe_ws_item_has_tag (x, "Media Integration") && x->subscriptions > 300000);
        CHECK (x->file_size == 27262976 && !g_strcmp0 (x->creator, "76561190000000006"));
        CHECK (y->result == 9);
        g_ptr_array_unref (a);
    }
    g_free (j);

    guint64 total = 0;
    j = fixture ("queryfiles_synthetic.json", &len);
    a = walpe_ws_parse_queryfiles (j, (gssize) len, &total, NULL);
    CHECK (a && a->len == 5 && total == 1234567);
    if (a) {
        WalpeWsItem *x = g_ptr_array_index (a, 0);
        CHECK (x->score > 0.89 && x->votes == 105 && x->description && *x->description);
        g_ptr_array_unref (a);
    }
    g_free (j);
    CHECK (walpe_ws_parse_details ("<html>", -1, NULL) == NULL);
}

static void test_bbcode (void)
{
    char *t = walpe_ws_bbcode_to_text (
        "[h1]Title[/h1]\r\n[b]bold[/b] and [url=https://x.y/]a link[/url] [url=https://e.f/][/url]\n"
        "[list][*]one[*]two[/list][img]https://i/x.png[/img][previewyoutube=abc;full][/previewyoutube]"
        "[noparse][x][/noparse] 1 < 2 [unknown]");
    CHECK (strstr (t, "Title\n"));
    CHECK (strstr (t, "bold and a link https://e.f/"));
    CHECK (strstr (t, "• one") && strstr (t, "• two"));
    CHECK (!strstr (t, "https://i/x.png") && !strstr (t, "abc;full"));
    CHECK (strstr (t, "[unknown]") && strstr (t, "1 < 2"));
    g_free (t);
}

static void test_settings (const char *home)
{
    WalpeWsSettings *s = walpe_ws_settings_load ();
    CHECK (s->hide_mature && !s->hide_questionable && !s->api_key);
    CHECK (!walpe_ws_valid_api_key ("abc"));
    CHECK (!walpe_ws_valid_api_key ("0123456789ABCDEF0123456789ABCDEZ"));
    CHECK (walpe_ws_valid_api_key ("0123456789abcdef0123456789ABCDEF"));
    s->api_key = g_strdup ("0123456789abcdef0123456789ABCDEF");
    s->hide_questionable = TRUE;
    CHECK (walpe_ws_settings_save (s, NULL));
    walpe_ws_settings_free (s);
    char *p = g_build_filename (home, "config", "walpe", "workshop.json", NULL);
    GStatBuf st;
    CHECK (g_stat (p, &st) == 0 && (st.st_mode & 0777) == 0600);
    g_chmod (p, 0644);
    s = walpe_ws_settings_load ();   /* repairs the mode */
    CHECK (g_stat (p, &st) == 0 && (st.st_mode & 0777) == 0600);
    CHECK (!g_strcmp0 (s->api_key, "0123456789abcdef0123456789ABCDEF") && s->hide_questionable);
    walpe_ws_settings_free (s);
    g_free (p);
}

/* ------------------------------------------------- fake steamcmd job */

static const char *fake_steamcmd =
    "#!/bin/sh\n"
    "# argv as WalPE passes it: +@ShutdownOnFailedCommand 1 +@NoPromptForPassword 1\n"
    "#   +login USER +workshop_download_item 431960 ID validate +quit\n"
    "user=$6; id=$9; root=\"$HOME/Steam\"\n"
    "echo \"Redirecting stderr to '$root/logs/stderr.txt'\"\n"
    "echo \"Logging in user '$user' [U:1:0] to Steam Public...\"\n"
    "if [ \"$FAKE_MODE\" = nologin ]; then\n"
    "  echo 'FAILED (No cached credentials and @NoPromptForPassword is set)'; exit 5; fi\n"
    "echo OK; echo \"Downloading item $id ...\"\n"
    "echo \"$id\" >> \"$HOME/fake-steamcmd-ids\"\n"
    "d=\"$root/steamapps/workshop/downloads/431960/$id\"; mkdir -p \"$d\"\n"
    "if [ \"$FAKE_MODE\" = ok ]; then\n"
    "  for i in 1 2 3; do head -c 400000 /dev/zero > \"$d/chunk$i\"; sleep 1.1; done; fi\n"
    "c=\"$root/steamapps/workshop/content/431960/$id\"\n"
    "# a download line naming a folder that is not the item's\n"
    "if [ \"$FAKE_MODE\" = elsewhere ]; then c=\"$HOME/elsewhere\"; fi\n"
    "mkdir -p \"$c\"\n"
    "dep=''; if [ \"$FAKE_MODE\" = negdep ]; then dep='\"dependency\":-5,'; fi\n"
    "printf '{%s\"title\":\"Fake item\",\"type\":\"scene\",\"file\":\"scene.json\"}' \"$dep\" > \"$c/project.json\"\n"
    "head -c 1200000 /dev/zero > \"$c/scene.pkg\"; rm -rf \"$d\"\n"
    "# a line that is not UTF-8 (a Latin-1 path, a localized message)\n"
    "if [ \"$FAKE_MODE\" = nonutf8 ]; then printf 'Update state \\377\\376\\351 caf\\351\\n'; fi\n"
    "echo \"Success. Downloaded item $id to \\\"$c\\\" (1200000 bytes)\"\n";

typedef struct { GMainLoop *loop; gboolean ok; char *dir, *error; double max_prog; } JobRes;

static void job_done (WalpeWorkshopJob *j, gboolean ok, const char *dir, const char *error,
                      gpointer user)
{
    (void) j;
    JobRes *r = user;
    r->ok = ok;
    r->dir = g_strdup (dir);
    r->error = g_strdup (error);
    g_main_loop_quit (r->loop);
}

static WalpeWorkshopJob *cur_job;
static const char *watch_lib;        /* library sampled for a visible work folder */
static gboolean saw_visible_part;
static gboolean sample_prog (gpointer user)
{
    JobRes *r = user;
    if (cur_job) {
        double p = walpe_workshop_progress (cur_job);
        if (p > r->max_prog && p < 1.0)
            r->max_prog = p;
    }
    /* the copy in progress must not be visible to the library scan */
    if (watch_lib) {
        char *vis = g_build_filename (watch_lib, "2222222222.part", NULL);
        if (g_file_test (vis, G_FILE_TEST_EXISTS))
            saw_visible_part = TRUE;
        g_free (vis);
    }
    return G_SOURCE_CONTINUE;
}

static void run_job (const char *mode, const char *lib, JobRes *r)
{
    g_setenv ("FAKE_MODE", mode, TRUE);
    memset (r, 0, sizeof *r);
    r->loop = g_main_loop_new (NULL, FALSE);
    GError *err = NULL;
    cur_job = walpe_workshop_install ("2222222222", "some_user", lib, job_done, r, &err);
    CHECK (cur_job != NULL);
    if (!cur_job) {
        g_printerr ("install: %s\n", err ? err->message : "?");
        g_clear_error (&err);
        return;
    }
    walpe_workshop_set_expected_size (cur_job, 1200000);
    guint s = g_timeout_add (200, sample_prog, r);
    g_main_loop_run (r->loop);
    g_source_remove (s);
    walpe_workshop_free (cur_job);
    cur_job = NULL;
    g_main_loop_unref (r->loop);
}

static void test_job (const char *home)
{
    char *bin = g_build_filename (home, "bin", NULL);
    g_mkdir_with_parents (bin, 0755);
    char *sc = g_build_filename (bin, "steamcmd", NULL);
    g_file_set_contents (sc, fake_steamcmd, -1, NULL);
    g_chmod (sc, 0755);
    char *orig_path = g_strdup (g_getenv ("PATH"));
    char *path = g_strconcat (bin, ":", orig_path, NULL);
    g_setenv ("PATH", path, TRUE);
    char *lib = g_build_filename (home, "library", NULL);
    JobRes r;

    run_job ("nologin", lib, &r);
    CHECK (!r.ok && r.error && strstr (r.error, "steamcmd +login some_user"));
    g_printerr ("  nologin -> %s\n", r.error ? r.error : "(null)");
    g_free (r.dir);
    g_free (r.error);

    /* steamcmd names another folder as the download: nothing is copied */
    run_job ("elsewhere", lib, &r);
    g_printerr ("  elsewhere -> %s\n", r.ok ? "installed (wrong)" : r.error);
    char *item = g_build_filename (lib, "2222222222", NULL);
    CHECK (!r.ok && r.error && !g_file_test (item, G_FILE_TEST_EXISTS));
    g_free (r.dir);
    g_free (r.error);

    /* "dependency": -5 is no workshop id: installed, steamcmd not run for it */
    run_job ("negdep", lib, &r);
    char *ids = NULL, *idlog = g_build_filename (home, "fake-steamcmd-ids", NULL);
    g_file_get_contents (idlog, &ids, NULL, NULL);
    g_printerr ("  negdep -> %s\n", r.ok ? "installed" : r.error);
    CHECK (r.ok && ids && !strstr (ids, "-5"));
    g_free (ids);
    g_free (idlog);
    g_free (item);
    g_free (r.dir);
    g_free (r.error);

    /* output with a line that is not valid UTF-8: still read to the end */
    run_job ("nonutf8", lib, &r);
    g_printerr ("  nonutf8 -> %s\n", r.ok ? "installed" : r.error);
    CHECK (r.ok && r.dir);
    g_free (r.dir);
    g_free (r.error);

    /* a steamcmd in PATH that cannot be executed: an error from
     * walpe_workshop_install itself, the done callback does not run before
     * the caller has the job */
    {
        char *bad = g_build_filename (home, "bin-bad", NULL);
        g_mkdir_with_parents (bad, 0755);
        char *bsc = g_build_filename (bad, "steamcmd", NULL);
        g_file_set_contents (bsc, "#!/nonexistent/interpreter\n", -1, NULL);
        g_chmod (bsc, 0755);
        /* without the working fake: the PATH search would fall through to it */
        char *bpath = g_strconcat (bad, ":", orig_path, NULL);
        g_setenv ("PATH", bpath, TRUE);
        memset (&r, 0, sizeof r);
        r.max_prog = -5;
        GError *berr = NULL;
        WalpeWorkshopJob *bj = walpe_workshop_install ("2222222222", "some_user", lib, job_done,
                                                       &r, &berr);
        g_printerr ("  cannot execute -> %s\n", bj ? "a job (wrong)" : berr ? berr->message : "?");
        CHECK (!bj && berr && strstr (berr->message, "steamcmd"));
        CHECK (!r.dir && !r.error && !r.ok);   /* job_done was not called */
        g_clear_error (&berr);
        if (bj)
            walpe_workshop_free (bj);
        g_setenv ("PATH", path, TRUE);
        g_free (bpath);
        g_free (bsc);
        g_free (bad);
    }

    /* work folders: a leftover "<id>.part" of an older version is removed,
     * the copy itself uses a hidden name, nothing but <id> stays behind */
    char *stale = g_build_filename (lib, "2222222222.part", "stale.txt", NULL);
    char *stale_dir = g_path_get_dirname (stale);
    g_mkdir_with_parents (stale_dir, 0755);
    g_file_set_contents (stale, "x", 1, NULL);
    run_job ("ok", lib, &r);
    watch_lib = lib;          /* sampled from now on (the leftover is gone) */
    CHECK (!g_file_test (stale_dir, G_FILE_TEST_EXISTS));
    g_free (r.dir);
    g_free (r.error);
    run_job ("ok", lib, &r);   /* again: replaces the installed copy */
    watch_lib = NULL;
    CHECK (!saw_visible_part);
    {
        GDir *ld = g_dir_open (lib, 0, NULL);
        const char *ln;
        while (ld && (ln = g_dir_read_name (ld)))
            if (strcmp (ln, "2222222222")) {
                g_printerr ("FAIL library has a leftover entry: %s\n", ln);
                failures++;
            }
        if (ld)
            g_dir_close (ld);
    }
    g_free (stale);
    g_free (stale_dir);
    g_printerr ("  ok -> %s %s, max progress seen %.2f\n", r.ok ? "installed" : "failed",
                r.ok ? r.dir : r.error, r.max_prog);
    CHECK (r.ok && r.dir && g_str_has_suffix (r.dir, "/library/2222222222"));
    char *pj = g_build_filename (lib, "2222222222", "project.json", NULL);
    CHECK (g_file_test (pj, G_FILE_TEST_IS_REGULAR));
    CHECK (r.max_prog > 0.2 && r.max_prog <= 0.99);   /* measured from the downloads dir */
    char *learned = g_build_filename (home, "cache", "walpe", "steamcmd-root", NULL);
    CHECK (g_file_test (learned, G_FILE_TEST_IS_REGULAR));
    g_free (learned);
    g_free (pj);
    g_free (r.dir);
    g_free (r.error);
    g_free (lib);
    g_free (path);
    g_free (orig_path);
    g_free (sc);
    g_free (bin);
}

/* ------------------------------------------------------------- live */

typedef struct { GMainLoop *loop; GAsyncResult *res; } Wait;

static void got (GObject *src, GAsyncResult *res, gpointer user)
{
    (void) src;
    Wait *w = user;
    w->res = g_object_ref (res);
    g_main_loop_quit (w->loop);
}

static GAsyncResult *wait_res (Wait *w)
{
    g_main_loop_run (w->loop);
    GAsyncResult *r = w->res;
    w->res = NULL;
    return r;
}

static void test_live (void)
{
    WalpeWsClient *c = walpe_ws_client_new ();
    Wait w = { g_main_loop_new (NULL, FALSE), NULL };
    const char *ex[] = { "Mature", NULL };
    const char *req[] = { "Scene", "Anime", NULL };
    WalpeWsQuery q = { NULL, WALPE_WS_SORT_TREND, 1, NULL, ex };
    GError *err = NULL;
    gint64 t0 = g_get_monotonic_time ();
    walpe_ws_search_async (c, &q, NULL, got, &w);
    GAsyncResult *res = wait_res (&w);
    WalpeWsResults *r = walpe_ws_search_finish (res, &err);
    g_object_unref (res);
    CHECK (r != NULL);
    if (!r) {
        g_printerr ("live search: %s\n", err->message);
        g_clear_error (&err);
        return;
    }
    g_printerr ("  live trend p1: %u items of %" G_GUINT64_FORMAT " (%u pages) via %s in %.1f s\n",
                r->items->len, r->total, r->total_pages, r->source,
                (g_get_monotonic_time () - t0) / 1e6);
    CHECK (r->items->len >= 20 && r->total > 100000);
    WalpeWsItem *first = r->items->len ? walpe_ws_item_ref (g_ptr_array_index (r->items, 0)) : NULL;
    for (guint i = 0; i < r->items->len; i++) {
        WalpeWsItem *it = g_ptr_array_index (r->items, i);
        CHECK (!walpe_ws_item_has_tag (it, "Mature"));
        CHECK (it->title && it->preview_url);
    }
    if (first)
        g_printerr ("  #1 %s \"%s\" by %s, %d stars/%d votes, %" G_GUINT64_FORMAT " subs, tags:",
                    first->id, first->title, first->author ? first->author : "?", first->stars,
                    first->votes, first->subscriptions);
    for (char **t = first ? first->tags : NULL; t && *t; t++)
        g_printerr (" [%s]", *t);
    g_printerr ("\n");
    walpe_ws_results_free (r);

    WalpeWsQuery q2 = { "cyberpunk", WALPE_WS_SORT_SUBSCRIBED, 2, req, ex };
    walpe_ws_search_async (c, &q2, NULL, got, &w);
    res = wait_res (&w);
    r = walpe_ws_search_finish (res, &err);
    g_object_unref (res);
    CHECK (r && r->items->len > 0);
    if (r) {
        g_printerr ("  live 'cyberpunk' +Scene +Anime -Mature p2 by subs: %u items of %"
                    G_GUINT64_FORMAT "\n", r->items->len, r->total);
        for (guint i = 0; i < r->items->len; i++) {
            WalpeWsItem *it = g_ptr_array_index (r->items, i);
            CHECK (walpe_ws_item_has_tag (it, "Scene") && walpe_ws_item_has_tag (it, "Anime"));
        }
        walpe_ws_results_free (r);
    } else {
        g_printerr ("live search 2: %s\n", err ? err->message : "?");
        g_clear_error (&err);
    }

    if (first) {
        walpe_ws_preview_async (c, first->id, first->preview_url, TRUE, 200, 200, NULL, got, &w);
        res = wait_res (&w);
        GdkPixbuf *pb = walpe_ws_preview_finish (res, &err);
        g_object_unref (res);
        CHECK (pb && gdk_pixbuf_get_width (pb) <= 200 && gdk_pixbuf_get_height (pb) <= 200);
        if (pb) {
            g_printerr ("  preview %dx%d\n", gdk_pixbuf_get_width (pb), gdk_pixbuf_get_height (pb));
            g_object_unref (pb);
        } else {
            g_printerr ("  preview: %s\n", err ? err->message : "?");
            g_clear_error (&err);
        }
        walpe_ws_author_async (c, first->creator, NULL, got, &w);
        res = wait_res (&w);
        char *a = walpe_ws_author_finish (res, &err);
        g_object_unref (res);
        g_printerr ("  author of %s: %s\n", first->creator, a ? a : err->message);
        CHECK (a != NULL);
        g_free (a);
        g_clear_error (&err);
        walpe_ws_item_unref (first);
    }
    const char *ids[] = { "3163060610", NULL };
    walpe_ws_details_async (c, ids, NULL, got, &w);
    res = wait_res (&w);
    GPtrArray *d = walpe_ws_details_finish (res, &err);
    g_object_unref (res);
    CHECK (d && d->len == 1);
    if (d)
        g_ptr_array_unref (d);
    g_clear_error (&err);

    /* cancellation: the callback still runs, with G_IO_ERROR_CANCELLED */
    GCancellable *cx = g_cancellable_new ();
    walpe_ws_search_async (c, &q, cx, got, &w);
    g_cancellable_cancel (cx);
    res = wait_res (&w);
    r = walpe_ws_search_finish (res, &err);
    CHECK (!r && g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED));
    g_clear_error (&err);
    g_object_unref (res);
    g_object_unref (cx);
    g_main_loop_unref (w.loop);
    walpe_ws_client_free (c);
}

int main (int argc, char **argv)
{
    (void) argc;
    /* private HOME/XDG dirs before GLib caches them */
    char *home = g_dir_make_tmp ("walpe-ws-test-XXXXXX", NULL);
    char *cfg = g_build_filename (home, "config", NULL);
    char *cache = g_build_filename (home, "cache", NULL);
    g_setenv ("HOME", home, TRUE);
    g_setenv ("XDG_CONFIG_HOME", cfg, TRUE);
    g_setenv ("XDG_CACHE_HOME", cache, TRUE);
    char *src = g_path_get_dirname (argv[0]);
    const char *fd = g_getenv ("WALPE_WS_FIXTURES");
    fixdir = fd ? g_strdup (fd) : g_build_filename (SRCDIR, "tests", "fixtures", "workshop", NULL);
    g_free (src);

    test_taxonomy ();
    test_urls ();
    test_browse_parse ();
    test_details_parse ();
    test_bbcode ();
    test_settings (home);
    test_job (home);
    if (g_getenv ("WALPE_WS_LIVE")) {
        if (walpe_ws_available ())
            test_live ();
        else
            g_printerr ("live: built without libsoup, skipped\n");
    }
    g_printerr ("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures,
                failures == 1 ? "" : "s");
    char *rm[] = { "rm", "-rf", home, NULL };
    g_spawn_sync (NULL, rm, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
    g_free (cfg);
    g_free (cache);
    g_free (home);
    g_free (fixdir);
    return failures ? 1 : 0;
}
