/* ctl_test - walpe-ctl's command line and the "set" request
 *
 *   ctl-test <walpe-ctl binary> <src/daemon/main.c>
 *
 * No daemon, no display. Three parts:
 *  1. bad input: walpe-ctl says what is wrong and exits with 1 before it
 *     talks to anybody (it used to send 0 for "abc", skip arguments without
 *     '=', take "--monitor" without a number for nothing); good input
 *     without a daemon exits with 2;
 *  2. a stand-in daemon on a private socket records what walpe-ctl sends:
 *     value types by key, several setprop pairs as one request, the
 *     --monitor / --path target, the sentence for a daemon that is older;
 *  3. common/setkeys.h: an unknown member or a value of the wrong type
 *     refuses the whole request, and every member the daemon's ipc_set ()
 *     reads is in the table (read from the source file). */
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "common/config.h"
#include "common/ipc.h"
#include "common/setkeys.h"
#include "common/quality.h"
#include "common/util.h"

static int fails;
#define CHECK(c) do { if (!(c)) { g_printerr ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static const char *ctl;
static GMainLoop *loop;
static GMutex lock;
static char *last_req;

/* run walpe-ctl with the arguments (NULL-terminated); *err_out = its stderr */
static int run_ctl (char **err_out, ...)
{
    GPtrArray *av = g_ptr_array_new ();
    g_ptr_array_add (av, (gpointer) ctl);
    va_list ap;
    va_start (ap, err_out);
    for (const char *a; (a = va_arg (ap, const char *));)
        g_ptr_array_add (av, (gpointer) a);
    va_end (ap);
    g_ptr_array_add (av, NULL);
    char *out = NULL, *err = NULL;
    int status = -1;
    GError *e = NULL;
    if (!g_spawn_sync (NULL, (char **) av->pdata, NULL, G_SPAWN_DEFAULT, NULL, NULL, &out, &err, &status,
                       &e)) {
        g_printerr ("spawn: %s\n", e->message);
        g_clear_error (&e);
    }
    g_ptr_array_free (av, TRUE);
    g_free (out);
    if (err_out)
        *err_out = err;
    else
        g_free (err);
    return WIFEXITED (status) ? WEXITSTATUS (status) : -1;
}

#define BAD(text, ...) do { \
        char *e_ = NULL; \
        int rc_ = run_ctl (&e_, __VA_ARGS__, NULL); \
        if (rc_ != 1 || !e_ || !strstr (e_, text)) { \
            g_printerr ("FAIL %s:%d: exit %d, stderr: %s\n", __FILE__, __LINE__, rc_, e_ ? e_ : ""); \
            fails++; \
        } \
        g_free (e_); \
    } while (0)
#define NODAEMON(...) CHECK (run_ctl (NULL, __VA_ARGS__, NULL) == 2)

/* the stand-in daemon: remembers the request; "quality" and "setprops"
 * are commands it does not have */
static char *handler (const char *req, gpointer user)
{
    (void) user;
    g_mutex_lock (&lock);
    g_free (last_req);
    last_req = g_strdup (req);
    g_mutex_unlock (&lock);
    if (strstr (req, "\"cmd\":\"quality\"") || strstr (req, "\"cmd\":\"cancel\""))
        return g_strdup ("{\"ok\":false,\"error\":\"unknown cmd\"}");
    if (strstr (req, "\"refuse\""))
        return g_strdup ("{\"ok\":false,\"error\":\"no\"}");
    return g_strdup ("{\"ok\":true}");
}

/* the request the stand-in got, as an object (unref) */
static JsonNode *sent (void)
{
    g_mutex_lock (&lock);
    JsonNode *n = last_req ? wpj_parse_data (last_req, -1) : NULL;
    g_clear_pointer (&last_req, g_free);
    g_mutex_unlock (&lock);
    return n;
}

static gboolean is_type (JsonObject *o, const char *k, GType t)
{
    JsonNode *n = o ? json_object_get_member (o, k) : NULL;
    return n && JSON_NODE_HOLDS_VALUE (n) && json_node_get_value_type (n) == t;
}

static gpointer client_thread (gpointer u)
{
    (void) u;
    JsonNode *n;
    JsonObject *o;
    char *err = NULL;

    /* set: several keys in one request, each with the type its key has */
    CHECK (run_ctl (NULL, "set", "fps=48", "covered=off", "steam_user=on", "focus=limit",
                    "transparent_windows=st,foot", "previews=off", "particle_density=60",
                    "software_threads=auto", "volume=0.25", "shared=TRUE", "preset=low", NULL) == 0);
    n = sent ();
    o = n ? json_node_get_object (n) : NULL;
    CHECK (o && !g_strcmp0 (wpj_str (o, "cmd", ""), "set"));
    CHECK (is_type (o, "fps", G_TYPE_INT64) && wpj_num (o, "fps", 0) == 48);
    CHECK (is_type (o, "pause_covered", G_TYPE_BOOLEAN) && !wpj_bool (o, "pause_covered", TRUE));
    CHECK (is_type (o, "steam_user", G_TYPE_STRING) && !g_strcmp0 (wpj_str (o, "steam_user", ""), "on"));
    CHECK (!g_strcmp0 (wpj_str (o, "focus_action", ""), "limit"));
    CHECK (!g_strcmp0 (wpj_str (o, "transparent_windows", ""), "st,foot"));
    CHECK (is_type (o, "preview_anim", G_TYPE_BOOLEAN));
    CHECK (wpj_num (o, "particle_density", 0) == 60);
    CHECK (!g_strcmp0 (wpj_str (o, "software_threads", ""), "auto"));
    CHECK (wpj_num (o, "volume", 0) == 0.25);
    CHECK (is_type (o, "render_shared", G_TYPE_BOOLEAN) && wpj_bool (o, "render_shared", FALSE));
    CHECK (!g_strcmp0 (wpj_str (o, "quality_preset", ""), "low"));
    CHECK (o && !walpe_set_check (o));      /* and the daemon would take all of it */
    if (n)
        json_node_unref (n);

    /* a key with a quote in it stays one member of a "set" (it used to
     * end the member and bring its own "cmd" along) */
    CHECK (run_ctl (NULL, "set", "zz\":1,\"cmd\":\"quit\",\"y=on", NULL) == 0);
    n = sent ();
    o = n ? json_node_get_object (n) : NULL;
    CHECK (o && !g_strcmp0 (wpj_str (o, "cmd", ""), "set") && json_object_get_size (o) == 2 &&
           json_object_has_member (o, "zz\":1,\"cmd\":\"quit\",\"y"));
    if (o) {
        char *e = walpe_set_check (o);      /* and the daemon names it as unknown */
        CHECK (e && strstr (e, "unknown setting"));
        g_free (e);
    }
    if (n)
        json_node_unref (n);

    /* setprop: one pair as "setprop", several as one "setprops"; the target */
    CHECK (run_ctl (NULL, "setprop", "speed=2", NULL) == 0);
    n = sent ();
    o = n ? json_node_get_object (n) : NULL;
    CHECK (o && !g_strcmp0 (wpj_str (o, "cmd", ""), "setprop") &&
           !g_strcmp0 (wpj_str (o, "key", ""), "speed") && wpj_num (o, "value", 0) == 2 &&
           !json_object_has_member (o, "monitor") && !json_object_has_member (o, "path"));
    if (n)
        json_node_unref (n);
    CHECK (run_ctl (NULL, "setprop", "--monitor", "1", "speed=2", "title=a b", "clock=off", NULL) == 0);
    n = sent ();
    o = n ? json_node_get_object (n) : NULL;
    JsonObject *pr = o ? wpj_obj (o, "props") : NULL;
    CHECK (o && !g_strcmp0 (wpj_str (o, "cmd", ""), "setprops") && wpj_num (o, "monitor", -1) == 1);
    CHECK (pr && json_object_get_size (pr) == 3 && wpj_num (pr, "speed", 0) == 2 &&
           !g_strcmp0 (wpj_str (pr, "title", ""), "a b") && is_type (pr, "clock", G_TYPE_BOOLEAN));
    if (n)
        json_node_unref (n);
    CHECK (run_ctl (NULL, "props", "--path=/lib/123", NULL) == 0);
    n = sent ();
    o = n ? json_node_get_object (n) : NULL;
    CHECK (o && !g_strcmp0 (wpj_str (o, "cmd", ""), "props") &&
           !g_strcmp0 (wpj_str (o, "path", ""), "/lib/123"));
    if (n)
        json_node_unref (n);
    CHECK (run_ctl (NULL, "resetprops", "--monitor=0", NULL) == 0);
    n = sent ();
    o = n ? json_node_get_object (n) : NULL;
    CHECK (o && !g_strcmp0 (wpj_str (o, "cmd", ""), "resetprops") && wpj_num (o, "monitor", -1) == 0);
    if (n)
        json_node_unref (n);

    /* click / scroll: the numbers as given */
    CHECK (run_ctl (NULL, "click", "500", "400", "3", "2", NULL) == 0);
    n = sent ();
    o = n ? json_node_get_object (n) : NULL;
    CHECK (o && wpj_num (o, "x", 0) == 500 && wpj_num (o, "y", 0) == 400 &&
           wpj_num (o, "button", 0) == 3 && wpj_num (o, "count", 0) == 2);
    if (n)
        json_node_unref (n);
    CHECK (run_ctl (NULL, "scroll", "500", "400", "-3", NULL) == 0);
    n = sent ();
    o = n ? json_node_get_object (n) : NULL;
    CHECK (o && !g_strcmp0 (wpj_str (o, "action", ""), "scroll") && wpj_num (o, "dy", 0) == -3);
    if (n)
        json_node_unref (n);

    /* open --monitor N */
    CHECK (run_ctl (NULL, "open", "/lib/123", "--monitor", "1", NULL) == 0);
    n = sent ();
    o = n ? json_node_get_object (n) : NULL;
    CHECK (o && !g_strcmp0 (wpj_str (o, "path", ""), "/lib/123") && wpj_num (o, "monitor", -1) == 1);
    if (n)
        json_node_unref (n);

    /* the daemon refused: 3; a command an older daemon does not have: 3 and a sentence */
    CHECK (run_ctl (NULL, "setprop", "refuse=1", NULL) == 3);
    CHECK (run_ctl (&err, "quality", NULL) == 3);
    CHECK (err && strstr (err, "older than this walpe-ctl"));
    g_free (err);

    g_main_loop_quit (loop);
    return NULL;
}

/* part 3: every member the "set" request is asked for is in the table */
static void scan_daemon (const char *path)
{
    char *txt = NULL;
    if (!g_file_get_contents (path, &txt, NULL, NULL)) {
        CHECK (!"daemon source readable");
        return;
    }
    /* the merged tree: the body of "set" is ipc_set_keys () (ipc_set () became
     * a wrapper that flushes the reloads once) and the effect shader keys
     * are read by ipc_fx_keys () */
    static const char *const heads[] = {
        "\nstatic void ipc_set_keys (JsonBuilder *b, JsonObject *o)\n{",
        "\nstatic gboolean ipc_fx_keys (JsonObject *o, gboolean *given, const char **err)\n{",
    };
    int found = 0;
    for (guint hi = 0; hi < G_N_ELEMENTS (heads); hi++) {
    const char *a = strstr (txt, heads[hi]);
    const char *z = a ? strstr (a, "\n}\n") : NULL;
    CHECK (a && z);
    if (a && z) {
        char *body = g_strndup (a, (gsize) (z - a));
        GRegex *re = g_regex_new ("\\(o, \"([a-z_0-9]+)\"", 0, 0, NULL);
        GMatchInfo *mi = NULL;
        g_regex_match (re, body, 0, &mi);
        while (g_match_info_matches (mi)) {
            char *k = g_match_info_fetch (mi, 1);
            found++;
            if (!walpe_set_key_find (k)) {
                g_printerr ("FAIL: ipc_set reads \"%s\", which common/setkeys.h does not list\n", k);
                fails++;
            }
            g_free (k);
            g_match_info_next (mi, NULL);
        }
        g_match_info_free (mi);
        g_regex_unref (re);
        g_free (body);
    }
    }
    CHECK (found > 40);     /* the scan saw the functions */
    g_free (txt);
}

static char *check_json (const char *req)
{
    JsonNode *n = wpj_parse_data (req, -1);
    char *e = n && JSON_NODE_HOLDS_OBJECT (n) ? walpe_set_check (json_node_get_object (n)) : g_strdup ("?");
    if (n)
        json_node_unref (n);
    return e;
}

#define REFUSED(req, text) do { \
        char *e_ = check_json (req); \
        if (!e_ || !strstr (e_, text)) { \
            g_printerr ("FAIL %s:%d: %s -> %s\n", __FILE__, __LINE__, req, e_ ? e_ : "(accepted)"); \
            fails++; \
        } \
        g_free (e_); \
    } while (0)
#define TAKEN(req) do { \
        char *e_ = check_json (req); \
        if (e_) { \
            g_printerr ("FAIL %s:%d: %s -> %s\n", __FILE__, __LINE__, req, e_); \
            fails++; \
        } \
        g_free (e_); \
    } while (0)

int main (int argc, char **argv)
{
    if (argc < 3) {
        g_printerr ("usage: ctl-test <walpe-ctl> <src/daemon/main.c>\n");
        return 2;
    }
    ctl = argv[1];
    /* everything private, before GLib reads the directories */
    char *dir = g_dir_make_tmp ("walpe-ctl-test-XXXXXX", NULL);
    g_setenv ("XDG_RUNTIME_DIR", dir, TRUE);
    g_setenv ("XDG_CONFIG_HOME", dir, TRUE);
    g_setenv ("HOME", dir, TRUE);
    char *sock = walpe_socket_path ();
    /* a temp dir too long for a socket address makes every WalPE program
     * fall back to a socket in a directory shared by all sessions of the
     * user: never talk to (or stand in for) whatever listens there */
    gboolean private_sock = g_str_has_prefix (sock, dir);
    if (!private_sock) {
        g_print ("ctl-test: %s is too long for a socket path (TMPDIR): the walpe-ctl runs are skipped\n", dir);
        goto table;
    }

    /* ---- 1. bad input, no daemon */
    CHECK (run_ctl (NULL, NULL) == 1);
    CHECK (run_ctl (NULL, "help", NULL) == 0);
    CHECK (run_ctl (NULL, "--help", NULL) == 0);
    CHECK (run_ctl (NULL, "frobnicate", NULL) == 1);
    BAD ("at least one", "set");
    BAD ("is not <key>=<value>", "set", "fps", "30");
    BAD ("is not <key>=<value>", "set", "fps=30", "covered");
    BAD ("is not <key>=<value>", "set", "=3");
    BAD ("needs a number", "set", "fps=abc");
    BAD ("needs a number", "set", "fps=");
    BAD ("needs a number", "set", "volume=loud");
    BAD ("threads or auto", "set", "software_threads=many");
    BAD ("max, high, balanced or low", "set", "preset=best");
    BAD ("fx_rate", "set", "fps=30", "fx_rate=7");
    BAD ("whole number", "click", "abc", "def");
    BAD ("whole number", "click", "10", "10x");
    BAD ("BUTTON", "click", "10", "10", "9");
    BAD ("COUNT", "click", "10", "10", "1", "0");
    BAD ("DY", "scroll", "10", "10", "down");
    BAD ("--monitor needs", "open", "/lib/123", "--monitor");
    BAD ("--monitor needs", "open", "/lib/123", "--monitor", "one");
    BAD ("one wallpaper", "open", "/lib/123", "/lib/456");
    BAD ("is not <key>=<value>", "setprop", "speed");
    BAD ("needs a value", "setprop", "speed=2", "--monitor");
    BAD ("setprop needs", "setprop", "--monitor", "1");
    BAD ("--monitor N or --path P", "props", "--all");
    BAD ("1..600", "profile", "0.5");
    BAD ("say which wallpaper", "quality", "fx_rate=2");
    CHECK (run_ctl (NULL, "click", "10", NULL) == 1);
    CHECK (run_ctl (NULL, "open", NULL) == 1);
    CHECK (run_ctl (NULL, "setprop", NULL) == 1);
    NODAEMON ("status");
    NODAEMON ("set", "fps=30", "covered=off");
    NODAEMON ("click", "10", "10");
    NODAEMON ("setprop", "a=1", "b=2");
    NODAEMON ("props", "--monitor", "1");
    NODAEMON ("quality");
    NODAEMON ("cancel-import");

    /* ---- 2. what goes over the socket */
    loop = g_main_loop_new (NULL, FALSE);
    CHECK (walpe_ipc_serve (sock, handler, NULL));
    GThread *t = g_thread_new ("client", client_thread, NULL);
    g_main_loop_run (loop);
    g_thread_join (t);
    walpe_ipc_release (sock);

table:
    /* ---- 3. the daemon's check of a "set" request */
    TAKEN ("{\"cmd\":\"set\",\"fps\":30}");
    TAKEN ("{\"cmd\":\"set\",\"pause_covered\":false,\"pause_fullscreen\":1,\"volume\":0.5}");
    TAKEN ("{\"cmd\":\"set\",\"software_threads\":\"auto\",\"preview_anim\":false,\"particle_lowres\":\"all\"}");
    TAKEN ("{\"cmd\":\"set\",\"fx_rate\":\"3\",\"post\":\"off\",\"render_scale\":50,\"quality_preset\":\"low\"}");
    TAKEN ("{\"cmd\":\"set\",\"we_assets\":\"\",\"steam_user\":\"me\",\"present\":\"limiter\"}");
    REFUSED ("{\"cmd\":\"set\",\"coverd\":false,\"fps\":30}", "unknown setting \"coverd\"");
    REFUSED ("{\"cmd\":\"set\",\"fps\":true}", "fps needs a number");
    REFUSED ("{\"cmd\":\"set\",\"fps\":\"30\"}", "fps needs a number");
    REFUSED ("{\"cmd\":\"set\",\"pause_covered\":\"yes\"}", "pause_covered is a switch");
    REFUSED ("{\"cmd\":\"set\",\"focus_action\":false}", "focus_action needs a name");
    REFUSED ("{\"cmd\":\"set\",\"parallax\":[true]}", "parallax is a switch");
    REFUSED ("{\"cmd\":\"set\",\"particle_density\":{\"a\":1}}", "single value");
    /* every quality key walpe-ctl knows is a member the daemon takes */
    static const char *const qk[] = { "preset", "quality_preset", "fx_rate", "fx_rate_mode", "fx_res",
                                      "fps_idle", "post", "post_processing", "render_scale", "scale",
                                      "tex_res", "tex", "texture_slack", "slack" };
    for (guint i = 0; i < G_N_ELEMENTS (qk); i++) {
        gboolean taken = FALSE;
        JsonBuilder *b = json_builder_new ();
        json_builder_begin_object (b);
        walpe_quality_kv_to_json (b, qk[i], "default", &taken, NULL);
        json_builder_end_object (b);
        g_object_unref (b);
        CHECK (taken && walpe_set_key_find (qk[i]));
    }
    scan_daemon (argv[2]);

    char *lk = g_strconcat (sock, ".lock", NULL);
    if (private_sock) {
        g_unlink (lk);
        g_unlink (sock);
    }
    g_rmdir (dir);
    g_free (lk);
    g_free (sock);
    g_free (dir);
    if (fails)
        g_printerr ("ctl-test: %d check(s) failed\n", fails);
    else
        g_print ("ctl-test: %s\n", private_sock ? "ok" : "ok (in part: see above)");
    return fails ? 1 : private_sock ? 0 : 77;
}
