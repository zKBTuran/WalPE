/* steamsub_test.c - Steam subscriptions (src/common/steamsub.c, the
 * subscribe/unsubscribe call of workshop_query.c, the delete plan's
 * "Steam client folder" flag).
 *
 * No Steam, no network: HOME and the XDG dirs are a private temp dir, the
 * "running Steam client" is this process holding <HOME>/.steam/steam.pipe
 * open with its pid in steam.pid (what Valve's steam.sh checks), `steam` and
 * `xdg-open` are stub scripts on a private PATH that record their arguments,
 * and api.steampowered.com is a libsoup server on 127.0.0.1
 * (WALPE_WS_API_BASE).
 *
 * Run: meson test -C build steamsub -v */
#include "walpe-build-config.h"
#include "common/steamsub.h"
#include "common/library.h"
#include "common/workshop.h"
#include "common/util.h"
#include <glib/gstdio.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef HAVE_SOUP
#include <libsoup/soup.h>
#endif

static int failures;
static char *T;   /* temp root (realpath) = HOME */

#define CHECK(c) do { if (!(c)) { g_printerr ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                                 failures++; } } while (0)

#define ID "3219908811"
#define KEY "0123456789ABCDEF0123456789ABCDEF"

static char *P (const char *rel)
{
    return g_build_filename (T, rel, NULL);
}

static void mkfile (const char *rel, const char *text, int mode)
{
    char *p = P (rel);
    char *d = g_path_get_dirname (p);
    g_mkdir_with_parents (d, 0755);
    if (!g_file_set_contents (p, text, -1, NULL))
        g_error ("cannot write %s", p);
    if (mode)
        g_chmod (p, mode);
    g_free (d);
    g_free (p);
}

static void rmfile (const char *rel)
{
    char *p = P (rel);
    g_unlink (p);
    g_free (p);
}

/* ------------------------------------------------------------ ids, urls */

static void test_urls (void)
{
    CHECK (walpe_steam_valid_id (ID));
    CHECK (walpe_steam_valid_id ("1"));
    CHECK (!walpe_steam_valid_id (""));
    CHECK (!walpe_steam_valid_id (NULL));
    CHECK (!walpe_steam_valid_id ("000"));
    CHECK (!walpe_steam_valid_id ("12a4"));
    CHECK (!walpe_steam_valid_id ("123/../4"));
    CHECK (!walpe_steam_valid_id ("123456789012345678901"));   /* 21 digits */
    char *u = walpe_steam_item_client_url (ID);
    CHECK (!g_strcmp0 (u, "steam://url/CommunityFilePage/" ID));
    g_free (u);
    u = walpe_steam_item_web_url (ID);
    CHECK (!g_strcmp0 (u, "https://steamcommunity.com/sharedfiles/filedetails/?id=" ID));
    g_free (u);
    CHECK (walpe_steam_item_client_url ("1;rm -rf") == NULL);
    CHECK (walpe_steam_item_web_url ("x") == NULL);
}

/* ------------------------------------------------------------------ marks */

static void test_marks (void)
{
    char *p = P ("cfg/walpe/workshop-subscribed"), *data = NULL;
    CHECK (!walpe_steam_sub_is_marked (ID));
    walpe_steam_sub_mark (ID, FALSE);            /* nothing to remove: no file */
    CHECK (!g_file_test (p, G_FILE_TEST_EXISTS));
    walpe_steam_sub_mark (ID, TRUE);
    walpe_steam_sub_mark ("111222333", TRUE);
    walpe_steam_sub_mark (ID, TRUE);             /* once */
    walpe_steam_sub_mark ("../etc", TRUE);       /* not an id */
    CHECK (walpe_steam_sub_is_marked (ID));
    CHECK (walpe_steam_sub_is_marked ("111222333"));
    CHECK (!walpe_steam_sub_is_marked ("111"));
    CHECK (g_file_get_contents (p, &data, NULL, NULL));
    CHECK (!g_strcmp0 (data, ID "\n111222333\n"));
    g_free (data);
    walpe_steam_sub_mark (ID, FALSE);
    CHECK (!walpe_steam_sub_is_marked (ID));
    CHECK (walpe_steam_sub_is_marked ("111222333"));
    walpe_steam_sub_mark ("111222333", FALSE);
    CHECK (g_file_get_contents (p, &data, NULL, NULL) && !*data);
    g_free (data);
    g_free (p);
}

/* ------------------------------------------------- "is Steam running" */

static int pipe_fd = -1;

static void fake_steam (gboolean on)
{
    char *pipe = P (".steam/steam.pipe");
    if (on && pipe_fd < 0) {
        char *dir = g_path_get_dirname (pipe);
        g_mkdir_with_parents (dir, 0755);
        g_free (dir);
        if (mkfifo (pipe, 0600) != 0 && errno != EEXIST)
            g_error ("mkfifo %s", pipe);
        pipe_fd = open (pipe, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        char *pid = g_strdup_printf ("%d\n", (int) getpid ());
        mkfile (".steam/steam.pid", pid, 0);
        g_free (pid);
    } else if (!on && pipe_fd >= 0) {
        close (pipe_fd);
        pipe_fd = -1;
    }
    g_free (pipe);
}

static void test_running (void)
{
    CHECK (!walpe_steam_client_running ());          /* no ~/.steam at all */
    char *pid = g_strdup_printf ("%d\n", (int) getpid ());
    mkfile (".steam/steam.pid", pid, 0);
    CHECK (!walpe_steam_client_running ());          /* live pid, no pipe: recycled pid */
    fake_steam (TRUE);
    CHECK (pipe_fd >= 0);
    CHECK (walpe_steam_client_running ());
    mkfile (".steam/steam.pid", "0\n", 0);
    CHECK (!walpe_steam_client_running ());
    mkfile (".steam/steam.pid", "not a pid", 0);
    CHECK (!walpe_steam_client_running ());
    mkfile (".steam/steam.pid", "", 0);
    CHECK (!walpe_steam_client_running ());
    mkfile (".steam/steam.pid", "4194999\n", 0);     /* above pid_max: nobody */
    CHECK (!walpe_steam_client_running ());
    mkfile (".steam/steam.pid", "1\n", 0);           /* init: not ours to read, no pipe */
    CHECK (!walpe_steam_client_running ());
    mkfile (".steam/steam.pid", pid, 0);
    CHECK (walpe_steam_client_running ());
    fake_steam (FALSE);                              /* Steam quit: stale pid file */
    CHECK (!walpe_steam_client_running ());
    g_free (pid);

    CHECK (!walpe_steam_we_installed ());
    mkfile (".local/share/Steam/steamapps/appmanifest_431960.acf", "\"AppState\"{}\n", 0);
    CHECK (walpe_steam_we_installed ());
    rmfile (".local/share/Steam/steamapps/appmanifest_431960.acf");
    CHECK (!walpe_steam_we_installed ());
}

/* --------------------------------------------------- the page command */

/* stub that appends "<name> <args>" to <T>/launched, then lingers */
static void stub (const char *name, gboolean present)
{
    char *rel = g_strdup_printf ("bin/%s", name);
    if (present) {
        char *sh = g_strdup_printf ("#!/bin/sh\nPATH=/usr/bin:/bin\necho \"%s $*\" >> '%s/launched'\n"
                                    "sleep 3\n", name, T);
        mkfile (rel, sh, 0755);
        g_free (sh);
    } else {
        rmfile (rel);
    }
    g_free (rel);
}

static char *launched (void)
{
    char *p = P ("launched"), *data = NULL;
    for (int i = 0; i < 100 && !data; i++) {   /* the child runs on its own */
        if (!g_file_get_contents (p, &data, NULL, NULL) || !*data) {
            g_clear_pointer (&data, g_free);
            g_usleep (50 * 1000);
        }
    }
    g_unlink (p);
    g_free (p);
    return data ? g_strstrip (data) : g_strdup ("");
}

static void test_page (void)
{
    char *bin = P ("bin");
    g_mkdir_with_parents (bin, 0755);
    g_setenv ("PATH", bin, TRUE);
    WalpeSteamPageWhere w = WALPE_STEAM_PAGE_CLIENT;
    GError *err = NULL;

    /* nothing installed, Steam not running: the caller opens the web URL */
    char **v = walpe_steam_item_page_argv (ID, &w, &err);
    CHECK (v == NULL && err == NULL && w == WALPE_STEAM_PAGE_BROWSER);

    /* bad id: an error, never a command */
    v = walpe_steam_item_page_argv ("12;x", &w, &err);
    CHECK (v == NULL && err != NULL);
    g_clear_error (&err);
    CHECK (!walpe_steam_open_item_page ("", &w, &err) && err != NULL);
    g_clear_error (&err);

    stub ("steam", TRUE);
    stub ("xdg-open", TRUE);
    /* Steam not running: never `steam` (it would start the client and stay) */
    v = walpe_steam_item_page_argv (ID, &w, &err);
    CHECK (v && g_strv_length (v) == 2 && !g_strcmp0 (v[0], "xdg-open"));
    CHECK (v && !g_strcmp0 (v[1], "https://steamcommunity.com/sharedfiles/filedetails/?id=" ID));
    CHECK (w == WALPE_STEAM_PAGE_BROWSER);
    g_strfreev (v);
    gint64 t0 = g_get_monotonic_time ();
    CHECK (walpe_steam_open_item_page (ID, &w, &err) && w == WALPE_STEAM_PAGE_BROWSER);
    CHECK (g_get_monotonic_time () - t0 < 1500000);   /* the stub sleeps 3 s: not waited for */
    char *l = launched ();
    CHECK (!g_strcmp0 (l, "xdg-open https://steamcommunity.com/sharedfiles/filedetails/?id=" ID));
    g_free (l);

    /* Steam running: the launcher hands the steam:// URL to the client */
    fake_steam (TRUE);
    v = walpe_steam_item_page_argv (ID, &w, &err);
    CHECK (v && g_strv_length (v) == 2 && !g_strcmp0 (v[0], "steam"));
    CHECK (v && !g_strcmp0 (v[1], "steam://url/CommunityFilePage/" ID));
    CHECK (w == WALPE_STEAM_PAGE_CLIENT);
    g_strfreev (v);
    t0 = g_get_monotonic_time ();
    CHECK (walpe_steam_open_item_page (ID, &w, &err) && w == WALPE_STEAM_PAGE_CLIENT);
    CHECK (g_get_monotonic_time () - t0 < 1500000);
    l = launched ();
    CHECK (!g_strcmp0 (l, "steam steam://url/CommunityFilePage/" ID));
    g_free (l);

    /* running, but no `steam` in PATH: the desktop's steam:// handler */
    stub ("steam", FALSE);
    v = walpe_steam_item_page_argv (ID, &w, &err);
    CHECK (v && !g_strcmp0 (v[0], "xdg-open") &&
           !g_strcmp0 (v[1], "steam://url/CommunityFilePage/" ID));
    CHECK (w == WALPE_STEAM_PAGE_CLIENT);
    g_strfreev (v);

    /* running, nothing to launch it with: the web URL is the caller's */
    stub ("xdg-open", FALSE);
    v = walpe_steam_item_page_argv (ID, &w, &err);
    CHECK (v == NULL && err == NULL && w == WALPE_STEAM_PAGE_BROWSER);

    /* a command that cannot be executed is an error, not a silent success */
    stub ("xdg-open", TRUE);
    char *x = P ("bin/xdg-open");
    g_chmod (x, 0644);
    v = walpe_steam_item_page_argv (ID, &w, &err);
    CHECK (v == NULL && w == WALPE_STEAM_PAGE_BROWSER);   /* not executable = not there */
    g_chmod (x, 0755);
    g_free (x);
    fake_steam (FALSE);
    g_free (bin);
}

/* ------------------------------------------------------------ delete plan */

static void wallpaper (const char *rel)
{
    char *pj = g_strdup_printf ("%s/project.json", rel);
    mkfile (pj, "{\"type\":\"scene\",\"title\":\"t\",\"file\":\"scene.json\"}\n", 0);
    g_free (pj);
}

#define STEAM ".local/share/Steam/steamapps/workshop/content/431960"
#define SCMD "scmd/steamapps/workshop/content/431960"

static void test_plan (void)
{
    char *lib = P ("lib");
    wallpaper ("lib/100000001");               /* WalPE library only */
    wallpaper ("lib/100000002");               /* + Steam client copy */
    wallpaper (STEAM "/100000002");
    wallpaper (STEAM "/100000003");            /* Steam client only */
    wallpaper ("lib/100000004");               /* + steamcmd download */
    wallpaper (SCMD "/100000004");
    wallpaper (SCMD "/100000005");             /* steamcmd download only */
    char *root = P ("scmd");
    mkfile ("cache/walpe/steamcmd-root", root, 0);
    g_free (root);

    char *p = P ("lib/100000001");
    WalpeDelPlan *pl = walpe_library_delete_plan (p, lib, NULL);
    CHECK (pl && pl->targets->len == 1 && !pl->steam_client && !pl->steam_managed);
    CHECK (pl && !g_strcmp0 (pl->workshop_id, "100000001"));
    walpe_del_plan_free (pl);
    g_free (p);

    p = P ("lib/100000002");
    pl = walpe_library_delete_plan (p, lib, NULL);
    CHECK (pl && pl->targets->len == 2 && pl->steam_client && pl->steam_managed);
    if (pl && pl->targets->len == 2) {
        WalpeDelTarget *a = pl->targets->pdata[0], *b = pl->targets->pdata[1];
        CHECK (!a->steam_client && a->root == WALPE_ROOT_LIBRARY);
        CHECK (b->steam_client && b->root == WALPE_ROOT_STEAM);
    }
    walpe_del_plan_free (pl);
    g_free (p);

    p = P (STEAM "/100000003");
    pl = walpe_library_delete_plan (p, lib, NULL);
    CHECK (pl && pl->targets->len == 1 && pl->steam_client);
    walpe_del_plan_free (pl);
    g_free (p);

    /* what steamcmd downloaded is not something Steam restores */
    p = P ("lib/100000004");
    pl = walpe_library_delete_plan (p, lib, NULL);
    CHECK (pl && pl->targets->len == 2 && pl->steam_managed && !pl->steam_client);
    walpe_del_plan_free (pl);
    g_free (p);
    p = P (SCMD "/100000005");
    pl = walpe_library_delete_plan (p, lib, NULL);
    CHECK (pl && pl->targets->len == 1 && pl->steam_managed && !pl->steam_client);
    walpe_del_plan_free (pl);
    g_free (p);

    /* the WalPE library pointed at Steam's own folder is a Steam folder */
    char *steam = P (STEAM);
    p = P (STEAM "/100000003");
    pl = walpe_library_delete_plan (p, steam, NULL);
    CHECK (pl && pl->targets->len == 1 && pl->steam_client);
    if (pl && pl->targets->len == 1)
        CHECK (((WalpeDelTarget *) pl->targets->pdata[0])->root == WALPE_ROOT_LIBRARY);
    walpe_del_plan_free (pl);
    g_free (p);
    g_free (steam);

    /* steamcmd sharing the Steam client's root: a Steam client folder */
    root = P (".local/share/Steam");
    mkfile ("cache/walpe/steamcmd-root", root, 0);
    g_free (root);
    p = P (STEAM "/100000003");
    pl = walpe_library_delete_plan (p, lib, NULL);
    CHECK (pl && pl->targets->len == 1 && pl->steam_client);
    walpe_del_plan_free (pl);
    g_free (p);
    g_free (lib);
}

/* -------------------------------------------------------------- settings */

static void test_settings (void)
{
    WalpeWsSettings *s = walpe_ws_settings_load ();
    CHECK (!s->subscribe_on_download && !s->unsubscribe_on_delete);   /* both opt-in */
    s->api_key = g_strdup (KEY);
    s->subscribe_on_download = TRUE;
    CHECK (walpe_ws_settings_save (s, NULL));
    walpe_ws_settings_free (s);
    walpe_ws_settings_set_unsubscribe_on_delete (TRUE);
    s = walpe_ws_settings_load ();
    CHECK (s->subscribe_on_download && s->unsubscribe_on_delete);
    CHECK (!g_strcmp0 (s->api_key, KEY));          /* the one-field update kept the rest */
    walpe_ws_settings_free (s);
    char *p = P ("cfg/walpe/workshop.json");
    GStatBuf st;
    CHECK (g_stat (p, &st) == 0 && (st.st_mode & 0777) == 0600);
    g_free (p);
    walpe_ws_settings_set_unsubscribe_on_delete (FALSE);
    s = walpe_ws_settings_load ();
    CHECK (s->subscribe_on_download && !s->unsubscribe_on_delete);
    walpe_ws_settings_free (s);
}

/* --------------------------------------------------------------- Web API */

#ifdef HAVE_SOUP

typedef enum { M_OK, M_OK_NO_HEADER, M_403, M_ERESULT, M_HTML, M_500, M_EMPTY } Mode;

static struct {
    Mode mode;
    int hits;
    char *path, *method, *body, *ctype;
} Srv;

static void handler (SoupServer *srv, SoupServerMessage *msg, const char *path,
                     GHashTable *query, gpointer user)
{
    (void) srv; (void) query; (void) user;
    Srv.hits++;
    g_free (Srv.path);
    g_free (Srv.method);
    g_free (Srv.body);
    g_free (Srv.ctype);
    Srv.path = g_strdup (path);
    Srv.method = g_strdup (soup_server_message_get_method (msg));
    SoupMessageBody *b = soup_server_message_get_request_body (msg);
    Srv.body = g_strndup (b->data ? b->data : "", (gsize) b->length);
    Srv.ctype = g_strdup (soup_message_headers_get_one (
        soup_server_message_get_request_headers (msg), "Content-Type"));
    SoupMessageHeaders *h = soup_server_message_get_response_headers (msg);
    const char *json = "{\"response\":{}}";
    switch (Srv.mode) {
    case M_OK:
        soup_message_headers_replace (h, "x-eresult", "1");
        /* fall through */
    case M_OK_NO_HEADER:
        soup_server_message_set_status (msg, 200, NULL);
        soup_server_message_set_response (msg, "application/json", SOUP_MEMORY_COPY, json,
                                          strlen (json));
        break;
    case M_403: {
        const char *t = "<html><body>Access is denied. Please verify your <pre>key=</pre> "
                        "parameter.</body></html>";
        soup_server_message_set_status (msg, 403, NULL);
        soup_server_message_set_response (msg, "text/html", SOUP_MEMORY_COPY, t, strlen (t));
        break;
    }
    case M_ERESULT:
        soup_message_headers_replace (h, "x-eresult", "15");
        soup_message_headers_replace (h, "x-error_message", "Access Denied for key " KEY);
        soup_server_message_set_status (msg, 200, NULL);
        soup_server_message_set_response (msg, "application/json", SOUP_MEMORY_COPY, "{}", 2);
        break;
    case M_HTML:
        soup_server_message_set_status (msg, 200, NULL);
        soup_server_message_set_response (msg, "text/html", SOUP_MEMORY_COPY, "<html>hi", 8);
        break;
    case M_EMPTY:
        soup_server_message_set_status (msg, 200, NULL);
        break;
    case M_500:
        soup_server_message_set_status (msg, 500, NULL);
        break;
    }
}

typedef struct { GMainLoop *loop; gboolean ok; GError *err; } Wait;

static void sub_done (GObject *src, GAsyncResult *res, gpointer user)
{
    (void) src;
    Wait *w = user;
    w->ok = walpe_ws_subscribe_finish (res, &w->err);
    g_main_loop_quit (w->loop);
}

/* one call against the mock; returns ok, *err for the caller to free */
static gboolean call (WalpeWsClient *c, const char *id, gboolean subscribe, Mode mode,
                      GError **err)
{
    Srv.mode = mode;
    Srv.hits = 0;
    Wait w = { g_main_loop_new (NULL, FALSE), FALSE, NULL };
    walpe_ws_subscribe_async (c, id, subscribe, NULL, sub_done, &w);
    g_main_loop_run (w.loop);
    g_main_loop_unref (w.loop);
    *err = w.err;
    return w.ok;
}

static void test_webapi (void)
{
    GError *err = NULL;
    SoupServer *srv = soup_server_new (NULL, NULL);
    soup_server_add_handler (srv, NULL, handler, NULL, NULL);
    if (!soup_server_listen_local (srv, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &err)) {
        g_printerr ("SKIP web api: cannot listen on 127.0.0.1 (%s)\n", err->message);
        g_clear_error (&err);
        g_object_unref (srv);
        return;
    }
    GSList *uris = soup_server_get_uris (srv);
    char *base = g_strdup_printf ("http://127.0.0.1:%d/", g_uri_get_port (uris->data));
    g_slist_free_full (uris, (GDestroyNotify) g_uri_unref);
    g_setenv ("WALPE_WS_API_BASE", base, TRUE);
    WalpeWsClient *c = walpe_ws_client_new ();

    /* no key: an error, and nothing is sent */
    CHECK (!call (c, ID, FALSE, M_OK, &err));
    CHECK (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED) && Srv.hits == 0);
    g_clear_error (&err);

    walpe_ws_client_set_api_key (c, KEY);
    CHECK (!call (c, "12x", FALSE, M_OK, &err));
    CHECK (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT) && Srv.hits == 0);
    g_clear_error (&err);

    /* unsubscribe, accepted */
    CHECK (call (c, ID, FALSE, M_OK, &err) && !err);
    CHECK (Srv.hits == 1 && !g_strcmp0 (Srv.method, "POST"));
    CHECK (!g_strcmp0 (Srv.path, "/IPublishedFileService/Unsubscribe/v1/"));
    CHECK (!g_strcmp0 (Srv.body, "key=" KEY "&publishedfileid=" ID
                                 "&list_type=1&appid=431960&notify_client=1"));
    CHECK (Srv.ctype && g_str_has_prefix (Srv.ctype, "application/x-www-form-urlencoded"));

    /* subscribe, accepted (a reply without the x-eresult header counts too) */
    CHECK (call (c, ID, TRUE, M_OK_NO_HEADER, &err) && !err);
    CHECK (Srv.hits == 1 && !g_strcmp0 (Srv.path, "/IPublishedFileService/Subscribe/v1/"));
    CHECK (Srv.body && strstr (Srv.body, "publishedfileid=" ID "&"));

    /* refused: HTTP 403 (what Steam answers to a key it does not take) */
    CHECK (!call (c, ID, FALSE, M_403, &err));
    CHECK (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED));
    CHECK (err && strstr (err->message, "403") && !strstr (err->message, KEY));
    g_clear_error (&err);

    /* HTTP 200 but EResult 15: not a success; the key is scrubbed */
    CHECK (!call (c, ID, FALSE, M_ERESULT, &err));
    CHECK (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED));
    CHECK (err && strstr (err->message, "EResult 15") && strstr (err->message, "access denied"));
    CHECK (err && !strstr (err->message, KEY) && strstr (err->message, "***"));
    g_clear_error (&err);

    /* HTTP 200 with something that is not the JSON answer: not a success */
    CHECK (!call (c, ID, TRUE, M_HTML, &err));
    CHECK (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA));
    g_clear_error (&err);
    CHECK (!call (c, ID, TRUE, M_EMPTY, &err));
    CHECK (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA));
    g_clear_error (&err);

    /* server error: a failure, and the request is not sent a second time
     * (a call that changes the account's subscriptions; tests/wsnet_test.c) */
    CHECK (!call (c, ID, FALSE, M_500, &err));
    CHECK (err != NULL && Srv.hits == 1 && !strstr (err->message, KEY));
    g_clear_error (&err);

    /* nobody listening: an error, not a hang */
    soup_server_disconnect (srv);
    g_object_unref (srv);
    CHECK (!call (c, ID, FALSE, M_OK, &err));
    CHECK (err != NULL && !strstr (err->message, KEY));
    g_clear_error (&err);

    walpe_ws_client_free (c);
    g_print ("steamsub: web api mock on %s exercised\n", base);

    /* the test override of the host is loopback http only: the key can not
     * be sent anywhere else by setting a variable (URL choice only; no
     * request is made here) */
    const char *real = "https://api.steampowered.com/";
    CHECK (!g_strcmp0 (walpe_ws_api_base (), base));
    const char *bad[] = {
        "http://example.invalid/", "https://127.0.0.1:8080/", "http://127.0.0.1:80@evil.invalid/",
        "http://127.0.0.1.evil.invalid:80/", "http://localhost:8080/", "http://127.0.0.1:8080",
        "http://127.0.0.1:8080/x/", "http://127.0.0.1:8080/?a=b", "http://user@127.0.0.1:8080/",
        "127.0.0.1:8080/", "", NULL,
    };
    for (int i = 0; bad[i]; i++) {
        g_setenv ("WALPE_WS_API_BASE", bad[i], TRUE);
        CHECK (!g_strcmp0 (walpe_ws_api_base (), real));
    }
    g_unsetenv ("WALPE_WS_API_BASE");
    CHECK (!g_strcmp0 (walpe_ws_api_base (), real));
    g_free (base);
}

#endif /* HAVE_SOUP */

int main (void)
{
    char *tmp = g_dir_make_tmp ("walpe-steamsub-XXXXXX", NULL);
    if (!tmp)
        g_error ("no temp dir");
    T = realpath (tmp, NULL);
    g_free (tmp);
    /* before GLib caches any of them */
    g_setenv ("HOME", T, TRUE);
    char *d = P ("cfg");
    g_setenv ("XDG_CONFIG_HOME", d, TRUE);
    g_free (d);
    d = P ("cache");
    g_setenv ("XDG_CACHE_HOME", d, TRUE);
    g_free (d);
    d = P ("data");
    g_setenv ("XDG_DATA_HOME", d, TRUE);
    g_free (d);
    /* the mock server is on loopback: no proxy in between */
    const char *px[] = { "http_proxy", "HTTP_PROXY", "https_proxy", "HTTPS_PROXY", "all_proxy",
                         "ALL_PROXY", NULL };
    for (int i = 0; px[i]; i++)
        g_unsetenv (px[i]);
    g_setenv ("no_proxy", "127.0.0.1,localhost", TRUE);
    g_setenv ("GSETTINGS_BACKEND", "memory", TRUE);

    test_urls ();
    test_marks ();
    test_running ();
    test_plan ();
    test_settings ();
#ifdef HAVE_SOUP
    if (walpe_ws_available ())
        test_webapi ();
#else
    g_printerr ("SKIP web api: built without libsoup\n");
#endif
    test_page ();   /* last: it replaces PATH */

    /* only what this test made */
    char *argv[] = { "/bin/rm", "-rf", "--", T, NULL };
    if (!failures && strstr (T, "walpe-steamsub-"))
        g_spawn_sync (NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, NULL, NULL, NULL);
    g_print (failures ? "steamsub: %d FAILED (kept %s)\n" : "steamsub: all ok\n", failures, T);
    return failures ? 1 : 0;
}
