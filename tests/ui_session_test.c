/* ui_session_test.c - the GUI code that looks at the session type, as
 * pure functions (no display is opened):
 *  - src/ui/appearance.h: the dark / light preference is taken from the
 *    desktop portal only where GTK does not get it by itself - never on
 *    X11, not with GTK_THEME set, not on GTK 4.20 and later - and what a
 *    color-scheme value turns into;
 *  - src/ui/monlist.h: the text for a monitor while no engine runs
 *    (connector name, device pixels). */
#include "ui/appearance.h"
#include "ui/monlist.h"
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

int main (void)
{
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

    if (fails) {
        fprintf (stderr, "ui-session: %d check(s) failed\n", fails);
        return 1;
    }
    printf ("ui-session: ok\n");
    return 0;
}
