/* delete.h - the browser's "Delete wallpaper" flow (delete.c) */
#pragma once
#include <gtk/gtk.h>

typedef struct {
    GtkWindow *parent;
    /* status bar */
    void (*status) (const char *msg, gboolean error);
    /* main thread, once per run that touched files: `path` is the entry
     * that was asked for; gone = it left the library */
    void (*done) (const char *path, gboolean gone);
} WalpeDeleteHost;

/* Check `path` (worker thread), ask, make the engine forget it, move it to
 * the trash. One run at a time: a second call while one is open only
 * raises the open dialog.
 *   title       shown in the dialog
 *   library_dir config wallpapers_dir
 *   in_use      sentence naming the monitors that show it now, or NULL */
void walpe_ui_delete (const WalpeDeleteHost *host, const char *path, const char *title,
                      const char *library_dir, const char *in_use);
gboolean walpe_ui_delete_busy (void);
