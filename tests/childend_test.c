/* childend_test - the daemon's wait for its child processes before a restart
 * in place (src/daemon/childend.h) ends by the clock.
 *
 * sw_end_children () sends SIGTERM, waits, sends SIGKILL - and then waited
 * with waitpid () and no limit. A child that does not come back from a
 * call into the kernel takes no signal, SIGKILL included, and the daemon
 * stood in its restart for as long. Children of this test stand in:
 *  1. one that ends on SIGTERM: gone and reaped within the first wait;
 *  2. one that ignores SIGTERM: killed after the first wait, reaped;
 *  3. one that no signal reaches (the signal function of the call is
 *     replaced by one that sends nothing - no program can make itself
 *     immune to SIGKILL): the call returns after both waits, says that
 *     one child is left and which, and has not blocked;
 *  4. a process id that is no child of this process, next to a real one;
 *  5. no children at all: returns at once.
 * No daemon, no display. */
#include <glib.h>
#include <stdio.h>
#include <unistd.h>
#include "daemon/childend.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; g_printerr ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* a child that sleeps; ignore_term: SIGTERM does not end it. The pipe says
 * when it is ready (its signal disposition is in place). */
static pid_t child (gboolean ignore_term)
{
    int fd[2];
    g_assert (pipe (fd) == 0);
    pid_t p = fork ();
    g_assert (p >= 0);
    if (p == 0) {
        close (fd[0]);
        signal (SIGTERM, ignore_term ? SIG_IGN : SIG_DFL);
        (void) !write (fd[1], "r", 1);
        close (fd[1]);
        for (;;)
            pause ();
    }
    close (fd[1]);
    char c;
    g_assert (read (fd[0], &c, 1) == 1);
    close (fd[0]);
    return p;
}

static gboolean is_gone (pid_t p)   /* ended and reaped: no such child any more */
{
    return waitpid (p, NULL, WNOHANG) < 0 && errno == ECHILD;
}

static int sent_term, sent_kill;
static int no_signal (pid_t pid, int sig)
{
    (void) pid;
    sent_term += sig == SIGTERM;
    sent_kill += sig == SIGKILL;
    return 0;
}

static double ms_since (gint64 t0)
{
    return (double) (g_get_monotonic_time () - t0) / 1000.0;
}

int main (void)
{
    /* 1. ends on SIGTERM */
    {
        GArray *kids = g_array_new (FALSE, FALSE, sizeof (pid_t));
        pid_t a = child (FALSE), b = child (FALSE);
        g_array_append_val (kids, a);
        g_array_append_val (kids, b);
        gint64 t0 = g_get_monotonic_time ();
        CHECK (walpe_children_end (kids, 2000, 2000, NULL) == 0);
        CHECK (ms_since (t0) < 1500);       /* not the whole first wait */
        CHECK (kids->len == 0 && is_gone (a) && is_gone (b));
        g_array_free (kids, TRUE);
    }
    /* 2. ignores SIGTERM: the first wait passes, SIGKILL ends it */
    {
        GArray *kids = g_array_new (FALSE, FALSE, sizeof (pid_t));
        pid_t a = child (TRUE), b = child (FALSE);
        g_array_append_val (kids, a);
        g_array_append_val (kids, b);
        gint64 t0 = g_get_monotonic_time ();
        CHECK (walpe_children_end (kids, 300, 5000, NULL) == 0);
        double ms = ms_since (t0);
        CHECK (ms >= 290 && ms < 4000);
        CHECK (is_gone (a) && is_gone (b));
        g_array_free (kids, TRUE);
    }
    /* 3. a child no signal reaches: both waits pass and the call returns */
    {
        GArray *kids = g_array_new (FALSE, FALSE, sizeof (pid_t));
        pid_t a = child (FALSE);
        g_array_append_val (kids, a);
        gint64 t0 = g_get_monotonic_time ();
        guint left = walpe_children_end (kids, 200, 400, no_signal);
        double ms = ms_since (t0);
        CHECK (left == 1 && kids->len == 1 && g_array_index (kids, pid_t, 0) == a);
        CHECK (sent_term == 1 && sent_kill == 1);
        if (!(ms >= 590 && ms < 3000)) {    /* 200 + 400, and it is over */
            fails++;
            g_printerr ("FAIL: the wait for a child that does not end took %.0f ms (limit 200 + 400)\n", ms);
        }
        CHECK (kill (a, 0) == 0);           /* it is still there: nothing was sent */
        /* (this test's own child: ended for real) */
        CHECK (walpe_children_end (kids, 2000, 2000, NULL) == 0 && is_gone (a));
        g_array_free (kids, TRUE);
    }
    /* 4. a process that is not ours to wait for is dropped, not waited for
     *    (and gets no SIGKILL for it: the signal function counts) */
    {
        GArray *kids = g_array_new (FALSE, FALSE, sizeof (pid_t));
        pid_t parent = getppid (), a = child (FALSE);
        g_array_append_val (kids, parent);
        g_array_append_val (kids, a);
        sent_term = sent_kill = 0;
        gint64 t0 = g_get_monotonic_time ();
        guint left = walpe_children_end (kids, 100, 100, no_signal);
        CHECK (left == 1 && g_array_index (kids, pid_t, 0) == a);   /* the stranger is off the list */
        CHECK (sent_term == 2 && sent_kill == 1 && ms_since (t0) < 2000);
        CHECK (walpe_children_end (kids, 2000, 2000, NULL) == 0 && is_gone (a));
        g_array_free (kids, TRUE);
    }
    /* 5. nothing to end */
    {
        GArray *kids = g_array_new (FALSE, FALSE, sizeof (pid_t));
        gint64 t0 = g_get_monotonic_time ();
        CHECK (walpe_children_end (kids, 5000, 5000, NULL) == 0);
        CHECK (ms_since (t0) < 1000);
        g_array_free (kids, TRUE);
    }
    if (fails)
        g_printerr ("childend-test: %d check(s) failed\n", fails);
    else
        g_print ("childend-test: ok\n");
    return fails ? 1 : 0;
}
