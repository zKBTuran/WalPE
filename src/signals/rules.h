/* rules.h - the pause settings that depend on something the session has
 * to report, and what to tell the user when nothing does.
 *
 * A switch such as "Pause for fullscreen apps" only does something where
 * the daemon can find out that a window is fullscreen. On X11 it always
 * can; on Wayland that depends on the compositor. Rather than leave such
 * a switch silently without effect, `walpe-ctl status` carries one entry
 * per setting ("pause_rules") saying which provider drives it, or why
 * none does - or, for the fullscreen rule, why it cannot tell the monitors
 * apart here - and the settings window prints that sentence under the
 * switch. Pure functions: the daemon (providers.c), the GUI and
 * tests/sig_select_test.c share them. */
#pragma once
#include <glib.h>
#include "platform/signals.h"

typedef struct {
    const char *key;        /* the setting, as in config.json and `walpe-ctl set` */
    WalpeSignal signal;     /* what it waits for */
    const char *what;       /* for the sentence: "fullscreen windows" */
} WalpePauseRule;

static const WalpePauseRule walpe_pause_rules[] = {
    { "pause_fullscreen", WALPE_SIG_FULLSCREEN, "fullscreen windows" },
    { "pause_covered", WALPE_SIG_COVERED, "windows covering a monitor" },
    { "pause_dpms", WALPE_SIG_ASLEEP, "sleeping displays, a locked session or suspend" },
    { "focus_action", WALPE_SIG_FOCUS_APP, "which window has the focus" },
    { "pause_idle", WALPE_SIG_IDLE, "user idle time" },
};
#define WALPE_PAUSE_RULES_N G_N_ELEMENTS (walpe_pause_rules)

/* the rule of a setting, NULL when the setting depends on no signal */
static inline const WalpePauseRule *walpe_pause_rule_find (const char *key)
{
    for (guint i = 0; key && i < WALPE_PAUSE_RULES_N; i++)
        if (!strcmp (walpe_pause_rules[i].key, key))
            return &walpe_pause_rules[i];
    return NULL;
}

/* sources = the running providers of the rule's signal ("" / NULL = none),
 * missing = why providers that could have reported it did not start
 * (walpe_signal_sources / walpe_signal_missing of the registry). */
static inline gboolean walpe_pause_rule_works (const char *sources)
{
    return sources && *sources;
}

/* The sentence for a rule nothing can drive; NULL when it works.
 * g_free. */
static inline char *walpe_pause_rule_note (const WalpePauseRule *r, const char *sources,
                                           const char *missing)
{
    if (walpe_pause_rule_works (sources))
        return NULL;
    if (missing && *missing)
        return g_strdup_printf ("not available in this session: %s", missing);
    return g_strdup_printf ("not available in this session: nothing here reports %s", r->what);
}

/* "Pause a covered monitor" has two sources on Wayland: the backend's own
 * ("wl-frame": the compositor stopped asking the wallpaper surface for
 * frames) and the compositor's window list ("wl-windows"). With the first
 * alone the rule counts as working, and it does for a locked screen or a
 * display that is off - but a compositor that goes on asking a surface
 * for frames while a window hides it (KWin does, 5.27 and 6.8) never
 * pauses the wallpaper behind a fullscreen or maximized window, and
 * nothing said so. The sentence for that; NULL when another source
 * reports windows too, or on X11. missing: why the window list did not
 * start (walpe_signal_missing). g_free. */
#define WALPE_RULE_SRC_FRAMES "wl-frame"

static inline char *walpe_pause_rule_frames_only_note (const char *sources, const char *missing)
{
    if (g_strcmp0 (sources, WALPE_RULE_SRC_FRAMES) != 0)
        return NULL;
    return g_strdup_printf ("only while the compositor does not show the wallpaper at all (a "
                            "locked screen, a display that is off, a compositor that leaves "
                            "hidden surfaces out); a window that covers the wallpaper is not "
                            "known in this session%s%s", missing && *missing ? ": " : "",
                            missing && *missing ? missing : "");
}

/* "Pause for fullscreen apps" pauses the monitor the focused fullscreen
 * window is on. Where the session reports such a window but not the
 * monitor it is on (a Wayland compositor that does not name the window's
 * output: signals/wlplan.h), the rule works as it did before it looked at
 * monitors: every monitor pauses. The sentence for that, so that a second
 * monitor that stops is explained where the user looks for it; NULL when
 * the monitor is known (everywhere_why NULL / "") or when every monitor is
 * what the user asked for (the setting pause_fullscreen_all). g_free. */
static inline char *walpe_pause_rule_everywhere_note (const char *everywhere_why,
                                                      gboolean all_wanted)
{
    if (all_wanted || !everywhere_why || !*everywhere_why)
        return NULL;
    return g_strdup_printf ("pauses every monitor, not only the one the window is on: %s",
                            everywhere_why);
}
