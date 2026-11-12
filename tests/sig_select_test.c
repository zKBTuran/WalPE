/* sig_select_test.c - which pause providers can work where, without a
 * compositor and without a bus:
 *  1. wlplan.h on capability lists made up after real compositors
 *     (wlroots with and without the newer protocols, GNOME Shell, KWin, a
 *     compositor that only has the ext window list, no connection);
 *  2. the window arithmetic (what counts as fullscreen / focus / covering,
 *     and whether a fullscreen window's output can be told);
 *  3. WALPE_LOGIND (logindopt.h);
 *  4. the status entry "pause_rules" with made-up providers, some of which
 *     decline: every setting names its source or the reason there is none;
 *  5. the real providers with nothing to talk to: each one declines with
 *     a sentence, none takes the process down, and on the X11 backend the
 *     Wayland ones are not even asked.
 * Links src/signals; the registry is tests/sig_stub.h. */
#include "walpe-build-config.h"
#include "sig_stub.h"
#include "signals/providers.h"
#include "signals/rules.h"
#include "signals/wlplan.h"
#include "signals/logindopt.h"

typedef struct {
    const char *iface;
    guint32 version;
} Glob;

static void plan_of (const Glob *g, WalpeWlPlan *p, WalpeWlCaps *caps_out)
{
    WalpeWlCaps c = { 0 };
    for (; g && g->iface; g++)
        walpe_wlcaps_add (&c, g->iface, g->version);
    walpe_wlplan (&c, p);
    if (caps_out)
        *caps_out = c;
}

static void test_plans (void)
{
    WalpeWlPlan p;

    /* wlroots 0.17 / sway 1.9: everything */
    static const Glob sway19[] = {
        { "wl_compositor", 6 }, { "wl_shm", 1 }, { "wl_seat", 9 }, { "wl_output", 4 },
        { "wl_output", 4 }, { "zwlr_layer_shell_v1", 4 }, { "ext_idle_notifier_v1", 1 },
        { "zwlr_foreign_toplevel_manager_v1", 3 }, { "ext_foreign_toplevel_list_v1", 1 },
        { NULL, 0 } };
    plan_of (sway19, &p, NULL);
    CHECK (p.idle && !p.idle_why, "sway 1.9: idle");
    CHECK (p.fullscreen && !p.fullscreen_why, "sway 1.9: fullscreen");
    CHECK (p.windows && !p.windows_why && p.maximized, "sway 1.9: windows");
    CHECK (p.output_names, "sway 1.9: outputs have names");

    /* an old wlroots compositor: window list version 1, wl_output 3, the
     * KDE idle protocol instead of ext-idle-notify */
    static const Glob old_wlroots[] = {
        { "wl_compositor", 4 }, { "wl_seat", 7 }, { "wl_output", 3 },
        { "org_kde_kwin_idle", 1 }, { "zwlr_foreign_toplevel_manager_v1", 1 }, { NULL, 0 } };
    plan_of (old_wlroots, &p, NULL);
    CHECK (!p.idle && p.idle_why && strstr (p.idle_why, "ext_idle_notifier_v1"), "old wlroots: no idle");
    CHECK (!p.fullscreen && p.fullscreen_why && strstr (p.fullscreen_why, "version 1"),
           "old wlroots: version 1 has no fullscreen state");
    CHECK (p.windows && !p.windows_why, "old wlroots: covered / focus still work");
    CHECK (!p.output_names, "old wlroots: wl_output 3 gives no names");

    /* GNOME Shell: idle notification, no window list of any kind */
    static const Glob gnome[] = {
        { "wl_compositor", 6 }, { "wl_seat", 8 }, { "wl_output", 4 }, { "xdg_wm_base", 6 },
        { "ext_idle_notifier_v1", 1 }, { NULL, 0 } };
    plan_of (gnome, &p, NULL);
    CHECK (p.idle, "GNOME: idle");
    CHECK (!p.fullscreen && !p.windows, "GNOME: no window rules");
    CHECK (p.fullscreen_why && p.fullscreen_why == p.windows_why &&
           strstr (p.fullscreen_why, "zwlr_foreign_toplevel_manager_v1"), "GNOME: names the protocol");

    /* a compositor with only the ext list: it must not be mistaken for a
     * list with state */
    static const Glob ext_only[] = {
        { "wl_seat", 9 }, { "wl_output", 4 }, { "ext_idle_notifier_v1", 2 },
        { "ext_foreign_toplevel_list_v1", 1 }, { NULL, 0 } };
    plan_of (ext_only, &p, NULL);
    CHECK (p.idle && !p.fullscreen && !p.windows, "ext list only: idle, no window rules");
    CHECK (p.windows_why && strstr (p.windows_why, "ext_foreign_toplevel_list_v1") &&
           strstr (p.windows_why, "state"), "ext list only: says the list has no state");

    /* idle notifier but no seat to count input on */
    static const Glob no_seat[] = { { "wl_output", 4 }, { "ext_idle_notifier_v1", 1 }, { NULL, 0 } };
    plan_of (no_seat, &p, NULL);
    CHECK (!p.idle && p.idle_why && strstr (p.idle_why, "wl_seat"), "no seat: no idle");

    /* mixed output versions: the lowest decides */
    static const Glob mixed[] = { { "wl_output", 4 }, { "wl_output", 2 }, { NULL, 0 } };
    WalpeWlCaps c;
    plan_of (mixed, &p, &c);
    CHECK (c.n_outputs == 2 && c.output == 2 && !p.output_names, "mixed outputs: lowest version");

    /* nothing answered */
    WalpeWlCaps none = { 0 };
    walpe_wlplan (&none, &p);
    CHECK (!p.idle && !p.fullscreen && !p.windows, "no connection: nothing");
    CHECK (p.idle_why && p.fullscreen_why && p.windows_why && strstr (p.idle_why, "no connection"),
           "no connection: every rule has the reason");
}

static void test_windows (void)
{
    guint a = WALPE_TL_ACTIVATED, m = WALPE_TL_MAXIMIZED, f = WALPE_TL_FULLSCREEN,
          n = WALPE_TL_MINIMIZED;
    CHECK (walpe_tl_is_fullscreen_focus (a | f), "activated + fullscreen");
    CHECK (!walpe_tl_is_fullscreen_focus (f), "fullscreen without focus is not the fullscreen rule");
    CHECK (!walpe_tl_is_fullscreen_focus (a | m), "maximized is not fullscreen");
    CHECK (!walpe_tl_is_fullscreen_focus (a | f | n), "a minimized window shows nothing");
    CHECK (walpe_tl_is_focus (a) && !walpe_tl_is_focus (0) && !walpe_tl_is_focus (a | n), "focus");

    /* default: only the activated window covers */
    CHECK (walpe_tl_covers (a | m, FALSE, FALSE), "focused maximized covers");
    CHECK (walpe_tl_covers (a | f, FALSE, FALSE), "focused fullscreen covers");
    CHECK (!walpe_tl_covers (m, FALSE, FALSE), "unfocused maximized: not by default");
    CHECK (!walpe_tl_covers (f, FALSE, FALSE), "unfocused fullscreen: not by default");
    CHECK (!walpe_tl_covers (a, FALSE, FALSE), "a plain focused window does not cover");
    /* WALPE_WL_COVERED=all */
    CHECK (walpe_tl_covers (m, TRUE, FALSE) && walpe_tl_covers (f, TRUE, FALSE), "all: any maximized / fullscreen");
    CHECK (!walpe_tl_covers (0, TRUE, FALSE), "all: still needs maximized / fullscreen");
    CHECK (!walpe_tl_covers (m | n, TRUE, FALSE), "minimized never covers");
    CHECK (!walpe_tl_covers (a | f, TRUE, TRUE), "a see-through window never covers");

    CHECK (walpe_tl_listed ("foot,st", "foot"), "listed");
    CHECK (walpe_tl_listed (" Foot , ST ", "st"), "blanks and case");
    CHECK (!walpe_tl_listed ("foot,st", "footclient"), "no prefix match");
    CHECK (!walpe_tl_listed ("footclient", "foot"), "no prefix match (other way)");
    CHECK (!walpe_tl_listed (NULL, "foot") && !walpe_tl_listed ("", "foot") &&
           !walpe_tl_listed ("foot", NULL) && !walpe_tl_listed ("foot", ""), "empty lists and ids");
    CHECK (walpe_tl_listed (",,org.wezfurlong.wezterm,", "org.wezfurlong.wezterm"), "empty entries");
}

/* which output a focused fullscreen window is on, as far as the
 * compositor's words can be matched to the daemon's outputs (wlplan.h):
 * the fullscreen rule pauses that monitor, or every monitor where the
 * answer is "cannot be told" */
static void test_where (void)
{
    const gboolean yes = TRUE, no = FALSE;
    /* one output, named, the daemon's: known */
    CHECK (walpe_tl_where (1, (guint32[]) { 11 }, (gboolean[]) { yes }) == WALPE_TL_WHERE_KNOWN,
           "on a named output of the daemon");
    /* spanning two of the daemon's outputs */
    CHECK (walpe_tl_where (2, (guint32[]) { 11, 22 }, (gboolean[]) { yes, yes }) == WALPE_TL_WHERE_KNOWN,
           "on two outputs");
    /* a named output the daemon shows nothing on (--monitor N): known, and
     * on none of the daemon's - no monitor pauses for it */
    CHECK (walpe_tl_where (1, (guint32[]) { 0 }, (gboolean[]) { yes }) == WALPE_TL_WHERE_KNOWN,
           "a named output that is not the daemon's is still an answer");
    CHECK (walpe_tl_where (2, (guint32[]) { 0, 22 }, (gboolean[]) { yes, yes }) == WALPE_TL_WHERE_KNOWN,
           "one of the daemon's and one that is not");
    /* no names, but the daemon has a single output: the lookup gave its id */
    CHECK (walpe_tl_where (1, (guint32[]) { 7 }, (gboolean[]) { no }) == WALPE_TL_WHERE_KNOWN,
           "an output without a name that the daemon could still tell (its only one)");
    /* no names and several outputs: no id, and no name to say whose it is */
    CHECK (walpe_tl_where (1, (guint32[]) { 0 }, (gboolean[]) { no }) == WALPE_TL_WHERE_NO_NAME,
           "an output without a name among several: cannot be matched");
    CHECK (walpe_tl_where (2, (guint32[]) { 11, 0 }, (gboolean[]) { yes, no }) == WALPE_TL_WHERE_NO_NAME,
           "one output matched, one not: the whole answer is unknown");
    /* the compositor names no output for the window at all */
    CHECK (walpe_tl_where (0, NULL, NULL) == WALPE_TL_WHERE_NO_OUTPUT, "no output announced");
    /* the sentences for status */
    CHECK (walpe_tl_where_why (WALPE_TL_WHERE_KNOWN) == NULL, "known: nothing to say");
    CHECK (strstr (walpe_tl_where_why (WALPE_TL_WHERE_NO_OUTPUT), "which output") != NULL,
           "no output: says the compositor does not tell");
    CHECK (strstr (walpe_tl_where_why (WALPE_TL_WHERE_NO_NAME), "wl_output") != NULL &&
           strstr (walpe_tl_where_why (WALPE_TL_WHERE_NO_NAME), "version 4") != NULL,
           "no name: names what is missing");

    /* rules.h: the note under "Pause for fullscreen apps" */
    char *n = walpe_pause_rule_everywhere_note (NULL, FALSE);
    CHECK (n == NULL, "the monitor is known: no note");
    g_free (n);
    n = walpe_pause_rule_everywhere_note ("", FALSE);
    CHECK (n == NULL, "an empty reason is no reason");
    g_free (n);
    n = walpe_pause_rule_everywhere_note (WALPE_WLPLAN_FS_NO_NAME, FALSE);
    CHECK (n && strstr (n, "pauses every monitor") && strstr (n, "wl_output"),
           "not known: says that every monitor pauses, and why: %s", n ? n : "(null)");
    g_free (n);
    n = walpe_pause_rule_everywhere_note (WALPE_WLPLAN_FS_NO_NAME, TRUE);
    CHECK (n == NULL, "every monitor is what the user set (pause_fullscreen_all): nothing to explain");
    g_free (n);

    /* rules.h: the note under "Pause when covered" where only the
     * compositor's frame events report it (KWin: no window list) */
    n = walpe_pause_rule_frames_only_note ("wl-frame", "wl-windows: the compositor has no window list");
    CHECK (n && strstr (n, "locked screen") && strstr (n, "is not known in this session") &&
           strstr (n, "wl-windows: the compositor has no window list"),
           "frames only: says what pauses, what does not, and why: %s", n ? n : "(null)");
    g_free (n);
    n = walpe_pause_rule_frames_only_note ("wl-frame", NULL);
    CHECK (n && !strstr (n, ": "), "frames only, no reason told: the sentence alone");
    g_free (n);
    n = walpe_pause_rule_frames_only_note ("wl-frame,wl-windows", "");
    CHECK (n == NULL, "the window list reports too: nothing to explain");
    g_free (n);
    n = walpe_pause_rule_frames_only_note ("x11-windows", "anything");
    CHECK (n == NULL, "another source alone (X11): no note");
    g_free (n);
    n = walpe_pause_rule_frames_only_note ("", "wl-windows: no");
    CHECK (n == NULL, "no source: that is the other note's case");
    g_free (n);
    n = walpe_pause_rule_frames_only_note (NULL, NULL);
    CHECK (n == NULL, "no source at all");
    g_free (n);
}

static void test_logind_opt (void)
{
    CHECK (walpe_logind_facts (NULL, TRUE) == 0, "X11 default: off");
    CHECK (walpe_logind_facts ("", TRUE) == 0, "X11, empty: off");
    CHECK (walpe_logind_facts (NULL, FALSE) == WALPE_LOGIND_ALL, "Wayland default: all");
    CHECK (walpe_logind_facts ("1", TRUE) == WALPE_LOGIND_ALL, "X11 opt-in");
    CHECK (walpe_logind_facts ("0", FALSE) == 0 && walpe_logind_facts ("off", FALSE) == 0, "Wayland opt-out");
    CHECK (walpe_logind_facts ("sleep", FALSE) == WALPE_LOGIND_SLEEP, "one fact");
    CHECK (walpe_logind_facts ("lock, vt", TRUE) == (WALPE_LOGIND_LOCK | WALPE_LOGIND_VT), "a list, on X11 too");
    CHECK (walpe_logind_facts ("bogus", FALSE) == WALPE_LOGIND_ALL && walpe_logind_facts ("bogus", TRUE) == 0,
           "an unknown word is the default, not 'off'");
    CHECK (walpe_logind_asleep (WALPE_LOGIND_ALL, TRUE, FALSE, FALSE), "sleeping");
    CHECK (walpe_logind_asleep (WALPE_LOGIND_ALL, FALSE, TRUE, FALSE), "locked");
    CHECK (walpe_logind_asleep (WALPE_LOGIND_ALL, FALSE, FALSE, TRUE), "inactive");
    CHECK (!walpe_logind_asleep (WALPE_LOGIND_ALL, FALSE, FALSE, FALSE), "awake");
    CHECK (!walpe_logind_asleep (WALPE_LOGIND_SLEEP, FALSE, TRUE, TRUE), "facts not in use do not count");
}

/* ---- made-up providers for the status entry */

static gboolean ok_start (const WalpeSignalProvider *p, char **why)
{
    (void) p; (void) why;
    return TRUE;
}

static gboolean no_start (const WalpeSignalProvider *p, char **why)
{
    *why = g_strdup_printf ("%s cannot work here", p->name);
    return FALSE;
}

static gboolean silent_no_start (const WalpeSignalProvider *p, char **why)
{
    (void) p; (void) why;   /* declines without a reason */
    return FALSE;
}

static void nop_stop (const WalpeSignalProvider *p)
{
    (void) p;
}

static JsonObject *status_of (const WalpeConfig *cfg, JsonNode **root)
{
    JsonBuilder *b = json_builder_new ();
    json_builder_begin_object (b);
    walpe_sigprov_status_json (b, cfg);
    json_builder_end_object (b);
    *root = json_builder_get_root (b);
    g_object_unref (b);
    JsonObject *o = json_node_get_object (*root);
    return json_object_has_member (o, "pause_rules") ? json_object_get_object_member (o, "pause_rules")
                                                     : NULL;
}

static void test_status (void)
{
    static const WalpeSignalProvider fs = { "fake-fs", WALPE_SIG_BIT (WALPE_SIG_FULLSCREEN), 0,
                                            ok_start, nop_stop, NULL };
    static const WalpeSignalProvider cov = { "fake-windows",
                                             WALPE_SIG_BIT (WALPE_SIG_COVERED) |
                                                 WALPE_SIG_BIT (WALPE_SIG_FOCUS_APP),
                                             0, no_start, nop_stop, NULL };
    static const WalpeSignalProvider idle = { "fake-idle", WALPE_SIG_BIT (WALPE_SIG_IDLE), 0,
                                              silent_no_start, nop_stop, NULL };
    /* an X11-only provider on a Wayland backend: neither a source nor missing */
    static const WalpeSignalProvider x11 = { "fake-x11", WALPE_SIG_BIT (WALPE_SIG_ASLEEP),
                                             1u << WALPE_BACKEND_X11, ok_start, nop_stop, NULL };
    stub_reset ();
    walpe_signals_register (&fs);
    walpe_signals_register (&cov);
    walpe_signals_register (&idle);
    walpe_signals_register (&x11);
    walpe_signals_start (WALPE_BACKEND_WAYLAND, NULL, NULL);

    WalpeConfig cfg = { 0 };
    cfg.pause_fullscreen = TRUE;
    cfg.pause_covered = TRUE;
    cfg.pause_dpms = TRUE;
    cfg.focus_action = WALPE_FOCUS_RUN;
    cfg.pause_idle = 0;
    JsonNode *root = NULL;
    JsonObject *r = status_of (&cfg, &root);
    CHECK (r != NULL, "status has pause_rules");
    if (r) {
        CHECK (json_object_get_size (r) == WALPE_PAUSE_RULES_N, "one entry per rule");
        JsonObject *o = json_object_get_object_member (r, "pause_fullscreen");
        CHECK (o && json_object_get_boolean_member (o, "on") && json_object_get_boolean_member (o, "works") &&
               !g_strcmp0 (json_object_get_string_member (o, "sources"), "fake-fs") &&
               !g_strcmp0 (json_object_get_string_member (o, "signal"), "fullscreen") &&
               !json_object_has_member (o, "note"), "a rule with a source: works, no note");
        o = json_object_get_object_member (r, "pause_covered");
        CHECK (o && json_object_get_boolean_member (o, "on") && !json_object_get_boolean_member (o, "works") &&
               json_object_has_member (o, "note") &&
               strstr (json_object_get_string_member (o, "note"), "fake-windows cannot work here"),
               "a rule whose provider declined: the note carries the provider's reason");
        o = json_object_get_object_member (r, "focus_action");
        CHECK (o && !json_object_get_boolean_member (o, "on") && !json_object_get_boolean_member (o, "works") &&
               json_object_has_member (o, "note"), "an unavailable rule is marked even while it is off");
        o = json_object_get_object_member (r, "pause_idle");
        CHECK (o && !json_object_get_boolean_member (o, "on") && json_object_has_member (o, "note") &&
               strstr (json_object_get_string_member (o, "note"), "not available in this session"),
               "a provider that declined without a reason still yields a sentence");
        o = json_object_get_object_member (r, "pause_dpms");
        CHECK (o && !json_object_get_boolean_member (o, "works") && json_object_has_member (o, "note") &&
               strstr (json_object_get_string_member (o, "note"), "nothing here reports"),
               "no provider for this backend at all: says nothing reports it");
    }
    json_node_unref (root);

    /* the same settings where it works */
    cfg.focus_action = WALPE_FOCUS_PAUSE;
    cfg.pause_idle = 120;
    stub_reset ();
    walpe_signals_register (&x11);
    walpe_signals_start (WALPE_BACKEND_X11, NULL, NULL);
    r = status_of (&cfg, &root);
    if (r) {
        JsonObject *o = json_object_get_object_member (r, "pause_dpms");
        CHECK (o && json_object_get_boolean_member (o, "works") && !json_object_has_member (o, "note"),
               "X11 backend: its provider is the source");
        o = json_object_get_object_member (r, "focus_action");
        CHECK (o && json_object_get_boolean_member (o, "on"), "focus_action counts as on when not 'run'");
        o = json_object_get_object_member (r, "pause_idle");
        CHECK (o && json_object_get_boolean_member (o, "on"), "pause_idle counts as on when > 0");
    }
    json_node_unref (root);
    stub_reset ();

    /* rules.h by itself */
    const WalpePauseRule *pr = walpe_pause_rule_find ("pause_covered");
    CHECK (pr && pr->signal == WALPE_SIG_COVERED, "rule lookup");
    CHECK (!walpe_pause_rule_find ("fps") && !walpe_pause_rule_find (NULL), "settings without a signal");
    char *note = walpe_pause_rule_note (pr, "x11", "whatever");
    CHECK (note == NULL, "a working rule has no note");
    g_free (note);
}

/* ---- the real providers with nothing to talk to */

static guint32 no_output (const char *name, gpointer user)
{
    (void) name; (void) user;
    return 0;
}

static void test_real_unavailable (void)
{
    walpe_sigprov_set_output_lookup (no_output, NULL);
    walpe_sigprov_set_idle_timeout (60);

    /* Wayland backend, no compositor at the socket named, logind switched off */
    stub_reset ();
    walpe_sigprov_register (WALPE_BACKEND_WAYLAND);
    walpe_signals_start (WALPE_BACKEND_WAYLAND, NULL, NULL);
    StubProv *sp = stub_prov (&walpe_sig_logind);
    CHECK (sp && sp->failed && sp->why && strstr (sp->why, "WALPE_LOGIND"),
           "logind switched off: declines and says by what");
#ifdef HAVE_WL_SIGNALS
    const WalpeSignalProvider *wl[] = { &walpe_sig_wl_idle, &walpe_sig_wl_windows,
                                        &walpe_sig_wl_fullscreen };
    for (guint i = 0; i < G_N_ELEMENTS (wl); i++) {
        sp = stub_prov (wl[i]);
        CHECK (sp != NULL, "%s is registered", wl[i]->name);
        CHECK (sp && sp->failed && !sp->started, "%s declines without a compositor", wl[i]->name);
        CHECK (sp && sp->why && strstr (sp->why, "no connection"), "%s says why: %s", wl[i]->name,
               sp && sp->why ? sp->why : "(nothing)");
    }
    CHECK (Stub.n_val == 0 || !walpe_signal_get (WALPE_SIG_IDLE, 0), "nothing is reported as on");
    CHECK (*walpe_signal_sources (WALPE_SIG_FULLSCREEN) == 0, "no source for fullscreen");
    CHECK (strstr (walpe_signal_missing (WALPE_SIG_FULLSCREEN), "no connection") != NULL,
           "the reason reaches the status question");
#endif
    stub_reset ();

    /* X11 backend: the Wayland providers are not asked, logind declines
     * (X11 keeps its own detection) unless told otherwise */
    g_unsetenv ("WALPE_LOGIND");
    walpe_sigprov_register (WALPE_BACKEND_X11);
    walpe_signals_start (WALPE_BACKEND_X11, NULL, NULL);
    sp = stub_prov (&walpe_sig_logind);
    CHECK (sp && sp->failed && sp->why && strstr (sp->why, "X11"), "logind on X11: off by default, says so");
#ifdef HAVE_WL_SIGNALS
    sp = stub_prov (&walpe_sig_wl_idle);
    CHECK (sp && !sp->started && !sp->failed, "wl-idle is not started on X11");
    CHECK (*walpe_signal_missing (WALPE_SIG_IDLE) == 0, "and is not listed as missing there");
#endif
    stub_reset ();
}

int main (void)
{
    /* before anything can start a thread */
    g_setenv ("WALPE_SIG_WAYLAND_DISPLAY", "/nonexistent/walpe-test/wayland-none", TRUE);
    g_setenv ("WALPE_LOGIND", "0", TRUE);
    test_plans ();
    test_where ();
    test_windows ();
    test_logind_opt ();
    test_status ();
    test_real_unavailable ();
    if (stub_fails) {
        fprintf (stderr, "sig-select: %d check(s) failed\n", stub_fails);
        return 1;
    }
    printf ("sig-select: ok\n");
    return 0;
}
