/* workshop_page.h - "Workshop" page: search/browse Wallpaper Engine's Steam
 * Workshop (src/common/workshop_query.h), filter by WE's tags, details,
 * Download through the daemon's steamcmd install (cached login only).
 *
 * The page is a plain widget so the main GUI can host it as a tab;
 * walpe_workshop_window_present() shows it in its own window instead. */
#pragma once
#include <gtk/gtk.h>

typedef struct {
    char *(*ipc) (const char *json);   /* daemon request -> reply (g_free) or NULL */
    void (*status) (const char *msg);  /* main window status line, may be NULL */
    void (*library_changed) (void);    /* rescan the library view, may be NULL */
    const char *library_dir;           /* WalPE library (config wallpapers_dir), copied */
} WalpeWorkshopHooks;

GtkWidget *walpe_workshop_page_new_with_hooks (const WalpeWorkshopHooks *hooks);
/* the main GUI's tab (-DWALPE_HAVE_WORKSHOP_PAGE): own daemon IPC, library
 * dir from the config */
GtkWidget *walpe_workshop_page_new (void);
/* the same, telling the host when an item arrived somewhere its own
 * directory monitor does not look: a subscribed item that the Steam client
 * downloaded into the Steam library */
GtkWidget *walpe_workshop_page_new_hosted (void (*library_changed) (void));
/* one window per process; presents the existing one when open */
void walpe_workshop_window_present (GtkWindow *parent, const WalpeWorkshopHooks *hooks);
