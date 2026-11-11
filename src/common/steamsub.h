/* steamsub.h - Steam Workshop subscriptions and WalPE (since 0.12).
 *
 * What Steam lets a third-party program do about a subscription (state of
 * November 2026; Valve documents neither of the two Web API calls):
 *   steamcmd     no subscribe / unsubscribe command exists. Its
 *                workshop_download_item downloads WITHOUT subscribing, so
 *                what the Workshop tab downloaded is never restored by Steam.
 *   steam://     no URL subscribes or unsubscribes. steam://url/
 *                CommunityFilePage/<id> (the URL Valve's own Workshop guide
 *                uses) shows the item's page in the running Steam client;
 *                the user clicks "Subscribe" / "Subscribed" there.
 *   Web API      IPublishedFileService/Subscribe|Unsubscribe/v1 take the
 *                user's own Web API key (workshop_query.h). Valve does not
 *                document them: opt-in, and every failure is reported.
 *   Steamworks   ISteamUGC::SubscribeItem/UnsubscribeItem would need Valve's
 *                libsteam_api.so (not ours to ship) and a process posing as
 *                Wallpaper Engine: not done.
 *
 * Nothing here touches the Steam password, Steam's cookies or its login
 * tokens. "Is Steam running" is decided the way Valve's own steam.sh does
 * (is_steam_running): ~/.steam/steam.pid names a live process that has
 * ~/.steam/steam.pipe open. Only then is `steam <url>` run - the launcher
 * hands the URL to the running client through that pipe and exits at once;
 * with no client running the same command would start Steam and stay.
 * So without a running client the https page is opened in the browser. */
#pragma once
#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/* a published file id: digits only, 1..20 of them, not zero */
gboolean walpe_steam_valid_id (const char *id);

/* https://steamcommunity.com/sharedfiles/filedetails/?id=<id>  (g_free) */
char *walpe_steam_item_web_url (const char *id);
/* steam://url/CommunityFilePage/<id>  (g_free) */
char *walpe_steam_item_client_url (const char *id);

/* the user's (native) Steam client is running right now. A Flatpak Steam
 * is not seen: its pid file holds a pid of the sandbox's own namespace. */
gboolean walpe_steam_client_running (void);

/* Wallpaper Engine is installed in a Steam client library
 * (steamapps/appmanifest_431960.acf): that client downloads the items the
 * account is subscribed to into its own workshop folder. */
gboolean walpe_steam_we_installed (void);

typedef enum {
    WALPE_STEAM_PAGE_CLIENT,   /* shown in the running Steam client */
    WALPE_STEAM_PAGE_BROWSER,  /* https page, default web browser */
} WalpeSteamPageWhere;

/* The command that shows the item's Workshop page (NULL-terminated argv,
 * g_strfreev):
 *   Steam running     steam steam://url/CommunityFilePage/<id>
 *                     (xdg-open steam://... when there is no `steam` in PATH)
 *   otherwise         xdg-open https://steamcommunity.com/sharedfiles/...
 * NULL with *where set to BROWSER when there is no xdg-open either: the
 * caller opens the web URL itself (walpe_steam_open_item_page does, through
 * GIO's default handler). NULL + err for a bad id. */
char **walpe_steam_item_page_argv (const char *id, WalpeSteamPageWhere *where, GError **err);

/* Show the item's Workshop page. The command is started detached (own
 * session, no stdio) and this returns at once; nothing waits for Steam or
 * the browser. FALSE + err when nothing could be started. */
gboolean walpe_steam_open_item_page (const char *id, WalpeSteamPageWhere *where, GError **err);

/* Items WalPE itself subscribed the account to (Workshop tab, "Subscribe on
 * Steam when downloading"): ids, one per line, in
 * ~/.config/walpe/workshop-subscribed. The delete dialog offers to
 * unsubscribe them although no copy lies in a Steam client folder. */
gboolean walpe_steam_sub_is_marked (const char *id);
void     walpe_steam_sub_mark (const char *id, gboolean subscribed);

G_END_DECLS
