/* rules.h - the pause settings that depend on something the session has
 * to report, and what to tell the user when nothing does.
 *
 * A switch such as "Pause for fullscreen apps" only does something where
 * the daemon can find out that a window is fullscreen. On X11 it always
 * can; on Wayland that depends on the compositor. Rather than leave such
 * a switch silently without effect, `walpe-ctl status` carries one entry
 * per setting ("pause_rules") saying which provider drives it, or why
 * none does, and the settings window prints that sentence under the
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
