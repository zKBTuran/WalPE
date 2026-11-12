/* ui_session_test.c - the GUI code that looks at the session type, as
 * pure functions (no display is opened):
 *  - src/ui/appearance.h: the dark / light preference is taken from the
 *    desktop portal only where GTK does not get it by itself - never on
 *    X11, not with GTK_THEME set, not on GTK 4.20 and later - and what a
 *    color-scheme value turns into;
 *  - src/ui/monlist.h: the text for a monitor while no engine runs
 *    (connector name, device pixels);
 *  - src/ui/suppoll.h: how often the window asks the engine for its
 *    status - never in the background while the state is pushed, and
 *    never in the background while the engine is stopped and its socket
 *    is watched;
 *  - src/ui/rescanpace.h: files that keep arriving in a new folder of the
 *    library do not hold its scan off for as long as they come. */
#include "ui/appearance.h"
#include "ui/monlist.h"
#include "ui/suppoll.h"
#include "ui/rescanpace.h"
#include "ui/winstate.h"
#include <stdio.h>
#include <string.h>

static int fails;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fails++;                                                          \
            fprintf (stderr, "FAIL %s:%d: (%s) ", __FILE__, __LINE__, #cond); \
            fprintf (stderr, __VA_ARGS__);                                    \
            fputc ('\n', stderr);                                             \
        }                                                                     \
    } while (0)

/* The window's timer for the scans after a change of the library, as main.c
 * runs it, in made-up time: one scan WALPE_RESCAN_QUIET_MS after the change,
 * one 3 s and one 10 s later; a change of the library folder itself starts
 * that anew, one of an arriving folder asks rescanpace.h first. */
typedef struct {
    gint64 due_us;      /* the timer; 0: none */
    int more;
    gint64 asked_us;
    int scans;
    gint64 first_scan_us, last_scan_us, longest_us;
} Pace;

static void pace_until (Pace *p, gint64 now)
{
    while (p->due_us && p->due_us <= now) {
        gint64 t = p->due_us;
        if (!p->scans)
            p->first_scan_us = t;
        else if (t - p->last_scan_us > p->longest_us)
            p->longest_us = t - p->last_scan_us;
        p->scans++;
        p->last_scan_us = t;
        p->due_us = p->more > 0 ? t + (p->more == 2 ? 3000 : 10000) * 1000 : 0;
        if (p->more > 0)
            p->more--;
    }
}

static gboolean pace_waits (const Pace *p)
{
    return p->due_us && p->more == 2;
}

static void pace_change (Pace *p, gint64 now)
{
    pace_until (p, now);
    if (!pace_waits (p))
        p->asked_us = now;
    p->more = 2;
    p->due_us = now + WALPE_RESCAN_QUIET_MS * 1000;
}

static void pace_arrival (Pace *p, gint64 now)
{
    pace_until (p, now);
    if (walpe_rescan_arrival_moves (pace_waits (p), p->asked_us, now))
        pace_change (p, now);
}

int main (void)
{
    /* ---- the scans while a folder arrives */
    {
        const gint64 ms = 1000;
        /* one file: as after any change, 0.6 s, 3.6 s and 13.6 s later */
        Pace p = { 0 };
        pace_arrival (&p, 0);
        pace_until (&p, 60000 * ms);
        CHECK (p.scans == 3 && p.first_scan_us == 600 * ms && p.last_scan_us == 13600 * ms,
               "one file event: %d scans, first after %d ms", p.scans, (int) (p.first_scan_us / ms));
        /* a second one while the scan waits: the wait starts again */
        p = (Pace) { 0 };
        pace_arrival (&p, 0);
        pace_arrival (&p, 300 * ms);
        pace_until (&p, 60000 * ms);
        CHECK (p.scans == 3 && p.first_scan_us == 900 * ms,
               "two file events: %d scans, first after %d ms", p.scans, (int) (p.first_scan_us / ms));
        /* a file every 150 ms for a minute: the library is still scanned,
         * first after 3.6 s at most and then never less often than that */
        p = (Pace) { 0 };
        for (gint64 t = 0; t <= 60000 * ms; t += 150 * ms)
            pace_arrival (&p, t);
        pace_until (&p, 60000 * ms);
        CHECK (p.scans >= 15, "files that keep arriving: %d scans in a minute", p.scans);
        CHECK (p.scans && p.first_scan_us <= (WALPE_RESCAN_ARRIVAL_MAX_MS + WALPE_RESCAN_QUIET_MS) * ms,
               "files that keep arriving: first scan after %d ms", (int) (p.first_scan_us / ms));
        CHECK (p.longest_us <= 4000 * ms,
               "files that keep arriving: %d ms without a scan", (int) (p.longest_us / ms));
        /* and not at every file: a scan reads the whole library */
        CHECK (p.scans <= 60 * 1000 / WALPE_RESCAN_ARRIVAL_MAX_MS + 1,
               "files that keep arriving: %d scans in a minute", p.scans);
        /* changes of the library folder itself wait for a pause, as they did */
        p = (Pace) { 0 };
        for (gint64 t = 0; t <= 10000 * ms; t += 150 * ms)
            pace_change (&p, t);
        pace_until (&p, 10000 * ms);
        CHECK (p.scans == 0, "changes of the library folder: %d scans before the pause", p.scans);
        pace_until (&p, 60000 * ms);
        CHECK (p.scans == 3 && p.first_scan_us == 10500 * ms,
               "changes of the library folder: %d scans, first at %d ms", p.scans, (int) (p.first_scan_us / ms));
    }

    /* ---- when the portal is asked */
    CHECK (!walpe_appearance_wanted (TRUE, NULL, 4, 14), "X11: never (the session stays as it was)");
    CHECK (!walpe_appearance_wanted (TRUE, NULL, 4, 6), "X11, old GTK: never");
    CHECK (walpe_appearance_wanted (FALSE, NULL, 4, 14), "Wayland, GTK 4.14: yes");
    CHECK (walpe_appearance_wanted (FALSE, "", 4, 18), "Wayland, empty GTK_THEME: yes");
    CHECK (!walpe_appearance_wanted (FALSE, "Adwaita:dark", 4, 14), "an explicit GTK_THEME wins");
    CHECK (!walpe_appearance_wanted (FALSE, NULL, 4, 20), "GTK 4.20 follows the preference itself");
    CHECK (!walpe_appearance_wanted (FALSE, NULL, 5, 0), "and so does anything later");

    /* ---- what a value means */
    CHECK (walpe_appearance_dark (1, FALSE) && walpe_appearance_dark (1, TRUE), "1 = dark");
    CHECK (!walpe_appearance_dark (2, TRUE) && !walpe_appearance_dark (2, FALSE), "2 = light");
    CHECK (walpe_appearance_dark (0, TRUE) && !walpe_appearance_dark (0, FALSE),
           "0 = no preference: what GTK's own settings said");
    CHECK (walpe_appearance_dark (7, TRUE) && !walpe_appearance_dark (7, FALSE), "a value from the future: as 0");

    /* ---- monitor text */
    char *t = walpe_ui_monitor_text ("DP-1", 2560, 1440, 1.0);
    CHECK (!strcmp (t, "DP-1 2560×1440"), "plain: %s", t);
    g_free (t);
    t = walpe_ui_monitor_text ("eDP-1", 1280, 800, 2.0);
    CHECK (!strcmp (t, "eDP-1 2560×1600"), "scale 2: device pixels: %s", t);
    g_free (t);
    t = walpe_ui_monitor_text ("HDMI-A-1", 1536, 864, 1.25);
    CHECK (!strcmp (t, "HDMI-A-1 1920×1080"), "fractional scale: %s", t);
    g_free (t);
    t = walpe_ui_monitor_text (NULL, 1920, 1080, 0.0);
    CHECK (!strcmp (t, "Monitor 1920×1080"), "no connector name, no scale: %s", t);
    g_free (t);
    t = walpe_ui_monitor_text ("", 800, 600, 1.0);
    CHECK (!strcmp (t, "Monitor 800×600"), "empty connector name: %s", t);
    g_free (t);
    CHECK (walpe_ui_monitors_line (NULL) == NULL, "no display: no line");

    /* ---- on screen or not (winstate.h): the one rule the previews, the
     * quality panel's timer and the picture cache follow. Without the focus
     * - a tiled window next to the one in use - is on screen. */
    CHECK (walpe_toplevel_state_hidden (GDK_TOPLEVEL_STATE_MINIMIZED), "an icon / another workspace");
    CHECK (walpe_toplevel_state_hidden (GDK_TOPLEVEL_STATE_MINIMIZED | GDK_TOPLEVEL_STATE_FOCUSED),
           "hidden wins over the focus");
#if GTK_CHECK_VERSION (4, 12, 0)
    CHECK (walpe_toplevel_state_hidden (GDK_TOPLEVEL_STATE_SUSPENDED), "suspended by the compositor");
#endif
    CHECK (!walpe_toplevel_state_hidden (0), "on screen without the focus: not hidden");
    CHECK (!walpe_toplevel_state_hidden (GDK_TOPLEVEL_STATE_TILED | GDK_TOPLEVEL_STATE_MAXIMIZED),
           "tiled, maximised: not hidden");
    CHECK (!walpe_toplevel_state_hidden (GDK_TOPLEVEL_STATE_FOCUSED | GDK_TOPLEVEL_STATE_FULLSCREEN),
           "focused: not hidden");
    CHECK (!walpe_surface_hidden (NULL) && walpe_widget_toplevel_surface (NULL) == NULL,
           "no window yet: not hidden");
    /* X11 window managers that hide a window without calling it minimised */
    CHECK (walpe_wm_state_gone (0), "WM_STATE withdrawn (i3: a workspace that is not shown)");
    CHECK (walpe_wm_state_gone (3), "WM_STATE iconic");
    CHECK (!walpe_wm_state_gone (1), "WM_STATE normal: on screen");
    /* the window manager went while the window was on a workspace not
     * shown (the server puts it under the root and maps it), or it maps
     * the window without writing WM_STATE: the newer word counts */
    CHECK (walpe_wm_marks_mapped (WALPE_WM_WITHDRAWN) == 0, "mapped after WM_STATE said withdrawn: on screen");
    CHECK (walpe_wm_marks_mapped (WALPE_WM_UNMAPPED | WALPE_WM_WITHDRAWN) == 0, "mapped: both marks go");
    CHECK (walpe_wm_marks_mapped (WALPE_WM_OFFSCREEN | WALPE_WM_WITHDRAWN) == WALPE_WM_OFFSCREEN,
           "mapped outside the screen: still not on screen");
    CHECK (walpe_wm_marks_unframed (WALPE_WM_WITHDRAWN) == 0, "back under the root: WM_STATE's frame is gone");
    CHECK (walpe_wm_marks_unframed (WALPE_WM_UNMAPPED | WALPE_WM_WITHDRAWN) == WALPE_WM_UNMAPPED,
           "back under the root and unmapped: not on screen");
    CHECK (walpe_wm_offscreen (-3840, 0, 1920, 1080, 1920, 1080), "parked left of the screen (dwm)");
    CHECK (walpe_wm_offscreen (0, 1080, 800, 600, 1920, 1080), "below the screen");
    CHECK (walpe_wm_offscreen (1920, 0, 800, 600, 1920, 1080), "right of the screen");
    CHECK (walpe_wm_offscreen (0, -600, 800, 600, 1920, 1080), "above the screen");
    CHECK (!walpe_wm_offscreen (0, 0, 960, 1080, 1920, 1080), "a tile of the screen");
    CHECK (!walpe_wm_offscreen (-799, 0, 800, 600, 1920, 1080), "one column still on the screen");
    CHECK (!walpe_wm_offscreen (1919, 1079, 800, 600, 1920, 1080), "one pixel still on the screen");
    CHECK (!walpe_wm_offscreen (2, 20, 956, 1058, 1920, 1080), "a position inside a frame");

    /* ---- the status poll */
    {
        WalpeSupPoll p = { .live = TRUE, .up = TRUE, .watching = TRUE };
        CHECK (walpe_sup_poll_secs (&p) == 0, "pushed state, window in the background: no timer");
        p.active = TRUE;
        CHECK (walpe_sup_poll_secs (&p) == 10, "pushed state, active window: every 10 s");
        /* the connection stands but the engine did not answer: it will
         * push nothing when it answers again, so it is asked */
        p = (WalpeSupPoll) { .live = TRUE, .watching = TRUE };
        CHECK (walpe_sup_poll_secs (&p) == 10, "pushed state, engine not answering, background: every 10 s");
        p.active = TRUE;
        CHECK (walpe_sup_poll_secs (&p) == 10, "the same, active");
        p = (WalpeSupPoll) { .unsupported = TRUE, .up = TRUE, .watching = TRUE };
        CHECK (walpe_sup_poll_secs (&p) == 0, "an engine that cannot push, background: no timer");
        p.active = TRUE;
        CHECK (walpe_sup_poll_secs (&p) == 2, "an engine that cannot push, active: every 2 s");
        /* starting, not answering, not seen yet: asked until it is known */
        p = (WalpeSupPoll) { .watching = TRUE };
        CHECK (walpe_sup_poll_secs (&p) == 2, "starting or unknown, background: every 2 s");
        p.active = TRUE;
        CHECK (walpe_sup_poll_secs (&p) == 2, "starting or unknown, active: every 2 s");
        p = (WalpeSupPoll) { .unsupported = TRUE, .watching = TRUE };
        CHECK (walpe_sup_poll_secs (&p) == 2, "an older engine that went away and is being restarted");
        /* stopped, nothing here starts it: the socket's watch reports a new one */
        p = (WalpeSupPoll) { .stopped = TRUE, .watching = TRUE };
        CHECK (walpe_sup_poll_secs (&p) == 0, "stopped, background: no timer");
        p.active = TRUE;
        CHECK (walpe_sup_poll_secs (&p) == 10, "stopped, active window: every 10 s");
        p = (WalpeSupPoll) { .stopped = TRUE };
        CHECK (walpe_sup_poll_secs (&p) == 2, "stopped but nothing watches for its socket: every 2 s");
        p.active = TRUE;
        CHECK (walpe_sup_poll_secs (&p) == 2, "the same, active");
    }

    if (fails) {
        fprintf (stderr, "ui-session: %d check(s) failed\n", fails);
        return 1;
    }
    printf ("ui-session: ok\n");
    return 0;
}
