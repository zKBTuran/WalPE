/* resdialog.h - the windows of the per-monitor render resolution:
 *
 *   - the "Monitor resolution" dialog: one row per connected monitor, a
 *     choice per row (the monitor's own size, the presets, a custom size),
 *     "Apply" and "Keep native for all". It opens by itself once per run
 *     when the window starts and the engine reports monitors nobody was
 *     asked about, and on request (application action, tray, notification).
 *     Nothing is sent before a button is pressed; closing it changes
 *     nothing.
 *   - the same choice as rows of the Engine settings window, where a change
 *     applies at once like every other setting there, with the texture
 *     limit and the video size limit next to it.
 *   - the announcement of a monitor that is connected while the window
 *     runs: a desktop notification that opens the dialog for that monitor.
 *     Never a window of its own accord, and nothing at all while a
 *     fullscreen application has the focus.
 *
 * The rules are in resmodel.c (no GTK, tested without a display); this
 * file is widgets. All daemon requests go through uiipc: nothing here
 * waits for the engine on the main thread. */
#pragma once
#include <gtk/gtk.h>
#include <json-glib/json-glib.h>
#include "resmodel.h"

/* the application action that opens the dialog; parameter "as" = the output
 * names to show, an empty array = every connected monitor */
#define WALPE_RES_ACTION WALPE_MONRES_ACTION

typedef struct {
    GtkApplication *app;        /* sends the notification */
    GtkWindow *parent;          /* the main window: the dialog is transient for it */
    /* the main window's status bar (may be NULL) */
    void (*status) (const char *msg, gboolean error);
} WalpeResUiHost;

/* once, when the main window exists */
void walpe_res_ui_init (const WalpeResUiHost *host);
/* every status reply's "state" object as it arrives (poll or push);
 * NULL = no engine. Opens the start-up question, sends and takes back the
 * announcements, keeps open windows current. */
void walpe_res_ui_status (JsonObject *state);
/* open the dialog, or raise it. only = NULL-terminated output names to
 * show (the others are left out), NULL or empty = every connected monitor */
void walpe_res_ui_show_dialog (const char *const *only);
/* the "Resolution" rows for the Engine settings window: one widget, to be
 * attached across the settings grid. Created once. */
GtkWidget *walpe_res_ui_settings_new (void);
/* the main window is being destroyed: the dialog goes with it and nothing
 * here touches a widget afterwards */
void walpe_res_ui_shutdown (void);
