/* workshop_query.h - browse/search the Steam Workshop of Wallpaper Engine
 * (app 431960): search, item details, preview thumbnails, tag taxonomy.
 *
 * Endpoints (no account, no password, no cookies):
 *   details  POST https://api.steampowered.com/ISteamRemoteStorage/GetPublishedFileDetails/v1/
 *            (no key needed; itemcount + publishedfileids[n], <= 100 per call)
 *   search   with a Steam Web API key (optional, user-entered, see
 *            WalpeWsSettings): GET .../IPublishedFileService/QueryFiles/v1/
 *            without one: the public browse page
 *            https://steamcommunity.com/workshop/browse/?appid=431960&... ;
 *            the item ids (+ star rating, author name, totals) are read from
 *            its embedded <script id="valve-ssr-data"> JSON, falling back to
 *            the filedetails/?id= links; details then come from
 *            GetPublishedFileDetails.
 *   authors  with a key ISteamUser/GetPlayerSummaries/v2, else the public
 *            https://steamcommunity.com/profiles/<id>/?xml=1
 *   previews the item's preview_url (Steam UGC CDN, https only), cached on
 *            disk under ~/.cache/walpe/workshop (LRU-pruned to 300 MB).
 *   subscribe / unsubscribe (version 0.12, opt-in, key required):
 *            POST .../IPublishedFileService/Subscribe/v1/ and .../Unsubscribe/v1/
 *            form: key, publishedfileid, list_type=1, appid=431960,
 *            notify_client=1. They act on the account the key belongs to.
 *            Valve does NOT document these two calls (they are listed in
 *            community references and answer 403 "verify your key=" without
 *            a key): they may change or be refused, so a failure is always
 *            reported and never assumed away. See steamsub.h.
 *
 * Politeness: one User-Agent naming WalPE, per-host minimum spacing
 * (community pages 1.5 s, Web API 0.25 s), <= 4 connections per CDN host,
 * 20 s I/O timeout, one retry on 5xx/timeouts (never for Subscribe /
 * Unsubscribe: they are sent once), 429 = clear "rate limited" error.
 *
 * What a reply can do is bounded: it is read up to the call's size limit
 * and dropped beyond it (counted after decompression), a request ends
 * after 120 s in all (Subscribe / Unsubscribe: 12 s), redirects are
 * followed only where walpe_ws_redirect_allowed() says so (the Web API:
 * never), JSON nested deeper than 128 levels is refused, and item counts
 * and string lengths are cut to a few times Steam's own limits. The API key is only ever put in the request URL: never logged,
 * never in an error message (errors are scrubbed).
 *
 * All async functions must be called from the thread that owns the main
 * context (libsoup sessions are single-threaded); callbacks run there too.
 * Requires libsoup-3.0 at build time (HAVE_SOUP); without it every
 * operation fails with G_IO_ERROR_NOT_SUPPORTED. */
#pragma once
#include <glib.h>
#include <gio/gio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

G_BEGIN_DECLS

/* ------------------------------------------------------------- taxonomy */

typedef enum {
    WALPE_WS_SORT_TREND,       /* "Most Popular (One Week)" = Steam trend, 7 days */
    WALPE_WS_SORT_POPULAR,     /* all-time popular (trend, days=-1 / RankedByVote) */
    WALPE_WS_SORT_RECENT,      /* most recent */
    WALPE_WS_SORT_SUBSCRIBED,  /* most (unique) subscribed */
    WALPE_WS_SORT_RELEVANCE,   /* text relevance (needs search text) */
    WALPE_WS_SORT_N
} WalpeWsSort;

const char *walpe_ws_sort_label (WalpeWsSort s);

/* Wallpaper Engine's workshop tags, exactly as the workshop stores them
 * (verified against the live browse page's tag list, 2026-11-08).
 * single = Steam shows the category as a <select> (one value at a time). */
typedef struct {
    const char *name;            /* "Type", "Age Rating", "Genre", ... */
    gboolean single;
    const char *const *tags;     /* NULL-terminated */
} WalpeWsTagCategory;

const WalpeWsTagCategory *walpe_ws_tag_categories (guint *n);
/* the canonical spelling of a known tag (case-insensitive match), or NULL */
const char *walpe_ws_tag_canonical (const char *tag);

/* ------------------------------------------------------------------ items */

typedef struct {
    int refs;
    char *id;
    char *title;
    char *description;     /* Steam BBCode; walpe_ws_bbcode_to_text() for display */
    char *preview_url;
    char *creator;         /* SteamID64 */
    char *author;          /* persona name when known, else NULL */
    char **tags;           /* NULL-terminated, never NULL */
    guint64 file_size;
    guint64 subscriptions, favorited, views;
    gint64 time_created, time_updated;
    int stars;             /* 0..5 from the browse page, -1 unknown */
    int votes;             /* total votes, -1 unknown */
    double score;          /* 0..1 vote score (Web API), -1 unknown */
    int result;            /* Steam EResult; 1 = OK, 9 = not found/hidden */
    gboolean banned;
} WalpeWsItem;

WalpeWsItem *walpe_ws_item_ref (WalpeWsItem *it);
void walpe_ws_item_unref (WalpeWsItem *it);
gboolean walpe_ws_item_has_tag (const WalpeWsItem *it, const char *tag);
/* plain text from Steam BBCode ([b], [url=..]x[/url], [list][*], [h1], ...) */
char *walpe_ws_bbcode_to_text (const char *bbcode);

typedef struct {
    const char *text;          /* search text or NULL */
    WalpeWsSort sort;
    guint page;                /* 1-based */
    const char *const *required;   /* tags every result has (AND), may be NULL */
    const char *const *excluded;   /* tags no result has, may be NULL; also
                                    * enforced client-side on what Steam returns */
} WalpeWsQuery;

#define WALPE_WS_PER_PAGE 30

typedef struct {
    GPtrArray *items;          /* WalpeWsItem*, page order */
    guint64 total;             /* matching items (Steam's count), 0 unknown */
    guint page, total_pages;
    const char *source;        /* "webapi" | "page" (static string) */
    char *notice;              /* non-fatal note for the UI (e.g. key rejected) */
} WalpeWsResults;

void walpe_ws_results_free (WalpeWsResults *r);

/* -------------------------------------------------------------- settings */

/* ~/.config/walpe/workshop.json, written 0600 (it may hold the key) */
typedef struct {
    char *api_key;             /* optional Steam Web API key, NULL = none */
    gboolean hide_mature;      /* default TRUE */
    gboolean hide_questionable;/* default FALSE */
    /* version 0.12: both need api_key and are off by default */
    gboolean subscribe_on_download;  /* Workshop tab: subscribe the account to
                                      * what it downloads */
    gboolean unsubscribe_on_delete;  /* last state of the delete dialog's
                                      * "Also unsubscribe on Steam" box */
} WalpeWsSettings;

WalpeWsSettings *walpe_ws_settings_load (void);
gboolean walpe_ws_settings_save (const WalpeWsSettings *s, GError **err);
void walpe_ws_settings_free (WalpeWsSettings *s);
/* Steam Web API keys are 32 hex digits */
gboolean walpe_ws_valid_api_key (const char *key);
/* load, change that one field, save (the Workshop page and the delete
 * dialog each hold their own copy of the settings) */
void walpe_ws_settings_set_unsubscribe_on_delete (gboolean on);

/* ---------------------------------------------------------------- client */

typedef struct _WalpeWsClient WalpeWsClient;

gboolean walpe_ws_available (void);              /* built with libsoup */
WalpeWsClient *walpe_ws_client_new (void);
void walpe_ws_client_free (WalpeWsClient *c);    /* cancels nothing: cancel first */
void walpe_ws_client_set_api_key (WalpeWsClient *c, const char *key_or_null);
gboolean walpe_ws_client_has_api_key (WalpeWsClient *c);
char *walpe_ws_cache_dir (void);                 /* ~/.cache/walpe/workshop */

void walpe_ws_search_async (WalpeWsClient *c, const WalpeWsQuery *q, GCancellable *cancel,
                            GAsyncReadyCallback cb, gpointer user);
WalpeWsResults *walpe_ws_search_finish (GAsyncResult *res, GError **err);

/* details of up to 100 ids; items with result != 1 are kept (caller decides) */
void walpe_ws_details_async (WalpeWsClient *c, const char *const *ids, GCancellable *cancel,
                             GAsyncReadyCallback cb, gpointer user);
GPtrArray *walpe_ws_details_finish (GAsyncResult *res, GError **err);

/* Subscribe the Web API key's account to workshop item `id`, or
 * unsubscribe it. Needs a key (G_IO_ERROR_NOT_INITIALIZED without one).
 * TRUE only when Steam answered HTTP 200, EResult OK and a "response"
 * object; any other answer is an error whose message can be shown as is
 * (the key never appears in it). Undocumented by Valve: see the top. */
void walpe_ws_subscribe_async (WalpeWsClient *c, const char *id, gboolean subscribe,
                               GCancellable *cancel, GAsyncReadyCallback cb, gpointer user);
gboolean walpe_ws_subscribe_finish (GAsyncResult *res, GError **err);
/* where the Web API calls go: https://api.steampowered.com/, or (tests) the
 * loopback mock named by WALPE_WS_API_BASE=http://127.0.0.1:<port>/ - any
 * other value of that variable is ignored */
const char *walpe_ws_api_base (void);

/* persona name of a SteamID64 (cached in memory) */
void walpe_ws_author_async (WalpeWsClient *c, const char *steamid, GCancellable *cancel,
                            GAsyncReadyCallback cb, gpointer user);
char *walpe_ws_author_finish (GAsyncResult *res, GError **err);

/* preview image scaled to fit max_w x max_h (first frame of a GIF).
 * small = ask the CDN for a ~256 px rendition (grid tiles). Disk-cached. */
void walpe_ws_preview_async (WalpeWsClient *c, const char *id, const char *url,
                             gboolean small, int max_w, int max_h, GCancellable *cancel,
                             GAsyncReadyCallback cb, gpointer user);
GdkPixbuf *walpe_ws_preview_finish (GAsyncResult *res, GError **err);
/* the disk-cached download behind a finished preview (g_free), NULL when
 * there is none: the caller can play it when it is an animated GIF. No
 * request of its own. */
char *walpe_ws_preview_file (GAsyncResult *res);

/* ----------------------------------------------- parsers (also for tests) */
/* may a redirect of a reply be followed to `to`? cls: 0 a steamcommunity.com
 * page (https + that host only), 1 the Web API (never), 2 a preview picture
 * (https + Valve's image hosts only) */
gboolean walpe_ws_redirect_allowed (int cls, const char *to);

char *walpe_ws_browse_url (const WalpeWsQuery *q);
char *walpe_ws_queryfiles_url (const WalpeWsQuery *q, const char *api_key);
/* ids in page order (char*), page items by id (WalpeWsItem*, partial: no
 * full description), totals. FALSE + err when the page holds neither the
 * SSR data nor any item link (error / captcha / rate-limit page). */
gboolean walpe_ws_parse_browse_page (const char *html, gsize len, GPtrArray *ids,
                                     GHashTable *items, guint64 *total,
                                     guint *total_pages, GError **err);
GPtrArray *walpe_ws_parse_details (const char *json, gssize len, GError **err);
GPtrArray *walpe_ws_parse_queryfiles (const char *json, gssize len, guint64 *total,
                                      GError **err);

G_END_DECLS
