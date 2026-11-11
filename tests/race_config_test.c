/* race_config_test - the option store of config.c under the threads that
 * use it (src/common/config.c).
 *
 * In the daemon the main thread owns the config: it stores an option when
 * the user changes one (walpe_config_set_prop; the first one ever creates
 * the table) while render threads ask for options of the wallpaper they
 * load (walpe_config_get_prop). The GUI and the tray also load and free
 * whole configs on worker threads.
 *
 *  1. readers ask for an option while the main thread stores the first one
 *     of a new config: every answer is either "not set" or a value that
 *     was stored, whole;
 *  2. readers keep asking while configs are loaded and freed on another
 *     thread: an answer never comes from a config that is being freed;
 *  3. the save counter counts every save made from two threads.
 *
 * ThreadSanitizer (meson test --setup=tsan in a -Db_sanitize=thread build)
 * must stay silent; before the change it reported the table pointer tested
 * outside props_lock and the unsynchronised active_config / save counter.
 * Private XDG_CONFIG_HOME, no daemon, no display. */
#include <glib.h>
#include <glib/gstdio.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <string.h>
#include "common/config.h"

#define READERS 3
#define ROUNDS 150

static atomic_int fails, stop, asked;
#define CHECK(c) do { if (!(c)) { atomic_fetch_add (&fails, 1); \
    g_printerr ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static const char *const WALL = "/nonexistent/walpe-race/wallpaper";
static pthread_barrier_t gate;

/* a stored value is "v" followed by 40 times the same digit */
static gboolean value_ok (const char *v)
{
    if (strlen (v) != 41 || v[0] != 'v' || !g_ascii_isdigit (v[1]))
        return FALSE;
    for (int i = 2; i < 41; i++)
        if (v[i] != v[1])
            return FALSE;
    return TRUE;
}

static void *reader (void *arg)
{
    (void) arg;
    pthread_barrier_wait (&gate);
    while (!atomic_load (&stop)) {
        char *v = walpe_config_get_prop (WALL, "schemecolor");
        CHECK (!v || value_ok (v));
        g_free (v);
        atomic_fetch_add (&asked, 1);
    }
    return NULL;
}

static void start_readers (pthread_t *t)
{
    atomic_store (&stop, 0);
    atomic_store (&asked, 0);
    pthread_barrier_init (&gate, NULL, READERS + 1);
    for (int i = 0; i < READERS; i++)
        pthread_create (&t[i], NULL, reader, NULL);
    pthread_barrier_wait (&gate);
}

static void stop_readers (pthread_t *t)
{
    /* (every reader has asked at least a few times around the writes) */
    int had = atomic_load (&asked);
    gint64 until = g_get_monotonic_time () + 2 * G_USEC_PER_SEC;
    while (atomic_load (&asked) < had + 3 * READERS && g_get_monotonic_time () < until)
        sched_yield ();
    atomic_store (&stop, 1);
    for (int i = 0; i < READERS; i++)
        pthread_join (t[i], NULL);
    pthread_barrier_destroy (&gate);
}

static void set_value (WalpeConfig *c, int digit)
{
    char v[42];
    v[0] = 'v';
    memset (v + 1, '0' + digit % 10, 40);
    v[41] = 0;
    walpe_config_set_prop (c, WALL, "schemecolor", v);
}

/* 1: the first option of a new config, stored under the readers' eyes */
static void test_first_option (void)
{
    pthread_t t[READERS];
    for (int r = 0; r < ROUNDS; r++) {
        WalpeConfig *c = walpe_config_load ();   /* a config without options */
        start_readers (t);
        for (int k = 0; k < 8; k++)
            set_value (c, r + k);
        CHECK (walpe_config_clear_props (c, WALL) == 1);
        set_value (c, r);
        stop_readers (t);
        char *v = walpe_config_get_prop (WALL, "schemecolor");
        CHECK (v && value_ok (v) && v[1] == '0' + r % 10);
        g_free (v);
        walpe_config_free (c);
        CHECK (walpe_config_get_prop (WALL, "schemecolor") == NULL);
    }
}

/* 2: configs come and go on this thread while the readers ask */
static void test_load_free (void)
{
    pthread_t t[READERS];
    start_readers (t);
    for (int r = 0; r < ROUNDS; r++) {
        WalpeConfig *c = walpe_config_load ();
        set_value (c, r);
        walpe_config_free (c);
    }
    stop_readers (t);
}

/* 3: saves from two threads, each with a config of its own (the GUI drops a
 * deleted wallpaper's options on a worker thread while its main thread may
 * save as well) */
static void *saver (void *arg)
{
    (void) arg;
    WalpeConfig *c = walpe_config_load ();
    for (int i = 0; i < 20; i++)
        walpe_config_save (c);
    walpe_config_free (c);
    return NULL;
}

static void test_save_count (void)
{
    guint before = walpe_config_save_count ();
    pthread_t a, b;
    pthread_create (&a, NULL, saver, NULL);
    pthread_create (&b, NULL, saver, NULL);
    pthread_join (a, NULL);
    pthread_join (b, NULL);
    CHECK (walpe_config_save_count () - before == 40);
}

int main (void)
{
    char *dir = g_dir_make_tmp ("walpe-race-config-XXXXXX", NULL);
    g_assert (dir);
    g_setenv ("XDG_CONFIG_HOME", dir, TRUE);   /* before any thread exists */
    test_first_option ();
    test_load_free ();
    test_save_count ();
    char *wd = g_build_filename (dir, "walpe", NULL);
    GDir *d = g_dir_open (wd, 0, NULL);
    const char *n;
    while (d && (n = g_dir_read_name (d))) {
        char *p = g_build_filename (wd, n, NULL);
        g_unlink (p);
        g_free (p);
    }
    if (d)
        g_dir_close (d);
    g_rmdir (wd);
    g_rmdir (dir);
    g_free (wd);
    g_free (dir);
    int f = atomic_load (&fails);
    if (f) {
        g_printerr ("race_config_test: %d check(s) failed\n", f);
        return 1;
    }
    g_print ("race_config_test: ok\n");
    return 0;
}
