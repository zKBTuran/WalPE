/* race_patterns_test - the shapes of shared state that were made safe for
 * CPUs with weak memory ordering, each hammered by several threads.
 *
 * WalPE runs one render thread per monitor and one decode thread per video,
 * all through the same functions. Six shapes in those functions were only
 * right on x86-64, or not defined by the C memory model at all:
 *
 *   lazy      a switch read on first use and kept in a static
 *             (now: WalpeLazy, common/lazy.h)
 *   limiter   "log this five times" with a shared counter
 *             (now: walpe_first_n)
 *   pointer   a text built on first use and handed on through a static
 *             pointer - glutil's shader headers (now: g_once_init_enter)
 *   table     a table created under a lock but tested for NULL outside it -
 *             glutil's texture jobs, the config's options (now: tested
 *             under the lock)
 *   note      a signal handler's note read by another thread - the
 *             daemon's termination signal (now: a lock-free atomic)
 *   group     several values read from the environment together, guarded
 *             by a flag set afterwards - the particle overrides (now:
 *             g_once_init_enter)
 *
 * This file is built twice. As it is, every shape is the one the tree uses
 * now: the invariants below must hold and ThreadSanitizer must stay silent
 * (meson test --setup=tsan in a -Db_sanitize=thread build). With
 * -DRACE_OLD every shape is the one the tree had: that build is only
 * registered in a ThreadSanitizer build, as a test that is expected to
 * fail, to show that the tool sees each of them on this machine. The old
 * shapes mostly "work" on x86-64, so without the sanitizer the two builds
 * cannot be told apart by their results; that is the point of the tool.
 *
 * No display, no daemon, no files. */
#include <glib.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <string.h>
#include "common/lazy.h"

#define THREADS 4
#define CELLS 512
#define ROUNDS 40

static atomic_int fails;
#define CHECK(c) do { if (!(c)) { atomic_fetch_add (&fails, 1); \
    g_printerr ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* every round all threads start together, so that first uses collide */
static pthread_barrier_t gate;

static void run_threads (void *(*fn) (void *))
{
    pthread_t t[THREADS];
    pthread_barrier_init (&gate, NULL, THREADS);
    for (gsize i = 0; i < THREADS; i++)
        pthread_create (&t[i], NULL, fn, (void *) i);
    for (int i = 0; i < THREADS; i++)
        pthread_join (t[i], NULL);
    pthread_barrier_destroy (&gate);
}

/* ------------------------------------------------------------------ lazy */

static int lazy_value (int i) { return i % 3; }   /* what "the environment" says */

#ifdef RACE_OLD
static int lazy_cell[CELLS];
static int lazy_get (int i)
{
    if (lazy_cell[i] < 0)
        lazy_cell[i] = lazy_value (i);
    return lazy_cell[i];
}
#else
static WalpeLazy lazy_cell[CELLS];
static int lazy_get (int i)
{
    int v = walpe_lazy_get (&lazy_cell[i]);
    if (v < 0)
        v = walpe_lazy_set (&lazy_cell[i], lazy_value (i));
    return v;
}
#endif

static void *lazy_thread (void *arg)
{
    (void) arg;
    pthread_barrier_wait (&gate);
    for (int pass = 0; pass < 3; pass++)
        for (int i = 0; i < CELLS; i++)
            CHECK (lazy_get (i) == lazy_value (i));
    return NULL;
}

static void test_lazy (void)
{
    for (int r = 0; r < ROUNDS; r++) {
        for (int i = 0; i < CELLS; i++)
#ifdef RACE_OLD
            lazy_cell[i] = -1;
#else
            atomic_store (&lazy_cell[i], WALPE_LAZY_UNSET);   /* (no thread runs between rounds) */
#endif
        run_threads (lazy_thread);
    }
}

/* --------------------------------------------------------------- limiter */

#define LIMIT 5
static atomic_int passed[CELLS];

#ifdef RACE_OLD
static int lim_cell[CELLS];
static gboolean lim_first (int i) { return lim_cell[i]++ < LIMIT; }
#else
static atomic_int lim_cell[CELLS];
static gboolean lim_first (int i) { return walpe_first_n (&lim_cell[i], LIMIT); }
#endif

static void *lim_thread (void *arg)
{
    (void) arg;
    pthread_barrier_wait (&gate);
    for (int k = 0; k < 20; k++)
        for (int i = 0; i < CELLS; i++)
            if (lim_first (i))
                atomic_fetch_add (&passed[i], 1);
    return NULL;
}

static void test_limiter (void)
{
    for (int r = 0; r < ROUNDS / 4; r++) {
        for (int i = 0; i < CELLS; i++) {
#ifdef RACE_OLD
            lim_cell[i] = 0;
#else
            atomic_store (&lim_cell[i], 0);
#endif
            atomic_store (&passed[i], 0);
        }
        run_threads (lim_thread);
#ifndef RACE_OLD
        for (int i = 0; i < CELLS; i++) {
            CHECK (atomic_load (&passed[i]) == LIMIT);   /* exactly, whoever asked */
            CHECK (atomic_load (&lim_cell[i]) >= LIMIT &&
                   atomic_load (&lim_cell[i]) < LIMIT + THREADS);   /* and it stopped counting */
        }
#endif
    }
}

/* --------------------------------------------------------------- pointer */

#define TEXT_LEN 48
static void text_fill (char *s, int i)
{
    g_snprintf (s, TEXT_LEN, "#version %03d es / precision highp float;", 300 + i);
}

#ifdef RACE_OLD
static char *text_cell[CELLS];
static const char *text_get (int i)
{
    if (!text_cell[i]) {
        char *s = g_malloc (TEXT_LEN);
        text_fill (s, i);
        text_cell[i] = s;       /* (the losers' copies leak: as the old code did) */
    }
    return text_cell[i];
}
#else
static char *text_cell[CELLS];
static gsize text_once[CELLS];
static const char *text_get (int i)
{
    if (g_once_init_enter (&text_once[i])) {
        char *s = g_malloc (TEXT_LEN);
        text_fill (s, i);
        text_cell[i] = s;
        g_once_init_leave (&text_once[i], 1);
    }
    return text_cell[i];
}
#endif

static void *text_thread (void *arg)
{
    (void) arg;
    char want[TEXT_LEN];
    pthread_barrier_wait (&gate);
    for (int i = 0; i < CELLS; i++) {
        const char *s = text_get (i);
        text_fill (want, i);
        CHECK (s && !strcmp (s, want));   /* never a pointer to a text not written yet */
    }
    return NULL;
}

static void test_pointer (void)
{
    /* (one round: a once cannot be taken back) */
    run_threads (text_thread);
#ifndef RACE_OLD
    for (int i = 0; i < CELLS; i++)
        g_free (text_cell[i]);
#endif
}

/* ----------------------------------------------------------------- table */

static pthread_mutex_t tab_lock = PTHREAD_MUTEX_INITIALIZER;
static GHashTable *tab[CELLS];

static void tab_announce (int i)
{
    pthread_mutex_lock (&tab_lock);
    if (!tab[i])
        tab[i] = g_hash_table_new (g_direct_hash, g_direct_equal);
    g_hash_table_insert (tab[i], GINT_TO_POINTER (i + 1), GINT_TO_POINTER (2 * i + 1));
    pthread_mutex_unlock (&tab_lock);
}

/* 0 = no entry, else the entry's value */
static int tab_take (int i)
{
#ifdef RACE_OLD
    if (!tab[i])
        return 0;
#else
    pthread_mutex_lock (&tab_lock);
    gboolean any = tab[i] != NULL;
    pthread_mutex_unlock (&tab_lock);
    if (!any)
        return 0;
#endif
    pthread_mutex_lock (&tab_lock);
    int v = GPOINTER_TO_INT (g_hash_table_lookup (tab[i], GINT_TO_POINTER (i + 1)));
    pthread_mutex_unlock (&tab_lock);
    return v;
}

static void *tab_thread (void *arg)
{
    gsize me = (gsize) arg;
    pthread_barrier_wait (&gate);
    for (int i = 0; i < CELLS; i++) {
        if (me == 0) {
            tab_announce (i);
        } else {
            int v = tab_take (i);
            CHECK (v == 0 || v == 2 * i + 1);
        }
    }
    return NULL;
}

static void test_table (void)
{
    for (int r = 0; r < ROUNDS; r++) {
        run_threads (tab_thread);
        for (int i = 0; i < CELLS; i++) {
            CHECK (tab_take (i) == 2 * i + 1);
            g_clear_pointer (&tab[i], g_hash_table_unref);
        }
    }
}

/* ------------------------------------------------------------------ note */

#ifdef RACE_OLD
static volatile sig_atomic_t note;
static void note_handler (int sig) { note = sig; }
static int note_read (void) { return note; }
static void note_clear (void) { note = 0; }
#else
static atomic_int note;
static void note_handler (int sig) { atomic_store (&note, sig); }
static int note_read (void) { return atomic_load (&note); }
static void note_clear (void) { atomic_store (&note, 0); }
#endif

static atomic_int note_quit;

/* stands for "any other thread of the daemon": the signal is sent to it */
static void *note_thread (void *arg)
{
    (void) arg;
    while (!atomic_load (&note_quit))
        g_usleep (200);
    return NULL;
}

static void test_note (void)
{
    struct sigaction sa, old;
    memset (&sa, 0, sizeof sa);
    sa.sa_handler = note_handler;
    sigemptyset (&sa.sa_mask);
    sigaction (SIGUSR1, &sa, &old);
    pthread_t t;
    pthread_create (&t, NULL, note_thread, NULL);
    for (int r = 0; r < 300; r++) {
        note_clear ();
        pthread_kill (t, SIGUSR1);          /* handled on the other thread */
        gint64 until = g_get_monotonic_time () + 5 * G_USEC_PER_SEC;
        while (!note_read () && g_get_monotonic_time () < until)
            sched_yield ();
        CHECK (note_read () == SIGUSR1);    /* this thread sees the note */
    }
    atomic_store (&note_quit, 1);
    pthread_join (t, NULL);
    sigaction (SIGUSR1, &old, NULL);
}

/* ----------------------------------------------------------------- group */

static float grp_tol[CELLS];    /* plain, like the particle tolerance */

#ifdef RACE_OLD
static gint grp_read[CELLS];
static void grp_env (int i)
{
    if (g_atomic_int_get (&grp_read[i]))
        return;
    grp_tol[i] = 8.0f + (float) i;
    g_atomic_int_set (&grp_read[i], 1);
}
#else
static gsize grp_once[CELLS];
static void grp_env (int i)
{
    if (!g_once_init_enter (&grp_once[i]))
        return;
    grp_tol[i] = 8.0f + (float) i;
    g_once_init_leave (&grp_once[i], 1);
}
#endif

static void *grp_thread (void *arg)
{
    (void) arg;
    pthread_barrier_wait (&gate);
    for (int i = 0; i < CELLS; i++) {
        grp_env (i);
        CHECK (grp_tol[i] == 8.0f + (float) i);
    }
    return NULL;
}

static void test_group (void)
{
    run_threads (grp_thread);
}

int main (void)
{
    test_lazy ();
    test_limiter ();
    test_pointer ();
    test_table ();
    test_note ();
    test_group ();
    int n = atomic_load (&fails);
    if (n) {
        g_printerr ("race_patterns_test: %d check(s) failed\n", n);
        return 1;
    }
    g_print ("race_patterns_test: ok (%s shapes)\n",
#ifdef RACE_OLD
             "old"
#else
             "current"
#endif
    );
    return 0;
}
