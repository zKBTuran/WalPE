/* backend.h - which window system the daemon draws on: X11 or Wayland.
 * Pure functions, no state, no window-system headers: shared by the daemon,
 * walpe-ctl and tests/backend_test.c (like pace.h and present.h).
 *
 * The setting ("backend" in config.json, `walpe-ctl set backend=...`,
 * WALPE_BACKEND in the environment, `walpe-daemon --backend NAME`):
 *   auto    - Wayland when WAYLAND_DISPLAY is set and the compositor offers
 *             the layer shell (zwlr_layer_shell_v1), else X11 when DISPLAY
 *             is set. The default.
 *   x11     - X11 only (also inside a Wayland session, through Xwayland).
 *   wayland - Wayland only; no fallback to X11.
 * It is read when the daemon starts; a change takes a restart of the daemon.
 * A build may hold one backend only (meson -Dx11=disabled / -Dwayland=disabled):
 * asking for the other one is an error that says so. */
#pragma once
#include <glib.h>
#include <string.h>

typedef enum {
    WALPE_BACKEND_AUTO = 0,
    WALPE_BACKEND_X11 = 1,
    WALPE_BACKEND_WAYLAND = 2,
} WalpeBackend;

static inline const char *walpe_backend_name (int b)
{
    return b == WALPE_BACKEND_X11 ? "x11" : b == WALPE_BACKEND_WAYLAND ? "wayland" : "auto";
}

/* "auto" / "x11" / "wayland" (any case; "xorg" and "x" are read as x11,
 * "wl" as wayland); anything else, NULL and "" give dflt */
static inline int walpe_backend_parse (const char *s, int dflt)
{
    if (!s)
        return dflt;
    while (*s == ' ' || *s == '\t')
        s++;
    char buf[16];
    gsize n = strlen (s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n'))
        n--;
    if (n == 0 || n >= sizeof buf)
        return dflt;
    for (gsize i = 0; i < n; i++)
        buf[i] = g_ascii_tolower (s[i]);
    buf[n] = 0;
    if (!strcmp (buf, "auto"))
        return WALPE_BACKEND_AUTO;
    if (!strcmp (buf, "x11") || !strcmp (buf, "xorg") || !strcmp (buf, "x"))
        return WALPE_BACKEND_X11;
    if (!strcmp (buf, "wayland") || !strcmp (buf, "wl"))
        return WALPE_BACKEND_WAYLAND;
    return dflt;
}

/* the backends a build holds, for `status` and messages: "x11,wayland",
 * "x11" or "wayland" */
static inline const char *walpe_backends_built (gboolean x11, gboolean wayland)
{
    return x11 && wayland ? "x11,wayland" : x11 ? "x11" : wayland ? "wayland" : "none";
}

/* everything the choice depends on */
typedef struct {
    gboolean built_x11, built_wayland;
    const char *cli;            /* --backend NAME, NULL = not given */
    const char *env;            /* WALPE_BACKEND, NULL / "" = unset */
    int cfg;                    /* the saved setting (WalpeBackend) */
    gboolean wayland_display;   /* WAYLAND_DISPLAY (or WAYLAND_SOCKET) is set */
    gboolean x_display;         /* DISPLAY is set */
    /* what a look at the compositor found (walpe_backend_wants_probe says
     * whether to look): 1 = it offers the layer shell, 0 = it answers but
     * has none, -1 = no connection could be made */
    int layer_shell;
    gboolean windowed;          /* --windowed: a plain window, no layer shell needed */
} WalpeBackendEnv;

typedef struct {
    int backend;                /* WALPE_BACKEND_X11 / _WAYLAND; _AUTO = none usable */
    int asked;                  /* what was asked for, after cli > env > config */
    const char *source;         /* who asked: "--backend", "WALPE_BACKEND", "config", "default" */
    char *why;                  /* one sentence for the log / the error (g_free) */
    char *warn;                 /* a value that was not understood, NULL = none (g_free) */
} WalpeBackendChoice;

/* the setting in force and where it came from; *bad receives a value that
 * was not understood (static storage of the caller's strings; may be NULL) */
static inline int walpe_backend_asked (const WalpeBackendEnv *e, const char **source,
                                       const char **bad)
{
    if (bad)
        *bad = NULL;
    if (e->cli && *e->cli) {
        int v = walpe_backend_parse (e->cli, -1);
        if (v >= 0) {
            *source = "--backend";
            return v;
        }
        if (bad)
            *bad = e->cli;
    }
    if (e->env && *e->env) {
        int v = walpe_backend_parse (e->env, -1);
        if (v >= 0) {
            *source = "WALPE_BACKEND";
            return v;
        }
        if (bad && !*bad)
            *bad = e->env;
    }
    if (e->cfg == WALPE_BACKEND_X11 || e->cfg == WALPE_BACKEND_WAYLAND) {
        *source = "config";
        return e->cfg;
    }
    *source = "default";
    return WALPE_BACKEND_AUTO;
}

/* TRUE = the choice needs to know whether the compositor has the layer
 * shell: fill e->layer_shell before walpe_backend_choose */
static inline gboolean walpe_backend_wants_probe (const WalpeBackendEnv *e)
{
    const char *src;
    return walpe_backend_asked (e, &src, NULL) == WALPE_BACKEND_AUTO && e->built_wayland &&
           e->wayland_display;
}

static inline void walpe_backend_choose (const WalpeBackendEnv *e, WalpeBackendChoice *out)
{
    const char *bad = NULL;
    memset (out, 0, sizeof (*out));
    out->asked = walpe_backend_asked (e, &out->source, &bad);
    if (bad)
        out->warn = g_strdup_printf ("backend \"%s\" is not understood (auto, x11, wayland)", bad);
    const char *built = walpe_backends_built (e->built_x11, e->built_wayland);
    out->backend = WALPE_BACKEND_AUTO;
    if (out->asked == WALPE_BACKEND_X11) {
        if (!e->built_x11)
            out->why = g_strdup_printf ("the X11 backend was asked for (%s) but this build has "
                                        "none (built: %s)", out->source, built);
        else {
            out->backend = WALPE_BACKEND_X11;
            out->why = g_strdup_printf ("x11, asked for (%s)", out->source);
        }
        return;
    }
    if (out->asked == WALPE_BACKEND_WAYLAND) {
        if (!e->built_wayland)
            out->why = g_strdup_printf ("the Wayland backend was asked for (%s) but this build "
                                        "has none (built: %s)", out->source, built);
        else {
            out->backend = WALPE_BACKEND_WAYLAND;
            out->why = g_strdup_printf ("wayland, asked for (%s)", out->source);
        }
        return;
    }
    /* auto */
    const char *wl_miss = NULL;
    if (!e->built_wayland)
        wl_miss = e->wayland_display ? "WAYLAND_DISPLAY is set but this build has no Wayland backend"
                                     : NULL;
    else if (!e->wayland_display)
        wl_miss = NULL;         /* not a Wayland session: nothing to say */
    else if (e->layer_shell < 0)
        wl_miss = "WAYLAND_DISPLAY is set but the compositor does not answer";
    else if (e->layer_shell == 0 && !e->windowed)
        wl_miss = "the Wayland compositor has no layer shell (zwlr_layer_shell_v1), which a "
                  "wallpaper needs";
    else {
        out->backend = WALPE_BACKEND_WAYLAND;
        out->why = g_strdup (e->windowed && e->layer_shell == 0
                                 ? "wayland (auto: WAYLAND_DISPLAY is set; --windowed needs no "
                                   "layer shell)"
                                 : "wayland (auto: WAYLAND_DISPLAY is set and the compositor "
                                   "offers the layer shell)");
        return;
    }
    if (e->built_x11 && e->x_display) {
        out->backend = WALPE_BACKEND_X11;
        out->why = wl_miss ? g_strdup_printf ("x11 (auto: %s; DISPLAY is set)", wl_miss)
                           : g_strdup ("x11 (auto: DISPLAY is set)");
        return;
    }
    if (wl_miss && e->built_x11)
        out->why = g_strdup_printf ("no display to draw on: %s, and DISPLAY is not set", wl_miss);
    else if (wl_miss)
        out->why = g_strdup_printf ("no display to draw on: %s (built: %s)", wl_miss, built);
    else if (!e->built_x11 && e->x_display)
        out->why = g_strdup_printf ("no display to draw on: DISPLAY is set but this build has no "
                                    "X11 backend, and WAYLAND_DISPLAY is not set (built: %s)", built);
    else
        out->why = g_strdup_printf ("no display to draw on: neither WAYLAND_DISPLAY nor DISPLAY "
                                    "is set (built: %s)", built);
}

static inline void walpe_backend_choice_clear (WalpeBackendChoice *c)
{
    g_free (c->why);
    g_free (c->warn);
    c->why = c->warn = NULL;
}

/* ---- output names
 * A monitor is named by its connector as the window system reports it:
 * RandR monitor names on X11 ("DP-1", "HDMI-1"; NVIDIA's driver "DP-0",
 * "HDMI-0"), wl_output / xdg_output names on Wayland ("DP-1", "HDMI-A-1").
 * These names are the keys of the per-monitor settings and are used as
 * they come. The two worlds spell some connectors differently; the
 * canonical form below is the same for both where only the spelling
 * differs, so a setting saved under one backend can be found under the
 * other:
 *   HDMI-A-1, HDMI-B-1, HDMI-1   -> HDMI-1
 *   DisplayPort-1, DP-1          -> DP-1
 *   DVI-D-1, DVI-I-1, DVI-A-1    -> DVI-1
 *   eDP-1, EDP-1, eDP1           -> EDP-1      (letters upper-case, "NAME<digits>" split)
 * It cannot undo a different numbering (NVIDIA's X driver counts from 0,
 * the kernel from 1): those names stay different, and the make / model /
 * serial fields of WalpeOutput are the way to match such monitors.
 * Returns a new string (g_free); NULL for NULL. */
static inline char *walpe_output_name_canon (const char *name)
{
    if (!name)
        return NULL;
    char *up = g_ascii_strup (name, -1);
    g_strstrip (up);
    static const struct { const char *from, *to; } map[] = {
        { "HDMI-A-", "HDMI-" }, { "HDMI-B-", "HDMI-" }, { "HDMIA-", "HDMI-" },
        { "DISPLAYPORT-", "DP-" }, { "DVI-D-", "DVI-" }, { "DVI-I-", "DVI-" },
        { "DVI-A-", "DVI-" },
    };
    char *res = NULL;
    for (guint i = 0; i < G_N_ELEMENTS (map) && !res; i++)
        if (g_str_has_prefix (up, map[i].from))
            res = g_strconcat (map[i].to, up + strlen (map[i].from), NULL);
    if (!res)
        res = g_strdup (up);
    g_free (up);
    /* "EDP1" / "HDMI2" (older X drivers) -> "EDP-1" / "HDMI-2": letters
     * followed directly by the trailing digits */
    gsize n = strlen (res), d = n;
    while (d > 0 && g_ascii_isdigit (res[d - 1]))
        d--;
    if (d > 0 && d < n && g_ascii_isalpha (res[d - 1]) && !strchr (res, '-')) {
        char *split = g_strdup_printf ("%.*s-%s", (int) d, res, res + d);
        g_free (res);
        res = split;
    }
    return res;
}

/* do two output names mean the same connector? (exact, or the same
 * canonical form) */
static inline gboolean walpe_output_name_same (const char *a, const char *b)
{
    if (!a || !b)
        return FALSE;
    if (!strcmp (a, b))
        return TRUE;
    char *ca = walpe_output_name_canon (a), *cb = walpe_output_name_canon (b);
    gboolean same = !strcmp (ca, cb);
    g_free (ca);
    g_free (cb);
    return same;
}

/* Which of n output names is `name`? The one spelled like it; only where
 * none is, the first that means the same connector by its canonical form.
 * Two connectors of one machine can share that form ("DVI-D-1" and
 * "DVI-I-1", "HDMI-A-1" and "HDMI-B-1"): asked for by the name the window
 * system uses, each has to find itself and not its neighbour. Entries may
 * be NULL. Returns the index, -1 = none. */
static inline int walpe_output_name_find (const char *const *names, int n, const char *name)
{
    if (!name)
        return -1;
    for (int i = 0; i < n; i++)
        if (names[i] && !strcmp (names[i], name))
            return i;
    for (int i = 0; i < n; i++)
        if (walpe_output_name_same (names[i], name))
            return i;
    return -1;
}
