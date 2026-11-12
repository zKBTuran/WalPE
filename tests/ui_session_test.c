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
 *    library do not hold its scan off for as long as they come;
 *  - src/ui/monline.h: the texts that say what each monitor shows, and
 *    that a pause which begins or ends writes them again;
 *  - src/ui/wlappid.h: which windows are given the application's id on
 *    Wayland - the toplevels GTK would name after the program - and none
 *    on X11;
 *  - src/ui/lastwords.h: the sentence of an engine that ended at once is
 *    found in what it wrote - its last line that is no informational
 *    one, without what GLib puts before it. */
#include "ui/appearance.h"
#include "ui/monlist.h"
#include "ui/suppoll.h"
#include "ui/rescanpace.h"
#include "ui/monline.h"
#include "ui/winstate.h"
#include "ui/wlappid.h"
#include "ui/lastwords.h"
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
    /* ---- what each monitor shows, as text */
    {
        WalpeMonLine m[2] = { { .index = 0, .name = "HDMI-0", .w = 1920, .h = 1080, .title = "Rain" },
                              { .index = 1, .name = "", .w = 1280, .h = 1024, .title = "Dunes" } };
        char *now = walpe_mon_line_now (m, 2), *tip = walpe_mon_line_tip (m, 2);
        char *marks = walpe_mon_line_marks (m, 2), *c = walpe_mon_line_choice (&m[1]);
        CHECK (!strcmp (now, "Showing  1 · HDMI-0: Rain     2 · Monitor: Dunes"), "the line: %s", now);
        CHECK (!strcmp (tip, "Currently showing:\n1 · HDMI-0 · 1920×1080: Rain\n2 · Monitor · 1280×1024: Dunes"),
               "the tool tip: %s", tip);
        CHECK (!strcmp (c, "2 · Monitor · 1280×1024"), "the choice: %s", c);
        g_free (c);
        /* one monitor pauses: both texts say so, and what is compared to
         * write them again is not what it was */
        m[1].paused = TRUE;
        char *now2 = walpe_mon_line_now (m, 2), *tip2 = walpe_mon_line_tip (m, 2);
        char *marks2 = walpe_mon_line_marks (m, 2);
        CHECK (!strcmp (now2, "Showing  1 · HDMI-0: Rain     2 · Monitor: Dunes (paused)"), "the line: %s", now2);
        CHECK (g_str_has_suffix (tip2, "Dunes (paused)"), "the tool tip: %s", tip2);
        CHECK (strcmp (marks, marks2) != 0, "a pause is a reason to write the texts again");
        /* every combination: the same texts exactly when the marks are the same */
        for (int a = 0; a < 4; a++)
            for (int b = 0; b < 4; b++) {
                WalpeMonLine x[2] = { m[0], m[1] }, y[2] = { m[0], m[1] };
                x[0].paused = a & 1; x[1].paused = a >> 1;
                y[0].paused = b & 1; y[1].paused = b >> 1;
                char *nx = walpe_mon_line_now (x, 2), *ny = walpe_mon_line_now (y, 2);
                char *tx = walpe_mon_line_tip (x, 2), *ty = walpe_mon_line_tip (y, 2);
                char *mx = walpe_mon_line_marks (x, 2), *my = walpe_mon_line_marks (y, 2);
                CHECK ((!strcmp (nx, ny) && !strcmp (tx, ty)) == !strcmp (mx, my), "pauses %d and %d", a, b);
                g_free (nx); g_free (ny); g_free (tx); g_free (ty); g_free (mx); g_free (my);
            }
        /* one monitor: no line, the tool tip stays */
        char *one = walpe_mon_line_now (m, 1);
        CHECK (!*one, "one monitor: %s", one);
        g_free (one);
        g_free (now); g_free (tip); g_free (marks); g_free (now2); g_free (tip2); g_free (marks2);
    }

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

    /* ---- which windows are given the application's id */
    CHECK (walpe_wl_app_id_wanted (TRUE, FALSE), "Wayland, a dialog of the GUI: yes");
    CHECK (!walpe_wl_app_id_wanted (TRUE, TRUE), "Wayland, the application's own window: GTK did it");
    CHECK (!walpe_wl_app_id_wanted (FALSE, FALSE) && !walpe_wl_app_id_wanted (FALSE, TRUE),
           "X11 (no Wayland toplevel): never");

    /* ---- what the engine said last */
    {
        char *w = walpe_last_words (
            "** Message: 20:05:11.299: display backend: wayland (auto)\n"
            "walpe-daemon: no display to draw on: the Wayland compositor has no layer shell\n");
        CHECK (w && !strcmp (w, "no display to draw on: the Wayland compositor has no layer shell"),
               "the engine's own sentence, without its name: %s", w ? w : "(null)");
        g_free (w);
        w = walpe_last_words ("** Message: 20:14:17.547: display backend: x11, asked for (--backend)\n"
                              "cannot open X display\n");
        CHECK (w && !strcmp (w, "cannot open X display"), "a plain line: %s", w ? w : "(null)");
        g_free (w);
        w = walpe_last_words ("\n** (process:18274): WARNING **: 20:10:41.844: wallpaper load failed: x\n"
                              "** Message: 20:10:41.845: pause signals: logind is not available\n"
                              "walpe-daemon-Message: 20:10:41.846: more of the same\n\n");
        CHECK (w && !strcmp (w, "wallpaper load failed: x"),
               "a warning before informational lines, without GLib's prefix: %s", w ? w : "(null)");
        g_free (w);
        w = walpe_last_words ("(walpe-daemon:77): Gtk-CRITICAL **: 12:00:00.000: gtk_x: assertion failed\n");
        CHECK (w && !strcmp (w, "gtk_x: assertion failed"), "a critical with a domain: %s", w ? w : "(null)");
        g_free (w);
        w = walpe_last_words ("** Message: 20:05:11.299: up\n** INFO: 20:05:11.300: fine\n"
                              "(walpe-daemon:1): walpe-DEBUG: 20:05:11.301: detail\n");
        CHECK (!w, "only informational lines: nothing to quote");
        g_free (w);
        CHECK (!walpe_last_words ("") && !walpe_last_words (NULL) && !walpe_last_words ("\n \n"),
               "nothing written: nothing to quote");
        w = walpe_last_words ("the Message: of this line is no GLib message\n");
        CHECK (w && g_str_has_prefix (w, "the Message: of"), "the word Message alone does not hide a line");
        g_free (w);
        char *lng = g_strnfill (1000, 'x');
        w = walpe_last_words (lng);
        CHECK (w && g_utf8_strlen (w, -1) == WALPE_LAST_WORDS_MAX + 1 && g_str_has_suffix (w, "…"),
               "a long line is cut: %ld characters", w ? g_utf8_strlen (w, -1) : -1L);
        g_free (w);
        g_free (lng);
        w = walpe_last_words ("bad bytes \xff\xfe here\n");
        CHECK (w && g_utf8_validate (w, -1, NULL), "bytes that are no UTF-8 do not get through");
        g_free (w);
    }

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
