/* wlplan.h - what a Wayland compositor's list of globals lets the pause
 * providers do. Pure functions, no Wayland header: wl_session.c feeds it
 * the registry's announcements, tests/sig_select_test.c feeds it lists
 * made up after real compositors.
 *
 * A Wayland client cannot look at other clients' windows. What it may
 * learn is what the compositor chooses to offer as a protocol:
 *
 *   ext_idle_notifier_v1               "no input for N ms" (most compositors)
 *   zwlr_foreign_toplevel_manager_v1   every toplevel with its state
 *                                      (activated, maximized, minimized,
 *                                      from version 2 fullscreen) and the
 *                                      outputs it is on: wlroots
 *                                      compositors (sway, Hyprland, river,
 *                                      labwc, Wayfire, niri ...), not
 *                                      GNOME Shell, not KWin
 *   ext_foreign_toplevel_list_v1       every toplevel with title and
 *                                      app id, but NO state and no output:
 *                                      it cannot say what is fullscreen,
 *                                      maximized or focused
 *   wl_output version 4                the output's name ("DP-1"), which
 *                                      is how a toplevel's output is
 *                                      matched to one of the daemon's
 *
 * Where a rule cannot work the plan carries the reason as a sentence for
 * `walpe-ctl status` and the settings window. */
#pragma once
#include <glib.h>
#include <string.h>

typedef struct {
    gboolean connected;         /* a compositor answered */
    guint32 seat;               /* wl_seat: highest version offered, 0 = none */
    guint32 output;             /* wl_output: LOWEST version among the outputs, 0 = none */
    guint n_outputs;
    guint32 idle_notifier;      /* ext_idle_notifier_v1 */
    guint32 wlr_toplevel;       /* zwlr_foreign_toplevel_manager_v1 */
    guint32 ext_toplevel_list;  /* ext_foreign_toplevel_list_v1 */
} WalpeWlCaps;

/* one wl_registry.global announcement */
static inline void walpe_wlcaps_add (WalpeWlCaps *c, const char *interface, guint32 version)
{
    c->connected = TRUE;
    if (!interface)
        return;
    if (!strcmp (interface, "wl_seat"))
        c->seat = MAX (c->seat, version);
    else if (!strcmp (interface, "wl_output")) {
        c->output = c->n_outputs ? MIN (c->output, version) : version;
        c->n_outputs++;
    } else if (!strcmp (interface, "ext_idle_notifier_v1"))
        c->idle_notifier = MAX (c->idle_notifier, version);
    else if (!strcmp (interface, "zwlr_foreign_toplevel_manager_v1"))
        c->wlr_toplevel = MAX (c->wlr_toplevel, version);
    else if (!strcmp (interface, "ext_foreign_toplevel_list_v1"))
        c->ext_toplevel_list = MAX (c->ext_toplevel_list, version);
}

typedef struct {
    /* "no input for N seconds" */
    gboolean idle;
    const char *idle_why;           /* NULL when idle */
    /* "a fullscreen window has the focus" */
    gboolean fullscreen;
    const char *fullscreen_why;
    /* per output: "windows cover it", "an application on it has the focus" */
    gboolean windows;
    const char *windows_why;
    /* the outputs of a window can be told apart by name. FALSE with
     * windows TRUE: per-output values can only be given when the daemon
     * shows on a single output (then every window is on that one) */
    gboolean output_names;
    /* covered: maximized windows count too (always with the wlr protocol;
     * kept as a field so a later protocol without that state fits in) */
    gboolean maximized;
} WalpeWlPlan;

#define WALPE_WLPLAN_NO_CONN "no connection to the Wayland compositor"
#define WALPE_WLPLAN_NO_IDLE "the compositor has no ext_idle_notifier_v1 (idle notification)"
#define WALPE_WLPLAN_NO_SEAT "the compositor announces no wl_seat"
#define WALPE_WLPLAN_NO_LIST                                                                 \
    "the compositor has no zwlr_foreign_toplevel_manager_v1 (the window list with state "     \
    "that wlroots compositors offer; GNOME Shell and KWin have none)"
#define WALPE_WLPLAN_EXT_ONLY                                                                \
    "the compositor lists windows (ext_foreign_toplevel_list_v1) but not their state: "       \
    "fullscreen, maximized and focused windows cannot be told from it; that takes "           \
    "zwlr_foreign_toplevel_manager_v1"
#define WALPE_WLPLAN_NO_FS_STATE                                                             \
    "the compositor's zwlr_foreign_toplevel_manager_v1 is version 1, which does not report " \
    "fullscreen windows (version 2 does)"

static inline void walpe_wlplan (const WalpeWlCaps *c, WalpeWlPlan *p)
{
    memset (p, 0, sizeof *p);
    if (!c->connected) {
        p->idle_why = p->fullscreen_why = p->windows_why = WALPE_WLPLAN_NO_CONN;
        return;
    }
    if (!c->idle_notifier)
        p->idle_why = WALPE_WLPLAN_NO_IDLE;
    else if (!c->seat)
        p->idle_why = WALPE_WLPLAN_NO_SEAT;
    else
        p->idle = TRUE;

    if (c->wlr_toplevel) {
        p->windows = TRUE;
        p->maximized = TRUE;
        if (c->wlr_toplevel >= 2)
            p->fullscreen = TRUE;
        else
            p->fullscreen_why = WALPE_WLPLAN_NO_FS_STATE;
    } else {
        const char *why = c->ext_toplevel_list ? WALPE_WLPLAN_EXT_ONLY : WALPE_WLPLAN_NO_LIST;
        p->fullscreen_why = p->windows_why = why;
    }
    p->output_names = c->n_outputs > 0 && c->output >= 4;
}

/* ---- windows -> values (the arithmetic of wl_toplevel.c, kept apart so a
 * test can run it on made-up window lists) */

enum {
    WALPE_TL_MAXIMIZED = 1u << 0,
    WALPE_TL_MINIMIZED = 1u << 1,
    WALPE_TL_ACTIVATED = 1u << 2,
    WALPE_TL_FULLSCREEN = 1u << 3,
};

/* a fullscreen window has the focus */
static inline gboolean walpe_tl_is_fullscreen_focus (guint state)
{
    return (state & WALPE_TL_ACTIVATED) && (state & WALPE_TL_FULLSCREEN) &&
           !(state & WALPE_TL_MINIMIZED);
}

/* the window has the focus (an application, as opposed to the desktop) */
static inline gboolean walpe_tl_is_focus (guint state)
{
    return (state & WALPE_TL_ACTIVATED) && !(state & WALPE_TL_MINIMIZED);
}

/* the window hides the wallpaper of an output it is on.
 * all = FALSE (the default): only the activated window counts. The
 * protocol does not say which workspace a window is on, and some
 * compositors keep reporting a window on its output while its workspace
 * is hidden: counting every maximized window would freeze a wallpaper the
 * user is looking at. The activated window is always on a shown workspace.
 * all = TRUE (WALPE_WL_COVERED=all): every fullscreen / maximized window.
 * see_through: its app id is in the "see-through windows" setting. */
static inline gboolean walpe_tl_covers (guint state, gboolean all, gboolean see_through)
{
    if (see_through || (state & WALPE_TL_MINIMIZED))
        return FALSE;
    if (!(state & (WALPE_TL_FULLSCREEN | WALPE_TL_MAXIMIZED)))
        return FALSE;
    return all || (state & WALPE_TL_ACTIVATED);
}

/* is `id` in a comma-separated list of app ids / window classes? Letter
 * case and blanks around the names do not matter; NULL / "" = empty list */
static inline gboolean walpe_tl_listed (const char *list, const char *id)
{
    if (!list || !id || !*id)
        return FALSE;
    gsize n = strlen (id);
    for (const char *p = list; *p;) {
        while (*p == ' ' || *p == '\t' || *p == ',')
            p++;
        const char *e = p;
        while (*e && *e != ',')
            e++;
        const char *t = e;
        while (t > p && (t[-1] == ' ' || t[-1] == '\t'))
            t--;
        if ((gsize) (t - p) == n && !g_ascii_strncasecmp (p, id, n))
            return TRUE;
        p = e;
    }
    return FALSE;
}
