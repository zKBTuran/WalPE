/* race_shim_test - is a ThreadSanitizer run of this tree to be trusted?
 *
 * Correct code that synchronises only through GLib, in each way the tree
 * does: a GMutex, a GCond with a predicate loop and with a timed wait,
 * g_once_init_enter, a GThreadPool, a GAsyncQueue, an idle source attached
 * to another thread's main loop, a GTask run in a worker thread.
 *
 * In a -Db_sanitize=thread build (meson test --setup=tsan) this must run
 * without a single report. It is the first test to look at there: a
 * report from it is not a bug in WalPE but a way of GLib's that the
 * sanitizer cannot follow on this machine (tests/tsan/gsync_shim.c says
 * which it makes visible and how), and until it is silent the reports of
 * the other tests mean nothing.
 *
 * In every other build it is an ordinary test of the same things (and of
 * the monotonic clock under g_cond_wait_until). No display, no files. */
#include <glib.h>
#include <gio/gio.h>
#include <string.h>

static int fails;   /* only the main thread counts (workers report through their results) */
#define CHECK(c) do { if (!(c)) { fails++; g_printerr ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define THREADS 4

/* ---------------------------------------------------------------- mutex */

static GMutex m_lock;           /* static: never initialised, as most in the tree */
static guint64 m_count;
static struct { GMutex lock; guint64 n; } *m_dyn;

static gpointer mutex_thread (gpointer u)
{
    (void) u;
    for (int i = 0; i < 20000; i++) {
        g_mutex_lock (&m_lock);
        m_count++;
        g_mutex_unlock (&m_lock);
        if (g_mutex_trylock (&m_dyn->lock)) {
            m_dyn->n++;
            g_mutex_unlock (&m_dyn->lock);
        }
    }
    return NULL;
}

static void test_mutex (void)
{
    GThread *t[THREADS];
    m_dyn = g_malloc0 (sizeof *m_dyn);
    g_mutex_init (&m_dyn->lock);
    for (int i = 0; i < THREADS; i++)
        t[i] = g_thread_new ("mutex", mutex_thread, NULL);
    for (int i = 0; i < THREADS; i++)
        g_thread_join (t[i]);
    CHECK (m_count == (guint64) THREADS * 20000);
    CHECK (m_dyn->n > 0 && m_dyn->n <= (guint64) THREADS * 20000);
    g_mutex_clear (&m_dyn->lock);
    g_free (m_dyn);
}

/* ----------------------------------------------------------------- cond */

static GMutex c_lock;
static GCond c_cond;
static int c_turn;              /* 0: ping's, 1: pong's */
static int c_rounds;

static gpointer pong_thread (gpointer u)
{
    (void) u;
    g_mutex_lock (&c_lock);
    for (int i = 0; i < 2000; i++) {
        while (c_turn != 1)
            g_cond_wait (&c_cond, &c_lock);
        c_rounds++;
        c_turn = 0;
        g_cond_signal (&c_cond);
    }
    g_mutex_unlock (&c_lock);
    return NULL;
}

static void test_cond (void)
{
    GThread *t = g_thread_new ("pong", pong_thread, NULL);
    g_mutex_lock (&c_lock);
    for (int i = 0; i < 2000; i++) {
        c_turn = 1;
        g_cond_signal (&c_cond);
        while (c_turn != 0)
            g_cond_wait (&c_cond, &c_lock);
    }
    CHECK (c_rounds == 2000);
    /* nobody signals: the timed wait ends at its time on the monotonic clock,
     * not at once and not never */
    gint64 t0 = g_get_monotonic_time ();
    gboolean signalled = TRUE;
    while (g_get_monotonic_time () < t0 + 60000 && signalled)
        signalled = g_cond_wait_until (&c_cond, &c_lock, t0 + 60000);
    gint64 waited = g_get_monotonic_time () - t0;
    CHECK (!signalled);
    CHECK (waited >= 55000 && waited < 5 * G_USEC_PER_SEC);
    g_mutex_unlock (&c_lock);
    g_thread_join (t);
}

/* ----------------------------------------------------------------- once */

typedef struct { int a, b, c; } Thing;
static Thing *o_thing;
static gsize o_once;
static gint o_inits;

static gpointer once_thread (gpointer u)
{
    (void) u;
    if (g_once_init_enter (&o_once)) {
        Thing *t = g_new0 (Thing, 1);
        g_usleep (2000);            /* the others arrive meanwhile and wait */
        t->a = 1; t->b = 2; t->c = 3;
        o_thing = t;
        g_atomic_int_inc (&o_inits);
        g_once_init_leave (&o_once, 1);
    }
    return GINT_TO_POINTER (o_thing->a + o_thing->b + o_thing->c);
}

static void test_once (void)
{
    GThread *t[THREADS];
    for (int i = 0; i < THREADS; i++)
        t[i] = g_thread_new ("once", once_thread, NULL);
    for (int i = 0; i < THREADS; i++)
        CHECK (GPOINTER_TO_INT (g_thread_join (t[i])) == 6);
    CHECK (g_atomic_int_get (&o_inits) == 1);
    g_free (o_thing);
}

/* ------------------------------------------------- pool and async queue */

typedef struct { int in; int out; } Job;
static GAsyncQueue *done_q;

static void pool_work (gpointer data, gpointer user)
{
    Job *j = data;
    j->out = j->in * 2 + GPOINTER_TO_INT (user);   /* reads what the pusher wrote */
    g_async_queue_push (done_q, j);
}

static void test_pool_queue (void)
{
    done_q = g_async_queue_new ();
    GThreadPool *pool = g_thread_pool_new (pool_work, GINT_TO_POINTER (1), 3, FALSE, NULL);
    int sum = 0, want = 0;
    for (int i = 0; i < 400; i++) {
        Job *j = g_new0 (Job, 1);
        j->in = i;
        want += i * 2 + 1;
        g_thread_pool_push (pool, j, NULL);
        if (i % 50 == 49)
            g_usleep (3000);        /* the workers go idle and are woken again */
    }
    for (int i = 0; i < 400; i++) {
        Job *j = g_async_queue_pop (done_q);
        sum += j->out;              /* reads what a worker wrote */
        g_free (j);
    }
    CHECK (sum == want);
    g_thread_pool_free (pool, FALSE, TRUE);
    g_async_queue_unref (done_q);
}

/* ------------------------------------------- idle source from a thread */

typedef struct { int n; char text[32]; } Note;
static GMainLoop *loop;
static int notes_seen, notes_sum;

static gboolean note_cb (gpointer data)
{
    Note *n = data;
    notes_sum += n->n + (int) strlen (n->text);   /* reads what the thread wrote */
    g_free (n);
    if (++notes_seen == 300)
        g_main_loop_quit (loop);
    return G_SOURCE_REMOVE;
}

static gpointer note_thread (gpointer u)
{
    (void) u;
    for (int i = 0; i < 300; i++) {
        Note *n = g_new0 (Note, 1);
        n->n = i;
        g_snprintf (n->text, sizeof n->text, "note %d", i);
        if (i & 1)
            g_idle_add (note_cb, n);
        else
            g_main_context_invoke (NULL, note_cb, n);
        if (i % 20 == 0)
            g_usleep (1000);        /* the loop sleeps in poll in between */
    }
    return NULL;
}

/* (started from inside the loop: g_main_context_invoke runs its callback on
 * the calling thread while nobody owns the context) */
static gboolean note_start (gpointer data)
{
    GThread **t = data;
    *t = g_thread_new ("notes", note_thread, NULL);
    return G_SOURCE_REMOVE;
}

static void test_idle (void)
{
    GThread *t = NULL;
    loop = g_main_loop_new (NULL, FALSE);
    g_idle_add (note_start, &t);
    g_main_loop_run (loop);
    g_thread_join (t);
    int want = 0;
    for (int i = 0; i < 300; i++)
        want += i + (i < 10 ? 6 : i < 100 ? 7 : 8);
    CHECK (notes_seen == 300 && notes_sum == want);
    g_main_loop_unref (loop);
}

/* ---------------------------------------------------------------- GTask */

static int tasks_done, tasks_sum;

static void task_thread (GTask *task, gpointer src, gpointer data, GCancellable *c)
{
    (void) src; (void) c;
    Job *j = data;
    Job *r = g_new0 (Job, 1);
    r->out = j->in + 1000;          /* reads the task data the main thread wrote */
    g_task_return_pointer (task, r, g_free);
}

static void task_done (GObject *src, GAsyncResult *res, gpointer user)
{
    (void) src;
    Job *r = g_task_propagate_pointer (G_TASK (res), NULL);
    tasks_sum += r ? r->out : 0;    /* reads what the worker wrote */
    g_free (r);
    if (++tasks_done == 60)
        g_main_loop_quit (user);
}

static void test_task (void)
{
    GMainLoop *l = g_main_loop_new (NULL, FALSE);
    int want = 0;
    for (int i = 0; i < 60; i++) {
        Job *j = g_new0 (Job, 1);
        j->in = i;
        want += i + 1000;
        GTask *t = g_task_new (NULL, NULL, task_done, l);
        g_task_set_task_data (t, j, g_free);
        g_task_run_in_thread (t, task_thread);
        g_object_unref (t);
    }
    g_main_loop_run (l);
    CHECK (tasks_done == 60 && tasks_sum == want);
    g_main_loop_unref (l);
}

int main (void)
{
    test_mutex ();
    test_cond ();
    test_once ();
    test_pool_queue ();
    test_idle ();
    test_task ();
    if (fails) {
        g_printerr ("race_shim_test: %d check(s) failed\n", fails);
        return 1;
    }
    g_print ("race_shim_test: ok\n");
    return 0;
}
