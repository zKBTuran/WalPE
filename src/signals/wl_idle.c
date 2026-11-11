/* wl_idle.c - provider "wl-idle": nobody has touched keyboard or pointer
 * for the time of the "pause_idle" setting (ext-idle-notify-v1).
 *
 * The compositor does the counting: the provider asks for one
 * notification object with the timeout and gets `idled` when it has
 * passed and `resumed` at the next input. Applications that inhibit
 * idling (a video player, a presentation) hold `idled` back, which is
 * what a wallpaper wants as well: someone is watching the screen.
 *
 * With the setting at 0 the rule is off: the provider stays started (so
 * that status can say the rule would work here) but keeps no object and
 * reports "not idle". */
#include "walpe-build-config.h"
#include "signals/providers.h"
#include "signals/wl_session.h"
#include "ext-idle-notify-v1-client-protocol.h"

static struct {
    WlSession *sess;
    struct ext_idle_notifier_v1 *notifier;
    struct ext_idle_notification_v1 *note;
    guint timeout_s;        /* of `note` */
    gboolean idle;
} I;

static void idle_set (gboolean idle)
{
    if (idle != I.idle) {
        I.idle = idle;
        g_debug ("wl-idle: %s", idle ? "idle" : "input again");
    }
    walpe_signal_set (&walpe_sig_wl_idle, WALPE_SIG_IDLE, 0, idle);
}

static void note_idled (void *data, struct ext_idle_notification_v1 *n)
{
    (void) data;
    if (n == I.note)
        idle_set (TRUE);
}

static void note_resumed (void *data, struct ext_idle_notification_v1 *n)
{
    (void) data;
    if (n == I.note)
        idle_set (FALSE);
}

static const struct ext_idle_notification_v1_listener note_listener = { note_idled, note_resumed };

/* (re)create the notification for the timeout in force */
static void idle_arm (void)
{
    guint want = walpe_sigprov_idle_timeout ();
    if (I.note && want == I.timeout_s)
        return;
    if (I.note) {
        ext_idle_notification_v1_destroy (I.note);
        I.note = NULL;
    }
    I.timeout_s = want;
    /* a new object starts counting from now: not idle until it says so */
    idle_set (FALSE);
    if (want && I.notifier && walpe_wlsess_alive (I.sess)) {
        /* (the protocol takes milliseconds in 32 bits: 49 days at most) */
        guint32 ms = want > G_MAXUINT32 / 1000 ? G_MAXUINT32 : want * 1000u;
        I.note = ext_idle_notifier_v1_get_idle_notification (I.notifier, ms,
                                                             walpe_wlsess_seat (I.sess));
        ext_idle_notification_v1_add_listener (I.note, &note_listener, NULL);
    }
    walpe_wlsess_flush (I.sess);
}

static void idle_lost (gpointer user)
{
    (void) user;
    /* the proxies are dead with the connection; only forget them */
    if (I.note)
        ext_idle_notification_v1_destroy (I.note);
    if (I.notifier)
        ext_idle_notifier_v1_destroy (I.notifier);
    I.note = NULL;
    I.notifier = NULL;
    idle_set (FALSE);
}

static const WlSessUser idle_user = { NULL, idle_lost, NULL };

static gboolean idle_start (const WalpeSignalProvider *self, char **why)
{
    (void) self;
    if (I.sess)
        return TRUE;
    WlSession *s = walpe_wlsess_ref (why);
    if (!s)
        return FALSE;
    WalpeWlPlan plan;
    walpe_wlplan (walpe_wlsess_caps (s), &plan);
    if (!plan.idle) {
        if (why)
            *why = g_strdup (plan.idle_why);
        walpe_wlsess_unref (s);
        return FALSE;
    }
    I.notifier = walpe_wlsess_bind (s, &ext_idle_notifier_v1_interface, 1, NULL);
    if (!I.notifier) {
        if (why)
            *why = g_strdup (WALPE_WLPLAN_NO_IDLE);
        walpe_wlsess_unref (s);
        return FALSE;
    }
    I.sess = s;
    I.idle = FALSE;
    I.timeout_s = 0;
    walpe_wlsess_add_user (s, &idle_user);
    idle_arm ();
    return TRUE;
}

static void idle_stop (const WalpeSignalProvider *self)
{
    (void) self;
    if (!I.sess)
        return;
    walpe_wlsess_remove_user (I.sess, &idle_user);
    if (I.note)
        ext_idle_notification_v1_destroy (I.note);
    if (I.notifier)
        ext_idle_notifier_v1_destroy (I.notifier);
    I.note = NULL;
    I.notifier = NULL;
    walpe_wlsess_flush (I.sess);
    walpe_wlsess_unref (I.sess);
    I.sess = NULL;
    I.idle = FALSE;
}

/* the setting changed */
static void idle_recheck (const WalpeSignalProvider *self, WalpeSignal sig, gboolean now)
{
    (void) self; (void) now;
    if (I.sess && sig == WALPE_SIG_IDLE)
        idle_arm ();
}

const WalpeSignalProvider walpe_sig_wl_idle = {
    .name = "wl-idle",
    .signals = WALPE_SIG_BIT (WALPE_SIG_IDLE),
    .backends = 1u << WALPE_BACKEND_WAYLAND,
    .start = idle_start,
    .stop = idle_stop,
    .recheck = idle_recheck,
};
