/* wlappid.h - one application id for every window of the GUI on Wayland.
 * See wlappid.c. */
#pragma once
#include <glib.h>

/* Is this file's work wanted for a toplevel? For every Wayland toplevel.
 * GTK names a window of the GtkApplication after the application only
 * when the process has a session bus (its Wayland code for application
 * windows is part of its D-Bus code); without one - a compositor started
 * from a console with no dbus-run-session - the library window said
 * "walpe" like the dialogs once did (seen with GTK 4.24 on labwc 0.20).
 * Saying the id a window already has changes nothing. Pure;
 * tests/ui_session_test.c. */
static inline gboolean walpe_wl_app_id_wanted (gboolean wayland_toplevel, gboolean application_window)
{
    (void) application_window;
    return wayland_toplevel;
}

/* From now on every toplevel of this process says app_id on a Wayland
 * session (after the display is open; main thread). Does nothing on X11,
 * where WM_CLASS names the program for all of them, and nothing in a
 * build without GTK's Wayland backend. app_id must stay valid. */
void walpe_wl_app_id_follow (const char *app_id);
