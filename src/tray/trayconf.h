/* trayconf.h - walpe-tray settings (~/.config/walpe/tray.ini) and the
 * helpers the GUI uses to start / stop the tray. GTK-free: compiled into
 * walpe-tray and the walpe GUI. */
#pragma once
#include <glib.h>

#define WALPE_TRAY_APP_ID "io.github.walpe.tray"
#define WALPE_UI_APP_ID   "io.github.walpe.ui"
#define WALPE_UI_OBJECT   "/io/github/walpe/ui"
#define WALPE_TRAY_OBJECT "/io/github/walpe/tray"

typedef struct {
    gboolean start_with_gui;  /* the GUI starts walpe-tray when it opens */
    gboolean confirm_quit;    /* "Quit" asks first */
    char *backend;            /* auto | sni | xembed */
} WalpeTrayConf;

WalpeTrayConf *walpe_tray_conf_load (void);
void           walpe_tray_conf_save (const WalpeTrayConf *c);
void           walpe_tray_conf_free (WalpeTrayConf *c);

/* a program next to the running executable (build dir / install prefix),
 * else the bare name for a PATH lookup. g_free() */
char    *walpe_tray_sibling_exe (const char *name);
/* walpe-tray owns its D-Bus name (<= timeout_ms; FALSE without a bus) */
gboolean walpe_tray_is_running (int timeout_ms);
/* start walpe-tray detached; FALSE + message on failure */
gboolean walpe_tray_spawn (GError **err);
/* ask a running walpe-tray to exit (the engine keeps running) */
void     walpe_tray_request_quit (void);
