/* backend_test.c - which window system the daemon draws on
 * (common/backend.h): the setting's names, who decides (--backend, then
 * WALPE_BACKEND, then the config), the automatic rule row by row with a
 * made-up environment, builds that hold one backend only, and the
 * canonical form of output names. Pure functions: no display, no daemon. */
#include <glib.h>
#include <string.h>
#include "common/backend.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; g_printerr ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* an environment with both backends built and nothing set */
static WalpeBackendEnv env0 (void)
{
    WalpeBackendEnv e;
    memset (&e, 0, sizeof e);
    e.built_x11 = e.built_wayland = TRUE;
    e.cfg = WALPE_BACKEND_AUTO;
    e.layer_shell = -1;
    return e;
}

/* choose and compare; `in_why`: a piece of text the sentence must hold */
static gboolean picks (const WalpeBackendEnv *e, int want, const char *source, const char *in_why)
{
    WalpeBackendChoice c;
    walpe_backend_choose (e, &c);
    gboolean ok = c.backend == want && c.why && *c.why &&
                  (!source || !g_strcmp0 (c.source, source)) &&
                  (!in_why || strstr (c.why, in_why) != NULL);
    if (!ok)
        g_printerr ("  got backend %s, source %s, why \"%s\"\n", walpe_backend_name (c.backend),
                    c.source ? c.source : "(null)", c.why ? c.why : "(null)");
    walpe_backend_choice_clear (&c);
    return ok;
}

static void test_names (void)
{
    CHECK (walpe_backend_parse ("auto", -1) == WALPE_BACKEND_AUTO);
    CHECK (walpe_backend_parse ("x11", -1) == WALPE_BACKEND_X11);
    CHECK (walpe_backend_parse ("wayland", -1) == WALPE_BACKEND_WAYLAND);
    CHECK (walpe_backend_parse ("X11", -1) == WALPE_BACKEND_X11);
    CHECK (walpe_backend_parse ("  Wayland \n", -1) == WALPE_BACKEND_WAYLAND);
    CHECK (walpe_backend_parse ("xorg", -1) == WALPE_BACKEND_X11);
    CHECK (walpe_backend_parse ("wl", -1) == WALPE_BACKEND_WAYLAND);
    CHECK (walpe_backend_parse ("", -1) == -1);
    CHECK (walpe_backend_parse (NULL, 7) == 7);
    CHECK (walpe_backend_parse ("mir", -1) == -1);
    CHECK (walpe_backend_parse ("x11 wayland", -1) == -1);
    CHECK (walpe_backend_parse ("waylandwaylandwayland", -1) == -1);    /* longer than any name */
    CHECK (walpe_backend_parse ("1", WALPE_BACKEND_AUTO) == WALPE_BACKEND_AUTO);
    for (int b = WALPE_BACKEND_AUTO; b <= WALPE_BACKEND_WAYLAND; b++)
        CHECK (walpe_backend_parse (walpe_backend_name (b), -1) == b);
    CHECK (!strcmp (walpe_backend_name (99), "auto"));
    CHECK (!strcmp (walpe_backends_built (TRUE, TRUE), "x11,wayland"));
    CHECK (!strcmp (walpe_backends_built (TRUE, FALSE), "x11"));
    CHECK (!strcmp (walpe_backends_built (FALSE, TRUE), "wayland"));
}

/* --backend beats WALPE_BACKEND beats the config; what is not a name is
 * reported and skipped */
static void test_precedence (void)
{
    WalpeBackendEnv e = env0 ();
    e.x_display = e.wayland_display = TRUE;
    e.layer_shell = 1;
    e.cfg = WALPE_BACKEND_X11;
    CHECK (picks (&e, WALPE_BACKEND_X11, "config", "asked for"));
    e.env = "wayland";
    CHECK (picks (&e, WALPE_BACKEND_WAYLAND, "WALPE_BACKEND", "asked for"));
    e.cli = "x11";
    CHECK (picks (&e, WALPE_BACKEND_X11, "--backend", "asked for"));
    /* "auto" given explicitly is a choice too: it hides what is below it */
    e.cli = "auto";
    CHECK (picks (&e, WALPE_BACKEND_WAYLAND, "--backend", "auto:"));
    e.cli = NULL;
    e.env = "auto";
    CHECK (picks (&e, WALPE_BACKEND_WAYLAND, "WALPE_BACKEND", "auto:"));
    /* empty = unset */
    e.env = "";
    CHECK (picks (&e, WALPE_BACKEND_X11, "config", NULL));
    /* not a name: warned about, the next one decides */
    e.env = "weston";
    WalpeBackendChoice c;
    walpe_backend_choose (&e, &c);
    CHECK (c.backend == WALPE_BACKEND_X11 && !g_strcmp0 (c.source, "config"));
    CHECK (c.warn && strstr (c.warn, "weston"));
    walpe_backend_choice_clear (&c);
    e.cli = "nope";
    e.env = NULL;
    e.cfg = WALPE_BACKEND_AUTO;
    walpe_backend_choose (&e, &c);
    CHECK (c.backend == WALPE_BACKEND_WAYLAND && !g_strcmp0 (c.source, "default"));
    CHECK (c.warn && strstr (c.warn, "nope"));
    walpe_backend_choice_clear (&c);
    /* a config value outside the enum is auto */
    e.cli = NULL;
    e.cfg = 42;
    CHECK (picks (&e, WALPE_BACKEND_WAYLAND, "default", NULL));
}

/* the automatic rule, both backends built */
static void test_auto (void)
{
    WalpeBackendEnv e = env0 ();
    /* a Wayland session whose compositor has the layer shell */
    e.wayland_display = TRUE;
    e.layer_shell = 1;
    CHECK (walpe_backend_wants_probe (&e));
    CHECK (picks (&e, WALPE_BACKEND_WAYLAND, "default", "layer shell"));
    e.x_display = TRUE;                 /* Xwayland there as well: still Wayland */
    CHECK (picks (&e, WALPE_BACKEND_WAYLAND, "default", NULL));
    /* a compositor without the layer shell (GNOME): nothing, and not X11
     * through Xwayland, where the wallpaper window lies above every other
     * window; the sentence names both the layer shell and Xwayland ... */
    e.layer_shell = 0;
    CHECK (picks (&e, WALPE_BACKEND_AUTO, "default", "no layer shell"));
    CHECK (picks (&e, WALPE_BACKEND_AUTO, NULL, "Xwayland"));
    CHECK (picks (&e, WALPE_BACKEND_AUTO, NULL, "--backend x11"));
    /* ... with one more sentence where the platform has one (GNOME: the
     * WalPE extension for GNOME Shell and how to install it) ... */
    e.no_shell_hint = "The extension: install.sh";
    CHECK (picks (&e, WALPE_BACKEND_AUTO, "default", "--backend x11). The extension: install.sh"));
    /* ... and Wayland after all where something keeps the daemon's windows
     * on the desktop (GNOME Shell with that extension): never Xwayland,
     * and the hint is not said */
    e.desk_windows = TRUE;
    CHECK (picks (&e, WALPE_BACKEND_WAYLAND, "default", "extension for GNOME Shell"));
    {
        WalpeBackendChoice c;
        walpe_backend_choose (&e, &c);
        CHECK (!strstr (c.why, "install.sh") && !strstr (c.why, "Xwayland"));
        walpe_backend_choice_clear (&c);
    }
    e.x_display = FALSE;
    CHECK (picks (&e, WALPE_BACKEND_WAYLAND, "default", "no layer shell, but"));
    e.x_display = TRUE;
    /* (with a layer shell the two say nothing: they are filled only without) */
    e.layer_shell = 1;
    CHECK (picks (&e, WALPE_BACKEND_WAYLAND, "default", "offers the layer shell"));
    e.layer_shell = 0;
    e.desk_windows = FALSE;
    e.no_shell_hint = NULL;
    /* ... X11 asked for by name is taken there all the same ... */
    e.cli = "x11";
    CHECK (picks (&e, WALPE_BACKEND_X11, "--backend", "asked for"));
    e.cli = NULL;
    e.cfg = WALPE_BACKEND_X11;
    CHECK (picks (&e, WALPE_BACKEND_X11, "config", "asked for"));
    e.cfg = WALPE_BACKEND_AUTO;
    /* ... and without DISPLAY the sentence is the one it always was */
    e.x_display = FALSE;
    CHECK (picks (&e, WALPE_BACKEND_AUTO, "default", "no layer shell"));
    CHECK (picks (&e, WALPE_BACKEND_AUTO, NULL, "DISPLAY is not set"));
    /* --windowed needs a window, not the layer shell */
    e.windowed = TRUE;
    CHECK (picks (&e, WALPE_BACKEND_WAYLAND, "default", "--windowed"));
    e.windowed = FALSE;
    /* WAYLAND_DISPLAY names a compositor that does not answer */
    e.layer_shell = -1;
    e.x_display = TRUE;
    CHECK (picks (&e, WALPE_BACKEND_X11, "default", "does not answer"));
    e.x_display = FALSE;
    CHECK (picks (&e, WALPE_BACKEND_AUTO, "default", "does not answer"));
    /* an X session */
    e = env0 ();
    e.x_display = TRUE;
    CHECK (!walpe_backend_wants_probe (&e));
    CHECK (picks (&e, WALPE_BACKEND_X11, "default", "DISPLAY is set"));
    /* no session */
    e.x_display = FALSE;
    CHECK (picks (&e, WALPE_BACKEND_AUTO, "default", "neither WAYLAND_DISPLAY nor DISPLAY"));
    /* no look at the compositor when the answer does not depend on it */
    e = env0 ();
    e.wayland_display = e.x_display = TRUE;
    e.cfg = WALPE_BACKEND_X11;
    CHECK (!walpe_backend_wants_probe (&e));
    e.cfg = WALPE_BACKEND_WAYLAND;
    CHECK (!walpe_backend_wants_probe (&e));
    e.cfg = WALPE_BACKEND_AUTO;
    e.env = "x11";
    CHECK (!walpe_backend_wants_probe (&e));
}

/* asked for by name: that one or an error, never the other */
static void test_explicit (void)
{
    WalpeBackendEnv e = env0 ();
    e.x_display = TRUE;                 /* an X session, Wayland asked for */
    e.cfg = WALPE_BACKEND_WAYLAND;
    CHECK (picks (&e, WALPE_BACKEND_WAYLAND, "config", NULL));  /* opening it will fail and say so */
    e = env0 ();
    e.wayland_display = TRUE;           /* a Wayland session without Xwayland, X11 asked for */
    e.layer_shell = 1;
    e.env = "x11";
    CHECK (picks (&e, WALPE_BACKEND_X11, "WALPE_BACKEND", NULL));
}

/* builds that hold one backend */
static void test_single_backend (void)
{
    WalpeBackendEnv e = env0 ();
    e.built_wayland = FALSE;            /* X11 only */
    e.x_display = TRUE;
    CHECK (picks (&e, WALPE_BACKEND_X11, "default", NULL));
    e.wayland_display = TRUE;           /* in a Wayland session: Xwayland */
    CHECK (!walpe_backend_wants_probe (&e));
    CHECK (picks (&e, WALPE_BACKEND_X11, "default", "no Wayland backend"));
    e.x_display = FALSE;
    CHECK (picks (&e, WALPE_BACKEND_AUTO, "default", "no Wayland backend"));
    e.cli = "wayland";
    CHECK (picks (&e, WALPE_BACKEND_AUTO, "--backend", "this build has none"));
    CHECK (picks (&e, WALPE_BACKEND_AUTO, NULL, "built: x11"));

    e = env0 ();
    e.built_x11 = FALSE;                /* Wayland only */
    e.wayland_display = TRUE;
    e.layer_shell = 1;
    CHECK (picks (&e, WALPE_BACKEND_WAYLAND, "default", NULL));
    e.layer_shell = 0;
    e.x_display = TRUE;                 /* no layer shell, and X11 is not in the build */
    CHECK (picks (&e, WALPE_BACKEND_AUTO, "default", "no layer shell"));
    e = env0 ();
    e.built_x11 = FALSE;
    e.x_display = TRUE;                 /* an X session */
    CHECK (picks (&e, WALPE_BACKEND_AUTO, "default", "no X11 backend"));
    e.cfg = WALPE_BACKEND_X11;
    CHECK (picks (&e, WALPE_BACKEND_AUTO, "config", "this build has none"));
    CHECK (picks (&e, WALPE_BACKEND_AUTO, NULL, "built: wayland"));
}

static gboolean canon_is (const char *name, const char *want)
{
    char *c = walpe_output_name_canon (name);
    gboolean ok = !g_strcmp0 (c, want);
    if (!ok)
        g_printerr ("  canon (%s) = %s, expected %s\n", name ? name : "(null)", c ? c : "(null)",
                    want ? want : "(null)");
    g_free (c);
    return ok;
}

/* one spelling for the connector names the two worlds spell differently */
static void test_output_names (void)
{
    CHECK (canon_is ("HDMI-A-1", "HDMI-1"));
    CHECK (canon_is ("HDMI-1", "HDMI-1"));
    CHECK (canon_is ("hdmi-a-2", "HDMI-2"));
    CHECK (canon_is ("HDMI-B-1", "HDMI-1"));
    CHECK (canon_is ("DP-3", "DP-3"));
    CHECK (canon_is ("DisplayPort-0", "DP-0"));
    CHECK (canon_is ("eDP-1", "EDP-1"));
    CHECK (canon_is ("eDP1", "EDP-1"));
    CHECK (canon_is ("HDMI2", "HDMI-2"));
    CHECK (canon_is ("DVI-D-1", "DVI-1"));
    CHECK (canon_is ("DVI-I-0", "DVI-0"));
    CHECK (canon_is (" DP-1 ", "DP-1"));
    CHECK (canon_is ("mon0", "MON-0"));         /* the fallback names too, consistently */
    CHECK (canon_is ("Virtual-1", "VIRTUAL-1"));
    CHECK (canon_is ("HEADLESS-1", "HEADLESS-1"));
    CHECK (canon_is ("windowed", "WINDOWED"));
    CHECK (canon_is ("", ""));
    CHECK (canon_is ("7", "7"));
    CHECK (canon_is (NULL, NULL));
    CHECK (walpe_output_name_same ("HDMI-A-1", "HDMI-1"));
    CHECK (walpe_output_name_same ("eDP-1", "eDP1"));
    CHECK (walpe_output_name_same ("DP-2", "DP-2"));
    CHECK (!walpe_output_name_same ("DP-1", "DP-2"));
    CHECK (!walpe_output_name_same ("DP-0", "DP-1"));   /* another numbering stays another name */
    CHECK (!walpe_output_name_same ("HDMI-1", "DP-1"));
    CHECK (!walpe_output_name_same (NULL, "DP-1") && !walpe_output_name_same ("DP-1", NULL));

    /* one name among several: the one spelled like it first. Two
     * connectors of one machine can share the canonical form; asked for by
     * its own name each finds itself (the pause signals of a window on the
     * second monitor went to the first) */
    static const char *const dvi[] = { "DVI-D-1", "DVI-I-1", "DP-1" };
    CHECK (walpe_output_name_same ("DVI-D-1", "DVI-I-1"));    /* (why the order matters) */
    CHECK (walpe_output_name_find (dvi, 3, "DVI-D-1") == 0);
    CHECK (walpe_output_name_find (dvi, 3, "DVI-I-1") == 1);
    CHECK (walpe_output_name_find (dvi, 3, "DP-1") == 2);
    static const char *const hdmi[] = { "HDMI-A-1", "HDMI-B-1" };
    CHECK (walpe_output_name_find (hdmi, 2, "HDMI-B-1") == 1);
    CHECK (walpe_output_name_find (hdmi, 2, "HDMI-A-1") == 0);
    /* no monitor is spelled like it: the same connector under its other
     * spelling (an X11 name asked for among Wayland names), the first one */
    CHECK (walpe_output_name_find (hdmi, 2, "HDMI-1") == 0);
    CHECK (walpe_output_name_find (dvi, 3, "DisplayPort-1") == 2);
    CHECK (walpe_output_name_find (dvi, 3, "DVI-1") == 0);
    /* none, no name, holes in the list, an empty list */
    CHECK (walpe_output_name_find (dvi, 3, "DP-2") == -1);
    CHECK (walpe_output_name_find (dvi, 3, NULL) == -1);
    static const char *const holes[] = { NULL, "eDP-1", NULL };
    CHECK (walpe_output_name_find (holes, 3, "eDP1") == 1);
    CHECK (walpe_output_name_find (holes, 3, "DP-1") == -1);
    CHECK (walpe_output_name_find (NULL, 0, "DP-1") == -1);
}

int main (void)
{
    test_names ();
    test_precedence ();
    test_auto ();
    test_explicit ();
    test_single_backend ();
    test_output_names ();
    if (fails)
        g_printerr ("backend-test: %d check(s) failed\n", fails);
    else
        g_print ("backend-test: ok\n");
    return fails ? 1 : 0;
}
