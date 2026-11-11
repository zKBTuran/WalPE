/* swrender_test.c - "Render on CPU" (src/common/swrender.c) without a
 * display or a GPU: finding Mesa's EGL vendor file among others (fake
 * vendor directories), the llvmpipe thread count, WALPE_SOFTWARE, the
 * software environment, the probe's report, the two config keys, and the
 * control socket that stays open across the daemon's restart in place.
 *
 * Works in a private temp dir that is also the XDG config dir.
 * Run: meson test -C build swrender -v */
#include "common/swrender.h"
#include "common/config.h"
#include "common/ipc.h"
#include <glib/gstdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; g_printerr ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static char *T;

static char *P (const char *rel)
{
    return g_build_filename (T, rel, NULL);
}

static void put (const char *rel, const char *txt)
{
    char *p = P (rel), *d = g_path_get_dirname (p);
    g_mkdir_with_parents (d, 0700);
    g_assert (g_file_set_contents (p, txt, -1, NULL));
    g_free (d);
    g_free (p);
}

#define ICD(lib) "{ \"file_format_version\" : \"1.0.0\", \"ICD\" : { \"library_path\" : \"" lib "\" } }"

static void test_vendor (void)
{
    /* an NVIDIA machine: the driver's file sorts first, Mesa's is there */
    put ("nv/10_nvidia.json", ICD ("libEGL_nvidia.so.0"));
    put ("nv/50_mesa.json", ICD ("libEGL_mesa.so.0"));
    char *d = P ("nv"), *searched = NULL;
    char *f = walpe_sw_find_mesa_vendor (d, &searched);
    CHECK (f && g_str_has_suffix (f, "/nv/50_mesa.json"));
    CHECK (!g_strcmp0 (searched, d));
    g_free (f);
    g_free (searched);
    g_free (d);

    /* no Mesa: nothing, and the place that was searched */
    put ("only/10_nvidia.json", ICD ("libEGL_nvidia.so.0"));
    d = P ("only");
    searched = NULL;
    f = walpe_sw_find_mesa_vendor (d, &searched);
    CHECK (f == NULL && !g_strcmp0 (searched, d));
    g_free (searched);
    g_free (d);

    /* found by what the file says, not by its name; a full library path;
     * broken and foreign files are skipped; the second directory counts */
    put ("a/00_broken.json", "{ this is not json");
    put ("a/20_other.json", "{ \"ICD\" : { } }");
    put ("a/notes.txt", ICD ("libEGL_mesa.so.0"));
    put ("b/99_gl.json", ICD ("/usr/lib64/libEGL_mesa.so.0"));
    char *a = P ("a"), *b = P ("b"), *none = P ("missing");
    char *dirs = g_strdup_printf ("%s:%s:%s", none, a, b);
    f = walpe_sw_find_mesa_vendor (dirs, NULL);
    CHECK (f && g_str_has_suffix (f, "/b/99_gl.json"));
    g_free (f);

    /* dirs NULL: where libglvnd looks - __EGL_VENDOR_LIBRARY_DIRS first */
    g_setenv ("__EGL_VENDOR_LIBRARY_DIRS", dirs, TRUE);
    f = walpe_sw_find_mesa_vendor (NULL, &searched);
    CHECK (f && g_str_has_suffix (f, "/b/99_gl.json") && !g_strcmp0 (searched, dirs));
    g_free (f);
    g_free (searched);
    g_setenv ("__EGL_VENDOR_LIBRARY_DIRS", a, TRUE);
    f = walpe_sw_find_mesa_vendor (NULL, NULL);
    CHECK (f == NULL);
    g_unsetenv ("__EGL_VENDOR_LIBRARY_DIRS");
    g_free (dirs);
    g_free (a);
    g_free (b);
    g_free (none);
}

static void test_threads_env (void)
{
    CHECK (walpe_sw_threads (0, 1) == 1);
    CHECK (walpe_sw_threads (0, 2) == 1);
    CHECK (walpe_sw_threads (0, 4) == 2);
    CHECK (walpe_sw_threads (0, 12) == 6);
    CHECK (walpe_sw_threads (0, 16) == 8);
    CHECK (walpe_sw_threads (0, 64) == 8);     /* never every core */
    CHECK (walpe_sw_threads (0, 0) == 1);
    CHECK (walpe_sw_threads (3, 16) == 3);     /* the user's number */
    CHECK (walpe_sw_threads (100, 16) == 32);

    CHECK (walpe_sw_env_override (NULL) == -1 && walpe_sw_env_override ("") == -1);
    CHECK (walpe_sw_env_override ("1") == 1 && walpe_sw_env_override ("on") == 1 &&
           walpe_sw_env_override ("TRUE") == 1 && walpe_sw_env_override ("yes") == 1);
    CHECK (walpe_sw_env_override ("0") == 0 && walpe_sw_env_override ("off") == 0 &&
           walpe_sw_env_override ("False") == 0 && walpe_sw_env_override ("no") == 0);
    CHECK (walpe_sw_env_override ("maybe") == -1);

    char **pairs = walpe_sw_env_pairs ("/v/50_mesa.json", 6, FALSE);
    char **base = g_new0 (char *, 4);
    base[0] = g_strdup ("HOME=/h");
    base[1] = g_strdup ("__EGL_VENDOR_LIBRARY_FILENAMES=/v/10_nvidia.json");
    base[2] = g_strdup ("LP_NUM_THREADS=16");
    char **env = walpe_sw_environ (base, pairs);
    CHECK (!g_strcmp0 (g_environ_getenv (env, "__EGL_VENDOR_LIBRARY_FILENAMES"), "/v/50_mesa.json"));
    CHECK (!g_strcmp0 (g_environ_getenv (env, "LIBGL_ALWAYS_SOFTWARE"), "1"));
    CHECK (!g_strcmp0 (g_environ_getenv (env, "GALLIUM_DRIVER"), "llvmpipe"));
    CHECK (!g_strcmp0 (g_environ_getenv (env, "__GLX_VENDOR_LIBRARY_NAME"), "mesa"));
    CHECK (!g_strcmp0 (g_environ_getenv (env, "LP_NUM_THREADS"), "6"));
    CHECK (!g_strcmp0 (g_environ_getenv (env, "HOME"), "/h"));
    g_strfreev (env);
    g_strfreev (pairs);
    /* the user's own LP_NUM_THREADS is kept */
    pairs = walpe_sw_env_pairs ("/v/50_mesa.json", 6, TRUE);
    for (int i = 0; pairs[i]; i += 2)
        CHECK (strcmp (pairs[i], "LP_NUM_THREADS") != 0);
    g_strfreev (pairs);
}

static void test_probe (void)
{
    char *v = NULL, *r = NULL, *ver = NULL, *why = NULL;
    CHECK (walpe_sw_parse_probe ("egl=Mesa Project\nvendor=Mesa\n"
                                 "renderer=llvmpipe (LLVM 20.1.2, 256 bits)\n"
                                 "version=4.5 (Core Profile) Mesa 25.2.8\ndrew=1\n",
                                 &v, &r, &ver, &why));
    CHECK (!g_strcmp0 (v, "Mesa") && r && g_str_has_prefix (r, "llvmpipe") && ver && !why);
    g_free (v); g_free (r); g_free (ver);

    /* the GPU driver answered after all */
    CHECK (!walpe_sw_parse_probe ("vendor=NVIDIA Corporation\nrenderer=NVIDIA GeForce RTX 4050\n"
                                  "version=4.6.0 NVIDIA\ndrew=1\n", NULL, &r, NULL, &why));
    CHECK (why && strstr (why, "not llvmpipe") && !strstr (why, "too slow"));
    g_free (r); g_free (why);
    /* Mesa without LLVM */
    CHECK (!walpe_sw_parse_probe ("vendor=Mesa\nrenderer=softpipe\nversion=3.3\ndrew=1\n",
                                  NULL, NULL, NULL, &why));
    CHECK (why && strstr (why, "softpipe") && strstr (why, "too slow"));
    g_free (why);
    /* llvmpipe, but the frame did not reach the window (X error: no drew line) */
    CHECK (!walpe_sw_parse_probe ("vendor=Mesa\nrenderer=llvmpipe (LLVM 19)\nversion=4.5\n",
                                  NULL, NULL, NULL, &why));
    CHECK (why && strstr (why, "could not draw"));
    g_free (why);
    CHECK (!walpe_sw_parse_probe ("vendor=Mesa\nrenderer=llvmpipe (LLVM 19)\ndrew=0\n",
                                  NULL, NULL, NULL, &why));
    g_free (why);
    CHECK (!walpe_sw_parse_probe ("fail=Mesa's EGL does not start on this X server\n",
                                  NULL, NULL, NULL, &why));
    CHECK (!g_strcmp0 (why, "Mesa's EGL does not start on this X server"));
    g_free (why);
    CHECK (!walpe_sw_parse_probe ("", NULL, NULL, NULL, &why) && why);
    g_free (why);
    CHECK (!walpe_sw_parse_probe (NULL, NULL, NULL, NULL, NULL));

    CHECK (walpe_sw_renderer_is_llvmpipe ("llvmpipe (LLVM 20.1.2, 256 bits)"));
    CHECK (walpe_sw_renderer_is_software ("softpipe") && !walpe_sw_renderer_is_llvmpipe ("softpipe"));
    CHECK (!walpe_sw_renderer_is_software ("NVIDIA GeForce RTX 4050 Laptop GPU/PCIe/SSE2"));
    CHECK (!walpe_sw_renderer_is_software (NULL));
}

static void test_config (void)
{
    WalpeConfig *c = walpe_config_load ();
    CHECK (!c->software_render && c->software_threads == 0);   /* off unless chosen */
    c->software_render = TRUE;
    c->software_threads = 3;
    walpe_config_save (c);
    walpe_config_free (c);
    c = walpe_config_load ();
    CHECK (c->software_render && c->software_threads == 3);
    c->software_render = FALSE;
    walpe_config_save (c);
    walpe_config_free (c);
    c = walpe_config_load ();
    CHECK (!c->software_render && c->software_threads == 3);
    walpe_config_free (c);
    /* a file of an older version has neither key; nonsense is clamped */
    put ("cfg/walpe/config.json", "{ \"config_version\" : 2, \"fps\" : 60 }");
    c = walpe_config_load ();
    CHECK (!c->software_render && c->software_threads == 0 && c->fps == 60);
    walpe_config_free (c);
    put ("cfg/walpe/config.json", "{ \"config_version\" : 2, \"software_render\" : true, "
                                  "\"software_threads\" : 4000 }");
    c = walpe_config_load ();
    CHECK (c->software_render && c->software_threads == 32);
    walpe_config_free (c);
}

/* ---- the control socket across a restart in place ---- */

static char *on_request (const char *req, gpointer user)
{
    (void) user;
    return g_strdup_printf ("{\"ok\":true,\"echo\":%s}", *req ? req : "null");
}

typedef struct { const char *path; char *reply; gint done; } Ask;

static gpointer ask_thread (gpointer p)
{
    Ask *a = p;
    a->reply = walpe_ipc_request (a->path, "{\"cmd\":\"status\"}", 3000);
    g_atomic_int_set (&a->done, 1);
    g_main_context_wakeup (NULL);
    return NULL;
}

static int listen_on (const char *path)
{
    int fd = socket (AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    g_strlcpy (a.sun_path, path, sizeof a.sun_path);
    g_unlink (path);
    g_assert (fd >= 0 && bind (fd, (struct sockaddr *) &a, sizeof a) == 0 && listen (fd, 8) == 0);
    return fd;
}

static void test_socket_handoff (void)
{
    char *sock = P ("walpe.sock");
    CHECK (walpe_ipc_listen_fd () == -1);
    /* the previous process image: a bound, listening socket and a client
     * that connected while nobody was accepting (it waits in the backlog) */
    int fd = listen_on (sock);
    Ask a = { .path = sock };
    GThread *th = g_thread_new ("ask", ask_thread, &a);
    g_usleep (150 * 1000);
    /* the new image adopts the socket: no "another daemon is running", no
     * unlink + bind, and the waiting client gets its answer */
    walpe_ipc_adopt_fd (fd);
    CHECK (walpe_ipc_serve (sock, on_request, NULL));
    CHECK (walpe_ipc_listen_fd () == fd);
    gint64 until = g_get_monotonic_time () + 4 * G_USEC_PER_SEC;
    while (!g_atomic_int_get (&a.done) && g_get_monotonic_time () < until)
        g_main_context_iteration (NULL, TRUE);
    g_thread_join (th);
    CHECK (a.reply && strstr (a.reply, "\"ok\":true") && strstr (a.reply, "\"cmd\":\"status\""));
    g_free (a.reply);
    g_free (sock);

    /* a descriptor that is not the listener of that path (the number comes
     * from the environment) is ignored and left open: a new socket is bound */
    char *other = P ("other.sock"), *sock2 = P ("walpe2.sock");
    int fd2 = listen_on (other);
    walpe_ipc_adopt_fd (fd2);
    CHECK (walpe_ipc_serve (sock2, on_request, NULL));
    CHECK (walpe_ipc_listen_fd () >= 0 && walpe_ipc_listen_fd () != fd2);
    CHECK (g_file_test (sock2, G_FILE_TEST_EXISTS));
    int acc = 0;
    socklen_t al = sizeof acc;
    CHECK (getsockopt (fd2, SOL_SOCKET, SO_ACCEPTCONN, &acc, &al) == 0 && acc);   /* still open */
    close (fd2);
    g_free (other);
    g_free (sock2);
}

int main (void)
{
    T = g_dir_make_tmp ("walpe-swrender-XXXXXX", NULL);
    g_assert (T);
    char *cfg = P ("cfg");
    g_setenv ("XDG_CONFIG_HOME", cfg, TRUE);
    g_free (cfg);

    test_vendor ();
    test_threads_env ();
    test_probe ();
    test_config ();
    test_socket_handoff ();

    char *cmd = g_strdup_printf ("rm -rf '%s'", T);
    if (system (cmd) != 0)
        g_printerr ("cleanup failed\n");
    g_free (cmd);
    if (fails)
        g_printerr ("swrender-test: %d failure(s)\n", fails);
    else
        g_print ("swrender-test: all checks passed\n");
    return fails ? 1 : 0;
}
