/* workshop_query.c - Steam Workshop browsing for Wallpaper Engine items
 * (see workshop_query.h for the endpoints and the politeness rules) */
#include "walpe-build-config.h"
#include "workshop_query.h"
#include "workshop.h"
#include "util.h"
#include "uitext.h"
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#ifdef HAVE_SOUP
#include <libsoup/soup.h>
#endif

#define WS_APPID 431960
#define BROWSE_BASE "https://steamcommunity.com/workshop/browse/"
#define API_BASE "https://api.steampowered.com/"
#define CACHE_MAX_BYTES  (300u * 1024 * 1024)
#define CACHE_KEEP_BYTES (240u * 1024 * 1024)

/* ------------------------------------------------------------- taxonomy */

/* Verified 2026-11-08 against the tag list embedded in
 * https://steamcommunity.com/workshop/browse/?appid=431960 (and the "tags"
 * of the 30 local project.json files: Anime, Unspecified, Music, Fantasy,
 * Technology, Pixel art, Abstract, Landscape - all spelled the same). */
static const char *const tags_type[] = { "Scene", "Video", "Web", "Application", NULL };
static const char *const tags_category[] = { "Wallpaper", "Preset", "Asset", NULL };
static const char *const tags_age[] = { "Everyone", "Questionable", "Mature", NULL };
static const char *const tags_genre[] = {
    "Abstract", "Animal", "Anime", "Cartoon", "CGI", "Cyberpunk", "Fantasy", "Game",
    "Girls", "Guys", "Landscape", "Medieval", "Memes", "MMD", "Music", "Nature",
    "Pixel art", "Relaxing", "Retro", "Sci-Fi", "Sports", "Technology", "Television",
    "Vehicle", "Unspecified", NULL };
static const char *const tags_resolution[] = {
    "Standard Definition", "1280 x 720", "1366 x 768", "1920 x 1080", "2560 x 1440",
    "3840 x 2160", "Ultrawide Standard Definition", "Ultrawide 2560 x 1080",
    "Ultrawide 3440 x 1440", "Dual Standard Definition", "Dual 3840 x 1080",
    "Dual 5120 x 1440", "Dual 7680 x 2160", "Triple Standard Definition",
    "Triple 4096 x 768", "Triple 5760 x 1080", "Triple 7680 x 1440", "Triple 11520 x 2160",
    "Portrait Standard Definition", "Portrait 720 x 1280", "Portrait 1080 x 1920",
    "Portrait 1440 x 2560", "Portrait 2160 x 3840", "Other resolution",
    "Dynamic resolution", NULL };
static const char *const tags_misc[] = {
    "Approved", "Audio responsive", "Customizable", "3D", "HDR", "Media Integration",
    "Puppet Warp", "User Shortcut", "Video Texture", "Asset Pack", NULL };

static const WalpeWsTagCategory categories[] = {
    { "Type", TRUE, tags_type },
    { "Category", TRUE, tags_category },
    { "Age Rating", TRUE, tags_age },
    { "Resolution", TRUE, tags_resolution },
    { "Genre", FALSE, tags_genre },
    { "Miscellaneous", FALSE, tags_misc },
};

const WalpeWsTagCategory *walpe_ws_tag_categories (guint *n)
{
    if (n)
        *n = G_N_ELEMENTS (categories);
    return categories;
}

const char *walpe_ws_tag_canonical (const char *tag)
{
    if (!tag)
        return NULL;
    for (guint c = 0; c < G_N_ELEMENTS (categories); c++)
        for (const char *const *t = categories[c].tags; *t; t++)
            if (!g_ascii_strcasecmp (*t, tag))
                return *t;
    return NULL;
}

const char *walpe_ws_sort_label (WalpeWsSort s)
{
    switch (s) {
    case WALPE_WS_SORT_TREND: return "Trending (this week)";
    case WALPE_WS_SORT_POPULAR: return "Most popular (all time)";
    case WALPE_WS_SORT_RECENT: return "Most recent";
    case WALPE_WS_SORT_SUBSCRIBED: return "Most subscribed";
    case WALPE_WS_SORT_RELEVANCE: return "Relevance";
    default: return "?";
    }
}

/* ------------------------------------------------------------------ items */

static WalpeWsItem *item_new (void)
{
    WalpeWsItem *it = g_new0 (WalpeWsItem, 1);
    it->refs = 1;
    it->stars = -1;
    it->votes = -1;
    it->score = -1;
    it->result = 1;
    it->tags = g_new0 (char *, 1);
    return it;
}

WalpeWsItem *walpe_ws_item_ref (WalpeWsItem *it)
{
    if (it)
        g_atomic_int_inc (&it->refs);
    return it;
}

void walpe_ws_item_unref (WalpeWsItem *it)
{
    if (!it || !g_atomic_int_dec_and_test (&it->refs))
        return;
    g_free (it->id);
    g_free (it->title);
    g_free (it->description);
    g_free (it->preview_url);
    g_free (it->creator);
    g_free (it->author);
    g_strfreev (it->tags);
    g_free (it);
}

gboolean walpe_ws_item_has_tag (const WalpeWsItem *it, const char *tag)
{
    if (!it || !tag)
        return FALSE;
    for (char **t = it->tags; t && *t; t++)
        if (!g_ascii_strcasecmp (*t, tag))
            return TRUE;
    return FALSE;
}

void walpe_ws_results_free (WalpeWsResults *r)
{
    if (!r)
        return;
    if (r->items)
        g_ptr_array_unref (r->items);
    g_free (r->notice);
    g_free (r);
}

/* ----------------------------------------------------------------- bbcode */

static gboolean bb_tag_is (const char *name, const char *const *set)
{
    for (; *set; set++)
        if (!g_ascii_strcasecmp (name, *set))
            return TRUE;
    return FALSE;
}

char *walpe_ws_bbcode_to_text (const char *bb)
{
    if (!bb)
        return g_strdup ("");
    static const char *const inline_tags[] = { "b", "i", "u", "strike", "spoiler", "noparse",
                                               "url", "quote", "code", "table", "tr", "td", "th",
                                               "olist", "list", "h1", "h2", "h3", "hr", NULL };
    static const char *const block_tags[] = { "h1", "h2", "h3", "list", "olist", "quote",
                                              "code", "table", "tr", "hr", NULL };
    static const char *const skip_tags[] = { "img", "previewyoutube", "video", NULL };
    GString *o = g_string_sized_new (strlen (bb));
    const char *p = bb;
    while (*p) {
        if (*p == '\r') {
            p++;
            continue;
        }
        if (*p != '[') {
            g_string_append_c (o, *p++);
            continue;
        }
        const char *end = strchr (p, ']');
        if (!end || end - p > 300) {
            g_string_append_c (o, *p++);
            continue;
        }
        gboolean closing = p[1] == '/';
        const char *n0 = p + 1 + closing;
        gsize nl = strcspn (n0, "]= ");
        if (nl == 0 || nl > 20 || n0 + nl > end) {
            g_string_append_c (o, *p++);
            continue;
        }
        char *name = g_strndup (n0, nl);
        if (!g_strcmp0 (name, "*")) {
            g_string_append (o, "\n• ");
        } else if (!closing && bb_tag_is (name, skip_tags)) {
            /* drop the whole element: its content is a URL / video id */
            char *close = g_strdup_printf ("[/%s]", name);
            const char *c = strstr (end + 1, close);
            if (!c) {   /* [previewyoutube=..;full][/previewyoutube] style or unclosed */
                c = end + 1;
                end = c - 1;
            } else {
                end = c + strlen (close) - 1;
            }
            g_free (close);
        } else if (closing && bb_tag_is (name, skip_tags)) {
            /* stray closer */
        } else if (bb_tag_is (name, inline_tags)) {
            if (bb_tag_is (name, block_tags))
                g_string_append_c (o, '\n');
            /* [url=X][/url] with empty text: show X */
            if (!closing && !g_ascii_strcasecmp (name, "url") && n0[nl] == '=' &&
                g_str_has_prefix (end + 1, "[/url]"))
                g_string_append_len (o, n0 + nl + 1, end - (n0 + nl + 1));
        } else {
            g_free (name);
            g_string_append_c (o, *p++);   /* not BBCode: keep literally */
            continue;
        }
        g_free (name);
        p = end + 1;
    }
    /* at most one blank line in a row, no trailing/leading space */
    GString *t = g_string_sized_new (o->len);
    int nls = 0;
    for (gsize i = 0; i < o->len; i++) {
        char ch = o->str[i];
        if (ch == '\n') {
            if (++nls > 2)
                continue;
        } else {
            nls = 0;
        }
        g_string_append_c (t, ch);
    }
    g_string_free (o, TRUE);
    char *s = g_string_free (t, FALSE);
    g_strstrip (s);
    return s;
}

/* ------------------------------------------------------------------- urls */

/* The Web API host. Tests only: a mock of api.steampowered.com on this
 * machine (WALPE_WS_API_BASE=http://127.0.0.1:<port>/). Plain http to the
 * literal loopback address and nothing else - no user info, no other host,
 * no path - so a variable cannot send the key off this machine. */
const char *walpe_ws_api_base (void)
{
    const char *e = g_getenv ("WALPE_WS_API_BASE");
    if (!e || !*e)
        return API_BASE;
    GUri *u = g_uri_parse (e, G_URI_FLAGS_NONE, NULL);
    gboolean ok = u && !g_strcmp0 (g_uri_get_scheme (u), "http") &&
                  !g_strcmp0 (g_uri_get_host (u), "127.0.0.1") && !g_uri_get_userinfo (u) &&
                  g_uri_get_port (u) > 0 && !g_strcmp0 (g_uri_get_path (u), "/") &&
                  !g_uri_get_query (u) && !g_uri_get_fragment (u);
    if (u)
        g_uri_unref (u);
    return ok ? e : API_BASE;
}

static void q_add (GString *u, const char *k, const char *v)
{
    char *e = g_uri_escape_string (v, NULL, FALSE);
    g_string_append_printf (u, "&%s=%s", k, e);
    g_free (e);
}

static gboolean has_text (const WalpeWsQuery *q)
{
    return q->text && *q->text;
}

char *walpe_ws_browse_url (const WalpeWsQuery *q)
{
    static const char *const sorts[] = { "trend", "trend", "mostrecent",
                                         "totaluniquesubscribers", "textsearch" };
    WalpeWsSort s = q->sort < WALPE_WS_SORT_N ? q->sort : WALPE_WS_SORT_TREND;
    if (s == WALPE_WS_SORT_RELEVANCE && !has_text (q))
        s = WALPE_WS_SORT_TREND;
    GString *u = g_string_new (BROWSE_BASE "?appid=431960");
    if (has_text (q))
        q_add (u, "searchtext", q->text);
    q_add (u, "browsesort", sorts[s]);
    q_add (u, "section", "readytouseitems");
    q_add (u, "actualsort", sorts[s]);
    if (s == WALPE_WS_SORT_TREND)
        q_add (u, "days", "7");
    else if (s == WALPE_WS_SORT_POPULAR)
        q_add (u, "days", "-1");
    for (const char *const *t = q->required; t && *t; t++)
        q_add (u, "requiredtags%5B%5D", *t);
    for (const char *const *t = q->excluded; t && *t; t++)
        q_add (u, "excludedtags%5B%5D", *t);
    g_string_append_printf (u, "&p=%u", q->page ? q->page : 1);
    return g_string_free (u, FALSE);
}

/* IPublishedFileService/QueryFiles query_type (EPublishedFileQueryType) */
static int api_query_type (WalpeWsSort s, gboolean text)
{
    switch (s) {
    case WALPE_WS_SORT_TREND: return 3;       /* RankedByTrend (+ days) */
    case WALPE_WS_SORT_POPULAR: return 0;     /* RankedByVote */
    case WALPE_WS_SORT_RECENT: return 1;      /* RankedByPublicationDate */
    case WALPE_WS_SORT_SUBSCRIBED: return 9;  /* RankedByTotalUniqueSubscriptions */
    case WALPE_WS_SORT_RELEVANCE: return text ? 12 : 3;  /* RankedByTextSearch */
    default: return 3;
    }
}

char *walpe_ws_queryfiles_url (const WalpeWsQuery *q, const char *api_key)
{
    GString *u = g_string_new (walpe_ws_api_base ());
    g_string_append (u, "IPublishedFileService/QueryFiles/v1/?");
    g_string_append_printf (u, "key=%s", api_key ? api_key : "");
    int qt = api_query_type (q->sort, has_text (q));
    g_string_append_printf (u, "&appid=%d&query_type=%d&page=%u&numperpage=%d", WS_APPID, qt,
                            q->page ? q->page : 1, WALPE_WS_PER_PAGE);
    g_string_append (u, "&filetype=0&match_all_tags=1&return_tags=1&return_vote_data=1"
                        "&return_previews=0&return_short_description=0&return_metadata=0");
    if (qt == 3)
        g_string_append (u, "&days=7&include_recent_votes_only=0");
    if (has_text (q))
        q_add (u, "search_text", q->text);
    int i = 0;
    for (const char *const *t = q->required; t && *t; t++, i++) {
        char k[32];
        g_snprintf (k, sizeof k, "requiredtags%%5B%d%%5D", i);
        q_add (u, k, *t);
    }
    i = 0;
    for (const char *const *t = q->excluded; t && *t; t++, i++) {
        char k[32];
        g_snprintf (k, sizeof k, "excludedtags%%5B%d%%5D", i);
        q_add (u, k, *t);
    }
    return g_string_free (u, FALSE);
}

/* ---------------------------------------------------------------- parsing */

/* What one reply may make WalPE keep and show. Steam's own limits are far
 * lower (title 128, description 8000, tag 255 characters, 30 items a page,
 * 100 per details call): a reply that goes beyond these is cut, not
 * trusted. Without them a page of 16 MB could name 700000 item ids (7000
 * detail requests) and a title could be megabytes long. */
#define ITEM_MAX_TITLE 256
#define ITEM_MAX_DESC 16000
#define ITEM_MAX_TAGS 64
#define ITEM_MAX_TAG 128
#define ITEM_MAX_NAME 128
#define ITEM_MAX_URL 2048
#define REPLY_MAX_ITEMS 200
#define BROWSE_MAX_IDS 100

/* s (owned) cut to max characters */
static char *clip (char *s, glong max)
{
    if (!s || g_utf8_strlen (s, -1) <= max)
        return s;
    char *c = g_utf8_substring (s, 0, max);
    g_free (s);
    return c;
}

/* a name of a reply (title, tag, persona) as one clean line of at most max
 * characters (uitext.h); s is consumed, nothing left = NULL */
static char *line_text (char *s, glong max)
{
    if (!s)
        return NULL;
    char *t = walpe_ui_text (s, max, WALPE_UITEXT_LINE);
    g_free (s);
    g_strstrip (t);
    if (!*t)
        g_clear_pointer (&t, g_free);
    return t;
}

static char *jstr (JsonObject *o, const char *k)
{
    JsonNode *n = o ? json_object_get_member (o, k) : NULL;
    if (!n || !JSON_NODE_HOLDS_VALUE (n))
        return NULL;
    GType t = json_node_get_value_type (n);
    if (t == G_TYPE_STRING) {
        /* an escaped lone surrogate ("\ud800") is not UTF-8 once decoded */
        const char *v = json_node_get_string (n);
        if (!v)
            return NULL;
        return g_utf8_validate (v, -1, NULL) ? g_strdup (v) : g_utf8_make_valid (v, -1);
    }
    if (t == G_TYPE_INT64)
        return g_strdup_printf ("%" G_GINT64_FORMAT, json_node_get_int (n));
    return NULL;
}

static gint64 jint (JsonObject *o, const char *k, gint64 dflt)
{
    JsonNode *n = o ? json_object_get_member (o, k) : NULL;
    if (!n || !JSON_NODE_HOLDS_VALUE (n))
        return dflt;
    GType t = json_node_get_value_type (n);
    if (t == G_TYPE_INT64)
        return json_node_get_int (n);
    if (t == G_TYPE_DOUBLE) {
        /* 1e300 or NaN: no gint64 holds it, the cast is undefined */
        double d = json_node_get_double (n);
        return d >= -9.2e18 && d <= 9.2e18 ? (gint64) d : dflt;
    }
    if (t == G_TYPE_BOOLEAN)
        return json_node_get_boolean (n);
    if (t == G_TYPE_STRING) {
        const char *s = json_node_get_string (n);
        char *e = NULL;
        gint64 v = g_ascii_strtoll (s, &e, 10);
        return e && e != s ? v : dflt;
    }
    return dflt;
}

static gboolean digits_only (const char *s)
{
    if (!s || !*s || strlen (s) > 20)
        return FALSE;
    for (; *s; s++)
        if (!g_ascii_isdigit (*s))
            return FALSE;
    return TRUE;
}

/* one item from GetPublishedFileDetails, QueryFiles or the browse page */
static WalpeWsItem *item_from_json (JsonObject *o)
{
    char *id = jstr (o, "publishedfileid");
    if (!digits_only (id)) {
        g_free (id);
        return NULL;
    }
    WalpeWsItem *it = item_new ();
    it->id = id;
    it->result = (int) CLAMP (jint (o, "result", 1), G_MININT, G_MAXINT);
    it->title = line_text (jstr (o, "title"), ITEM_MAX_TITLE);
    it->description = jstr (o, "description");
    if (!it->description)
        it->description = jstr (o, "file_description");
    if (!it->description)
        it->description = jstr (o, "short_description");
    it->description = clip (it->description, ITEM_MAX_DESC);
    it->preview_url = jstr (o, "preview_url");
    if (it->preview_url && strlen (it->preview_url) > ITEM_MAX_URL)
        g_clear_pointer (&it->preview_url, g_free);
    it->creator = jstr (o, "creator");
    if (!digits_only (it->creator))
        g_clear_pointer (&it->creator, g_free);   /* a SteamID64, or nothing */
    it->file_size = (guint64) MAX (0, jint (o, "file_size", 0));
    it->subscriptions = (guint64) MAX (0, jint (o, "subscriptions", 0));
    it->favorited = (guint64) MAX (0, jint (o, "favorited", 0));
    it->views = (guint64) MAX (0, jint (o, "views", 0));
    it->time_created = jint (o, "time_created", 0);
    it->time_updated = jint (o, "time_updated", 0);
    it->banned = jint (o, "banned", 0) != 0;
    if (json_object_has_member (o, "star_rating"))
        it->stars = (int) CLAMP (jint (o, "star_rating", -1), -1, 5);
    if (json_object_has_member (o, "total_votes"))
        it->votes = (int) CLAMP (jint (o, "total_votes", -1), -1, G_MAXINT);
    JsonObject *vd = wpj_obj (o, "vote_data");
    if (vd) {
        /* numbers of a reply: kept inside what the fields hold (the sum of
         * two 64-bit counts overflowed, a score of 1e300 printed 300 digits) */
        double sc = wpj_num (vd, "score", -1);
        it->score = sc >= 0 ? MIN (sc, 1.0) : -1;
        it->votes = (int) (CLAMP (jint (vd, "votes_up", 0), 0, G_MAXINT / 2) +
                           CLAMP (jint (vd, "votes_down", 0), 0, G_MAXINT / 2));
    }
    JsonArray *tags = wpj_arr (o, "tags");
    GPtrArray *tv = g_ptr_array_new ();
    for (guint i = 0; i < wpj_arr_len (tags) && tv->len < ITEM_MAX_TAGS; i++) {
        char *t = line_text (jstr (wpj_arr_obj (tags, i), "tag"), ITEM_MAX_TAG);
        if (t && *t)
            g_ptr_array_add (tv, t);
        else
            g_free (t);
    }
    g_ptr_array_add (tv, NULL);
    g_strfreev (it->tags);
    it->tags = (char **) g_ptr_array_free (tv, FALSE);
    return it;
}

/* json-glib parses by recursion: a reply nested some ten thousand levels
 * deep ("[[[[[...", 100 KB of it) overflowed the stack and took the GUI
 * down. Steam's replies have a handful of levels. Brackets inside strings
 * do not count - and what a string is has to be read the way json-glib's
 * scanner reads it (single quotes, comments): a count that knew double
 * quotes only let /"/[[[[... through, because the quote inside the comment
 * opened a string for the count alone. wpj_depth_ok counts under every
 * such reading. */
#define JSON_MAX_DEPTH 128

static JsonNode *parse_json (const char *data, gssize len, GError **err)
{
    if (!data || !wpj_depth_ok (data, len < 0 ? strlen (data) : (gsize) len, JSON_MAX_DEPTH)) {
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                     "JSON nested deeper than %d levels", JSON_MAX_DEPTH);
        return NULL;
    }
    /* the first value of the reply only: what this json-glib parses behind
     * it is never freed (util.c, wpj_parse_first) */
    gsize n = len < 0 ? strlen (data) : (gsize) len;
    char *text = g_memdup2 (data, n);
    JsonNode *root = wpj_parse_first (text, n, err);
    g_free (text);
    if (!root && err && *err) {
        /* json-glib quotes the text it stumbled over - bytes of the reply,
         * not always UTF-8, as long as the reply likes. The message is
         * shown: one clean line. */
        char *m = walpe_ui_text ((*err)->message, 200, WALPE_UITEXT_LINE);
        g_clear_error (err);
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "not the expected JSON (%s)", m);
        g_free (m);
    }
    if (!root && err && !*err)
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "empty JSON");
    return root;
}

static GPtrArray *items_array (JsonArray *a)
{
    GPtrArray *out = g_ptr_array_new_with_free_func ((GDestroyNotify) walpe_ws_item_unref);
    for (guint i = 0; i < wpj_arr_len (a) && out->len < REPLY_MAX_ITEMS; i++) {
        WalpeWsItem *it = item_from_json (wpj_arr_obj (a, i));
        if (it)
            g_ptr_array_add (out, it);
    }
    return out;
}

GPtrArray *walpe_ws_parse_details (const char *json, gssize len, GError **err)
{
    JsonNode *root = parse_json (json, len, err);
    if (!root)
        return NULL;
    JsonObject *resp = JSON_NODE_HOLDS_OBJECT (root)
                           ? wpj_obj (json_node_get_object (root), "response") : NULL;
    JsonArray *a = resp ? wpj_arr (resp, "publishedfiledetails") : NULL;
    GPtrArray *out = NULL;
    if (!a)
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                     "GetPublishedFileDetails: unexpected reply");
    else
        out = items_array (a);
    json_node_unref (root);
    return out;
}

GPtrArray *walpe_ws_parse_queryfiles (const char *json, gssize len, guint64 *total,
                                      GError **err)
{
    JsonNode *root = parse_json (json, len, err);
    if (!root)
        return NULL;
    JsonObject *resp = JSON_NODE_HOLDS_OBJECT (root)
                           ? wpj_obj (json_node_get_object (root), "response") : NULL;
    GPtrArray *out = NULL;
    if (!resp) {
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "QueryFiles: unexpected reply");
    } else {
        if (total)
            *total = (guint64) MAX (0, jint (resp, "total", 0));
        out = items_array (wpj_arr (resp, "publishedfiledetails"));  /* absent = none */
    }
    json_node_unref (root);
    return out;
}

static void add_id (GPtrArray *ids, GHashTable *seen, const char *id)
{
    if (!digits_only (id) || g_hash_table_contains (seen, id) || ids->len >= BROWSE_MAX_IDS)
        return;
    g_hash_table_add (seen, g_strdup (id));
    g_ptr_array_add (ids, g_strdup (id));
}

/* <script type="application/json" id="valve-ssr-data"> ... </script>:
 * {"renderContext":{"queryData":"<JSON string>"}} -> queries[] whose
 * queryKey[0] == "workshop_browse" -> state.data {results, total_count, ...} */
static gboolean parse_ssr (const char *html, gsize len, GPtrArray *ids, GHashTable *seen,
                           GHashTable *items, guint64 *total, guint *total_pages)
{
    const char *tag = g_strstr_len (html, (gssize) len, "id=\"valve-ssr-data\"");
    if (!tag)
        return FALSE;
    /* the reply is not NUL-terminated: search inside it only (strchr ran
     * past its end when the page stopped within this tag) */
    const char *start = memchr (tag, '>', len - (gsize) (tag - html));
    const char *end = start ? g_strstr_len (start, (gssize) (len - (gsize) (start - html)),
                                            "</script>") : NULL;
    if (!start || !end)
        return FALSE;
    JsonNode *root = parse_json (start + 1, end - start - 1, NULL);
    if (!root)
        return FALSE;
    gboolean ok = FALSE;
    JsonObject *rc = JSON_NODE_HOLDS_OBJECT (root)
                         ? wpj_obj (json_node_get_object (root), "renderContext") : NULL;
    const char *qd = rc ? wpj_str (rc, "queryData", NULL) : NULL;
    JsonNode *qroot = qd ? parse_json (qd, -1, NULL) : NULL;
    JsonArray *queries = qroot && JSON_NODE_HOLDS_OBJECT (qroot)
                             ? wpj_arr (json_node_get_object (qroot), "queries") : NULL;
    for (guint i = 0; !ok && i < wpj_arr_len (queries); i++) {
        JsonObject *q = wpj_arr_obj (queries, i);
        JsonArray *key = wpj_arr (q, "queryKey");
        if (!key || json_array_get_length (key) < 1)
            continue;
        JsonNode *k0 = json_array_get_element (key, 0);
        if (!JSON_NODE_HOLDS_VALUE (k0) || json_node_get_value_type (k0) != G_TYPE_STRING ||
            g_strcmp0 (json_node_get_string (k0), "workshop_browse"))
            continue;
        JsonObject *data = wpj_obj (wpj_obj (q, "state"), "data");
        JsonArray *res = data ? wpj_arr (data, "results") : NULL;
        if (!res)
            continue;
        ok = TRUE;
        if (total)
            *total = (guint64) MAX (0, jint (data, "total_count", 0));
        if (total_pages)
            *total_pages = (guint) MAX (0, jint (data, "total_pages", 0));
        /* persona names of the creators on this page */
        GHashTable *names = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
        JsonArray *cd = wpj_arr (data, "creator_player_link_details");
        for (guint c = 0; c < wpj_arr_len (cd); c++) {
            JsonObject *pd = wpj_obj (wpj_arr_obj (cd, c), "public_data");
            char *sid = jstr (pd, "steamid");
            const char *pn = wpj_str (pd, "persona_name", NULL);
            char *pname = pn ? line_text (g_strdup (pn), ITEM_MAX_NAME) : NULL;
            if (sid && pname && g_hash_table_size (names) < REPLY_MAX_ITEMS) {
                g_hash_table_replace (names, sid, pname);
            } else {
                g_free (sid);
                g_free (pname);
            }
        }
        for (guint r = 0; r < wpj_arr_len (res) && r < REPLY_MAX_ITEMS; r++) {
            WalpeWsItem *it = item_from_json (wpj_arr_obj (res, r));
            if (!it)
                continue;
            if (it->creator && g_hash_table_contains (names, it->creator))
                it->author = g_strdup (g_hash_table_lookup (names, it->creator));
            gboolean fresh = !g_hash_table_contains (seen, it->id);
            add_id (ids, seen, it->id);
            if (items && fresh)
                g_hash_table_replace (items, g_strdup (it->id), walpe_ws_item_ref (it));
            walpe_ws_item_unref (it);
        }
        g_hash_table_unref (names);
    }
    if (qroot)
        json_node_unref (qroot);
    json_node_unref (root);
    return ok;
}

gboolean walpe_ws_parse_browse_page (const char *html, gsize len, GPtrArray *ids,
                                     GHashTable *items, guint64 *total,
                                     guint *total_pages, GError **err)
{
    if (total)
        *total = 0;
    if (total_pages)
        *total_pages = 0;
    GHashTable *seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    gboolean ok = parse_ssr (html, len, ids, seen, items, total, total_pages);
    if (!ok) {
        /* older/other markup: item links in document order */
        static const char *const pats[] = { "filedetails/?id=", "data-publishedfileid=\"", NULL };
        const char *end = html + len;
        for (const char *p = html; p < end && ids->len < BROWSE_MAX_IDS; p++) {
            for (int k = 0; pats[k]; k++) {
                gsize pl = strlen (pats[k]);
                if ((gsize) (end - p) > pl && !memcmp (p, pats[k], pl)) {
                    const char *d = p + pl;
                    gsize n = 0;
                    while (d + n < end && n < 21 && g_ascii_isdigit (d[n]))
                        n++;
                    if (n > 0 && n <= 20) {
                        char *id = g_strndup (d, n);
                        add_id (ids, seen, id);
                        g_free (id);
                    }
                }
            }
        }
        ok = ids->len > 0;
        if (!ok)
            g_set_error (err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                         "the workshop page had no results data (Steam error, login or "
                         "rate-limit page?)");
    }
    g_hash_table_unref (seen);
    return ok;
}

/* --------------------------------------------------------------- settings */

static char *settings_path (void)
{
    return g_build_filename (g_get_user_config_dir (), "walpe", "workshop.json", NULL);
}

gboolean walpe_ws_valid_api_key (const char *key)
{
    if (!key || strlen (key) != 32)
        return FALSE;
    for (const char *p = key; *p; p++)
        if (!g_ascii_isxdigit (*p))
            return FALSE;
    return TRUE;
}

WalpeWsSettings *walpe_ws_settings_load (void)
{
    WalpeWsSettings *s = g_new0 (WalpeWsSettings, 1);
    s->hide_mature = TRUE;
    char *path = settings_path ();
    GStatBuf st;
    if (g_stat (path, &st) == 0 && (st.st_mode & 077))
        g_chmod (path, 0600);   /* it may hold the API key */
    JsonNode *root = wpj_parse_file (path);
    if (root && JSON_NODE_HOLDS_OBJECT (root)) {
        JsonObject *o = json_node_get_object (root);
        const char *k = wpj_str (o, "api_key", NULL);
        if (walpe_ws_valid_api_key (k))
            s->api_key = g_strdup (k);
        s->hide_mature = wpj_bool (o, "hide_mature", TRUE);
        s->hide_questionable = wpj_bool (o, "hide_questionable", FALSE);
        s->subscribe_on_download = wpj_bool (o, "subscribe_on_download", FALSE);
        s->unsubscribe_on_delete = wpj_bool (o, "unsubscribe_on_delete", FALSE);
    }
    if (root)
        json_node_unref (root);
    g_free (path);
    return s;
}

gboolean walpe_ws_settings_save (const WalpeWsSettings *s, GError **err)
{
    char *dir = g_build_filename (g_get_user_config_dir (), "walpe", NULL);
    g_mkdir_with_parents (dir, 0700);
    g_free (dir);
    JsonBuilder *b = json_builder_new ();
    json_builder_begin_object (b);
    if (s->api_key && walpe_ws_valid_api_key (s->api_key)) {
        json_builder_set_member_name (b, "api_key");
        json_builder_add_string_value (b, s->api_key);
    }
    json_builder_set_member_name (b, "hide_mature");
    json_builder_add_boolean_value (b, s->hide_mature);
    json_builder_set_member_name (b, "hide_questionable");
    json_builder_add_boolean_value (b, s->hide_questionable);
    json_builder_set_member_name (b, "subscribe_on_download");
    json_builder_add_boolean_value (b, s->subscribe_on_download);
    json_builder_set_member_name (b, "unsubscribe_on_delete");
    json_builder_add_boolean_value (b, s->unsubscribe_on_delete);
    json_builder_end_object (b);
    JsonGenerator *g = json_generator_new ();
    JsonNode *root = json_builder_get_root (b);
    json_generator_set_root (g, root);
    json_generator_set_pretty (g, TRUE);
    gsize len = 0;
    char *data = json_generator_to_data (g, &len);
    json_node_unref (root);
    g_object_unref (g);
    g_object_unref (b);

    /* tmp created 0600 (never world-readable, not even briefly), then renamed */
    char *path = settings_path ();
    char *tmp = g_strconcat (path, ".tmp", NULL);
    g_unlink (tmp);
    int fd = open (tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    gboolean ok = fd >= 0;
    if (ok) {
        fchmod (fd, 0600);
        gsize off = 0;
        while (ok && off < len) {
            ssize_t w = write (fd, data + off, len - off);
            if (w < 0 && errno == EINTR)
                continue;
            ok = w > 0;
            off += w > 0 ? (gsize) w : 0;
        }
        ok = (fsync (fd) == 0) && ok;
        ok = (close (fd) == 0) && ok;
        ok = ok && rename (tmp, path) == 0;
    }
    if (!ok) {
        int e = errno;
        g_unlink (tmp);
        g_set_error (err, G_IO_ERROR, g_io_error_from_errno (e), "cannot write %s: %s", path,
                     g_strerror (e));
    }
    memset (data, 0, len);   /* the key */
    g_free (data);
    g_free (tmp);
    g_free (path);
    return ok;
}

void walpe_ws_settings_free (WalpeWsSettings *s)
{
    if (!s)
        return;
    if (s->api_key)
        memset (s->api_key, 0, strlen (s->api_key));
    g_free (s->api_key);
    g_free (s);
}

void walpe_ws_settings_set_unsubscribe_on_delete (gboolean on)
{
    WalpeWsSettings *s = walpe_ws_settings_load ();
    if (!s->unsubscribe_on_delete != !on) {
        s->unsubscribe_on_delete = on;
        walpe_ws_settings_save (s, NULL);
    }
    walpe_ws_settings_free (s);
}

char *walpe_ws_cache_dir (void)
{
    return g_build_filename (g_get_user_cache_dir (), "walpe", "workshop", NULL);
}

/* ================================================================ client */

#ifndef HAVE_SOUP

gboolean walpe_ws_available (void) { return FALSE; }
gboolean walpe_ws_redirect_allowed (int cls, const char *to) { (void) cls; (void) to; return FALSE; }
WalpeWsClient *walpe_ws_client_new (void) { return NULL; }
void walpe_ws_client_free (WalpeWsClient *c) { (void) c; }
void walpe_ws_client_set_api_key (WalpeWsClient *c, const char *k) { (void) c; (void) k; }
gboolean walpe_ws_client_has_api_key (WalpeWsClient *c) { (void) c; return FALSE; }

static void unsupported (GAsyncReadyCallback cb, gpointer user, GCancellable *cancel)
{
    GTask *t = g_task_new (NULL, cancel, cb, user);
    g_task_return_new_error (t, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                             "WalPE was built without libsoup-3.0: workshop browsing unavailable");
    g_object_unref (t);
}
void walpe_ws_search_async (WalpeWsClient *c, const WalpeWsQuery *q, GCancellable *x,
                            GAsyncReadyCallback cb, gpointer u)
{ (void) c; (void) q; unsupported (cb, u, x); }
WalpeWsResults *walpe_ws_search_finish (GAsyncResult *r, GError **e)
{ return g_task_propagate_pointer (G_TASK (r), e); }
void walpe_ws_details_async (WalpeWsClient *c, const char *const *ids, GCancellable *x,
                             GAsyncReadyCallback cb, gpointer u)
{ (void) c; (void) ids; unsupported (cb, u, x); }
GPtrArray *walpe_ws_details_finish (GAsyncResult *r, GError **e)
{ return g_task_propagate_pointer (G_TASK (r), e); }
void walpe_ws_subscribe_async (WalpeWsClient *c, const char *id, gboolean sub, GCancellable *x,
                               GAsyncReadyCallback cb, gpointer u)
{ (void) c; (void) id; (void) sub; unsupported (cb, u, x); }
gboolean walpe_ws_subscribe_finish (GAsyncResult *r, GError **e)
{ return g_task_propagate_boolean (G_TASK (r), e); }
void walpe_ws_author_async (WalpeWsClient *c, const char *s, GCancellable *x,
                            GAsyncReadyCallback cb, gpointer u)
{ (void) c; (void) s; unsupported (cb, u, x); }
char *walpe_ws_author_finish (GAsyncResult *r, GError **e)
{ return g_task_propagate_pointer (G_TASK (r), e); }
void walpe_ws_preview_async (WalpeWsClient *c, const char *id, const char *url, gboolean sm,
                             int w, int h, GCancellable *x, GAsyncReadyCallback cb, gpointer u)
{ (void) c; (void) id; (void) url; (void) sm; (void) w; (void) h; unsupported (cb, u, x); }
GdkPixbuf *walpe_ws_preview_finish (GAsyncResult *r, GError **e)
{ return g_task_propagate_pointer (G_TASK (r), e); }
char *walpe_ws_preview_file (GAsyncResult *r)
{ (void) r; return NULL; }

#else /* HAVE_SOUP */

typedef enum { HOST_COMMUNITY, HOST_API, HOST_CDN, HOST_N } HostClass;
static const gint64 host_gap_us[HOST_N] = { 1500000, 250000, 0 };
static const char *const host_name[HOST_N] = { "steamcommunity.com", "api.steampowered.com",
                                               "the Steam image CDN" };

struct _WalpeWsClient {
    int refs;
    SoupSession *session;
    char *api_key;
    gint64 next_ok[HOST_N];
    GHashTable *authors;       /* steamid -> persona */
    char *cache_dir;
};

gboolean walpe_ws_available (void) { return TRUE; }

static WalpeWsClient *client_ref (WalpeWsClient *c)
{
    c->refs++;
    return c;
}

static void client_unref (WalpeWsClient *c)
{
    if (!c || --c->refs > 0)
        return;
    g_object_unref (c->session);
    if (c->api_key)
        memset (c->api_key, 0, strlen (c->api_key));
    g_free (c->api_key);
    g_hash_table_unref (c->authors);
    g_free (c->cache_dir);
    g_free (c);
}

typedef struct { char *path; gint64 mtime; gint64 size; } CacheEnt;

static gint cache_cmp (gconstpointer a, gconstpointer b)
{
    const CacheEnt *x = *(CacheEnt *const *) a, *y = *(CacheEnt *const *) b;
    return x->mtime < y->mtime ? -1 : x->mtime > y->mtime;
}

static void cache_ent_free (gpointer p)
{
    CacheEnt *e = p;
    g_free (e->path);
    g_free (e);
}

/* worker: oldest previews go first once the cache is over CACHE_MAX_BYTES */
static void prune_thread (GTask *t, gpointer src, gpointer data, GCancellable *cancel)
{
    (void) src; (void) cancel;
    const char *dir = data;
    GDir *d = g_dir_open (dir, 0, NULL);
    if (!d) {
        g_task_return_boolean (t, TRUE);
        return;
    }
    GPtrArray *ents = g_ptr_array_new_with_free_func (cache_ent_free);
    gint64 total = 0;
    const char *n;
    while ((n = g_dir_read_name (d))) {
        char *p = g_build_filename (dir, n, NULL);
        GStatBuf st;
        if (g_lstat (p, &st) == 0 && S_ISREG (st.st_mode)) {
            CacheEnt *e = g_new0 (CacheEnt, 1);
            e->path = p;
            e->mtime = st.st_mtime;
            e->size = st.st_size;
            total += st.st_size;
            g_ptr_array_add (ents, e);
        } else {
            g_free (p);
        }
    }
    g_dir_close (d);
    if (total > (gint64) CACHE_MAX_BYTES) {
        g_ptr_array_sort (ents, cache_cmp);
        for (guint i = 0; i < ents->len && total > (gint64) CACHE_KEEP_BYTES; i++) {
            CacheEnt *e = g_ptr_array_index (ents, i);
            if (g_unlink (e->path) == 0)
                total -= e->size;
        }
    }
    g_ptr_array_unref (ents);
    g_task_return_boolean (t, TRUE);
}

WalpeWsClient *walpe_ws_client_new (void)
{
    WalpeWsClient *c = g_new0 (WalpeWsClient, 1);
    c->refs = 1;
    c->session = soup_session_new_with_options (
        "user-agent", "WalPE/" WALPE_VERSION " (Linux; Wallpaper Engine workshop browser)",
        "timeout", 20, "idle-timeout", 30, "max-conns-per-host", 4, "max-conns", 12, NULL);
    c->authors = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
    c->cache_dir = walpe_ws_cache_dir ();
    g_mkdir_with_parents (c->cache_dir, 0700);
    GTask *t = g_task_new (NULL, NULL, NULL, NULL);
    g_task_set_task_data (t, g_strdup (c->cache_dir), g_free);
    g_task_run_in_thread (t, prune_thread);
    g_object_unref (t);
    return c;
}

void walpe_ws_client_free (WalpeWsClient *c)
{
    client_unref (c);
}

void walpe_ws_client_set_api_key (WalpeWsClient *c, const char *key)
{
    if (!c)
        return;
    if (c->api_key)
        memset (c->api_key, 0, strlen (c->api_key));
    g_free (c->api_key);
    c->api_key = walpe_ws_valid_api_key (key) ? g_strdup (key) : NULL;
}

gboolean walpe_ws_client_has_api_key (WalpeWsClient *c)
{
    return c && c->api_key;
}

/* never let the key reach a message */
static char *scrub (WalpeWsClient *c, const char *msg)
{
    if (!msg)
        return g_strdup ("?");
    if (!c->api_key || !strstr (msg, c->api_key))
        return g_strdup (msg);
    char **parts = g_strsplit (msg, c->api_key, -1);
    char *r = g_strjoinv ("***", parts);
    g_strfreev (parts);
    return r;
}

/* ------------------------------------------------------------- fetching */

/* A reply is read in pieces and given up as soon as it is larger than the
 * caller allows: the limit counts the bytes as they come out of libsoup's
 * content decoder, so neither an endless body nor a few KB of gzip that
 * unpack to gigabytes are ever held in memory (the whole reply used to be
 * read first and measured afterwards). */
#define FETCH_CHUNK (64 * 1024)
/* redirects are followed here, not by libsoup, and only where
 * walpe_ws_redirect_allowed() says so */
#define FETCH_MAX_REDIRECTS 4
/* one request from start to end, the retry and redirects included: the
 * session's 20 s limit is per read, a reply that trickles in never hit it */
#define FETCH_TOTAL_S 120
#define FETCH_ONCE_TOTAL_S 12

enum {
    FETCH_ERESULT = 1 << 0,    /* a 2xx reply with x-eresult != 1 is a failure */
    FETCH_ONCE = 1 << 1,       /* a call that changes something on Steam: sent
                                * once (no retry: nobody knows whether the first
                                * one was carried out) and given 12 s */
};

typedef struct {
    WalpeWsClient *c;
    HostClass host;
    char *method, *url;
    GBytes *body;              /* POST form body or NULL */
    gsize max_bytes;
    int attempt;
    int redirects;
    SoupMessagePriority prio;
    SoupMessage *msg;
    guint flags;               /* FETCH_* */
    GCancellable *cancel;      /* this request: the caller's cancel or the deadline */
    GCancellable *outer;       /* the caller's, or NULL */
    gulong outer_id;
    guint deadline_id, wait_id;
    gboolean timed_out, too_large, redirect_refused;
    GInputStream *in;          /* the reply's body while it is read */
    GByteArray *buf;
} Fetch;

static void fetch_free (gpointer p)
{
    Fetch *f = p;
    if (f->outer_id)
        g_cancellable_disconnect (f->outer, f->outer_id);
    g_clear_object (&f->outer);
    if (f->deadline_id)
        g_source_remove (f->deadline_id);
    if (f->wait_id)
        g_source_remove (f->wait_id);
    if (f->in)
        g_input_stream_close_async (f->in, G_PRIORITY_DEFAULT, NULL, NULL, NULL);
    g_clear_object (&f->in);
    g_clear_pointer (&f->buf, g_byte_array_unref);
    g_clear_object (&f->cancel);
    if (f->url)
        memset (f->url, 0, strlen (f->url));   /* may hold the key */
    g_free (f->url);
    g_free (f->method);
    if (f->body)
        g_bytes_unref (f->body);
    g_clear_object (&f->msg);
    client_unref (f->c);
    g_free (f);
}

static void fetch_go (GTask *t);
static gboolean preview_url_ok (const char *url);

static gboolean fetch_go_cb (gpointer p)
{
    Fetch *f = g_task_get_task_data (p);
    f->wait_id = 0;
    fetch_go (p);
    return G_SOURCE_REMOVE;
}

/* reserve the host's next slot and start when it comes */
static void fetch_schedule (GTask *t, guint extra_ms)
{
    Fetch *f = g_task_get_task_data (t);
    gint64 now = g_get_monotonic_time ();
    gint64 at = MAX (now + (gint64) extra_ms * 1000, f->c->next_ok[f->host]);
    f->c->next_ok[f->host] = at + host_gap_us[f->host];
    guint ms = (guint) ((at - now) / 1000);
    if (ms == 0)
        fetch_go (t);
    else
        f->wait_id = g_timeout_add (ms, fetch_go_cb, t);
}

static gboolean host_is (const char *host, const char *name)
{
    gsize hl = strlen (host), nl = strlen (name);
    return !g_ascii_strcasecmp (host, name) ||
           (hl > nl + 1 && host[hl - nl - 1] == '.' && !g_ascii_strcasecmp (host + hl - nl, name));
}

/* May a redirect to `to` be followed? cls: 0 a steamcommunity.com page,
 * 1 the Web API, 2 the image CDN.
 *   Web API   never: its calls carry the user's key and Steam's API does
 *             not redirect; an answer that does is an error.
 *   page      https://steamcommunity.com (or a host under it) only
 *   pictures  https on Valve's image hosts only (the rule for the URL the
 *             listing names)
 * So no reply can send WalPE to plain http or to a host of its choosing
 * (libsoup followed any Location, an http:// one included). */
gboolean walpe_ws_redirect_allowed (int cls, const char *to)
{
    if (!to || cls == HOST_API)
        return FALSE;
    if (cls == HOST_CDN)
        return preview_url_ok (to);
    if (cls != HOST_COMMUNITY)
        return FALSE;
    GUri *u = g_uri_parse (to, G_URI_FLAGS_NONE, NULL);
    if (!u)
        return FALSE;
    const char *host = g_uri_get_host (u);
    gboolean ok = !g_strcmp0 (g_uri_get_scheme (u), "https") && host && !g_uri_get_userinfo (u) &&
                  (g_uri_get_port (u) == -1 || g_uri_get_port (u) == 443) &&
                  host_is (host, "steamcommunity.com");
    g_uri_unref (u);
    return ok;
}

/* Steam's service calls report their result in the x-eresult header
 * (1 = OK) even on HTTP 200. TRUE = it failed and the task was returned. */
static gboolean eresult_failed (Fetch *f, GTask *t)
{
    SoupMessageHeaders *h = soup_message_get_response_headers (f->msg);
    const char *er = h ? soup_message_headers_get_one (h, "x-eresult") : NULL;
    if (!er || !*er)
        return FALSE;   /* no header: the caller judges the body */
    int code = atoi (er);
    if (code == 1)
        return FALSE;
    static const struct { int code; const char *name; } names[] = {
        { 2, "failed" }, { 8, "invalid parameter" }, { 9, "item not found" },
        { 15, "access denied" }, { 16, "timeout" }, { 20, "service unavailable" },
        { 21, "not logged on" }, { 25, "limit exceeded" }, { 42, "no match" },
        { 84, "rate limit exceeded" },
    };
    const char *name = NULL;
    for (guint i = 0; i < G_N_ELEMENTS (names); i++)
        if (names[i].code == code)
            name = names[i].name;
    const char *em = soup_message_headers_get_one (h, "x-error_message");
    /* a header of the reply: shown, so one line of bounded length */
    char *detail = NULL;
    if (em && *em) {
        char *v = g_utf8_make_valid (em, -1);
        char *cut = g_utf8_substring (v, 0, MIN (g_utf8_strlen (v, -1), 200));
        for (char *q = cut; *q; q++)
            if ((guchar) *q < 0x20 || *q == 0x7f)
                *q = ' ';
        detail = scrub (f->c, cut);
        g_free (cut);
        g_free (v);
    }
    g_task_return_new_error (t, G_IO_ERROR,
                             code == 15 || code == 21 ? G_IO_ERROR_PERMISSION_DENIED
                                                      : G_IO_ERROR_FAILED,
                             "Steam answered EResult %d%s%s%s%s%s", code, name ? " (" : "",
                             name ? name : "", name ? ")" : "", detail ? ": " : "",
                             detail ? detail : "");
    g_free (detail);
    return TRUE;
}

/* the end of one attempt: b = the body of a 2xx reply, err = why there was
 * no reply (both consumed). Retries once, or returns the task. */
static void fetch_complete (GTask *t, GBytes *b, GError *err)
{
    Fetch *f = g_task_get_task_data (t);
    gboolean replied = f->in != NULL;   /* the headers came */
    guint status = replied && f->msg ? soup_message_get_status (f->msg) : 0;
    if (f->in) {
        /* not read to its end (too large, an error page): closing it drops
         * the connection, nothing more is received */
        g_input_stream_close_async (f->in, G_PRIORITY_DEFAULT, NULL, NULL, NULL);
        g_clear_object (&f->in);
    }
    g_clear_pointer (&f->buf, g_byte_array_unref);
    if (g_task_return_error_if_cancelled (t))
        goto out;
    if (f->timed_out) {
        g_task_return_new_error (t, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                                 "%s took too long to answer", host_name[f->host]);
        goto out;
    }
    gboolean transient = err ? !g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED)
                             : (replied && status >= 500);
    if (transient && f->attempt == 0 && !(f->flags & FETCH_ONCE)) {
        f->attempt++;
        g_clear_error (&err);
        if (b)
            g_bytes_unref (b);
        g_clear_object (&f->msg);
        f->too_large = f->redirect_refused = FALSE;
        fetch_schedule (t, 2000);   /* one polite retry */
        return;
    }
    if (err || !replied) {
        char *m = scrub (f->c, err ? err->message : "no reply");
        g_task_return_new_error (t, G_IO_ERROR,
                                 err && err->domain == G_IO_ERROR ? err->code : G_IO_ERROR_FAILED,
                                 "%s did not answer: %s", host_name[f->host], m);
        g_free (m);
    } else if (f->redirect_refused) {
        g_task_return_new_error (t, G_IO_ERROR, G_IO_ERROR_FAILED,
                                 "%s answered with a redirect (HTTP %u) to somewhere WalPE does "
                                 "not follow", host_name[f->host], status);
    } else if (status == 429) {
        g_task_return_new_error (t, G_IO_ERROR, G_IO_ERROR_BUSY,
                                 "%s is rate-limiting requests from this computer (HTTP 429); "
                                 "wait a minute and try again", host_name[f->host]);
    } else if (status == 401 || status == 403) {
        g_task_return_new_error (t, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                 "%s refused the request (HTTP %u)%s", host_name[f->host], status,
                                 f->host == HOST_API && f->c->api_key
                                     ? ": check the Steam Web API key" : "");
    } else if (status < 200 || status >= 300) {
        g_task_return_new_error (t, G_IO_ERROR, status == 404 ? G_IO_ERROR_NOT_FOUND
                                                              : G_IO_ERROR_FAILED,
                                 "%s answered HTTP %u", host_name[f->host], status);
    } else if (f->too_large || !b) {
        g_task_return_new_error (t, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE,
                                 "reply from %s too large", host_name[f->host]);
    } else if ((f->flags & FETCH_ERESULT) && eresult_failed (f, t)) {
        /* returned */
    } else {
        g_task_return_pointer (t, g_bytes_ref (b), (GDestroyNotify) g_bytes_unref);
    }
out:
    g_clear_error (&err);
    if (b)
        g_bytes_unref (b);
    g_object_unref (t);
}

static void fetch_read_cb (GObject *src, GAsyncResult *res, gpointer user)
{
    GTask *t = user;
    Fetch *f = g_task_get_task_data (t);
    GError *err = NULL;
    GBytes *chunk = g_input_stream_read_bytes_finish (G_INPUT_STREAM (src), res, &err);
    if (!chunk) {
        fetch_complete (t, NULL, err);
        return;
    }
    gsize n = g_bytes_get_size (chunk);
    if (n == 0) {   /* the end */
        g_bytes_unref (chunk);
        GBytes *b = g_byte_array_free_to_bytes (f->buf);
        f->buf = NULL;
        fetch_complete (t, b, NULL);
        return;
    }
    if (n > f->max_bytes || f->buf->len > f->max_bytes - n) {
        g_bytes_unref (chunk);
        f->too_large = TRUE;
        fetch_complete (t, NULL, NULL);
        return;
    }
    g_byte_array_append (f->buf, g_bytes_get_data (chunk, NULL), (guint) n);
    g_bytes_unref (chunk);
    g_input_stream_read_bytes_async (f->in, FETCH_CHUNK, G_PRIORITY_DEFAULT, f->cancel,
                                     fetch_read_cb, t);
}

/* the headers of the reply are in: follow an allowed redirect, stop at an
 * error status or an announced size over the limit, else read the body */
static void fetch_sent (GObject *src, GAsyncResult *res, gpointer user)
{
    GTask *t = user;
    Fetch *f = g_task_get_task_data (t);
    GError *err = NULL;
    g_clear_object (&f->in);
    f->in = soup_session_send_finish (SOUP_SESSION (src), res, &err);
    if (!f->in) {
        fetch_complete (t, NULL, err);
        return;
    }
    guint status = soup_message_get_status (f->msg);
    SoupMessageHeaders *h = soup_message_get_response_headers (f->msg);
    if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
        const char *loc = soup_message_headers_get_one (h, "Location");
        char *next = loc ? g_uri_resolve_relative (f->url, loc, G_URI_FLAGS_NONE, NULL) : NULL;
        if (next && !f->body && !strcmp (f->method, "GET") &&
            f->redirects < FETCH_MAX_REDIRECTS && walpe_ws_redirect_allowed (f->host, next) &&
            !g_cancellable_is_cancelled (f->cancel)) {
            f->redirects++;
            g_input_stream_close_async (f->in, G_PRIORITY_DEFAULT, NULL, NULL, NULL);
            g_clear_object (&f->in);
            memset (f->url, 0, strlen (f->url));
            g_free (f->url);
            f->url = next;
            fetch_go (t);
            return;
        }
        g_free (next);
        f->redirect_refused = TRUE;
        fetch_complete (t, NULL, NULL);
        return;
    }
    if (status < 200 || status >= 300) {
        fetch_complete (t, NULL, NULL);   /* the text of an error page is not needed */
        return;
    }
    goffset announced = soup_message_headers_get_content_length (h);
    if (announced > 0 && (guint64) announced > f->max_bytes) {
        f->too_large = TRUE;
        fetch_complete (t, NULL, NULL);
        return;
    }
    f->buf = g_byte_array_new ();
    g_input_stream_read_bytes_async (f->in, FETCH_CHUNK, G_PRIORITY_DEFAULT, f->cancel,
                                     fetch_read_cb, t);
}

static void fetch_go (GTask *t)
{
    Fetch *f = g_task_get_task_data (t);
    if (g_cancellable_is_cancelled (f->cancel)) {
        /* cancelled, or over its time, while it waited for its slot */
        GError *e = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED, "cancelled");
        fetch_complete (t, NULL, e);
        return;
    }
    g_clear_object (&f->msg);
    f->msg = soup_message_new (f->method, f->url);
    if (!f->msg) {
        g_task_return_new_error (t, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "bad URL");
        g_object_unref (t);
        return;
    }
    soup_message_set_priority (f->msg, f->prio);
    soup_message_add_flags (f->msg, SOUP_MESSAGE_NO_REDIRECT);   /* see fetch_sent */
    SoupMessageHeaders *h = soup_message_get_request_headers (f->msg);
    soup_message_headers_replace (h, "Accept-Language", "en-US,en;q=0.8");
    if (f->body)
        soup_message_set_request_body_from_bytes (f->msg, "application/x-www-form-urlencoded",
                                                  f->body);
    soup_session_send_async (f->c->session, f->msg, G_PRIORITY_DEFAULT, f->cancel, fetch_sent, t);
}

/* seconds one request may take in all (WALPE_WS_TOTAL_TIMEOUT: tests) */
static guint walpe_ws_fetch_seconds (gboolean once)
{
    const char *e = g_getenv ("WALPE_WS_TOTAL_TIMEOUT");
    guint64 v = e ? g_ascii_strtoull (e, NULL, 10) : 0;
    if (v >= 1 && v <= 3600)
        return (guint) v;
    return once ? FETCH_ONCE_TOTAL_S : FETCH_TOTAL_S;
}

static void fetch_outer_cancelled (GCancellable *outer, gpointer inner)
{
    (void) outer;
    g_cancellable_cancel (inner);
}

static gboolean fetch_deadline_cb (gpointer p)
{
    Fetch *f = g_task_get_task_data (p);
    f->deadline_id = 0;
    f->timed_out = TRUE;
    g_cancellable_cancel (f->cancel);
    return G_SOURCE_REMOVE;
}

static void fetch_async_full (WalpeWsClient *c, HostClass host, const char *method,
                              const char *url, GBytes *body, gsize max_bytes,
                              SoupMessagePriority prio, guint flags,
                              GCancellable *cancel, GAsyncReadyCallback cb, gpointer user)
{
    Fetch *f = g_new0 (Fetch, 1);
    f->c = client_ref (c);
    f->host = host;
    f->method = g_strdup (method);
    f->url = g_strdup (url);
    f->body = body ? g_bytes_ref (body) : NULL;
    f->max_bytes = max_bytes;
    f->prio = prio;
    f->flags = flags;
    f->cancel = g_cancellable_new ();
    GTask *t = g_task_new (NULL, cancel, cb, user);
    g_task_set_task_data (t, f, fetch_free);
    if (cancel) {
        f->outer = g_object_ref (cancel);
        f->outer_id = g_cancellable_connect (cancel, G_CALLBACK (fetch_outer_cancelled),
                                             g_object_ref (f->cancel), g_object_unref);
    }
    f->deadline_id = g_timeout_add_seconds (walpe_ws_fetch_seconds (flags & FETCH_ONCE),
                                            fetch_deadline_cb, t);
    fetch_schedule (t, 0);
}

static void fetch_async_prio (WalpeWsClient *c, HostClass host, const char *method,
                              const char *url, GBytes *body, gsize max_bytes,
                              SoupMessagePriority prio, GCancellable *cancel,
                              GAsyncReadyCallback cb, gpointer user)
{
    fetch_async_full (c, host, method, url, body, max_bytes, prio, 0, cancel, cb, user);
}

static void fetch_async (WalpeWsClient *c, HostClass host, const char *method, const char *url,
                         GBytes *body, gsize max_bytes, GCancellable *cancel,
                         GAsyncReadyCallback cb, gpointer user)
{
    fetch_async_prio (c, host, method, url, body, max_bytes, SOUP_MESSAGE_PRIORITY_NORMAL,
                      cancel, cb, user);
}

static GBytes *fetch_finish (GAsyncResult *res, GError **err)
{
    return g_task_propagate_pointer (G_TASK (res), err);
}

/* -------------------------------------------------------------- details */

static GBytes *details_body (const char *const *ids, guint from, guint n)
{
    GString *b = g_string_new (NULL);
    g_string_append_printf (b, "itemcount=%u", n);
    for (guint i = 0; i < n; i++)
        g_string_append_printf (b, "&publishedfileids%%5B%u%%5D=%s", i, ids[from + i]);
    gsize len = b->len;
    return g_bytes_new_take (g_string_free (b, FALSE), len);
}

typedef struct {
    WalpeWsClient *c;
    GPtrArray *ids;            /* char* */
    guint next;                /* first id of the next batch */
    GPtrArray *out;            /* WalpeWsItem* */
} DetailsCtx;

static void details_ctx_free (gpointer p)
{
    DetailsCtx *d = p;
    g_ptr_array_unref (d->ids);
    if (d->out)
        g_ptr_array_unref (d->out);
    client_unref (d->c);
    g_free (d);
}

static void details_next (GTask *t);

static void details_got (GObject *src, GAsyncResult *res, gpointer user)
{
    (void) src;
    GTask *t = user;
    DetailsCtx *d = g_task_get_task_data (t);
    GError *err = NULL;
    GBytes *b = fetch_finish (res, &err);
    if (!b) {
        g_task_return_error (t, err);
        g_object_unref (t);
        return;
    }
    gsize len = 0;
    const char *data = g_bytes_get_data (b, &len);
    GPtrArray *items = walpe_ws_parse_details (data, (gssize) len, &err);
    g_bytes_unref (b);
    if (!items) {
        g_task_return_error (t, err);
        g_object_unref (t);
        return;
    }
    for (guint i = 0; i < items->len; i++)
        g_ptr_array_add (d->out, walpe_ws_item_ref (g_ptr_array_index (items, i)));
    g_ptr_array_unref (items);
    details_next (t);
}

static void details_next (GTask *t)
{
    DetailsCtx *d = g_task_get_task_data (t);
    if (d->next >= d->ids->len) {
        g_task_return_pointer (t, g_ptr_array_ref (d->out), (GDestroyNotify) g_ptr_array_unref);
        g_object_unref (t);
        return;
    }
    guint n = MIN (100u, d->ids->len - d->next);
    GBytes *body = details_body ((const char *const *) d->ids->pdata, d->next, n);
    d->next += n;
    char *url = g_strconcat (walpe_ws_api_base (),
                             "ISteamRemoteStorage/GetPublishedFileDetails/v1/", NULL);
    fetch_async (d->c, HOST_API, "POST", url, body, 8 * 1024 * 1024,
                 g_task_get_cancellable (t), details_got, t);
    g_free (url);
    g_bytes_unref (body);
}

void walpe_ws_details_async (WalpeWsClient *c, const char *const *ids, GCancellable *cancel,
                             GAsyncReadyCallback cb, gpointer user)
{
    GTask *t = g_task_new (NULL, cancel, cb, user);
    DetailsCtx *d = g_new0 (DetailsCtx, 1);
    d->c = client_ref (c);
    d->ids = g_ptr_array_new_with_free_func (g_free);
    for (const char *const *i = ids; i && *i; i++)
        if (digits_only (*i))
            g_ptr_array_add (d->ids, g_strdup (*i));
    d->out = g_ptr_array_new_with_free_func ((GDestroyNotify) walpe_ws_item_unref);
    g_task_set_task_data (t, d, details_ctx_free);
    details_next (t);
}

GPtrArray *walpe_ws_details_finish (GAsyncResult *res, GError **err)
{
    return g_task_propagate_pointer (G_TASK (res), err);
}

/* ----------------------------------------------- subscribe / unsubscribe */

static void zero_free (gpointer p)
{
    char *s = p;
    memset (s, 0, strlen (s));   /* the form body holds the key */
    g_free (s);
}

static void subscribe_got (GObject *src, GAsyncResult *res, gpointer user)
{
    (void) src;
    GTask *t = user;
    GError *err = NULL;
    GBytes *b = fetch_finish (res, &err);
    if (!b) {
        /* 401/403: a wrong key - or Steam no longer takes this call (it is
         * not a documented one) with a user's key */
        if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED))
            g_prefix_error (&err, "Steam does not let this Web API key change your "
                                  "subscriptions: ");
        g_task_return_error (t, err);
        g_object_unref (t);
        return;
    }
    /* success is {"response":{}}; anything else is not taken for one */
    gsize len = 0;
    const char *data = g_bytes_get_data (b, &len);
    JsonNode *root = len ? parse_json (data, (gssize) len, NULL) : NULL;
    gboolean ok = root && JSON_NODE_HOLDS_OBJECT (root) &&
                  json_object_has_member (json_node_get_object (root), "response");
    if (root)
        json_node_unref (root);
    g_bytes_unref (b);
    if (ok)
        g_task_return_boolean (t, TRUE);
    else
        g_task_return_new_error (t, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                 "Steam's answer was not the expected one (the request may "
                                 "not have been carried out)");
    g_object_unref (t);
}

void walpe_ws_subscribe_async (WalpeWsClient *c, const char *id, gboolean subscribe,
                               GCancellable *cancel, GAsyncReadyCallback cb, gpointer user)
{
    GTask *t = g_task_new (NULL, cancel, cb, user);
    if (!c || !c->api_key) {
        g_task_return_new_error (t, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                                 "no Steam Web API key is saved (Workshop tab, settings)");
        g_object_unref (t);
        return;
    }
    if (!id || !digits_only (id) || strlen (id) > 20) {
        g_task_return_new_error (t, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                 "not a workshop item id");
        g_object_unref (t);
        return;
    }
    char *url = g_strconcat (walpe_ws_api_base (), "IPublishedFileService/",
                             subscribe ? "Subscribe" : "Unsubscribe", "/v1/", NULL);
    /* the key travels in the POST body, not in the URL. list_type 1 = the
     * account's subscriptions; notify_client: a running Steam client is
     * told at once (it then downloads / removes the item itself) */
    char *form = g_strdup_printf ("key=%s&publishedfileid=%s&list_type=1&appid=%d"
                                  "&notify_client=1", c->api_key, id, WS_APPID);
    GBytes *body = g_bytes_new_with_free_func (form, strlen (form), zero_free, form);
    fetch_async_full (c, HOST_API, "POST", url, body, 64 * 1024, SOUP_MESSAGE_PRIORITY_HIGH,
                      FETCH_ERESULT | FETCH_ONCE, cancel, subscribe_got, t);
    g_bytes_unref (body);
    g_free (url);
}

gboolean walpe_ws_subscribe_finish (GAsyncResult *res, GError **err)
{
    return g_task_propagate_boolean (G_TASK (res), err);
}

/* --------------------------------------------------------------- search */

typedef struct {
    WalpeWsClient *c;
    char *text;
    WalpeWsSort sort;
    guint page;
    char **req, **excl;
    WalpeWsQuery q;
    WalpeWsResults *r;
    GPtrArray *ids;
    GHashTable *page_items;    /* id -> WalpeWsItem (browse page data) */
} SearchCtx;

static void search_ctx_free (gpointer p)
{
    SearchCtx *s = p;
    g_free (s->text);
    g_strfreev (s->req);
    g_strfreev (s->excl);
    walpe_ws_results_free (s->r);
    if (s->ids)
        g_ptr_array_unref (s->ids);
    if (s->page_items)
        g_hash_table_unref (s->page_items);
    client_unref (s->c);
    g_free (s);
}

static char **strv_canon (const char *const *v)
{
    GPtrArray *a = g_ptr_array_new ();
    for (; v && *v; v++) {
        if (!**v)
            continue;
        const char *c = walpe_ws_tag_canonical (*v);
        g_ptr_array_add (a, g_strdup (c ? c : *v));
    }
    g_ptr_array_add (a, NULL);
    return (char **) g_ptr_array_free (a, FALSE);
}

/* drop hidden/removed items and anything carrying an excluded tag */
static void search_return (GTask *t)
{
    SearchCtx *s = g_task_get_task_data (t);
    GPtrArray *keep = g_ptr_array_new_with_free_func ((GDestroyNotify) walpe_ws_item_unref);
    for (guint i = 0; i < s->r->items->len; i++) {
        WalpeWsItem *it = g_ptr_array_index (s->r->items, i);
        gboolean drop = it->result != 1 || it->banned;
        for (char **e = s->excl; !drop && e && *e; e++)
            drop = walpe_ws_item_has_tag (it, *e);
        if (!drop)
            g_ptr_array_add (keep, walpe_ws_item_ref (it));
    }
    g_ptr_array_unref (s->r->items);
    s->r->items = keep;
    if (!s->r->total_pages && s->r->total)
        s->r->total_pages = (guint) ((s->r->total + WALPE_WS_PER_PAGE - 1) / WALPE_WS_PER_PAGE);
    s->r->page = s->page;
    WalpeWsResults *r = s->r;
    s->r = NULL;
    g_task_return_pointer (t, r, (GDestroyNotify) walpe_ws_results_free);
    g_object_unref (t);
}

static void page_details_got (GObject *src, GAsyncResult *res, gpointer user)
{
    (void) src;
    GTask *t = user;
    SearchCtx *s = g_task_get_task_data (t);
    GError *err = NULL;
    GPtrArray *det = walpe_ws_details_finish (res, &err);
    if (g_task_return_error_if_cancelled (t)) {
        g_clear_error (&err);
        if (det)
            g_ptr_array_unref (det);
        g_object_unref (t);
        return;
    }
    GHashTable *by = g_hash_table_new (g_str_hash, g_str_equal);
    for (guint i = 0; det && i < det->len; i++) {
        WalpeWsItem *it = g_ptr_array_index (det, i);
        g_hash_table_replace (by, it->id, it);
    }
    for (guint i = 0; i < s->ids->len; i++) {
        const char *id = g_ptr_array_index (s->ids, i);
        WalpeWsItem *d = g_hash_table_lookup (by, id);
        WalpeWsItem *p = g_hash_table_lookup (s->page_items, id);
        if (d && d->result == 1) {
            if (p) {
                if (d->stars < 0) d->stars = p->stars;
                if (d->votes < 0) d->votes = p->votes;
                if (!d->author && p->author) d->author = g_strdup (p->author);
            }
            g_ptr_array_add (s->r->items, walpe_ws_item_ref (d));
        } else if (p && !d) {
            g_ptr_array_add (s->r->items, walpe_ws_item_ref (p));   /* details failed */
        }
    }
    if (!det) {
        g_free (s->r->notice);
        s->r->notice = g_strdup_printf ("item details unavailable (%s); showing the workshop "
                                        "page's summary", err ? err->message : "?");
    }
    g_clear_error (&err);
    g_hash_table_unref (by);
    if (det)
        g_ptr_array_unref (det);
    search_return (t);
}

static void page_got (GObject *src, GAsyncResult *res, gpointer user)
{
    (void) src;
    GTask *t = user;
    SearchCtx *s = g_task_get_task_data (t);
    GError *err = NULL;
    GBytes *b = fetch_finish (res, &err);
    if (!b) {
        g_task_return_error (t, err);
        g_object_unref (t);
        return;
    }
    gsize len = 0;
    const char *html = g_bytes_get_data (b, &len);
    s->ids = g_ptr_array_new_with_free_func (g_free);
    s->page_items = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                           (GDestroyNotify) walpe_ws_item_unref);
    guint64 total = 0;
    guint pages = 0;
    gboolean ok = walpe_ws_parse_browse_page (html, len, s->ids, s->page_items, &total, &pages,
                                              &err);
    g_bytes_unref (b);
    s->r->source = "page";
    s->r->total = total;
    s->r->total_pages = pages;
    if (!ok) {
        /* "no items" pages still carry the SSR data; this is something else */
        g_task_return_error (t, err);
        g_object_unref (t);
        return;
    }
    if (s->ids->len == 0) {
        search_return (t);
        return;
    }
    g_ptr_array_add (s->ids, NULL);
    walpe_ws_details_async (s->c, (const char *const *) s->ids->pdata,
                            g_task_get_cancellable (t), page_details_got, t);
    g_ptr_array_remove_index (s->ids, s->ids->len - 1);
}

static void search_by_page (GTask *t)
{
    SearchCtx *s = g_task_get_task_data (t);
    char *url = walpe_ws_browse_url (&s->q);
    fetch_async (s->c, HOST_COMMUNITY, "GET", url, NULL, 16 * 1024 * 1024,
                 g_task_get_cancellable (t), page_got, t);
    g_free (url);
}

static void summaries_got (GObject *src, GAsyncResult *res, gpointer user)
{
    (void) src;
    GTask *t = user;
    SearchCtx *s = g_task_get_task_data (t);
    GBytes *b = res ? fetch_finish (res, NULL) : NULL;   /* names are optional */
    if (b) {
        gsize len = 0;
        const char *data = g_bytes_get_data (b, &len);
        JsonNode *root = parse_json (data, (gssize) len, NULL);
        JsonObject *resp = root && JSON_NODE_HOLDS_OBJECT (root)
                               ? wpj_obj (json_node_get_object (root), "response") : NULL;
        JsonArray *pl = resp ? wpj_arr (resp, "players") : NULL;
        for (guint i = 0; i < wpj_arr_len (pl); i++) {
            JsonObject *p = wpj_arr_obj (pl, i);
            char *sid = jstr (p, "steamid");
            const char *pn = wpj_str (p, "personaname", NULL);
            char *pname = pn ? line_text (g_strdup (pn), ITEM_MAX_NAME) : NULL;
            if (digits_only (sid) && pname && i < REPLY_MAX_ITEMS) {
                g_hash_table_replace (s->c->authors, sid, pname);
            } else {
                g_free (sid);
                g_free (pname);
            }
        }
        if (root)
            json_node_unref (root);
        g_bytes_unref (b);
    }
    for (guint i = 0; i < s->r->items->len; i++) {
        WalpeWsItem *it = g_ptr_array_index (s->r->items, i);
        const char *n = it->creator ? g_hash_table_lookup (s->c->authors, it->creator) : NULL;
        if (!it->author && n)
            it->author = g_strdup (n);
    }
    if (g_task_return_error_if_cancelled (t)) {
        g_object_unref (t);
        return;
    }
    search_return (t);
}

static void api_got (GObject *src, GAsyncResult *res, gpointer user)
{
    (void) src;
    GTask *t = user;
    SearchCtx *s = g_task_get_task_data (t);
    GError *err = NULL;
    GBytes *b = fetch_finish (res, &err);
    GPtrArray *items = NULL;
    guint64 total = 0;
    if (b) {
        gsize len = 0;
        const char *data = g_bytes_get_data (b, &len);
        items = walpe_ws_parse_queryfiles (data, (gssize) len, &total, &err);
        g_bytes_unref (b);
    }
    if (!items) {
        if (g_task_return_error_if_cancelled (t)) {
            g_clear_error (&err);
            g_object_unref (t);
            return;
        }
        /* key rejected / API down: the public page still works */
        g_free (s->r->notice);
        s->r->notice = g_strdup_printf ("Steam Web API search failed (%s); used the public "
                                        "workshop page instead", err ? err->message : "?");
        g_clear_error (&err);
        search_by_page (t);
        return;
    }
    s->r->source = "webapi";
    s->r->total = total;
    for (guint i = 0; i < items->len; i++)
        g_ptr_array_add (s->r->items, walpe_ws_item_ref (g_ptr_array_index (items, i)));
    g_ptr_array_unref (items);
    /* creator names: one GetPlayerSummaries call for the page */
    GString *ids = g_string_new (NULL);
    GHashTable *seen = g_hash_table_new (g_str_hash, g_str_equal);
    for (guint i = 0; i < s->r->items->len; i++) {
        WalpeWsItem *it = g_ptr_array_index (s->r->items, i);
        if (!digits_only (it->creator) || g_hash_table_contains (seen, it->creator) ||
            g_hash_table_contains (s->c->authors, it->creator))
            continue;
        g_hash_table_add (seen, it->creator);
        g_string_append_printf (ids, "%s%s", ids->len ? "," : "", it->creator);
    }
    g_hash_table_unref (seen);
    if (ids->len && s->c->api_key) {
        char *url = g_strdup_printf ("%sISteamUser/GetPlayerSummaries/v2/?key=%s&steamids=%s",
                                     walpe_ws_api_base (), s->c->api_key, ids->str);
        fetch_async (s->c, HOST_API, "GET", url, NULL, 4 * 1024 * 1024,
                     g_task_get_cancellable (t), summaries_got, t);
        memset (url, 0, strlen (url));
        g_free (url);
    } else {
        summaries_got (NULL, NULL, t);
    }
    g_string_free (ids, TRUE);
}

void walpe_ws_search_async (WalpeWsClient *c, const WalpeWsQuery *q, GCancellable *cancel,
                            GAsyncReadyCallback cb, gpointer user)
{
    GTask *t = g_task_new (NULL, cancel, cb, user);
    SearchCtx *s = g_new0 (SearchCtx, 1);
    s->c = client_ref (c);
    s->text = q->text ? g_strstrip (g_strdup (q->text)) : NULL;
    s->sort = q->sort < WALPE_WS_SORT_N ? q->sort : WALPE_WS_SORT_TREND;
    s->page = CLAMP (q->page, 1u, 100000u);
    s->req = strv_canon (q->required);
    s->excl = strv_canon (q->excluded);
    s->q.text = s->text;
    s->q.sort = s->sort;
    s->q.page = s->page;
    s->q.required = (const char *const *) s->req;
    s->q.excluded = (const char *const *) s->excl;
    s->r = g_new0 (WalpeWsResults, 1);
    s->r->items = g_ptr_array_new_with_free_func ((GDestroyNotify) walpe_ws_item_unref);
    s->r->source = "page";
    g_task_set_task_data (t, s, search_ctx_free);
    if (c->api_key) {
        char *url = walpe_ws_queryfiles_url (&s->q, c->api_key);
        fetch_async (c, HOST_API, "GET", url, NULL, 16 * 1024 * 1024, cancel, api_got, t);
        memset (url, 0, strlen (url));
        g_free (url);
    } else {
        search_by_page (t);
    }
}

WalpeWsResults *walpe_ws_search_finish (GAsyncResult *res, GError **err)
{
    return g_task_propagate_pointer (G_TASK (res), err);
}

/* --------------------------------------------------------------- author */

typedef struct { WalpeWsClient *c; char *sid; } AuthorCtx;

static void author_ctx_free (gpointer p)
{
    AuthorCtx *a = p;
    g_free (a->sid);
    client_unref (a->c);
    g_free (a);
}

static void author_got (GObject *src, GAsyncResult *res, gpointer user)
{
    (void) src;
    GTask *t = user;
    AuthorCtx *a = g_task_get_task_data (t);
    GError *err = NULL;
    GBytes *b = fetch_finish (res, &err);
    if (!b) {
        g_task_return_error (t, err);
        g_object_unref (t);
        return;
    }
    gsize len = 0;
    const char *data = g_bytes_get_data (b, &len);
    char *name = NULL;
    if (a->c->api_key) {
        JsonNode *root = parse_json (data, (gssize) len, NULL);
        JsonObject *resp = root && JSON_NODE_HOLDS_OBJECT (root)
                               ? wpj_obj (json_node_get_object (root), "response") : NULL;
        JsonArray *pl = resp ? wpj_arr (resp, "players") : NULL;
        if (wpj_arr_len (pl))
            name = g_strdup (wpj_str (wpj_arr_obj (pl, 0), "personaname", NULL));
        if (root)
            json_node_unref (root);
    } else {
        /* <steamID><![CDATA[name]]></steamID> */
        const char *s = g_strstr_len (data, (gssize) len, "<steamID><![CDATA[");
        const char *e = s ? g_strstr_len (s, (gssize) (len - (gsize) (s - data)), "]]></steamID>")
                          : NULL;
        if (s && e && e - s - 18 < 256)
            name = g_strndup (s + 18, (gsize) (e - s - 18));
    }
    g_bytes_unref (b);
    name = line_text (name, ITEM_MAX_NAME);   /* valid UTF-8, one line, or NULL */
    if (name) {
        g_hash_table_replace (a->c->authors, g_strdup (a->sid), g_strdup (name));
        g_task_return_pointer (t, name, g_free);
    } else {
        g_free (name);
        g_task_return_new_error (t, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "author unknown");
    }
    g_object_unref (t);
}

void walpe_ws_author_async (WalpeWsClient *c, const char *steamid, GCancellable *cancel,
                            GAsyncReadyCallback cb, gpointer user)
{
    GTask *t = g_task_new (NULL, cancel, cb, user);
    if (!digits_only (steamid)) {
        g_task_return_new_error (t, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "bad SteamID");
        g_object_unref (t);
        return;
    }
    const char *known = g_hash_table_lookup (c->authors, steamid);
    if (known) {
        g_task_return_pointer (t, g_strdup (known), g_free);
        g_object_unref (t);
        return;
    }
    AuthorCtx *a = g_new0 (AuthorCtx, 1);
    a->c = client_ref (c);
    a->sid = g_strdup (steamid);
    g_task_set_task_data (t, a, author_ctx_free);
    char *url;
    HostClass h;
    if (c->api_key) {
        url = g_strdup_printf ("%sISteamUser/GetPlayerSummaries/v2/?key=%s&steamids=%s",
                               walpe_ws_api_base (), c->api_key, steamid);
        h = HOST_API;
    } else {
        url = g_strdup_printf ("https://steamcommunity.com/profiles/%s/?xml=1", steamid);
        h = HOST_COMMUNITY;
    }
    fetch_async (c, h, "GET", url, NULL, 1024 * 1024, cancel, author_got, t);
    memset (url, 0, strlen (url));
    g_free (url);
}

char *walpe_ws_author_finish (GAsyncResult *res, GError **err)
{
    return g_task_propagate_pointer (G_TASK (res), err);
}

/* -------------------------------------------------------------- previews */

typedef struct {
    char *path;                /* cache file */
    GBytes *data;              /* downloaded bytes to store, or NULL = cached */
    int w, h;
} PreviewCtx;

static void preview_ctx_free (gpointer p)
{
    PreviewCtx *x = p;
    g_free (x->path);
    if (x->data)
        g_bytes_unref (x->data);
    g_free (x);
}

/* https on Valve's image hosts only: page data never makes us fetch
 * arbitrary URLs */
static gboolean preview_url_ok (const char *url)
{
    GUri *u = url ? g_uri_parse (url, G_URI_FLAGS_NONE, NULL) : NULL;
    if (!u)
        return FALSE;
    const char *host = g_uri_get_host (u);
    gboolean ok = !g_strcmp0 (g_uri_get_scheme (u), "https") && host &&
                  (g_str_has_suffix (host, ".steamusercontent.com") ||
                   g_str_has_suffix (host, ".steamstatic.com") ||
                   g_str_has_suffix (host, ".steampowered.com") ||
                   !g_strcmp0 (host, "steamuserimages-a.akamaihd.net"));
    g_uri_unref (u);
    return ok;
}

/* Workshop previews are JPEG, PNG or GIF (the CDN's renditions too, or
 * WebP). The bytes go to the decoder of the format their own first bytes
 * name and to no other (util.h, walpe_picture_decode): gdk-pixbuf asked to
 * find the format itself runs whichever loader the system has for them -
 * SVG (librsvg), XPM, TGA, ICO, TIFF, PNM, ... - before it can be asked
 * what it found, on bytes that came off the network.
 * A preview is decoded only when its header declares a size worth
 * decoding: JPEG is scaled while it is read, every other format is decoded
 * in full before it is scaled, and a few KB of PNG can declare gigabytes of
 * pixels. A JPEG that is progressive or has a scan per component is not
 * scaled while it is read: libjpeg holds it whole first (util.h,
 * walpe_jpeg_store_bytes), and the decode refuses it beyond
 * WALPE_JPEG_STORE_MAX. */
static const char *const preview_kinds[] = { "jpeg", "png", "gif", "webp", NULL };

typedef struct {
    int w, h;                  /* the box the picture is fitted into */
    int dw, dh;                /* the declared size */
    gboolean too_large;
} PreviewSize;

static gboolean preview_size (const char *kind, int w, int h, int *sw, int *sh, gpointer user)
{
    PreviewSize *ps = user;
    ps->dw = w;
    ps->dh = h;
    if (w < 1 || h < 1)
        return FALSE;
    if (strcmp (kind, "jpeg") &&
        (w > 16384 || h > 16384 || (gint64) w * h > (gint64) 8192 * 8192)) {
        ps->too_large = TRUE;
        return FALSE;
    }
    /* fitted into the box with its shape kept */
    if ((double) h * ps->w > (double) w * ps->h) {
        w = (int) (0.5 + (double) w * ps->h / h);
        h = ps->h;
    } else {
        h = (int) (0.5 + (double) h * ps->w / w);
        w = ps->w;
    }
    *sw = MAX (w, 1);
    *sh = MAX (h, 1);
    return TRUE;
}

static void preview_thread (GTask *t, gpointer src, gpointer data, GCancellable *cancel)
{
    (void) src; (void) cancel;
    PreviewCtx *x = data;
    GError *err = NULL;
    PreviewSize ps = { x->w, x->h, 0, 0, FALSE };
    GdkPixbuf *pb;
    if (x->data) {
        gsize len = 0;
        const guint8 *d = g_bytes_get_data (x->data, &len);
        pb = walpe_picture_decode (d, len, preview_kinds, preview_size, &ps, &err);
        if (pb) {
            /* only a preview that decoded is kept. tmp + rename: a
             * concurrent reader never sees half a file */
            char *tmp = g_strdup_printf ("%s.%p.tmp", x->path, (void *) x);
            if (g_file_set_contents (tmp, (const char *) d, (gssize) len, NULL))
                g_rename (tmp, x->path);
            g_unlink (tmp);
            g_free (tmp);
        }
    } else {
        pb = walpe_picture_decode_file (x->path, preview_kinds, preview_size, &ps, &err);
        if (pb)
            utimes (x->path, NULL);   /* LRU: recently used */
        else
            g_unlink (x->path);       /* a cache entry that cannot be shown */
    }
    if (pb) {
        g_task_return_pointer (t, pb, g_object_unref);
        return;
    }
    if (ps.too_large) {
        g_clear_error (&err);
        g_set_error (&err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                     "the preview picture is %d x %d pixels, too large to show", ps.dw, ps.dh);
    } else if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED)) {
        g_clear_error (&err);
        g_set_error_literal (&err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                             "the preview is not a JPEG, PNG, GIF or WebP picture");
    }
    g_task_return_error (t, err);
}

static void preview_got (GObject *src, GAsyncResult *res, gpointer user)
{
    (void) src;
    GTask *t = user;
    PreviewCtx *x = g_task_get_task_data (t);
    GError *err = NULL;
    x->data = fetch_finish (res, &err);
    if (!x->data) {
        g_task_return_error (t, err);
        g_object_unref (t);
        return;
    }
    g_task_run_in_thread (t, preview_thread);
    g_object_unref (t);
}

void walpe_ws_preview_async (WalpeWsClient *c, const char *id, const char *url, gboolean small,
                             int max_w, int max_h, GCancellable *cancel,
                             GAsyncReadyCallback cb, gpointer user)
{
    GTask *t = g_task_new (NULL, cancel, cb, user);
    g_task_set_priority (t, small ? G_PRIORITY_LOW : G_PRIORITY_DEFAULT);
    if (!digits_only (id) || !preview_url_ok (url)) {
        g_task_return_new_error (t, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "no preview");
        g_object_unref (t);
        return;
    }
    /* no client ref: the task may be finalised on the decode thread */
    PreviewCtx *x = g_new0 (PreviewCtx, 1);
    x->w = MAX (16, max_w);
    x->h = MAX (16, max_h);
    /* the URL hash in the name: an updated preview gets a new file */
    char *h = g_compute_checksum_for_string (G_CHECKSUM_SHA1, url, -1);
    char *name = g_strdup_printf ("%s-%.12s-%c.img", id, h, small ? 's' : 'f');
    x->path = g_build_filename (c->cache_dir, name, NULL);
    g_free (name);
    g_free (h);
    g_task_set_task_data (t, x, preview_ctx_free);
    if (g_file_test (x->path, G_FILE_TEST_IS_REGULAR)) {
        g_task_run_in_thread (t, preview_thread);
        g_object_unref (t);
        return;
    }
    /* the UGC CDN renders a smaller copy on request (what the workshop
     * page itself uses for its grid) */
    char *full = small ? g_strdup_printf ("%s%cima=fit&impolicy=Letterbox&imcolor=%%23000000"
                                          "&imw=256&imh=256&letterbox=false", url,
                                          strchr (url, '?') ? '&' : '?')
                       : g_strdup (url);
    /* tiles queue behind the preview the user is looking at */
    fetch_async_prio (c, HOST_CDN, "GET", full, NULL, 24 * 1024 * 1024,
                      small ? SOUP_MESSAGE_PRIORITY_LOW : SOUP_MESSAGE_PRIORITY_VERY_HIGH,
                      cancel, preview_got, t);
    g_free (full);
}

GdkPixbuf *walpe_ws_preview_finish (GAsyncResult *res, GError **err)
{
    return g_task_propagate_pointer (G_TASK (res), err);
}

char *walpe_ws_preview_file (GAsyncResult *res)
{
    PreviewCtx *x = G_IS_TASK (res) ? g_task_get_task_data (G_TASK (res)) : NULL;
    return x && x->path && g_file_test (x->path, G_FILE_TEST_IS_REGULAR) ? g_strdup (x->path) : NULL;
}

#endif /* HAVE_SOUP */
