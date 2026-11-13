/* webbg_test.c - the background of a page's view by WebKitGTK version
 * (src/engine/webbg.h): see-through as ever, opaque black with the
 * releases that wipe what a page did not repaint from the frame of a
 * see-through view (2.53.1 - 2.54.2) - but not where frames are WebKit
 * snapshots (WALPE_WEB_SNAPSHOT=1) -, and WALPE_WEB_OPAQUE either way.
 * Pure: no display, no GTK, no WebKit. What the picture is with each is
 * scripts/webcheck.py's (`live`, `pausedopen`). */
#include "engine/webbg.h"
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
    static const struct { guint major, minor, micro; gboolean opaque; const char *what; } v[] = {
        { 2, 44, 0, FALSE, "2.44.0 (Ubuntu 24.04 at release)" },
        { 2, 52, 0, FALSE, "2.52.0" },
        { 2, 52, 6, FALSE, "2.52.6 (Ubuntu 24.04 today): paints without the clear" },
        { 2, 53, 1, TRUE, "2.53.1: the first with the clear" },
        { 2, 53, 91, TRUE, "2.53.91" },
        { 2, 54, 0, TRUE, "2.54.0" },
        { 2, 54, 2, TRUE, "2.54.2 (Arch today)" },
        { 2, 54, 9, TRUE, "a later 2.54: opaque whether it is fixed there or not" },
        { 2, 55, 1, FALSE, "2.55: WebKit's main branch limits the clear to what is repainted" },
        { 2, 56, 0, FALSE, "2.56.0" },
        { 3, 54, 0, FALSE, "another major version" },
    };
    for (gsize i = 0; i < G_N_ELEMENTS (v); i++) {
        CHECK (web_bg_wiped_by (v[i].major, v[i].minor, v[i].micro) == v[i].opaque, "%s", v[i].what);
        CHECK (web_bg_opaque (NULL, FALSE, v[i].major, v[i].minor, v[i].micro) == v[i].opaque, "no setting: %s",
               v[i].what);
        CHECK (web_bg_opaque ("", FALSE, v[i].major, v[i].minor, v[i].micro) == v[i].opaque, "empty setting: %s",
               v[i].what);
        CHECK (web_bg_opaque ("auto", FALSE, v[i].major, v[i].minor, v[i].micro) == v[i].opaque,
               "another word: %s", v[i].what);
        /* frames are WebKit snapshots: nothing is wiped from those, and the
         * opaque view makes them dear - see-through with every version */
        CHECK (!web_bg_opaque (NULL, TRUE, v[i].major, v[i].minor, v[i].micro), "snapshots, no setting: %s",
               v[i].what);
        CHECK (!web_bg_opaque ("auto", TRUE, v[i].major, v[i].minor, v[i].micro), "snapshots, another word: %s",
               v[i].what);
        /* the setting wins, however frames are taken */
        for (int snap = 0; snap < 2; snap++) {
            CHECK (web_bg_opaque ("1", snap, v[i].major, v[i].minor, v[i].micro), "WALPE_WEB_OPAQUE=1: %s (%d)",
                   v[i].what, snap);
            CHECK (web_bg_opaque ("on", snap, v[i].major, v[i].minor, v[i].micro), "=on: %s (%d)", v[i].what, snap);
            CHECK (!web_bg_opaque ("0", snap, v[i].major, v[i].minor, v[i].micro), "WALPE_WEB_OPAQUE=0: %s (%d)",
                   v[i].what, snap);
            CHECK (!web_bg_opaque ("OFF", snap, v[i].major, v[i].minor, v[i].micro), "=OFF: %s (%d)", v[i].what,
                   snap);
        }
    }
    if (fails) {
        fprintf (stderr, "webbg: %d check(s) failed\n", fails);
        return 1;
    }
    printf ("webbg: ok\n");
    return 0;
}
