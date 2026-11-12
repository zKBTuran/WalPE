/* childend.h - end child processes, and stop waiting for them in time.
 *
 * Not a public header: main.c includes it for sw_end_children (), which
 * runs right before the daemon replaces itself (a restart in place), and
 * tests/childend_test.c runs it against children of its own.
 *
 * The children - WebKit's network and page processes - are told to end
 * (SIGTERM), given a moment, and killed (SIGKILL). The wait after the kill
 * was waitpid () without a limit: SIGKILL cannot be refused, but a process
 * that sits in the kernel takes it only when the call it is in comes back
 * - a driver that does not answer, a file system whose server is gone, a
 * frozen control group - and until then the daemon stood still in the
 * middle of its restart, wallpapers gone, the control socket open and
 * unanswered. Both waits end by the clock now; a child that outlives the
 * second one is left behind and named.
 *
 * Nothing runs unless a restart in place does: no timer, no thread. */
#pragma once
#include <errno.h>
#include <glib.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>

#define WALPE_CHILD_TERM_MS 1000    /* for a child to end by itself after SIGTERM */
#define WALPE_CHILD_KILL_MS 2000    /* for the kernel to take it away after SIGKILL */

/* kill (2), or what a test puts in its place */
typedef int (*WalpeChildSignal) (pid_t pid, int sig);

/* takes from kids every process that has ended (and is reaped by this) or
 * that is no child of this process (any more); returns when none is left
 * or `until` (monotonic, microseconds) has passed. Looks at least once. */
static inline void walpe_children_reap_until (GArray *kids, gint64 until)
{
    for (;;) {
        for (guint i = 0; i < kids->len;) {
            pid_t k = g_array_index (kids, pid_t, i);
            pid_t r = waitpid (k, NULL, WNOHANG);
            if (r == k || (r < 0 && errno != EINTR))
                g_array_remove_index_fast (kids, i);
            else
                i++;
        }
        gint64 now = g_get_monotonic_time ();
        if (!kids->len || now >= until)
            return;
        g_usleep ((gulong) MIN (until - now, 50 * 1000));
    }
}

/* kids: process ids (pid_t) of children of this process. Each is sent
 * SIGTERM and they get term_ms to end; those that are left are sent
 * SIGKILL and get kill_ms more. The array holds what is still there
 * afterwards, the return value is its length (0 = every child is gone and
 * reaped). Takes at most term_ms + kill_ms and a few milliseconds.
 * sig: NULL = kill (2). */
static inline guint walpe_children_end (GArray *kids, guint term_ms, guint kill_ms,
                                        WalpeChildSignal sig)
{
    if (!sig)
        sig = kill;
    for (guint i = 0; i < kids->len; i++)
        sig (g_array_index (kids, pid_t, i), SIGTERM);
    walpe_children_reap_until (kids, g_get_monotonic_time () + (gint64) term_ms * 1000);
    for (guint i = 0; i < kids->len; i++)
        sig (g_array_index (kids, pid_t, i), SIGKILL);
    walpe_children_reap_until (kids, g_get_monotonic_time () + (gint64) kill_ms * 1000);
    return kids->len;
}
