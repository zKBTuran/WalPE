/* logindopt.h - which of logind's facts pause the wallpaper (WALPE_LOGIND).
 * Pure; logind.c and tests/sig_select_test.c include it.
 *
 *   sleep  the machine is about to suspend / hibernate (PrepareForSleep)
 *   lock   the session is locked (the session's LockedHint: set by GNOME
 *          Shell, KDE's locker and lockers started with a helper that
 *          sets it; swaylock and i3lock by themselves do not)
 *   vt     the session is not the active one of its seat (the user
 *          switched to another virtual terminal or another user's session)
 *
 * WALPE_LOGIND unset: all three on a Wayland backend, none on X11 (there
 * the X server's DPMS and screensaver state are followed, as before).
 * WALPE_LOGIND=1 / on / all: all three on either backend. =0 / off: none.
 * Or a comma-separated list of the names above. */
#pragma once
#include <glib.h>
#include <string.h>

enum {
    WALPE_LOGIND_SLEEP = 1u << 0,
    WALPE_LOGIND_LOCK = 1u << 1,
    WALPE_LOGIND_VT = 1u << 2,
    WALPE_LOGIND_ALL = 7u,
};

static inline guint walpe_logind_facts (const char *env, gboolean backend_is_x11)
{
    if (!env || !*env)
        return backend_is_x11 ? 0 : WALPE_LOGIND_ALL;
    if (!g_ascii_strcasecmp (env, "0") || !g_ascii_strcasecmp (env, "off") ||
        !g_ascii_strcasecmp (env, "no") || !g_ascii_strcasecmp (env, "none"))
        return 0;
    if (!g_ascii_strcasecmp (env, "1") || !g_ascii_strcasecmp (env, "on") ||
        !g_ascii_strcasecmp (env, "yes") || !g_ascii_strcasecmp (env, "all"))
        return WALPE_LOGIND_ALL;
    guint f = 0;
    char **v = g_strsplit_set (env, ", ", -1);
    for (char **p = v; *p; p++) {
        if (!g_ascii_strcasecmp (*p, "sleep") || !g_ascii_strcasecmp (*p, "suspend"))
            f |= WALPE_LOGIND_SLEEP;
        else if (!g_ascii_strcasecmp (*p, "lock"))
            f |= WALPE_LOGIND_LOCK;
        else if (!g_ascii_strcasecmp (*p, "vt") || !g_ascii_strcasecmp (*p, "active"))
            f |= WALPE_LOGIND_VT;
    }
    g_strfreev (v);
    /* a value with no known word in it is not "everything off by typo":
     * it falls back to the backend's default */
    return f ? f : (backend_is_x11 ? 0 : WALPE_LOGIND_ALL);
}

/* asleep, from the facts in use */
static inline gboolean walpe_logind_asleep (guint use, gboolean sleeping, gboolean locked,
                                            gboolean inactive)
{
    return ((use & WALPE_LOGIND_SLEEP) && sleeping) || ((use & WALPE_LOGIND_LOCK) && locked) ||
           ((use & WALPE_LOGIND_VT) && inactive);
}
