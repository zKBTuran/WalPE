/* race_media_test - the life of the media thread under several users
 * (src/common/media.c).
 *
 * Scenes and web pages that show "now playing" hold a reference each
 * (walpe_media_ref / _unref, from render threads and the main thread) and
 * copy the published state whenever they draw (walpe_media_get). The
 * "walpe-media" thread starts with the first reference, unsubscribes
 * GRACE_MS (3 s) after the last one and is retired with its main context;
 * the next reference starts a successor.
 *
 *  1. several threads take and drop references and copy the state all the
 *     time: the copy is always whole (strings never NULL);
 *  2. the last reference is dropped for longer than the grace time, so the
 *     thread retires, and then taken again by several threads at once: one
 *     successor starts and everybody gets its state;
 *  3. references taken and dropped around the moment the grace time ends.
 *
 * WALPE_NO_MEDIA=1: no session bus is contacted; the thread, its context,
 * its timers and the published state behave as with one. ThreadSanitizer
 * (meson test --setup=tsan in a -Db_sanitize=thread build) must stay
 * silent. Takes about 12 seconds (it waits for the grace time three
 * times). No display. */
#include <glib.h>
#include <pthread.h>
#include <stdatomic.h>
#include "common/media.h"

#define USERS 4
#define GRACE_US (3 * G_USEC_PER_SEC)

static atomic_int fails, stop;
#define CHECK(c) do { if (!(c)) { atomic_fetch_add (&fails, 1); \
    g_printerr ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void look (void)
{
    WalpeNowPlaying np;
    walpe_media_get (&np);
    CHECK (np.title && np.artist && np.album_title && np.content_type);
    CHECK (np.state == WALPE_MEDIA_STOPPED);   /* nothing can play without a bus */
    walpe_media_now_clear (&np);
}

static void *user (void *arg)
{
    guint seed = (guint) (gsize) arg * 7919u + 1u;
    while (!atomic_load (&stop)) {
        walpe_media_ref ();
        for (int i = 0; i < 20; i++)
            look ();
        seed = seed * 1103515245u + 12345u;
        g_usleep (200 + (seed >> 16) % 3000);
        walpe_media_unref ();
        look ();   /* also without a reference: the last state, or the empty one */
        seed = seed * 1103515245u + 12345u;
        g_usleep ((seed >> 16) % 2000);
    }
    return NULL;
}

static void run_users (gint64 for_us)
{
    pthread_t t[USERS];
    atomic_store (&stop, 0);
    for (gsize i = 0; i < USERS; i++)
        pthread_create (&t[i], NULL, user, (void *) i);
    g_usleep ((gulong) for_us);
    atomic_store (&stop, 1);
    for (int i = 0; i < USERS; i++)
        pthread_join (t[i], NULL);
}

static void *edge_user (void *arg)
{
    gint64 at = *(gint64 *) arg;
    gint64 now = g_get_monotonic_time ();
    if (at > now)
        g_usleep ((gulong) (at - now));
    walpe_media_ref ();
    look ();
    walpe_media_unref ();
    return NULL;
}

int main (void)
{
    g_setenv ("WALPE_NO_MEDIA", "1", TRUE);   /* before any thread exists */

    /* 1 */
    run_users (1500 * 1000);
    char *st = walpe_media_status ();
    CHECK (st != NULL);
    g_free (st);

    /* 2: nobody for longer than the grace time, then everybody at once */
    g_usleep (GRACE_US + 700 * 1000);
    look ();
    run_users (800 * 1000);

    /* 3: around the end of the grace time, 4 ms apart */
    gint64 end = g_get_monotonic_time () + GRACE_US;   /* (about: the last unref was just now) */
    pthread_t t[8];
    gint64 at[8];
    for (int i = 0; i < 8; i++) {
        at[i] = end - 16000 + i * 4000;
        pthread_create (&t[i], NULL, edge_user, &at[i]);
    }
    for (int i = 0; i < 8; i++)
        pthread_join (t[i], NULL);
    look ();

    /* leave with the thread retired: nothing of it runs at exit */
    g_usleep (GRACE_US + 700 * 1000);
    int f = atomic_load (&fails);
    if (f) {
        g_printerr ("race_media_test: %d check(s) failed\n", f);
        return 1;
    }
    g_print ("race_media_test: ok\n");
    return 0;
}
