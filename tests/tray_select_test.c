/* tray_select_test.c - which tray protocol walpe-tray uses in which
 * session (src/tray/traysel.h). No display, no bus: the function is pure. */
#include "tray/traysel.h"
#include <stdio.h>

static int fails;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fails++;                                                          \
            fprintf (stderr, "FAIL %s:%d: ", __FILE__, __LINE__);             \
            fprintf (stderr, __VA_ARGS__);                                    \
            fputc ('\n', stderr);                                             \
        }                                                                     \
    } while (0)

static TrayPick pick (const char *want, gboolean bus, gboolean watcher, gboolean host,
                      gboolean x11, gboolean xdisplay, gboolean built, const char **note)
{
    TrayEnv e = { want, bus, watcher, host, x11, xdisplay, built };
    return tray_pick (&e, note);
}

int main (void)
{
    const char *n;

    /* ---- X11 session, build with XEmbed: the rule from before Wayland
     * support, in every combination of bus / watcher / host */
    for (int bus = 0; bus < 2; bus++)
        for (int w = 0; w < 2; w++)
            for (int h = 0; h < 2; h++) {
                TrayPick want = bus && w && h ? TRAY_PICK_SNI : TRAY_PICK_XEMBED;
                TrayPick got = pick ("auto", bus, w, h, TRUE, TRUE, TRUE, &n);
                CHECK (got == want, "x11 auto bus=%d watcher=%d host=%d: got %d", bus, w, h, got);
                CHECK (n == NULL, "x11 auto has nothing to say (bus=%d watcher=%d host=%d): %s",
                       bus, w, h, n ? n : "");
                /* NULL, "" and a name nobody knows are auto */
                CHECK (pick (NULL, bus, w, h, TRUE, TRUE, TRUE, NULL) == want, "NULL = auto");
                CHECK (pick ("", bus, w, h, TRUE, TRUE, TRUE, NULL) == want, "\"\" = auto");
                CHECK (pick ("systray", bus, w, h, TRUE, TRUE, TRUE, NULL) == want, "unknown = auto");
            }

    /* ---- Wayland session: never XEmbed by itself */
    CHECK (pick ("auto", TRUE, TRUE, TRUE, FALSE, TRUE, TRUE, &n) == TRAY_PICK_SNI && !n,
           "wayland, watcher + host: sni, nothing to say");
    CHECK (pick ("auto", TRUE, TRUE, FALSE, FALSE, TRUE, TRUE, &n) == TRAY_PICK_SNI && n,
           "wayland, watcher without host: sni and a note");
    CHECK (pick ("auto", TRUE, FALSE, FALSE, FALSE, TRUE, TRUE, &n) == TRAY_PICK_SNI && n,
           "wayland, no watcher: sni (registers later) and a note");
    CHECK (n && strstr (n, "StatusNotifierWatcher"), "the note names what is missing");
    CHECK (pick ("auto", TRUE, FALSE, FALSE, FALSE, FALSE, TRUE, NULL) == TRAY_PICK_SNI,
           "wayland without Xwayland: sni");
    CHECK (pick ("auto", FALSE, FALSE, FALSE, FALSE, TRUE, TRUE, &n) == TRAY_PICK_NONE && n,
           "wayland without a session bus: no icon, and it says so (Xwayland does not help)");

    /* ---- asked for by name */
    CHECK (pick ("sni", TRUE, FALSE, FALSE, TRUE, TRUE, TRUE, &n) == TRAY_PICK_SNI && !n,
           "sni by name on X11 without a watcher");
    CHECK (pick ("sni", FALSE, FALSE, FALSE, TRUE, TRUE, TRUE, &n) == TRAY_PICK_NONE && n,
           "sni by name without a bus");
    CHECK (pick ("xembed", TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, &n) == TRAY_PICK_XEMBED && !n,
           "xembed by name wins over a present host");
    CHECK (pick ("xembed", TRUE, TRUE, TRUE, FALSE, TRUE, TRUE, &n) == TRAY_PICK_XEMBED && !n,
           "xembed by name in a Wayland session with Xwayland");
    CHECK (pick ("xembed", TRUE, TRUE, TRUE, FALSE, FALSE, TRUE, &n) == TRAY_PICK_NONE && n &&
           strstr (n, "DISPLAY"), "xembed by name without an X display");

    /* ---- a build without the X11 tray */
    CHECK (pick ("xembed", TRUE, TRUE, TRUE, TRUE, TRUE, FALSE, &n) == TRAY_PICK_NONE && n,
           "xembed by name in a build without it");
    CHECK (pick ("auto", TRUE, FALSE, FALSE, TRUE, TRUE, FALSE, &n) == TRAY_PICK_SNI && n,
           "X11 session, no XEmbed in the build: sni, waiting for a watcher");
    CHECK (pick ("auto", FALSE, FALSE, FALSE, TRUE, TRUE, FALSE, &n) == TRAY_PICK_NONE && n,
           "X11 session, no XEmbed in the build, no bus: nothing");

    if (fails) {
        fprintf (stderr, "tray-select: %d check(s) failed\n", fails);
        return 1;
    }
    printf ("tray-select: ok\n");
    return 0;
}
