/* x11desk_test.c - a desktop's own desktop window and where the wallpaper
 * window of a monitor goes because of it (platform/x11desk.h): into an
 * opaque one that lies over the monitor, below a translucent one, on the
 * root on a bare window manager. Made-up windows as the X server would
 * describe them: no display, no daemon. The windows are those the desktops
 * were found to make (docs/DESKTOPS.md). */
#include <glib.h>
#include "platform/x11desk.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; g_printerr ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define M(x, y, w, h) (&(WalpeDeskRect) { (x), (y), (w), (h) })

static void test_parse (void)
{
    WalpeX11DeskMode m = WALPE_X11_DESK_ROOT;
    CHECK (walpe_x11_desk_parse (NULL, &m) == 0 && m == WALPE_X11_DESK_AUTO);
    m = WALPE_X11_DESK_ROOT;
    CHECK (walpe_x11_desk_parse ("", &m) == 0 && m == WALPE_X11_DESK_AUTO);
    CHECK (walpe_x11_desk_parse ("auto", &m) == 1 && m == WALPE_X11_DESK_AUTO);
    CHECK (walpe_x11_desk_parse ("root", &m) == 1 && m == WALPE_X11_DESK_ROOT);
    CHECK (walpe_x11_desk_parse ("ROOT", &m) == 1 && m == WALPE_X11_DESK_ROOT);
    CHECK (walpe_x11_desk_parse ("window", &m) == 1 && m == WALPE_X11_DESK_WINDOW);
    m = WALPE_X11_DESK_ROOT;
    CHECK (walpe_x11_desk_parse ("desktop", &m) == -1 && m == WALPE_X11_DESK_AUTO);
    CHECK (walpe_x11_desk_parse ("1", &m) == -1 && m == WALPE_X11_DESK_AUTO);
    CHECK (!g_strcmp0 (walpe_x11_desk_mode_name (WALPE_X11_DESK_AUTO), "auto"));
    CHECK (!g_strcmp0 (walpe_x11_desk_mode_name (WALPE_X11_DESK_ROOT), "root"));
    CHECK (!g_strcmp0 (walpe_x11_desk_mode_name (WALPE_X11_DESK_WINDOW), "window"));
}

/* a bare window manager: no desktop window at all */
static void test_none (void)
{
    for (int cm = 0; cm < 2; cm++)
        for (int mode = WALPE_X11_DESK_AUTO; mode <= WALPE_X11_DESK_WINDOW; mode++) {
            CHECK (walpe_x11_desk_host ((WalpeX11DeskMode) mode, M (0, 0, 1920, 1080), NULL, 0, cm) == -1);
            CHECK (walpe_x11_desk_hides (M (0, 0, 1920, 1080), NULL, 0, cm) == -1);
        }
    CHECK (!walpe_x11_desk_is_top (0x400001, NULL, 0));
}

/* xfdesktop under xfwm4: one 24-bit window over the whole screen (two
 * monitors side by side), in a frame of the window manager; xfwm4
 * composites */
static void test_one_for_the_screen (void)
{
    const WalpeDeskWin xf[] = { { 0xe00028, 0x601cb7, { 0, 0, 3840, 1080 }, 24 } };
    for (int cm = 0; cm < 2; cm++) {
        CHECK (walpe_x11_desk_host (WALPE_X11_DESK_AUTO, M (0, 0, 1920, 1080), xf, 1, cm) == 0);
        CHECK (walpe_x11_desk_host (WALPE_X11_DESK_AUTO, M (1920, 0, 1920, 1080), xf, 1, cm) == 0);
        CHECK (walpe_x11_desk_host (WALPE_X11_DESK_WINDOW, M (1920, 0, 1920, 1080), xf, 1, cm) == 0);
        CHECK (walpe_x11_desk_host (WALPE_X11_DESK_ROOT, M (0, 0, 1920, 1080), xf, 1, cm) == -1);
        /* asked to stay on the root: the wallpaper is behind that window */
        CHECK (walpe_x11_desk_hides (M (0, 0, 1920, 1080), xf, 1, cm) == 0);
    }
    /* a monitor the window does not reach (a third one switched on a
     * moment ago, the file manager has not followed yet): the root */
    CHECK (walpe_x11_desk_host (WALPE_X11_DESK_AUTO, M (3840, 0, 1920, 1080), xf, 1, FALSE) == -1);
    CHECK (walpe_x11_desk_hides (M (3840, 0, 1920, 1080), xf, 1, FALSE) == -1);
    /* ... and one it covers only in part */
    CHECK (walpe_x11_desk_host (WALPE_X11_DESK_AUTO, M (1920, 0, 2560, 1440), xf, 1, FALSE) == -1);
    /* the pointer over the frame is over the desktop */
    CHECK (walpe_x11_desk_is_top (0x601cb7, xf, 1));
    CHECK (walpe_x11_desk_is_top (0xe00028, xf, 1));
    CHECK (!walpe_x11_desk_is_top (0x601d9c, xf, 1));
    CHECK (!walpe_x11_desk_is_top (0, xf, 1));
}

/* pcmanfm --desktop / pcmanfm-qt --desktop: one window per monitor */
static void test_one_per_monitor (void)
{
    const WalpeDeskWin pc[] = { { 0xa00003, 0x800186, { 0, 0, 1920, 1080 }, 24 },
                                { 0xa00010, 0x800190, { 1920, 0, 2560, 1440 }, 24 } };
    CHECK (walpe_x11_desk_host (WALPE_X11_DESK_AUTO, M (0, 0, 1920, 1080), pc, 2, FALSE) == 0);
    CHECK (walpe_x11_desk_host (WALPE_X11_DESK_AUTO, M (1920, 0, 2560, 1440), pc, 2, FALSE) == 1);
    CHECK (walpe_x11_desk_hides (M (1920, 0, 2560, 1440), pc, 2, FALSE) == 1);
    /* only the first monitor has one */
    CHECK (walpe_x11_desk_host (WALPE_X11_DESK_AUTO, M (1920, 0, 2560, 1440), pc, 1, FALSE) == -1);
}

/* nemo-desktop on Cinnamon, the icons window of GNOME: a 32-bit window */
static void test_translucent (void)
{
    const WalpeDeskWin nemo[] = { { 0x1c00007, 0x1c00007, { 0, 0, 1920, 1080 }, 32 } };
    /* under a compositing manager what is below it is seen between the
     * icons: the wallpaper window stays on the root */
    CHECK (walpe_x11_desk_translucent (&nemo[0], TRUE));
    CHECK (walpe_x11_desk_host (WALPE_X11_DESK_AUTO, M (0, 0, 1920, 1080), nemo, 1, TRUE) == -1);
    CHECK (walpe_x11_desk_hides (M (0, 0, 1920, 1080), nemo, 1, TRUE) == -1);
    /* asked for by name: into it all the same */
    CHECK (walpe_x11_desk_host (WALPE_X11_DESK_WINDOW, M (0, 0, 1920, 1080), nemo, 1, TRUE) == 0);
    CHECK (walpe_x11_desk_host (WALPE_X11_DESK_ROOT, M (0, 0, 1920, 1080), nemo, 1, TRUE) == -1);
    /* without a compositing manager a 32-bit window lets nothing through */
    CHECK (!walpe_x11_desk_translucent (&nemo[0], FALSE));
    CHECK (walpe_x11_desk_host (WALPE_X11_DESK_AUTO, M (0, 0, 1920, 1080), nemo, 1, FALSE) == 0);
    CHECK (walpe_x11_desk_hides (M (0, 0, 1920, 1080), nemo, 1, FALSE) == 0);
}

/* desktop-type windows that are no file manager's desktop: a system
 * monitor that asks for the type to stay below everything */
static void test_small_ones (void)
{
    const WalpeDeskWin w[] = { { 0x2000001, 0x2000001, { 1500, 40, 380, 600 }, 32 },
                               { 0x2200001, 0x2200001, { 0, 0, 1920, 1079 }, 24 },
                               { 0xe00028, 0x601cb7, { 0, 0, 1920, 1080 }, 24 } };
    CHECK (!walpe_x11_desk_covers (&w[0], M (0, 0, 1920, 1080)));
    CHECK (!walpe_x11_desk_covers (&w[1], M (0, 0, 1920, 1080)));     /* one row short */
    CHECK (walpe_x11_desk_covers (&w[2], M (0, 0, 1920, 1080)));
    CHECK (walpe_x11_desk_host (WALPE_X11_DESK_AUTO, M (0, 0, 1920, 1080), w, 2, FALSE) == -1);
    CHECK (walpe_x11_desk_host (WALPE_X11_DESK_WINDOW, M (0, 0, 1920, 1080), w, 2, TRUE) == -1);
    /* the real one is found behind them in the list */
    CHECK (walpe_x11_desk_host (WALPE_X11_DESK_AUTO, M (0, 0, 1920, 1080), w, 3, FALSE) == 2);
    /* a monitor without a size (a made-up state) is covered by nothing */
    CHECK (!walpe_x11_desk_covers (&w[2], M (0, 0, 0, 0)));
    /* an entry without a window is none */
    const WalpeDeskWin gone[] = { { 0, 0, { 0, 0, 1920, 1080 }, 24 } };
    CHECK (walpe_x11_desk_host (WALPE_X11_DESK_AUTO, M (0, 0, 1920, 1080), gone, 1, FALSE) == -1);
    CHECK (walpe_x11_desk_hides (M (0, 0, 1920, 1080), gone, 1, FALSE) == -1);
}

/* two that qualify: a translucent icons window and an opaque one. The
 * first opaque one in the list is the host; the first one stays the
 * choice when a second opaque one appears */
static void test_several (void)
{
    const WalpeDeskWin w[] = { { 0x1c00007, 0x1c00007, { 0, 0, 1920, 1080 }, 32 },
                               { 0xa00003, 0x800186, { 0, 0, 1920, 1080 }, 24 },
                               { 0xb00003, 0x800200, { 0, 0, 1920, 1080 }, 24 } };
    CHECK (walpe_x11_desk_host (WALPE_X11_DESK_AUTO, M (0, 0, 1920, 1080), w, 3, TRUE) == 1);
    CHECK (walpe_x11_desk_host (WALPE_X11_DESK_AUTO, M (0, 0, 1920, 1080), w, 2, TRUE) == 1);
    CHECK (walpe_x11_desk_host (WALPE_X11_DESK_AUTO, M (0, 0, 1920, 1080), w, 3, FALSE) == 0);
    CHECK (walpe_x11_desk_host (WALPE_X11_DESK_WINDOW, M (0, 0, 1920, 1080), w, 3, TRUE) == 0);
    CHECK (walpe_x11_desk_hides (M (0, 0, 1920, 1080), w, 3, TRUE) == 1);
}

/* WALPE_X11_WINDOW and the window managers that paint an
 * override-redirect window above everything */
static void test_window_kind (void)
{
    WalpeX11WinMode m = WALPE_X11_WIN_MANAGED;
    CHECK (walpe_x11_win_parse (NULL, &m) == 0 && m == WALPE_X11_WIN_AUTO);
    CHECK (walpe_x11_win_parse ("", &m) == 0 && m == WALPE_X11_WIN_AUTO);
    CHECK (walpe_x11_win_parse ("auto", &m) == 1 && m == WALPE_X11_WIN_AUTO);
    CHECK (walpe_x11_win_parse ("override", &m) == 1 && m == WALPE_X11_WIN_OVERRIDE);
    CHECK (walpe_x11_win_parse ("Managed", &m) == 1 && m == WALPE_X11_WIN_MANAGED);
    CHECK (walpe_x11_win_parse ("desktop", &m) == -1 && m == WALPE_X11_WIN_AUTO);
    CHECK (!g_strcmp0 (walpe_x11_win_mode_name (WALPE_X11_WIN_AUTO), "auto"));
    CHECK (!g_strcmp0 (walpe_x11_win_mode_name (WALPE_X11_WIN_OVERRIDE), "override"));
    CHECK (!g_strcmp0 (walpe_x11_win_mode_name (WALPE_X11_WIN_MANAGED), "managed"));

    /* the names the window managers give themselves (wmctrl -m) */
    static const char *const mutter[] = { "Mutter (Muffin)", "GNOME Shell", "Mutter", "Mutter(Budgie)",
                                          "Mutter(Gala)", "mutter" };
    static const char *const others[] = { "dwm", "Xfwm4", "Openbox", "Metacity (Marco)", "i3", "IceWM 3.4.5 (Linux/x86_64)",
                                          "Fluxbox", "bspwm", "awesome", "LG3D", "", NULL };
    /* KWin: on top as well, and Plasma's 32-bit desktop window is opaque */
    CHECK (walpe_x11_wm_is_kwin ("KWin") && walpe_x11_wm_is_kwin ("kwin_x11") && !walpe_x11_wm_is_kwin ("Xfwm4") &&
           !walpe_x11_wm_is_kwin (NULL));
    CHECK (walpe_x11_wm_override_on_top ("KWin"));
    CHECK (walpe_x11_win_managed (WALPE_X11_WIN_AUTO, "KWin") && !walpe_x11_win_managed (WALPE_X11_WIN_OVERRIDE, "KWin"));
    CHECK (!walpe_x11_desk_see_through ("KWin", TRUE) && !walpe_x11_desk_see_through ("KWin", FALSE));
    CHECK (walpe_x11_desk_see_through ("Mutter (Muffin)", TRUE) && !walpe_x11_desk_see_through ("Mutter (Muffin)", FALSE));
    CHECK (walpe_x11_desk_see_through (NULL, TRUE) && walpe_x11_desk_see_through ("Xfwm4", TRUE));
    {
        /* plasmashell's desktop window: the host under KWin, left alone under any other name */
        const WalpeDeskWin plasma[] = { { 0xa00015, 0x200027, { 0, 0, 1920, 1080 }, 32 } };
        const WalpeDeskRect mon = { 0, 0, 1920, 1080 };
        CHECK (walpe_x11_desk_host (WALPE_X11_DESK_AUTO, &mon, plasma, 1, walpe_x11_desk_see_through ("KWin", TRUE)) == 0);
        CHECK (walpe_x11_desk_host (WALPE_X11_DESK_AUTO, &mon, plasma, 1,
                                    walpe_x11_desk_see_through ("Mutter (Muffin)", TRUE)) == -1);
    }
    for (guint i = 0; i < G_N_ELEMENTS (mutter); i++) {
        CHECK (walpe_x11_wm_override_on_top (mutter[i]) && !walpe_x11_wm_is_kwin (mutter[i]));
        CHECK (walpe_x11_win_managed (WALPE_X11_WIN_AUTO, mutter[i]));
        CHECK (!walpe_x11_win_managed (WALPE_X11_WIN_OVERRIDE, mutter[i]));
        CHECK (walpe_x11_win_managed (WALPE_X11_WIN_MANAGED, mutter[i]));
    }
    for (guint i = 0; i < G_N_ELEMENTS (others); i++) {
        CHECK (!walpe_x11_wm_override_on_top (others[i]));
        CHECK (!walpe_x11_win_managed (WALPE_X11_WIN_AUTO, others[i]));
        CHECK (!walpe_x11_win_managed (WALPE_X11_WIN_OVERRIDE, others[i]));
        CHECK (walpe_x11_win_managed (WALPE_X11_WIN_MANAGED, others[i]));
    }
}

/* A window of the desktop type is the desktop only to a window manager
 * that says it knows the type (_NET_SUPPORTED). The lists are shortened
 * ones of what dwm 6.4, i3 4.23, openbox 3.6.1 and xfwm4 4.18 set; the
 * atoms are numbers made up here. */
static void test_wm_knows_desktop (void)
{
    enum { A_SUPPORTED = 300, A_NAME, A_STATE, A_CHECK, A_FULLSCREEN, A_ACTIVE, A_TYPE, A_DIALOG,
           A_CLIENTS, A_DOCK, A_NORMAL, A_DESKTOP, A_UTILITY };
    const unsigned long dwm[] = { A_SUPPORTED, A_NAME, A_STATE, A_CHECK, A_FULLSCREEN, A_ACTIVE,
                                  A_TYPE, A_DIALOG, A_CLIENTS };
    const unsigned long i3[] = { A_SUPPORTED, A_NAME, A_STATE, A_CHECK, A_FULLSCREEN, A_ACTIVE, A_TYPE,
                                 A_NORMAL, A_DOCK, A_DIALOG, A_UTILITY, A_CLIENTS };
    const unsigned long openbox[] = { A_SUPPORTED, A_CLIENTS, A_ACTIVE, A_CHECK, A_NAME, A_TYPE,
                                      A_DESKTOP, A_DOCK, A_DIALOG, A_NORMAL, A_STATE, A_FULLSCREEN };
    const unsigned long xfwm4[] = { A_ACTIVE, A_CLIENTS, A_SUPPORTED, A_CHECK, A_STATE, A_FULLSCREEN,
                                    A_TYPE, A_DIALOG, A_DOCK, A_NORMAL, A_UTILITY, A_DESKTOP };
    CHECK (!walpe_x11_wm_knows_desktop (dwm, G_N_ELEMENTS (dwm), A_DESKTOP));
    CHECK (!walpe_x11_wm_knows_desktop (i3, G_N_ELEMENTS (i3), A_DESKTOP));
    CHECK (walpe_x11_wm_knows_desktop (openbox, G_N_ELEMENTS (openbox), A_DESKTOP));
    CHECK (walpe_x11_wm_knows_desktop (xfwm4, G_N_ELEMENTS (xfwm4), A_DESKTOP));
    /* the last one of the list counts, one after its end does not */
    CHECK (!walpe_x11_wm_knows_desktop (xfwm4, G_N_ELEMENTS (xfwm4) - 1, A_DESKTOP));
    /* no window manager: no property; an atom the server does not have */
    CHECK (!walpe_x11_wm_knows_desktop (NULL, 0, A_DESKTOP));
    CHECK (!walpe_x11_wm_knows_desktop (NULL, 12, A_DESKTOP));
    CHECK (!walpe_x11_wm_knows_desktop (openbox, G_N_ELEMENTS (openbox), 0));

    /* what follows from it: `auto` goes into a desktop window only there,
     * `window` everywhere, `root` nowhere */
    CHECK (walpe_x11_desk_allowed (WALPE_X11_DESK_AUTO, TRUE));
    CHECK (!walpe_x11_desk_allowed (WALPE_X11_DESK_AUTO, FALSE));
    CHECK (walpe_x11_desk_allowed (WALPE_X11_DESK_WINDOW, FALSE));
    CHECK (walpe_x11_desk_allowed (WALPE_X11_DESK_WINDOW, TRUE));
    CHECK (!walpe_x11_desk_allowed (WALPE_X11_DESK_ROOT, TRUE));
    CHECK (!walpe_x11_desk_allowed (WALPE_X11_DESK_ROOT, FALSE));
}

/* names other clients wrote, on their way into the log and diag */
static void test_name_clean (void)
{
    char *c = walpe_x11_name_clean ("Xfdesktop");
    CHECK (!g_strcmp0 (c, "Xfdesktop"));
    g_free (c);
    c = walpe_x11_name_clean ("Mutter (Muffin)");
    CHECK (!g_strcmp0 (c, "Mutter (Muffin)") && walpe_x11_wm_override_on_top (c));
    g_free (c);
    c = walpe_x11_name_clean ("IceWM 3.4.5 (Linux/x86_64)");
    CHECK (!g_strcmp0 (c, "IceWM 3.4.5 (Linux/x86_64)"));
    g_free (c);
    /* a line break, an escape sequence, a byte that is no ASCII */
    c = walpe_x11_name_clean ("a\nb\033[2Jc\xc3\xa9" "d\177");
    CHECK (!g_strcmp0 (c, "a?b?[2Jc??d?"));
    g_free (c);
    /* 64 bytes of a long one */
    char big[30001];
    memset (big, 'J', sizeof big - 1);
    big[sizeof big - 1] = 0;
    c = walpe_x11_name_clean (big);
    CHECK (c && strlen (c) == 64);
    g_free (c);
    c = walpe_x11_name_clean ("");
    CHECK (c && !*c);
    g_free (c);
    CHECK (walpe_x11_name_clean (NULL) == NULL);
}

int main (void)
{
    test_name_clean ();
    test_wm_knows_desktop ();
    test_window_kind ();
    test_parse ();
    test_none ();
    test_one_for_the_screen ();
    test_one_per_monitor ();
    test_translucent ();
    test_small_ones ();
    test_several ();
    if (fails)
        g_printerr ("x11desk-test: %d check(s) failed\n", fails);
    else
        g_print ("x11desk-test: ok\n");
    return fails ? 1 : 0;
}
