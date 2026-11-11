/* traysel.h - which tray protocol walpe-tray uses, as a pure function
 * (main.c and tests/tray_select_test.c include it; no GTK, no X).
 *
 * Two protocols exist. StatusNotifierItem travels over the session bus
 * and works under X11 and Wayland alike; XEmbed is an X11 protocol
 * (_NET_SYSTEM_TRAY_S<n>) and the only one dwm's systray, stalonetray,
 * trayer and polybar speak.
 *
 * On an X11 session `auto` keeps its rule: StatusNotifierItem when a
 * watcher exists and a host is registered with it (a stray watcher with
 * nothing that shows icons would hide the icon), XEmbed otherwise.
 *
 * On a Wayland session there is no XEmbed tray to dock into (Xwayland has
 * no tray manager; without Xwayland there is no X at all). `auto` is
 * StatusNotifierItem there, whether or not a watcher is up yet: the item
 * registers itself when one appears (sni.c watches the name). XEmbed is
 * used only when it is asked for by name and an X display exists (an X11
 * tray program running under Xwayland). */
#pragma once
#include <glib.h>
#include <string.h>

typedef enum {
    TRAY_PICK_NONE,     /* nothing can show an icon; *note says why */
    TRAY_PICK_SNI,
    TRAY_PICK_XEMBED,
} TrayPick;

typedef struct {
    const char *want;       /* "auto", "sni", "xembed"; NULL / "" / unknown = auto */
    gboolean have_bus;      /* a session bus connection */
    gboolean watcher;       /* org.kde.StatusNotifierWatcher has an owner */
    gboolean host;          /* ... and reports a registered host */
    gboolean session_x11;   /* the toolkit runs on X11 (not Wayland) */
    gboolean x_display;     /* DISPLAY is set (Xwayland counts) */
    gboolean built_xembed;  /* this build holds the XEmbed backend */
} TrayEnv;

/* *note (static text, may be NULL): something the user should read in the
 * log - why no icon can appear, or what the icon is waiting for. NULL =
 * nothing to say. */
static inline TrayPick tray_pick (const TrayEnv *e, const char **note)
{
    const char *n = NULL;
    TrayPick p;
    const char *want = e->want && *e->want ? e->want : "auto";
    if (!strcmp (want, "sni")) {
        p = e->have_bus ? TRAY_PICK_SNI : TRAY_PICK_NONE;
        if (!e->have_bus)
            n = "StatusNotifierItem was asked for but there is no session bus";
    } else if (!strcmp (want, "xembed")) {
        if (!e->built_xembed) {
            p = TRAY_PICK_NONE;
            n = "XEmbed was asked for but this build has no X11 tray backend";
        } else if (!e->x_display) {
            p = TRAY_PICK_NONE;
            n = "XEmbed was asked for but DISPLAY is not set (XEmbed is an X11 protocol)";
        } else {
            p = TRAY_PICK_XEMBED;
        }
    } else if (e->session_x11 && e->built_xembed) {
        /* the X11 rule, unchanged */
        p = e->have_bus && e->watcher && e->host ? TRAY_PICK_SNI : TRAY_PICK_XEMBED;
    } else if (!e->have_bus) {
        p = TRAY_PICK_NONE;
        n = e->session_x11
                ? "no session bus and no X11 tray backend in this build: no tray icon"
                : "no session bus: a tray icon on Wayland needs one (StatusNotifierItem "
                  "travels over D-Bus)";
    } else {
        p = TRAY_PICK_SNI;
        if (!e->watcher)
            n = "no StatusNotifierWatcher on the session bus yet: the icon appears when a "
                "panel with a tray starts (KDE Plasma, waybar, swaybar, xfce4-panel; GNOME "
                "Shell needs the AppIndicator / KStatusNotifierItem extension)";
        else if (!e->host)
            n = "a StatusNotifierWatcher runs but no panel has registered as a host yet: the "
                "icon appears when one does";
    }
    if (note)
        *note = n;
    return p;
}
