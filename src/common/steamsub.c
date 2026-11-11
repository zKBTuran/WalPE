/* steamsub.c - see steamsub.h */
#include "steamsub.h"
#include "workshop.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

gboolean walpe_steam_valid_id (const char *id)
{
    gsize n = id ? strlen (id) : 0;
    if (n < 1 || n > 20 || strspn (id, "0123456789") != n)
        return FALSE;
    return strspn (id, "0") != n;
}

char *walpe_steam_item_web_url (const char *id)
{
    if (!walpe_steam_valid_id (id))
        return NULL;
    return g_strconcat ("https://steamcommunity.com/sharedfiles/filedetails/?id=", id, NULL);
}

char *walpe_steam_item_client_url (const char *id)
{
    if (!walpe_steam_valid_id (id))
        return NULL;
    return g_strconcat ("steam://url/CommunityFilePage/", id, NULL);
}

/* ---------------------------------------------------------- Steam running */

/* process `pid` has `pipe` (either spelling) among its open files */
static gboolean pid_has_open (const char *pid, const char *pipe_a, const char *pipe_b)
{
    char *fds = g_build_filename ("/proc", pid, "fd", NULL);
    GDir *d = g_dir_open (fds, 0, NULL);
    gboolean found = FALSE;
    const char *n;
    while (d && !found && (n = g_dir_read_name (d))) {
        char *l = g_build_filename (fds, n, NULL);
        char *t = g_file_read_link (l, NULL);
        found = t && (!strcmp (t, pipe_a) || (pipe_b && !strcmp (t, pipe_b)));
        g_free (t);
        g_free (l);
    }
    if (d)
        g_dir_close (d);
    g_free (fds);
    return found;
}

gboolean walpe_steam_client_running (void)
{
    /* steam.sh is_steam_running(): a pid file alone may be stale and its
     * pid recycled; the real client holds ~/.steam/steam.pipe open */
    char *cfg = g_build_filename (g_get_home_dir (), ".steam", NULL);
    char *pidf = g_build_filename (cfg, "steam.pid", NULL);
    char *pipe_a = g_build_filename (cfg, "steam.pipe", NULL);
    char *rcfg = realpath (cfg, NULL);   /* /proc shows resolved paths */
    char *pipe_b = rcfg ? g_build_filename (rcfg, "steam.pipe", NULL) : NULL;
    char *pid = NULL;
    gsize len = 0;
    gboolean running = FALSE;
    if (g_file_get_contents (pidf, &pid, &len, NULL) && len > 0 && len < 32) {
        g_strstrip (pid);
        gsize n = strlen (pid);
        if (n > 0 && strspn (pid, "0123456789") == n && strspn (pid, "0") != n)
            running = pid_has_open (pid, pipe_a, pipe_b);
    }
    g_free (pid);
    g_free (pipe_b);
    free (rcfg);
    g_free (pipe_a);
    g_free (pidf);
    g_free (cfg);
    return running;
}

gboolean walpe_steam_we_installed (void)
{
    char **roots = walpe_steam_library_roots ();
    gboolean found = FALSE;
    for (int i = 0; roots[i] && !found; i++) {
        char *acf = g_build_filename (roots[i], "steamapps",
                                      "appmanifest_" WALPE_WE_APPID ".acf", NULL);
        found = g_file_test (acf, G_FILE_TEST_IS_REGULAR);
        g_free (acf);
    }
    g_strfreev (roots);
    return found;
}

/* ------------------------------------------------------ the Workshop page */

static char **argv2 (const char *prog, const char *arg)
{
    char **v = g_new0 (char *, 3);
    v[0] = g_strdup (prog);
    v[1] = g_strdup (arg);
    return v;
}

static gboolean in_path (const char *prog)
{
    char *p = g_find_program_in_path (prog);
    gboolean found = p != NULL;
    g_free (p);
    return found;
}

char **walpe_steam_item_page_argv (const char *id, WalpeSteamPageWhere *where, GError **err)
{
    WalpeSteamPageWhere w = WALPE_STEAM_PAGE_BROWSER;
    char **argv = NULL;
    if (!walpe_steam_valid_id (id)) {
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                     "“%s” is not a Steam Workshop id", id ? id : "");
        if (where)
            *where = w;
        return NULL;
    }
    if (walpe_steam_client_running ()) {
        /* Valve's launcher forwards the URL to the running client and exits */
        char *url = walpe_steam_item_client_url (id);
        if (in_path ("steam"))
            argv = argv2 ("steam", url);
        else if (in_path ("xdg-open"))   /* x-scheme-handler/steam */
            argv = argv2 ("xdg-open", url);
        g_free (url);
        if (argv)
            w = WALPE_STEAM_PAGE_CLIENT;
    }
    if (!argv && in_path ("xdg-open")) {
        char *url = walpe_steam_item_web_url (id);
        argv = argv2 ("xdg-open", url);
        g_free (url);
    }
    if (where)
        *where = w;
    return argv;
}

static void detach_child (gpointer user)
{
    (void) user;
    setsid ();   /* not in our process group: it outlives the GUI */
}

gboolean walpe_steam_open_item_page (const char *id, WalpeSteamPageWhere *where, GError **err)
{
    WalpeSteamPageWhere w = WALPE_STEAM_PAGE_BROWSER;
    GError *e = NULL;
    char **argv = walpe_steam_item_page_argv (id, &w, &e);
    if (where)
        *where = w;
    if (e) {
        g_propagate_error (err, e);
        return FALSE;
    }
    gboolean ok;
    if (argv) {
        /* the GUI's own renderer choice (ui/main.c) is not the browser's */
        char **envp = g_get_environ ();
        if (g_getenv ("WALPE_UI_SET_GSK"))
            envp = g_environ_unsetenv (envp, "GSK_RENDERER");
        envp = g_environ_unsetenv (envp, "WALPE_UI_SET_GSK");
        envp = g_environ_unsetenv (envp, "WALPE_UI_STATS");
        /* without DO_NOT_REAP_CHILD GLib forks twice: nothing to reap, and
         * this returns as soon as the command was exec'd */
        ok = g_spawn_async (NULL, argv, envp,
                            G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
                                G_SPAWN_STDERR_TO_DEV_NULL,
                            detach_child, NULL, NULL, err);
        g_strfreev (envp);
        g_strfreev (argv);
    } else {
        /* no xdg-open: whatever GIO knows as the handler for https */
        char *url = walpe_steam_item_web_url (id);
        ok = g_app_info_launch_default_for_uri (url, NULL, err);
        g_free (url);
    }
    return ok;
}

/* ------------------------------------------------- subscribed by WalPE */

static char *marks_path (void)
{
    return g_build_filename (g_get_user_config_dir (), "walpe", "workshop-subscribed", NULL);
}

/* the ids in the file, in order (never NULL) */
static GPtrArray *marks_load (void)
{
    GPtrArray *a = g_ptr_array_new_with_free_func (g_free);
    char *path = marks_path ();
    char *data = NULL;
    gsize len = 0;
    if (g_file_get_contents (path, &data, &len, NULL) && len < 4 * 1024 * 1024) {
        char **lines = g_strsplit (data, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            g_strstrip (lines[i]);
            if (walpe_steam_valid_id (lines[i]))
                g_ptr_array_add (a, g_strdup (lines[i]));
        }
        g_strfreev (lines);
    }
    g_free (data);
    g_free (path);
    return a;
}

gboolean walpe_steam_sub_is_marked (const char *id)
{
    if (!walpe_steam_valid_id (id))
        return FALSE;
    GPtrArray *a = marks_load ();
    gboolean found = FALSE;
    for (guint i = 0; i < a->len && !found; i++)
        found = !strcmp (a->pdata[i], id);
    g_ptr_array_unref (a);
    return found;
}

void walpe_steam_sub_mark (const char *id, gboolean subscribed)
{
    if (!walpe_steam_valid_id (id))
        return;
    GPtrArray *a = marks_load ();
    gboolean had = FALSE;
    for (guint i = 0; i < a->len; i++)
        if (!strcmp (a->pdata[i], id)) {
            had = TRUE;
            if (!subscribed)
                g_ptr_array_remove_index (a, i--);
        }
    if (subscribed && !had)
        g_ptr_array_add (a, g_strdup (id));
    if (had != subscribed) {
        GString *s = g_string_new (NULL);
        for (guint i = 0; i < a->len; i++)
            g_string_append_printf (s, "%s\n", (char *) a->pdata[i]);
        char *path = marks_path ();
        char *dir = g_path_get_dirname (path);
        g_mkdir_with_parents (dir, 0700);
        g_file_set_contents (path, s->str, (gssize) s->len, NULL);   /* tmp + rename */
        g_free (dir);
        g_free (path);
        g_string_free (s, TRUE);
    }
    g_ptr_array_unref (a);
}
