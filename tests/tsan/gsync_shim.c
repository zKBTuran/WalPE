/* gsync_shim.c - GLib's locks and queues made visible to ThreadSanitizer.
 *
 * Linked into every program of a -Db_sanitize=thread build (meson.build)
 * and into nothing else.
 *
 * Why it is needed. ThreadSanitizer decides whether two accesses race from
 * the synchronisation it sees between them, and it sees what goes through
 * libc: pthread mutexes and condition variables, thread creation and join,
 * reads and writes of pipes and eventfds, and the atomics of instrumented
 * code. GLib on Linux implements GMutex and GCond directly on the futex
 * system call, inside libglib, which a distribution does not build with the
 * sanitizer: g_mutex_lock () is invisible to it. Every variable WalPE
 * guards with a GMutex - nearly all of its shared state - would be reported
 * as a race, thousands of true-looking false reports that no suppression
 * file can tell from real ones. The same goes for what GLib hands from one
 * thread to another through its own queues (GThreadPool, GAsyncQueue) and
 * for g_once_init_enter (), whose store is made inside libglib.
 *
 * What it does. It defines the GLib functions below in the program itself;
 * the dynamic linker then binds every call to them from the program, its
 * modules and the other libraries (GIO, GTK, WebKit: they call GLib
 * through the PLT) to these definitions:
 *
 *   g_mutex_* / g_cond_*    the same semantics on pthread mutexes and
 *                           condition variables (GLib's own portable
 *                           implementation does exactly this), which the
 *                           sanitizer follows;
 *   g_once_init_enter/leave on a pthread mutex and an atomic store made
 *                           here, in instrumented code;
 *   g_thread_pool_new/push, g_async_queue_push/pop
 *                           the real functions (dlsym RTLD_NEXT), with the
 *                           hand-over of the pushed pointer announced to
 *                           the sanitizer (__tsan_release where it is
 *                           pushed, __tsan_acquire where the worker takes
 *                           it). GTask's worker threads are a GThreadPool
 *                           of GIO, so g_task_run_in_thread is covered.
 *
 * What it leaves alone. libglib's own internal calls are bound inside the
 * library and keep the futex implementation; they guard GLib's own data,
 * which the sanitizer does not look at. No GLib function takes a caller's
 * GMutex except g_cond_wait (), which is replaced together with it, so no
 * mutex is ever used through both implementations. Sources attached to a
 * main context from another thread (g_idle_add, g_main_context_invoke) are
 * followed without help: the attach writes to the context's eventfd and
 * the loop reads it before it dispatches, and the sanitizer treats that
 * write and read as a hand-over. GRecMutex and GRWLock are pthread objects
 * in GLib itself.
 *
 * The lock semantics a test run sees are GLib's; only the primitive under
 * them differs. A release build never contains this file. */
#include <glib.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

#define SHIM __attribute__ ((visibility ("default")))

void __tsan_acquire (void *addr);
void __tsan_release (void *addr);

static void shim_fail (const char *what, int rc)
{
    g_error ("gsync_shim: %s failed (%d)", what, rc);
}

/* ---------------------------------------------------------------- GMutex
 * mutex->p holds the pthread mutex. A GMutex needs no initialisation when
 * it is static (all zero): the first user allocates, as in GLib's portable
 * implementation. */

static pthread_mutex_t *mutex_new (void)
{
    pthread_mutex_t *m = malloc (sizeof *m);
    int rc = m ? pthread_mutex_init (m, NULL) : ENOMEM;
    if (rc)
        shim_fail ("pthread_mutex_init", rc);
    return m;
}

static pthread_mutex_t *mutex_get (GMutex *mutex)
{
    pthread_mutex_t *m = g_atomic_pointer_get (&mutex->p);
    if (G_UNLIKELY (!m)) {
        m = mutex_new ();
        if (!g_atomic_pointer_compare_and_exchange (&mutex->p, NULL, m)) {
            pthread_mutex_destroy (m);
            free (m);
            m = g_atomic_pointer_get (&mutex->p);
        }
    }
    return m;
}

SHIM void g_mutex_init (GMutex *mutex)
{
    g_atomic_pointer_set (&mutex->p, mutex_new ());
}

SHIM void g_mutex_clear (GMutex *mutex)
{
    pthread_mutex_t *m = g_atomic_pointer_get (&mutex->p);
    if (m) {
        pthread_mutex_destroy (m);
        free (m);
    }
    g_atomic_pointer_set (&mutex->p, NULL);
}

SHIM void g_mutex_lock (GMutex *mutex)
{
    int rc = pthread_mutex_lock (mutex_get (mutex));
    if (G_UNLIKELY (rc))
        shim_fail ("pthread_mutex_lock", rc);
}

SHIM void g_mutex_unlock (GMutex *mutex)
{
    int rc = pthread_mutex_unlock (mutex_get (mutex));
    if (G_UNLIKELY (rc))
        shim_fail ("pthread_mutex_unlock", rc);
}

SHIM gboolean g_mutex_trylock (GMutex *mutex)
{
    int rc = pthread_mutex_trylock (mutex_get (mutex));
    if (G_UNLIKELY (rc && rc != EBUSY))
        shim_fail ("pthread_mutex_trylock", rc);
    return rc == 0;
}

/* ----------------------------------------------------------------- GCond
 * cond->p holds the pthread condition variable, on the monotonic clock:
 * g_cond_wait_until () takes g_get_monotonic_time () values. */

static pthread_cond_t *cond_new (void)
{
    pthread_condattr_t attr;
    pthread_cond_t *c = malloc (sizeof *c);
    int rc = c ? pthread_condattr_init (&attr) : ENOMEM;
    if (!rc)
        rc = pthread_condattr_setclock (&attr, CLOCK_MONOTONIC);
    if (!rc)
        rc = pthread_cond_init (c, &attr);
    if (rc)
        shim_fail ("pthread_cond_init", rc);
    pthread_condattr_destroy (&attr);
    return c;
}

static pthread_cond_t *cond_get (GCond *cond)
{
    pthread_cond_t *c = g_atomic_pointer_get (&cond->p);
    if (G_UNLIKELY (!c)) {
        c = cond_new ();
        if (!g_atomic_pointer_compare_and_exchange (&cond->p, NULL, c)) {
            pthread_cond_destroy (c);
            free (c);
            c = g_atomic_pointer_get (&cond->p);
        }
    }
    return c;
}

SHIM void g_cond_init (GCond *cond)
{
    g_atomic_pointer_set (&cond->p, cond_new ());
}

SHIM void g_cond_clear (GCond *cond)
{
    pthread_cond_t *c = g_atomic_pointer_get (&cond->p);
    if (c) {
        pthread_cond_destroy (c);
        free (c);
    }
    g_atomic_pointer_set (&cond->p, NULL);
}

SHIM void g_cond_wait (GCond *cond, GMutex *mutex)
{
    int rc = pthread_cond_wait (cond_get (cond), mutex_get (mutex));
    if (G_UNLIKELY (rc))
        shim_fail ("pthread_cond_wait", rc);
}

SHIM void g_cond_signal (GCond *cond)
{
    int rc = pthread_cond_signal (cond_get (cond));
    if (G_UNLIKELY (rc))
        shim_fail ("pthread_cond_signal", rc);
}

SHIM void g_cond_broadcast (GCond *cond)
{
    int rc = pthread_cond_broadcast (cond_get (cond));
    if (G_UNLIKELY (rc))
        shim_fail ("pthread_cond_broadcast", rc);
}

SHIM gboolean g_cond_wait_until (GCond *cond, GMutex *mutex, gint64 end_time)
{
    struct timespec ts;
    ts.tv_sec = (time_t) (end_time / 1000000);
    ts.tv_nsec = (long) (end_time % 1000000) * 1000;
    int rc = pthread_cond_timedwait (cond_get (cond), mutex_get (mutex), &ts);
    if (rc == 0)
        return TRUE;
    if (G_UNLIKELY (rc != ETIMEDOUT))
        shim_fail ("pthread_cond_timedwait", rc);
    return FALSE;
}

/* ------------------------------------------------------ g_once_init_enter
 * One thread initialises a location, the others wait until its value is
 * stored. The callers' fast path is the header's inline atomic load; the
 * store that load has to see is made here. */

#define ONCE_PENDING_MAX 64
static pthread_mutex_t once_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t once_cond = PTHREAD_COND_INITIALIZER;
static void *once_pending[ONCE_PENDING_MAX];   /* locations being initialised */

static int once_find (void *location)
{
    for (int i = 0; i < ONCE_PENDING_MAX; i++)
        if (once_pending[i] == location)
            return i;
    return -1;
}

static gboolean once_enter (void *location)
{
    gboolean mine = FALSE;
    pthread_mutex_lock (&once_lock);
    if (g_atomic_pointer_get ((gpointer *) location) == NULL) {
        if (once_find (location) < 0) {
            int slot = once_find (NULL);
            if (slot < 0)
                shim_fail ("g_once_init_enter (too many at once)", ONCE_PENDING_MAX);
            once_pending[slot] = location;
            mine = TRUE;
        } else {
            do
                pthread_cond_wait (&once_cond, &once_lock);
            while (once_find (location) >= 0);
        }
    }
    pthread_mutex_unlock (&once_lock);
    return mine;
}

static void once_leave (void *location, gpointer result)
{
    g_atomic_pointer_set ((gpointer *) location, result);
    pthread_mutex_lock (&once_lock);
    int slot = once_find (location);
    if (slot >= 0)
        once_pending[slot] = NULL;
    pthread_cond_broadcast (&once_cond);
    pthread_mutex_unlock (&once_lock);
}

SHIM gboolean (g_once_init_enter) (volatile void *location)
{
    return once_enter ((void *) location);
}

SHIM void (g_once_init_leave) (volatile void *location, gsize result)
{
    once_leave ((void *) location, (gpointer) result);
}

#if GLIB_CHECK_VERSION (2, 80, 0)
SHIM gboolean (g_once_init_enter_pointer) (void *location)
{
    return once_enter (location);
}

SHIM void (g_once_init_leave_pointer) (void *location, gpointer result)
{
    once_leave (location, result);
}
#endif

/* ----------------------------------------------- hand-overs through GLib
 * A pointer pushed into a pool or queue was filled in by the pushing
 * thread and is read by the worker that pops it; the queue's own lock is
 * inside libglib. The pointer's address names the hand-over. (Small
 * integers stored in the pointer are not addresses: left alone.) */

static gboolean is_address (gpointer p)
{
    return (guintptr) p >= 65536;
}

static void *real_fn (const char *name)
{
    void *f = dlsym (RTLD_NEXT, name);
    if (!f)
        shim_fail (name, 0);
    return f;
}

typedef struct {
    GFunc func;
    gpointer user;
} PoolTramp;

static void pool_tramp (gpointer data, gpointer user)
{
    PoolTramp *t = user;
    if (is_address (data))
        __tsan_acquire (data);
    t->func (data, t->user);
}

SHIM GThreadPool *g_thread_pool_new (GFunc func, gpointer user_data, gint max_threads,
                                     gboolean exclusive, GError **error)
{
    static GThreadPool *(*real) (GFunc, gpointer, gint, gboolean, GError **);
    static gsize once;
    if (g_once_init_enter (&once)) {
        real = (GThreadPool *(*) (GFunc, gpointer, gint, gboolean, GError **))
               real_fn ("g_thread_pool_new");
        g_once_init_leave (&once, 1);
    }
    PoolTramp *t = malloc (sizeof *t);   /* lives as long as the pool (pools here are never freed) */
    if (!t)
        shim_fail ("malloc", ENOMEM);
    t->func = func;
    t->user = user_data;
    return real (pool_tramp, t, max_threads, exclusive, error);
}

SHIM gboolean g_thread_pool_push (GThreadPool *pool, gpointer data, GError **error)
{
    static gboolean (*real) (GThreadPool *, gpointer, GError **);
    static gsize once;
    if (g_once_init_enter (&once)) {
        real = (gboolean (*) (GThreadPool *, gpointer, GError **)) real_fn ("g_thread_pool_push");
        g_once_init_leave (&once, 1);
    }
    if (is_address (data))
        __tsan_release (data);
    return real (pool, data, error);
}

SHIM void g_async_queue_push (GAsyncQueue *queue, gpointer data)
{
    static void (*real) (GAsyncQueue *, gpointer);
    static gsize once;
    if (g_once_init_enter (&once)) {
        real = (void (*) (GAsyncQueue *, gpointer)) real_fn ("g_async_queue_push");
        g_once_init_leave (&once, 1);
    }
    if (is_address (data))
        __tsan_release (data);
    real (queue, data);
}

SHIM gpointer g_async_queue_pop (GAsyncQueue *queue)
{
    static gpointer (*real) (GAsyncQueue *);
    static gsize once;
    if (g_once_init_enter (&once)) {
        real = (gpointer (*) (GAsyncQueue *)) real_fn ("g_async_queue_pop");
        g_once_init_leave (&once, 1);
    }
    gpointer data = real (queue);
    if (is_address (data))
        __tsan_acquire (data);
    return data;
}
