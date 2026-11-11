/* appearance.h - the desktop's dark / light preference for the GUI on
 * sessions where GTK does not pick it up by itself. See appearance.c. */
#pragma once
#include <glib.h>

/* the value of org.freedesktop.appearance color-scheme -> what
 * gtk-application-prefer-dark-theme should be: 1 = prefer dark, 2 = prefer
 * light, anything else = no preference (keep `base`, the value GTK had
 * from its own settings). Pure; tests/ui_session_test.c. */
static inline gboolean walpe_appearance_dark (guint32 color_scheme, gboolean base)
{
    return color_scheme == 1 ? TRUE : color_scheme == 2 ? FALSE : base;
}

/* Is following the portal this file's job in this session? Not on X11
 * (the theme arrives through XSETTINGS / settings.ini there and nothing
 * changes), not when GTK_THEME names a theme, not on a GTK that follows
 * the preference by itself (4.20 and later). Pure. */
static inline gboolean walpe_appearance_wanted (gboolean session_x11, const char *gtk_theme_env,
                                                guint gtk_major, guint gtk_minor)
{
    if (session_x11)
        return FALSE;
    if (gtk_theme_env && *gtk_theme_env)
        return FALSE;
    return !(gtk_major > 4 || (gtk_major == 4 && gtk_minor >= 20));
}

/* Start following (after the display is open; main thread). Does nothing
 * where walpe_appearance_wanted says so, or without a session bus / portal. */
void walpe_appearance_follow (void);
