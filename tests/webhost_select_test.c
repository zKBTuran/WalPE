/* webhost_select_test.c - which page host a web wallpaper gets
 * (src/engine/webhostsel.h): X11 sessions stay on the off-screen X
 * window, a Wayland backend uses it through Xwayland as well and takes
 * the private display only where there is no X display (elsewhere it is
 * opt-in by name), forced names, builds that hold only one of the two,
 * and what follows a host that did not come up: on a Wayland backend
 * the other one, with the reason (DISPLAY set and no X server behind it
 * ends on the private display), by name or on an X11 backend nothing
 * else. Pure: no display, no GTK. */
#include "engine/webhostsel.h"
#include <stdio.h>

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
    const char *why;

    /* ---- X11 backend: the X window, whatever else is around */
    {
        WebHostEnv e = { .backend = 1, .x_display = TRUE, .built_x11 = TRUE, .built_private = TRUE };
        CHECK (web_host_choose (&e, &why) == WEB_HOST_X11 && !why, "X11 backend: x11, nothing to say");
        e.wayland_display = TRUE;   /* the X11 backend inside a Wayland session (Xwayland) */
        CHECK (web_host_choose (&e, &why) == WEB_HOST_X11 && !why, "X11 backend under Wayland: still x11");
        e.setting = "";
        CHECK (web_host_choose (&e, NULL) == WEB_HOST_X11, "empty setting = auto");
        e.setting = "nonsense";
        CHECK (web_host_choose (&e, NULL) == WEB_HOST_X11, "unknown setting = auto");
        e.setting = "AUTO";
        CHECK (web_host_choose (&e, NULL) == WEB_HOST_X11, "auto in capitals");
    }

    /* ---- Wayland backend */
    {
        WebHostEnv e = { .backend = 2, .x_display = TRUE, .wayland_display = TRUE, .built_x11 = TRUE,
                         .built_private = TRUE };
        CHECK (web_host_choose (&e, &why) == WEB_HOST_X11 && !why,
               "Wayland backend with Xwayland: x11, nothing to say (the private host is opt-in there)");
        e.x_display = FALSE;
        CHECK (web_host_choose (&e, &why) == WEB_HOST_PRIVATE && !why, "Wayland without Xwayland: private");
        /* the private display failed its start-up test */
        e.private_failed = TRUE;
        CHECK (web_host_choose (&e, &why) == WEB_HOST_NONE && why && strstr (why, "Xwayland"),
               "private failed, no Xwayland: nothing, and it says what would help");
        e.x_display = TRUE;
        CHECK (web_host_choose (&e, &why) == WEB_HOST_X11 && why && strstr (why, "Xwayland"),
               "private failed, Xwayland there: x11 with a note");
        /* a build without the private host */
        e.private_failed = FALSE;
        e.built_private = FALSE;
        CHECK (web_host_choose (&e, &why) == WEB_HOST_X11 && !why,
               "no private host in the build: Xwayland, which is the first choice anyway");
        e.x_display = FALSE;
        CHECK (web_host_choose (&e, &why) == WEB_HOST_NONE && why, "neither: nothing");
        /* a build without X */
        e.built_private = TRUE;
        e.built_x11 = FALSE;
        e.x_display = TRUE;
        CHECK (web_host_choose (&e, &why) == WEB_HOST_PRIVATE && !why,
               "DISPLAY set but no X11 page host in the build: private");
        e.private_failed = TRUE;
        CHECK (web_host_choose (&e, &why) == WEB_HOST_NONE && why, "private failed in a build without X: nothing");
    }

    /* ---- the X11 page host did not come up: DISPLAY names no running X
     * server, or WebKit failed its start-up test there */
    {
        WebHostEnv e = { .backend = 2, .x_display = TRUE, .wayland_display = TRUE, .built_x11 = TRUE,
                         .built_private = TRUE, .x11_failed = TRUE };
        CHECK (web_host_choose (&e, &why) == WEB_HOST_PRIVATE && why && strstr (why, "private page host instead"),
               "Wayland backend, x11 failed: private, and it says why");
        e.private_failed = TRUE;
        CHECK (web_host_choose (&e, &why) == WEB_HOST_NONE && why && strstr (why, "both"),
               "both failed: nothing, and it names both");
        e.private_failed = FALSE;
        e.built_private = FALSE;
        CHECK (web_host_choose (&e, &why) == WEB_HOST_NONE && why && strstr (why, "no private page host"),
               "x11 failed in a build without the private host: nothing");
        /* asked for by name: the name is kept */
        e.built_private = TRUE;
        e.setting = "x11";
        CHECK (web_host_choose (&e, &why) == WEB_HOST_X11 && !why, "x11 by name stays x11 after a failure");
        e.setting = "private";
        CHECK (web_host_choose (&e, &why) == WEB_HOST_PRIVATE && !why, "private by name: nothing to say");
        /* an X11 backend draws on that X display itself: nothing changes there */
        e.setting = NULL;
        e.backend = 1;
        CHECK (web_host_choose (&e, &why) == WEB_HOST_X11 && !why,
               "X11 backend: x11 whatever its start-up test said (the private host is never started there)");
        e.wayland_display = FALSE;
        CHECK (web_host_choose (&e, &why) == WEB_HOST_X11 && !why, "X11 session: the same");
        /* the start-up test's process on a Wayland backend, told so by WALPE_BACKEND */
        e.backend = 0;
        e.backend_env = "wayland";
        CHECK (web_host_choose (&e, &why) == WEB_HOST_PRIVATE && why,
               "the test process on a Wayland backend follows the same rule");
        /* without the failure the same environment is on x11 */
        e.x11_failed = FALSE;
        CHECK (web_host_choose (&e, &why) == WEB_HOST_X11 && !why, "no failure: x11 as before");
    }

    /* ---- asked for by name */
    {
        WebHostEnv e = { .backend = 1, .x_display = TRUE, .built_x11 = TRUE, .built_private = TRUE,
                         .setting = "private" };
        CHECK (web_host_choose (&e, &why) == WEB_HOST_PRIVATE && !why, "private by name on an X11 backend");
        e.private_failed = TRUE;
        CHECK (web_host_choose (&e, NULL) == WEB_HOST_PRIVATE, "by name: no silent fallback after a failure");
        e.built_private = FALSE;
        CHECK (web_host_choose (&e, &why) == WEB_HOST_NONE && why, "private by name, not built");
        e.setting = "x11";
        e.backend = 2;
        e.built_private = TRUE;
        e.private_failed = FALSE;
        CHECK (web_host_choose (&e, &why) == WEB_HOST_X11 && !why, "x11 by name on a Wayland backend");
        e.setting = "Xwayland";
        CHECK (web_host_choose (&e, NULL) == WEB_HOST_X11, "xwayland is a name for x11");
        e.x_display = FALSE;
        CHECK (web_host_choose (&e, &why) == WEB_HOST_NONE && why && strstr (why, "DISPLAY"),
               "x11 by name without DISPLAY");
        e.x_display = TRUE;
        e.built_x11 = FALSE;
        CHECK (web_host_choose (&e, &why) == WEB_HOST_NONE && why, "x11 by name, not built");
    }

    /* ---- the start-up test's process: no daemon to ask */
    {
        WebHostEnv e = { .backend = 0, .x_display = TRUE, .built_x11 = TRUE, .built_private = TRUE };
        CHECK (web_host_choose (&e, NULL) == WEB_HOST_X11, "unknown backend, X only: x11");
        e.wayland_display = TRUE;
        CHECK (web_host_choose (&e, NULL) == WEB_HOST_X11, "unknown backend, both displays set: x11");
        e.x_display = FALSE;
        CHECK (web_host_choose (&e, NULL) == WEB_HOST_PRIVATE, "unknown backend, only WAYLAND_DISPLAY set: private");
        e.x_display = TRUE;
        e.backend_env = "x11";
        CHECK (web_host_choose (&e, NULL) == WEB_HOST_X11, "WALPE_BACKEND=x11 says the daemon is on X11");
        /* Which backend it is shows in what is said after a failed
         * private display: on a Wayland backend x11 comes with the note
         * about it, an X11 backend has nothing to say */
        e.private_failed = TRUE;
        e.backend_env = "wayland";
        e.wayland_display = FALSE;
        CHECK (web_host_choose (&e, &why) == WEB_HOST_X11 && why, "WALPE_BACKEND=wayland says Wayland");
        e.backend_env = "auto";
        CHECK (web_host_choose (&e, &why) == WEB_HOST_X11 && !why, "WALPE_BACKEND=auto: by the displays");
        /* a known backend beats the hints */
        e.backend = 1;
        e.backend_env = "wayland";
        e.wayland_display = TRUE;
        CHECK (web_host_choose (&e, &why) == WEB_HOST_X11 && !why, "the daemon's answer beats the environment");
    }

    /* ---- no display of any kind, X11 backend not possible */
    {
        WebHostEnv e = { .backend = 0, .built_x11 = TRUE, .built_private = TRUE };
        CHECK (web_host_choose (&e, NULL) == WEB_HOST_PRIVATE, "no display at all: the private host needs none");
        e.built_private = FALSE;
        CHECK (web_host_choose (&e, &why) == WEB_HOST_NONE && why && strstr (why, "DISPLAY"), "nothing to host on");
    }

    CHECK (!strcmp (web_host_name (WEB_HOST_X11), "x11") && !strcmp (web_host_name (WEB_HOST_PRIVATE), "private"),
           "names as status shows them");

    if (fails) {
        fprintf (stderr, "webhost-select: %d check(s) failed\n", fails);
        return 1;
    }
    printf ("webhost-select: ok\n");
    return 0;
}
