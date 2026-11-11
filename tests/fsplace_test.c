/* fsplace_test.c - which monitors a focused fullscreen window is on
 * (platform/fsplace.h): the rule "Pause for fullscreen apps" uses on X11
 * to pause the monitor of the window and no other. Made-up layouts and
 * window rectangles in root coordinates: no display, no daemon. */
#include <glib.h>
#include <string.h>
#include "platform/fsplace.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; g_printerr ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define MAXMON 8

/* place a window on a layout; the answer as a text of 0 / 1 per monitor,
 * "*" in front when the window counts for every monitor */
static const char *where (gboolean focus_fs, const WalpeFsRect *win, const WalpeFsRect *mons, int n)
{
    static char buf[MAXMON + 2];
    gboolean on[MAXMON + 1], everywhere = FALSE;
    memset (on, 0x55, sizeof on);       /* (stale values must not survive) */
    walpe_fs_decide (focus_fs, win, mons, n, on, &everywhere);
    char *p = buf;
    if (everywhere)
        *p++ = '*';
    for (int i = 0; i < n; i++)
        *p++ = on[i] ? '1' : '0';
    *p = 0;
    return buf;
}

#define W(x, y, w, h) (&(WalpeFsRect) { (x), (y), (w), (h) })
#define IS(got, want) (!strcmp ((got), (want)))

/* two monitors side by side, as HDMI-0 and DP-2 of the report this rule
 * was written for */
static const WalpeFsRect two[] = { { 0, 0, 1920, 1080 }, { 1920, 0, 2560, 1440 } };

static void test_overlap (void)
{
    CHECK (walpe_fs_overlap (W (0, 0, 10, 10), W (5, 5, 10, 10)) == 25);
    CHECK (walpe_fs_overlap (W (0, 0, 10, 10), W (10, 0, 10, 10)) == 0);    /* touching edges */
    CHECK (walpe_fs_overlap (W (0, 0, 10, 10), W (-5, -5, 5, 5)) == 0);
    CHECK (walpe_fs_overlap (W (2, 2, 3, 3), W (0, 0, 100, 100)) == 9);     /* inside */
    CHECK (walpe_fs_overlap (W (0, 0, 0, 10), W (0, 0, 10, 10)) == 0);      /* empty */
    CHECK (walpe_fs_overlap (W (0, 0, 10, -1), W (0, 0, 10, 10)) == 0);
    /* sizes whose product does not fit in 32 bits */
    CHECK (walpe_fs_overlap (W (0, 0, 65536, 65536), W (0, 0, 65536, 65536)) ==
           (gint64) 65536 * 65536);
    /* coordinates at the ends of int do not wrap */
    CHECK (walpe_fs_overlap (W (G_MAXINT - 10, 0, 100, 10), W (G_MAXINT - 5, 0, 100, 10)) == 950);
}

static void test_one_monitor (void)
{
    /* exactly the monitor */
    CHECK (IS (where (TRUE, W (0, 0, 1920, 1080), two, 2), "10"));
    /* the second monitor */
    CHECK (IS (where (TRUE, W (1920, 0, 2560, 1440), two, 2), "01"));
    /* a tiling window manager keeps a 2 px border around the fullscreen
     * window: the client is slightly smaller than the monitor */
    CHECK (IS (where (TRUE, W (2, 2, 1916, 1076), two, 2), "10"));
    CHECK (IS (where (TRUE, W (1922, 2, 2556, 1436), two, 2), "01"));
    /* ... or a gap of 20 px and a bar's height at the top */
    CHECK (IS (where (TRUE, W (1940, 44, 2520, 1376), two, 2), "01"));
    /* the border hangs over the monitor's edge into the neighbour: a
     * client of the monitor's size inside a 3 px frame */
    CHECK (IS (where (TRUE, W (-3, -3, 1926, 1086), two, 2), "10"));
    CHECK (IS (where (TRUE, W (1917, -3, 2566, 1446), two, 2), "01"));
    /* "fake fullscreen": the window has the state and stays in its tile,
     * half or a quarter of its monitor */
    CHECK (IS (where (TRUE, W (1920, 0, 1280, 1440), two, 2), "01"));
    CHECK (IS (where (TRUE, W (960, 540, 960, 540), two, 2), "10"));
    /* a small window with the state (a video player's first frame) */
    CHECK (IS (where (TRUE, W (2000, 100, 200, 100), two, 2), "01"));
    CHECK (IS (where (TRUE, W (10, 10, 1, 1), two, 2), "10"));
    /* most of it on the first monitor, a strip on the second */
    CHECK (IS (where (TRUE, W (1000, 100, 1000, 500), two, 2), "10"));
    /* most of it on the second */
    CHECK (IS (where (TRUE, W (1800, 100, 1000, 500), two, 2), "01"));
}

static void test_spanning (void)
{
    /* a window across both monitors (a game on two screens; what a window
     * manager makes of _NET_WM_FULLSCREEN_MONITORS): on both */
    CHECK (IS (where (TRUE, W (0, 0, 4480, 1440), two, 2), "11"));
    /* two equal monitors, the window over both */
    static const WalpeFsRect eq[] = { { 0, 0, 1920, 1080 }, { 1920, 0, 1920, 1080 } };
    CHECK (IS (where (TRUE, W (0, 0, 3840, 1080), eq, 2), "11"));
    /* three in a row, the window on the two on the right */
    static const WalpeFsRect three[] = { { 0, 0, 1920, 1080 }, { 1920, 0, 1920, 1080 },
                                         { 3840, 0, 1920, 1080 } };
    CHECK (IS (where (TRUE, W (1920, 0, 3840, 1080), three, 3), "011"));
    CHECK (IS (where (TRUE, W (0, 0, 5760, 1080), three, 3), "111"));
    /* all of the first, a little more than half of the second: both */
    CHECK (IS (where (TRUE, W (0, 0, 1920 + 1000, 1080), eq, 2), "11"));
    /* all of the first, a little less than half of the second: the first */
    CHECK (IS (where (TRUE, W (0, 0, 1920 + 900, 1080), eq, 2), "10"));
    /* exactly half of the neighbour counts */
    CHECK (IS (where (TRUE, W (0, 0, 1920 + 960, 1080), eq, 2), "11"));
    /* a small window astride the edge, the same amount on each: the
     * monitor its centre is on (the right one: an edge belongs to the
     * monitor that starts there), and only that one */
    CHECK (IS (where (TRUE, W (1820, 400, 200, 200), eq, 2), "01"));
    /* one pixel more on the left */
    CHECK (IS (where (TRUE, W (1819, 400, 200, 200), eq, 2), "10"));
    /* mirrored outputs (two monitors showing the same rectangle): both */
    static const WalpeFsRect mirror[] = { { 0, 0, 1920, 1080 }, { 0, 0, 1920, 1080 } };
    CHECK (IS (where (TRUE, W (0, 0, 1920, 1080), mirror, 2), "11"));
    /* a small monitor inside a large window's rectangle */
    static const WalpeFsRect big_small[] = { { 0, 0, 3840, 2160 }, { 3840, 0, 800, 600 } };
    CHECK (IS (where (TRUE, W (0, 0, 4640, 2160), big_small, 2), "11"));
}

static void test_layouts (void)
{
    /* the second monitor below and to the left, a gap between them */
    static const WalpeFsRect off[] = { { 1000, 0, 1920, 1080 }, { 0, 1200, 1280, 1024 } };
    CHECK (IS (where (TRUE, W (0, 1200, 1280, 1024), off, 2), "01"));
    CHECK (IS (where (TRUE, W (1000, 0, 1920, 1080), off, 2), "10"));
    /* a turned monitor (portrait: the rectangle is already the turned
     * one) to the right of a landscape one that sits lower */
    static const WalpeFsRect rot[] = { { 0, 420, 1920, 1080 }, { 1920, 0, 1080, 1920 } };
    CHECK (IS (where (TRUE, W (1920, 0, 1080, 1920), rot, 2), "01"));
    CHECK (IS (where (TRUE, W (0, 420, 1920, 1080), rot, 2), "10"));
    /* a layout that starts left of and above the root's origin never
     * happens on X11, but nothing here depends on that */
    static const WalpeFsRect neg[] = { { -1920, -100, 1920, 1080 }, { 0, 0, 1920, 1080 } };
    CHECK (IS (where (TRUE, W (-1920, -100, 1920, 1080), neg, 2), "10"));
    CHECK (IS (where (TRUE, W (0, 0, 1920, 1080), neg, 2), "01"));
    /* the daemon shows on one of two monitors only (--monitor 1): a
     * window on the other one is on none of its monitors */
    static const WalpeFsRect second_only[] = { { 1920, 0, 2560, 1440 } };
    CHECK (IS (where (TRUE, W (0, 0, 1920, 1080), second_only, 1), "0"));
    CHECK (IS (where (TRUE, W (1920, 0, 2560, 1440), second_only, 1), "1"));
    /* a window parked outside every monitor (window managers move the
     * windows of hidden workspaces off-screen): on none */
    CHECK (IS (where (TRUE, W (-5000, 0, 1920, 1080), two, 2), "00"));
    CHECK (IS (where (TRUE, W (0, 5000, 1920, 1080), two, 2), "00"));
    /* a monitor of no size is nobody's place */
    static const WalpeFsRect empty[] = { { 0, 0, 0, 0 }, { 0, 0, 1920, 1080 } };
    CHECK (IS (where (TRUE, W (0, 0, 1920, 1080), empty, 2), "01"));
}

static void test_focus_and_unknown (void)
{
    /* the focus is on a window that is not fullscreen, while a fullscreen
     * window sits on the other monitor: the rule needs the focus, so no
     * monitor has it, whatever rectangle the focused window has */
    CHECK (IS (where (FALSE, W (1920, 0, 2560, 1440), two, 2), "00"));
    CHECK (IS (where (FALSE, W (0, 0, 1920, 1080), two, 2), "00"));
    CHECK (IS (where (FALSE, NULL, two, 2), "00"));
    /* a focused fullscreen window whose rectangle could not be asked for
     * (it is going away) counts for every monitor, as before */
    CHECK (IS (where (TRUE, NULL, two, 2), "*00"));
    CHECK (IS (where (TRUE, W (0, 0, 0, 0), two, 2), "*00"));
    CHECK (IS (where (TRUE, W (10, 10, -5, 100), two, 2), "*00"));
    /* no monitor at all: nothing to say, nothing written, no crash */
    CHECK (IS (where (TRUE, W (0, 0, 1920, 1080), two, 0), ""));
    CHECK (IS (where (TRUE, NULL, two, 0), "*"));
    CHECK (IS (where (FALSE, NULL, NULL, 0), ""));
    gboolean on[1] = { TRUE };
    CHECK (walpe_fs_place (W (0, 0, 10, 10), NULL, 0, on) == 0 && on[0] == TRUE);
}

static void test_counts (void)
{
    gboolean on[3];
    static const WalpeFsRect three[] = { { 0, 0, 1920, 1080 }, { 1920, 0, 1920, 1080 },
                                         { 3840, 0, 1920, 1080 } };
    CHECK (walpe_fs_place (W (0, 0, 1920, 1080), three, 3, on) == 1 && on[0] && !on[1] && !on[2]);
    CHECK (walpe_fs_place (W (0, 0, 5760, 1080), three, 3, on) == 3 && on[0] && on[1] && on[2]);
    CHECK (walpe_fs_place (W (9000, 0, 100, 100), three, 3, on) == 0 && !on[0] && !on[1] && !on[2]);
    /* the monitor with most of the window is counted once, also when the
     * window covers half of it */
    CHECK (walpe_fs_place (W (1920, 0, 1920, 600), three, 3, on) == 1 && on[1]);
}

int main (void)
{
    test_overlap ();
    test_one_monitor ();
    test_spanning ();
    test_layouts ();
    test_focus_and_unknown ();
    test_counts ();
    if (fails)
        g_printerr ("fsplace-test: %d check(s) failed\n", fails);
    else
        g_print ("fsplace-test: ok\n");
    return fails ? 1 : 0;
}
