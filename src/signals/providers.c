/* providers.c - what the daemon calls for the providers of src/signals:
 * registration, the few values they need from it, and the "pause_rules"
 * entry of the status. See providers.h. */
#include "walpe-build-config.h"
#include "signals/providers.h"
#include "signals/rules.h"

static struct {
    WalpeBackend backend;
    WalpeSigOutputId lookup;
    gpointer lookup_user;
    guint idle_timeout_s;
    char *see_through;
} P;

WalpeBackend walpe_sigprov_backend (void)
{
    return P.backend;
}

void walpe_sigprov_register (WalpeBackend backend)
{
    P.backend = backend;
    /* (the registry starts a provider only on a backend it lists; the
     * Wayland ones list Wayland, logind decides in its start function) */
    walpe_signals_register (&walpe_sig_logind);
#ifdef HAVE_WL_SIGNALS
    walpe_signals_register (&walpe_sig_wl_idle);
    /* windows before fullscreen: on a compositor whose window list is
     * version 1 the second one declines, and the tracker stays up */
    walpe_signals_register (&walpe_sig_wl_windows);
    walpe_signals_register (&walpe_sig_wl_fullscreen);
#endif
}

void walpe_sigprov_set_output_lookup (WalpeSigOutputId fn, gpointer user)
{
    P.lookup = fn;
    P.lookup_user = user;
    walpe_sigprov_outputs_changed ();
}

guint32 walpe_sigprov_output_id (const char *name)
{
    return P.lookup ? P.lookup (name, P.lookup_user) : 0;
}

void walpe_sigprov_outputs_changed (void)
{
#ifdef HAVE_WL_SIGNALS
    walpe_sig_wl_outputs_changed ();
#endif
}

void walpe_sigprov_set_idle_timeout (guint seconds)
{
    P.idle_timeout_s = seconds;
}

guint walpe_sigprov_idle_timeout (void)
{
    return P.idle_timeout_s;
}

void walpe_sigprov_set_see_through (const char *app_ids)
{
    if (!g_strcmp0 (P.see_through, app_ids))
        return;
    g_free (P.see_through);
    P.see_through = g_strdup (app_ids);
#ifdef HAVE_WL_SIGNALS
    if (walpe_sig_wl_windows.recheck)
        walpe_sig_wl_windows.recheck (&walpe_sig_wl_windows, WALPE_SIG_COVERED, FALSE);
#endif
}

const char *walpe_sigprov_see_through (void)
{
    return P.see_through;
}

/* is the setting switched on? (what "on" means differs per setting) */
static gboolean rule_on (const WalpePauseRule *r, const WalpeConfig *cfg)
{
    switch (r->signal) {
    case WALPE_SIG_FULLSCREEN:
        return cfg->pause_fullscreen;
    case WALPE_SIG_COVERED:
        return cfg->pause_covered;
    case WALPE_SIG_ASLEEP:
        return cfg->pause_dpms;
    case WALPE_SIG_FOCUS_APP:
        return cfg->focus_action != WALPE_FOCUS_RUN;
    case WALPE_SIG_IDLE:
        return cfg->pause_idle > 0;
    default:
        return FALSE;
    }
}

void walpe_sigprov_status_json (JsonBuilder *b, const WalpeConfig *cfg)
{
    json_builder_set_member_name (b, "pause_rules");
    json_builder_begin_object (b);
    for (guint i = 0; i < WALPE_PAUSE_RULES_N; i++) {
        const WalpePauseRule *r = &walpe_pause_rules[i];
        /* (the registry's strings are valid until its next call for the
         * same signal: copy before asking for the other one) */
        char *sources = g_strdup (walpe_signal_sources (r->signal));
        char *missing = g_strdup (walpe_signal_missing (r->signal));
        char *note = walpe_pause_rule_note (r, sources, missing);
        json_builder_set_member_name (b, r->key);
        json_builder_begin_object (b);
        json_builder_set_member_name (b, "on");
        json_builder_add_boolean_value (b, rule_on (r, cfg));
        json_builder_set_member_name (b, "signal");
        json_builder_add_string_value (b, walpe_signal_name (r->signal));
        json_builder_set_member_name (b, "sources");
        json_builder_add_string_value (b, sources ? sources : "");
        json_builder_set_member_name (b, "works");
        json_builder_add_boolean_value (b, walpe_pause_rule_works (sources));
        if (note) {
            json_builder_set_member_name (b, "note");
            json_builder_add_string_value (b, note);
        }
        json_builder_end_object (b);
        g_free (note);
        g_free (sources);
        g_free (missing);
    }
    json_builder_end_object (b);
}
