/* wlappid.h - one application id for every window of the GUI on Wayland.
 * See wlappid.c. */
#pragma once
#include <glib.h>

/* Is this file's work wanted for a toplevel? Only where the toolkit would
 * name the window after the program ("walpe"): a Wayland toplevel that is
 * not a window of the GtkApplication (those carry its id already). Pure;
 * tests/ui_session_test.c. */
static inline gboolean walpe_wl_app_id_wanted (gboolean wayland_toplevel, gboolean application_window)
{
    return wayland_toplevel && !application_window;
}

/* From now on every toplevel of this process says app_id on a Wayland
 * session (after the display is open; main thread). Does nothing on X11,
 * where WM_CLASS names the program for all of them, and nothing in a
 * build without GTK's Wayland backend. app_id must stay valid. */
void walpe_wl_app_id_follow (const char *app_id);
