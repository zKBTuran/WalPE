/* workshop.h - install Wallpaper Engine workshop items with steamcmd, and
 * find the user's existing Steam WE library.
 *
 * Install: steamcmd is run with an argv array (no shell):
 *   steamcmd +@ShutdownOnFailedCommand 1 +@NoPromptForPassword 1
 *            +login <steam_user> +workshop_download_item 431960 <id> validate +quit
 * WalPE never sees, stores or passes a password: the user logs in to
 * steamcmd once in a terminal (`steamcmd +login <user> +quit`; steamcmd
 * caches the session and Steam Guard), after which the cached login is
 * reused; without it steamcmd fails at once (no prompt) and the job reports
 * how to log in. The downloaded folder is copied (no symlinks followed or
 * created) into <library>/<id> through a hidden "<library>/.<id>.part"
 * directory renamed into place (the library scan skips dot names); a preset's project.json "dependency" is installed too (depth <= 3).
 * Async on the thread-default main context; one steamcmd at a time. */
#pragma once
#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define WALPE_WE_APPID "431960"

/* "3219908811", or a steamcommunity.com .../filedetails/?id=3219908811 URL
 * (also ?l=..&id=.., http/https, with or without www). Digits only,
 * 1..20 of them, nonzero. Returns the id string (g_free) or NULL. */
char *walpe_workshop_parse_id (const char *input);
/* Steam account names: [A-Za-z0-9_], 2..64 chars */
gboolean walpe_workshop_valid_user (const char *user);

typedef struct _WalpeWorkshopJob WalpeWorkshopJob;
typedef void (*WalpeWorkshopDone) (WalpeWorkshopJob *job, gboolean ok,
                                   const char *installed_dir, const char *error,
                                   gpointer user);

/* NULL + err when nothing can start (bad id/user, steamcmd missing) */
WalpeWorkshopJob *walpe_workshop_install (const char *id, const char *steam_user,
                                          const char *library_dir,
                                          WalpeWorkshopDone done, gpointer user,
                                          GError **err);
const char *walpe_workshop_phase (WalpeWorkshopJob *job);  /* human-readable */
const char *walpe_workshop_current_id (WalpeWorkshopJob *job);
const char *walpe_workshop_title (WalpeWorkshopJob *job);  /* after download */
/* the requested item's size (workshop file_size): enables the on-disk
 * download progress; 0 = unknown */
void walpe_workshop_set_expected_size (WalpeWorkshopJob *job, guint64 bytes);
/* 0..1 for the requested item (download 0..0.9, copy 0.9..1), -1 unknown */
double walpe_workshop_progress (WalpeWorkshopJob *job);
/* kill steamcmd, remove partial copies; done is not called */
void walpe_workshop_cancel (WalpeWorkshopJob *job);
void walpe_workshop_free (WalpeWorkshopJob *job);

/* existing Steam installs: every <library>/steamapps/workshop/content/431960
 * dir that exists, from ~/.steam/steam, ~/.local/share/Steam,
 * ~/.var/app/com.valvesoftware.Steam/.local/share/Steam and every "path" in
 * their steamapps/libraryfolders.vdf. NULL-terminated, g_strfreev. */
char **walpe_workshop_steam_libraries (void);

/* Steam library roots (realpath, deduplicated): ~/.local/share/Steam,
 * ~/.steam/steam, ~/.steam/root, the Flatpak Steam root, and every "path"
 * of their steamapps/ and config/ libraryfolders.vdf. NULL-terminated,
 * g_strfreev. */
char **walpe_steam_library_roots (void);

/* search, details, previews, tag taxonomy (version 0.11) */
#include "workshop_query.h"

G_END_DECLS
