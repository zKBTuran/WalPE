/* suppoll.h - how often the window asks the engine for its status, as a
 * pure function (main.c and tests/ui_session_test.c include it).
 *
 * The state is pushed over a subscribed connection (common/ipcsub.h).
 * While that is live the window does not ask at all unless it is the
 * active one, and then every 10 s (an engine that hangs is still noticed);
 * a window that shows such an engine as not answering goes on asking at
 * that rate without the focus too, until it answers.
 * An older engine that cannot push is asked every 2 s while the window is
 * active. An engine that is starting, not answering or not seen yet is
 * asked every 2 s.
 *
 * An engine that is stopped and that nothing here is about to start (it
 * was told to quit, it keeps crashing, it could not be started) used to be
 * asked every 2 s as well, for as long as the window stayed open: a
 * request to a socket that is not there, 200 wake-ups a minute. The
 * subscription watches the socket's directory and connects by itself when
 * somebody starts an engine, so there is nothing to ask for: no timer
 * while the window is in the background, every 10 s while it is active.
 * Without that watch the 2 s stay. */
#pragma once
#include <glib.h>

typedef struct {
    gboolean live;          /* the subscription receives pushes */
    gboolean unsupported;   /* the engine that answered cannot push */
    gboolean up;            /* the last status request was answered */
    gboolean stopped;       /* down, and no start or restart is under way here */
    gboolean watching;      /* the subscription watches for the engine's socket */
    gboolean active;        /* the window is the active one */
} WalpeSupPoll;

/* seconds between status requests, 0 = no timer */
static inline guint walpe_sup_poll_secs (const WalpeSupPoll *p)
{
    /* (live but not up: the engine's connection stands and it does not
     * answer, or was taken for gone. When it answers again it has nothing
     * to push - its state did not change - so a window in the background
     * went on saying "Engine not responding" until it got the focus. It
     * is asked at the active window's rate until an answer comes.) */
    if (p->live)
        return (p->active || !p->up) ? 10 : 0;
    if (p->unsupported && p->up)
        return p->active ? 2 : 0;
    if (p->stopped && p->watching)
        return p->active ? 10 : 0;
    return 2;
}
