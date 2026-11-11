/* wsnet_test.c - what a reply from "Steam" can do to the Workshop client
 * (src/common/workshop_query.c).
 *
 * No Steam, no network: api.steampowered.com is a libsoup server on
 * 127.0.0.1 (WALPE_WS_API_BASE) that answers with generated data; HOME and
 * the XDG dirs are a private temp dir.
 *
 *   size      a body that never ends, a few KB of gzip that unpack to
 *             160 MB, an announced length over the limit: the call fails
 *             with "too large" and the reply is not read / held in memory
 *   redirect  the Web API never follows one (the calls carry the key);
 *             walpe_ws_redirect_allowed for pages and pictures
 *   json      100000 nested brackets (stack overflow in json-glib), numbers
 *             no field holds, an escaped lone surrogate
 *   limits    thousands of items / ids / tags, megabyte strings, control
 *             and direction characters in names
 *   calls     Subscribe / Unsubscribe are sent once and end after the
 *             total time limit; a reply that trickles in ends too
 *   previews  only JPEG / PNG / GIF / WebP bytes reach a decoder
 *
 * Run: meson test -C build wsnet -v */
#include "walpe-build-config.h"
#include "common/workshop_query.h"
#include "common/util.h"
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef HAVE_SOUP
#include <libsoup/soup.h>
#endif

static int failures;
#define CHECK(c) do { if (!(c)) { g_printerr ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                                 failures++; } } while (0)

#define KEY "0123456789abcdef0123456789abcdef"

/* ------------------------------------------------------- parser limits */

static char *repeat (const char *unit, gsize n)
{
    GString *s = g_string_sized_new (strlen (unit) * n + 1);
    for (gsize i = 0; i < n; i++)
        g_string_append (s, unit);
    return g_string_free (s, FALSE);
}

static gboolean has_control (const char *s)
{
    for (const char *p = s; *p; p = g_utf8_next_char (p)) {
        gunichar c = g_utf8_get_char (p);
        if (c < 0x20 || (c >= 0x7f && c < 0xa0) || c == 0x202e || c == 0x2066 || c == 0x200f)
            return TRUE;
    }
    return FALSE;
}

static void test_limits (void)
{
    GError *err = NULL;
    /* 1000 items; the first has a 300000-character title with a line break
     * and U+202E in it, 500 tags (one of 5000 characters, one "Mature" with
     * a direction mark), a megabyte of description, numbers of no use */
    char *title = repeat ("W", 300000);
    char *desc = repeat ("x", 1024 * 1024);
    char *tag = repeat ("T", 5000);
    GString *j = g_string_new ("{\"response\":{\"publishedfiledetails\":[");
    g_string_append_printf (j, "{\"publishedfileid\":\"1001\",\"result\":1,"
                               "\"title\":\"a\\nb\\u202ec\\u0007%s\",\"description\":\"%s\","
                               "\"creator\":\"76561190000000001\",\"preview_url\":\"https://x/%s\","
                               "\"file_size\":1e300,\"subscriptions\":-5,\"views\":\"12\","
                               "\"vote_data\":{\"score\":1e300,\"votes_up\":9223372036854775807,"
                               "\"votes_down\":9223372036854775807},\"tags\":[", title, desc, title);
    for (int i = 0; i < 500; i++)
        g_string_append_printf (j, "%s{\"tag\":\"%s\"}", i ? "," : "",
                                i == 0 ? tag : i == 1 ? "Mature\\u200f" : "t");
    g_string_append (j, "]}");
    for (int i = 0; i < 999; i++)
        g_string_append_printf (j, ",{\"publishedfileid\":\"%d\",\"title\":\"t\",\"creator\":\"x<y\"}",
                                2000 + i);
    g_string_append (j, "]}}");
    GPtrArray *items = walpe_ws_parse_details (j->str, (gssize) j->len, &err);
    CHECK (items != NULL && !err);
    if (err)
        g_printerr ("parse: %s\n", err->message);
    if (items) {
        CHECK (items->len > 0 && items->len <= 200);
        WalpeWsItem *it = items->len ? g_ptr_array_index (items, 0) : NULL;
        if (it) {
            CHECK (it->title && g_utf8_strlen (it->title, -1) <= 257);
            CHECK (it->title && !has_control (it->title) && g_str_has_prefix (it->title, "a bc"));
            CHECK (it->description && g_utf8_strlen (it->description, -1) <= 16000);
            CHECK (it->preview_url == NULL);            /* 300000 characters of URL */
            CHECK (g_strv_length (it->tags) <= 64);
            CHECK (g_utf8_strlen (it->tags[0], -1) <= 129);
            CHECK (walpe_ws_item_has_tag (it, "Mature"));   /* the mark is gone: still filtered */
            for (char **t = it->tags; *t; t++)
                CHECK (g_utf8_validate (*t, -1, NULL) && !has_control (*t));
            CHECK (it->file_size == 0 && it->subscriptions == 0 && it->views == 12);
            CHECK (it->score >= 0 && it->score <= 1 && it->votes >= 0);
            char *plain = walpe_ws_bbcode_to_text (it->description);
            CHECK (plain && strlen (plain) <= 16000 * 4);
            g_free (plain);
        }
        WalpeWsItem *it2 = items->len > 1 ? g_ptr_array_index (items, 1) : NULL;
        CHECK (it2 && it2->creator == NULL);            /* not a SteamID64 */
        g_ptr_array_unref (items);
    }
    g_clear_error (&err);
    g_string_free (j, TRUE);
    g_free (title);
    g_free (desc);
    g_free (tag);

    /* an escaped lone surrogate is no character: the reply is refused, and
     * the message about it (json-glib quotes the bytes) is clean text */
    const char *sur = "{\"response\":{\"publishedfiledetails\":[{\"publishedfileid\":\"9\","
                      "\"title\":\"\\ud800\"}]}}";
    items = walpe_ws_parse_details (sur, -1, &err);
    CHECK (!items || (items->len == 1 &&
                      g_utf8_validate (((WalpeWsItem *) g_ptr_array_index (items, 0))->title
                                           ? ((WalpeWsItem *) g_ptr_array_index (items, 0))->title
                                           : "", -1, NULL)));
    CHECK (items || (err && g_utf8_validate (err->message, -1, NULL) &&
                     g_utf8_strlen (err->message, -1) < 300));
    if (items)
        g_ptr_array_unref (items);
    g_clear_error (&err);

    /* a page with 50000 item links: one page's worth of ids is taken */
    GString *h = g_string_new ("<html>");
    for (int i = 0; i < 50000; i++)
        g_string_append_printf (h, "<a href=\"https://steamcommunity.com/sharedfiles/"
                                   "filedetails/?id=%d\">x</a>\n", 100000 + i);
    GPtrArray *ids = g_ptr_array_new_with_free_func (g_free);
    guint64 total = 0;
    guint pages = 0;
    CHECK (walpe_ws_parse_browse_page (h->str, h->len, ids, NULL, &total, &pages, &err));
    CHECK (ids->len > 0 && ids->len <= 100);
    g_clear_error (&err);
    g_ptr_array_unref (ids);
    g_string_free (h, TRUE);

    /* the same through the page's embedded data: 5000 results */
    GString *q = g_string_new ("{\"queries\":[{\"queryKey\":[\"workshop_browse\"],\"state\":{\"data\":"
                               "{\"total_count\":5000,\"total_pages\":1e300,\"results\":[");
    for (int i = 0; i < 5000; i++)
        g_string_append_printf (q, "%s{\"publishedfileid\":\"%d\",\"title\":\"t\"}", i ? "," : "",
                                700000 + i);
    g_string_append (q, "]}}}]}");
    char *esc = g_strescape (q->str, NULL);
    char *page = g_strdup_printf ("<script type=\"application/json\" id=\"valve-ssr-data\">"
                                  "{\"renderContext\":{\"queryData\":\"%s\"}}</script>", esc);
    ids = g_ptr_array_new_with_free_func (g_free);
    GHashTable *by = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                            (GDestroyNotify) walpe_ws_item_unref);
    CHECK (walpe_ws_parse_browse_page (page, strlen (page), ids, by, &total, &pages, &err));
    CHECK (ids->len > 0 && ids->len <= 100 && g_hash_table_size (by) <= 200);
    g_clear_error (&err);
    g_hash_table_unref (by);
    g_ptr_array_unref (ids);
    g_free (page);
    g_free (esc);
    g_string_free (q, TRUE);
}

/* 100000 nested arrays / objects: json-glib 1.8 recurses once per level and
 * runs off the stack (SIGSEGV) - the reply has to be refused before that */
static void test_depth (void)
{
    for (int kind = 0; kind < 3; kind++) {
        GString *s = g_string_new (NULL);
        if (kind == 0) {
            for (int i = 0; i < 100000; i++)
                g_string_append_c (s, '[');
        } else if (kind == 1) {
            g_string_append (s, "{\"response\":{\"publishedfiledetails\":[{\"publishedfileid\":\"1\",\"tags\":");
            for (int i = 0; i < 100000; i++)
                g_string_append_c (s, '[');
            for (int i = 0; i < 100000; i++)
                g_string_append_c (s, ']');
            g_string_append (s, "}]}}");
        } else {
            g_string_append (s, "{\"response\":");
            for (int i = 0; i < 100000; i++)
                g_string_append (s, "{\"a\":");
            g_string_append (s, "1");
            for (int i = 0; i < 100000; i++)
                g_string_append_c (s, '}');
            g_string_append (s, "}");
        }
        GError *err = NULL;
        GPtrArray *a = walpe_ws_parse_details (s->str, (gssize) s->len, &err);
        CHECK (a == NULL && err != NULL);
        g_clear_error (&err);
        guint64 total = 0;
        a = walpe_ws_parse_queryfiles (s->str, (gssize) s->len, &total, &err);
        CHECK (a == NULL && err != NULL);
        g_clear_error (&err);
        /* the browse page's embedded JSON, and the JSON string inside it */
        char *page = g_strdup_printf ("<script id=\"valve-ssr-data\">%s</script>", s->str);
        GPtrArray *ids = g_ptr_array_new_with_free_func (g_free);
        CHECK (!walpe_ws_parse_browse_page (page, strlen (page), ids, NULL, NULL, NULL, &err));
        g_clear_error (&err);
        g_free (page);
        char *esc = g_strescape (s->str, NULL);
        page = g_strdup_printf ("<script id=\"valve-ssr-data\">{\"renderContext\":"
                                "{\"queryData\":\"%s\"}}</script>", esc);
        CHECK (!walpe_ws_parse_browse_page (page, strlen (page), ids, NULL, NULL, NULL, &err));
        g_clear_error (&err);
        g_free (page);
        g_free (esc);
        g_ptr_array_unref (ids);
        g_string_free (s, TRUE);
    }
    /* brackets inside a string are text, not nesting */
    char *br = repeat ("[", 5000);
    char *ok = g_strdup_printf ("{\"response\":{\"publishedfiledetails\":[{\"publishedfileid\":"
                                "\"7\",\"title\":\"%s\"}]}}", br);
    GError *err = NULL;
    GPtrArray *a = walpe_ws_parse_details (ok, -1, &err);
    CHECK (a && a->len == 1);
    if (a)
        g_ptr_array_unref (a);
    g_clear_error (&err);
    g_free (ok);
    g_free (br);
}

static void test_redirect_rule (void)
{
    /* 0 = a steamcommunity.com page, 1 = the Web API, 2 = a preview picture */
    CHECK (walpe_ws_redirect_allowed (0, "https://steamcommunity.com/workshop/browse/?appid=431960"));
    CHECK (walpe_ws_redirect_allowed (0, "https://www.steamcommunity.com/x"));
    CHECK (!walpe_ws_redirect_allowed (0, "http://steamcommunity.com/workshop/browse/"));
    CHECK (!walpe_ws_redirect_allowed (0, "https://steamcommunity.com.evil.example/"));
    CHECK (!walpe_ws_redirect_allowed (0, "https://evilsteamcommunity.com/"));
    CHECK (!walpe_ws_redirect_allowed (0, "https://steamcommunity.com@evil.example/"));
    CHECK (!walpe_ws_redirect_allowed (0, "https://steamcommunity.com:8443/"));
    CHECK (!walpe_ws_redirect_allowed (0, "https://127.0.0.1/"));
    CHECK (!walpe_ws_redirect_allowed (0, "file:///etc/passwd"));
    CHECK (!walpe_ws_redirect_allowed (0, NULL));
    CHECK (!walpe_ws_redirect_allowed (1, "https://api.steampowered.com/ISteamUser/x"));
    CHECK (!walpe_ws_redirect_allowed (1, "http://127.0.0.1:1/"));
    CHECK (walpe_ws_redirect_allowed (2, "https://images.steamusercontent.com/ugc/1/2/"));
    CHECK (walpe_ws_redirect_allowed (2, "https://steamuserimages-a.akamaihd.net/ugc/1/2/"));
    CHECK (!walpe_ws_redirect_allowed (2, "http://images.steamusercontent.com/ugc/1/2/"));
    CHECK (!walpe_ws_redirect_allowed (2, "https://example.org/a.png"));
    CHECK (!walpe_ws_redirect_allowed (2, "https://192.168.1.1/a.png"));
    CHECK (!walpe_ws_redirect_allowed (7, "https://steamcommunity.com/"));
}

#ifdef HAVE_SOUP

/* ---------------------------------------------------------- the stand-in */

typedef enum {
    M_NAME,         /* a normal GetPlayerSummaries / details answer */
    M_ENDLESS,      /* chunk after chunk, up to ENDLESS_MAX */
    M_GZIP,         /* Content-Encoding: gzip, GZIP_PLAIN bytes once unpacked */
    M_ANNOUNCED,    /* Content-Length far over the limit, the body trickles */
    M_REDIRECT,     /* 302 to the second server */
    M_REDIRECT_SELF,/* 307 to another path of this server */
    M_DEEP,         /* 200, 100000 x "[" */
    M_500,
    M_HANG,         /* never answers */
    M_TRICKLE,      /* one small chunk every 300 ms, for ever */
} Mode;

#define ENDLESS_CHUNK (256 * 1024)
#define ENDLESS_MAX (64u * 1024 * 1024)
#define GZIP_PLAIN (160u * 1024 * 1024)

static struct {
    Mode mode;
    int hits, hits2;            /* requests seen by the stand-in / by the second server */
    gsize sent;                 /* body bytes handed to the socket in M_ENDLESS */
    char *base2;                /* the second server */
    GBytes *gz;
    char *last_path, *last_query;
    GPtrArray *paused;          /* SoupServerMessage* left unanswered */
    guint trickle_id;
    SoupServerMessage *trickle_msg;
} Srv;

static char zeros[ENDLESS_CHUNK];

static void endless_more (SoupServerMessage *msg, gpointer user)
{
    (void) user;
    SoupMessageBody *b = soup_server_message_get_response_body (msg);
    if (Srv.sent >= ENDLESS_MAX) {
        soup_message_body_complete (b);
    } else {
        soup_message_body_append (b, SOUP_MEMORY_STATIC, zeros, sizeof zeros);
        Srv.sent += sizeof zeros;
    }
    soup_server_message_unpause (msg);
}

static gboolean trickle_cb (gpointer user)
{
    (void) user;
    if (!Srv.trickle_msg)
        return G_SOURCE_CONTINUE;
    SoupMessageBody *b = soup_server_message_get_response_body (Srv.trickle_msg);
    soup_message_body_append (b, SOUP_MEMORY_STATIC, " ", 1);
    soup_server_message_unpause (Srv.trickle_msg);
    return G_SOURCE_CONTINUE;
}

static void trickle_finished (SoupServerMessage *msg, gpointer user)
{
    (void) user;
    if (Srv.trickle_msg == msg)
        Srv.trickle_msg = NULL;
}

static GBytes *make_gzip (gsize plain)
{
    GZlibCompressor *z = g_zlib_compressor_new (G_ZLIB_COMPRESSOR_FORMAT_GZIP, 9);
    GByteArray *out = g_byte_array_new ();
    guint8 buf[65536];
    gsize left = plain;
    for (;;) {
        gsize rd = 0, wr = 0;
        gsize in = MIN (left, sizeof zeros);
        GConverterResult r = g_converter_convert (G_CONVERTER (z), zeros, in, buf, sizeof buf,
                                                  left > in ? G_CONVERTER_NO_FLAGS
                                                            : G_CONVERTER_INPUT_AT_END,
                                                  &rd, &wr, NULL);
        if (r == G_CONVERTER_ERROR)
            break;
        g_byte_array_append (out, buf, (guint) wr);
        left -= rd;
        if (r == G_CONVERTER_FINISHED)
            break;
    }
    g_object_unref (z);
    return g_byte_array_free_to_bytes (out);
}

static void handler (SoupServer *srv, SoupServerMessage *msg, const char *path,
                     GHashTable *query, gpointer user)
{
    (void) srv; (void) query; (void) user;
    Srv.hits++;
    g_free (Srv.last_path);
    Srv.last_path = g_strdup (path);
    g_free (Srv.last_query);
    Srv.last_query = g_strdup (g_uri_get_query (soup_server_message_get_uri (msg)));
    SoupMessageHeaders *h = soup_server_message_get_response_headers (msg);
    SoupMessageBody *b = soup_server_message_get_response_body (msg);
    static const char name[] = "{\"response\":{\"players\":[{\"steamid\":\"76561190000000009\","
                               "\"personaname\":\"Mock\\u202e Author\\n\"}]}}";
    switch (Srv.mode) {
    case M_NAME:
        soup_server_message_set_status (msg, 200, NULL);
        soup_server_message_set_response (msg, "application/json", SOUP_MEMORY_STATIC, name,
                                          strlen (name));
        break;
    case M_ENDLESS:
        soup_server_message_set_status (msg, 200, NULL);
        soup_message_headers_set_encoding (h, SOUP_ENCODING_CHUNKED);
        soup_message_headers_replace (h, "Content-Type", "application/json");
        soup_message_body_set_accumulate (b, FALSE);
        g_signal_connect (msg, "wrote-headers", G_CALLBACK (endless_more), NULL);
        g_signal_connect (msg, "wrote-chunk", G_CALLBACK (endless_more), NULL);
        break;
    case M_GZIP: {
        gsize len = 0;
        const char *d = g_bytes_get_data (Srv.gz, &len);
        soup_server_message_set_status (msg, 200, NULL);
        soup_message_headers_replace (h, "Content-Encoding", "gzip");
        soup_server_message_set_response (msg, "application/json", SOUP_MEMORY_STATIC, d, len);
        break;
    }
    case M_ANNOUNCED:
        soup_server_message_set_status (msg, 200, NULL);
        soup_message_headers_set_encoding (h, SOUP_ENCODING_CONTENT_LENGTH);
        soup_message_headers_set_content_length (h, (goffset) 1 << 30);
        soup_message_headers_replace (h, "Content-Type", "application/json");
        soup_message_body_set_accumulate (b, FALSE);
        soup_message_body_append (b, SOUP_MEMORY_STATIC, "{", 1);
        g_ptr_array_add (Srv.paused, g_object_ref (msg));   /* the rest never comes */
        break;
    case M_REDIRECT:
        soup_server_message_set_redirect (msg, 302, Srv.base2);
        break;
    case M_REDIRECT_SELF:
        if (g_str_has_prefix (path, "/elsewhere")) {
            soup_server_message_set_status (msg, 200, NULL);
            soup_server_message_set_response (msg, "application/json", SOUP_MEMORY_STATIC, name,
                                              strlen (name));
        } else {
            soup_server_message_set_redirect (msg, 307, "/elsewhere");
        }
        break;
    case M_DEEP: {
        char *deep = repeat ("[", 100000);
        soup_server_message_set_status (msg, 200, NULL);
        soup_server_message_set_response (msg, "application/json", SOUP_MEMORY_TAKE, deep,
                                          strlen (deep));
        break;
    }
    case M_500:
        soup_server_message_set_status (msg, 500, NULL);
        break;
    case M_HANG:
        soup_server_message_pause (msg);
        g_ptr_array_add (Srv.paused, g_object_ref (msg));
        break;
    case M_TRICKLE:
        soup_server_message_set_status (msg, 200, NULL);
        soup_message_headers_set_encoding (h, SOUP_ENCODING_CHUNKED);
        soup_message_body_set_accumulate (b, FALSE);
        soup_message_body_append (b, SOUP_MEMORY_STATIC, "{", 1);
        Srv.trickle_msg = msg;
        g_signal_connect (msg, "finished", G_CALLBACK (trickle_finished), NULL);
        if (!Srv.trickle_id)
            Srv.trickle_id = g_timeout_add (300, trickle_cb, NULL);
        break;
    }
}

static void handler2 (SoupServer *srv, SoupServerMessage *msg, const char *path,
                      GHashTable *query, gpointer user)
{
    (void) srv; (void) path; (void) query; (void) user;
    Srv.hits2++;
    static const char name[] = "{\"response\":{\"players\":[{\"steamid\":\"1\","
                               "\"personaname\":\"Elsewhere\"}],\"publishedfiledetails\":[]}}";
    soup_server_message_set_status (msg, 200, NULL);
    soup_server_message_set_response (msg, "application/json", SOUP_MEMORY_STATIC, name,
                                      strlen (name));
}

static SoupServer *listen_on (SoupServerCallback cb, char **base)
{
    GError *err = NULL;
    SoupServer *srv = soup_server_new (NULL, NULL);
    soup_server_add_handler (srv, NULL, cb, NULL, NULL);
    if (!soup_server_listen_local (srv, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &err)) {
        g_printerr ("SKIP: cannot listen on 127.0.0.1 (%s)\n", err->message);
        g_clear_error (&err);
        g_object_unref (srv);
        return NULL;
    }
    GSList *uris = soup_server_get_uris (srv);
    *base = g_strdup_printf ("http://127.0.0.1:%d/", g_uri_get_port (uris->data));
    g_slist_free_full (uris, (GDestroyNotify) g_uri_unref);
    return srv;
}

/* -------------------------------------------------------------- calls */

typedef struct { GMainLoop *loop; char *name; gboolean ok; GPtrArray *items; GError *err; } Wait;

static void author_done (GObject *src, GAsyncResult *res, gpointer user)
{
    (void) src;
    Wait *w = user;
    w->name = walpe_ws_author_finish (res, &w->err);
    g_main_loop_quit (w->loop);
}

static void details_done (GObject *src, GAsyncResult *res, gpointer user)
{
    (void) src;
    Wait *w = user;
    w->items = walpe_ws_details_finish (res, &w->err);
    g_main_loop_quit (w->loop);
}

static void sub_done (GObject *src, GAsyncResult *res, gpointer user)
{
    (void) src;
    Wait *w = user;
    w->ok = walpe_ws_subscribe_finish (res, &w->err);
    g_main_loop_quit (w->loop);
}

static gboolean give_up (gpointer user)
{
    Wait *w = user;
    g_printerr ("FAIL: the call did not end in time\n");
    failures++;
    g_main_loop_quit (w->loop);
    return G_SOURCE_REMOVE;
}

static int steamid_ctr;

/* GET ISteamUser/GetPlayerSummaries (limit 1 MiB): name or *err */
static char *author (WalpeWsClient *c, Mode mode, GError **err, double *secs)
{
    Srv.mode = mode;
    Srv.hits = Srv.hits2 = 0;
    Srv.sent = 0;
    Wait w = { g_main_loop_new (NULL, FALSE), NULL, FALSE, NULL, NULL };
    char *sid = g_strdup_printf ("7656119000000%04d", ++steamid_ctr);   /* never cached */
    gint64 t0 = g_get_monotonic_time ();
    guint guard = g_timeout_add_seconds (90, give_up, &w);
    walpe_ws_author_async (c, sid, NULL, author_done, &w);
    g_main_loop_run (w.loop);
    g_source_remove (guard);
    if (secs)
        *secs = (double) (g_get_monotonic_time () - t0) / 1e6;
    g_main_loop_unref (w.loop);
    g_free (sid);
    *err = w.err;
    return w.name;
}

/* POST ISteamRemoteStorage/GetPublishedFileDetails (limit 8 MiB) */
static GPtrArray *details (WalpeWsClient *c, Mode mode, GError **err)
{
    Srv.mode = mode;
    Srv.hits = Srv.hits2 = 0;
    Srv.sent = 0;
    Wait w = { g_main_loop_new (NULL, FALSE), NULL, FALSE, NULL, NULL };
    const char *ids[] = { "3219908811", NULL };
    guint guard = g_timeout_add_seconds (90, give_up, &w);
    walpe_ws_details_async (c, ids, NULL, details_done, &w);
    g_main_loop_run (w.loop);
    g_source_remove (guard);
    g_main_loop_unref (w.loop);
    *err = w.err;
    return w.items;
}

static gboolean subscribe (WalpeWsClient *c, Mode mode, GError **err, double *secs)
{
    Srv.mode = mode;
    Srv.hits = Srv.hits2 = 0;
    Wait w = { g_main_loop_new (NULL, FALSE), NULL, FALSE, NULL, NULL };
    gint64 t0 = g_get_monotonic_time ();
    guint guard = g_timeout_add_seconds (90, give_up, &w);
    walpe_ws_subscribe_async (c, "3219908811", TRUE, NULL, sub_done, &w);
    g_main_loop_run (w.loop);
    g_source_remove (guard);
    if (secs)
        *secs = (double) (g_get_monotonic_time () - t0) / 1e6;
    g_main_loop_unref (w.loop);
    *err = w.err;
    return w.ok;
}

static long max_rss_kb (void)
{
    struct rusage r;
    getrusage (RUSAGE_SELF, &r);
    return r.ru_maxrss;
}

static void test_replies (void)
{
    GError *err = NULL;
    char *base = NULL;
    SoupServer *srv = listen_on (handler, &base);
    SoupServer *srv2 = srv ? listen_on (handler2, &Srv.base2) : NULL;
    if (!srv || !srv2) {
        g_clear_object (&srv);
        return;
    }
    Srv.paused = g_ptr_array_new_with_free_func (g_object_unref);
    Srv.gz = make_gzip (GZIP_PLAIN);
    CHECK (g_bytes_get_size (Srv.gz) > 1000 && g_bytes_get_size (Srv.gz) < 1024 * 1024);
    g_setenv ("WALPE_WS_API_BASE", base, TRUE);
    WalpeWsClient *c = walpe_ws_client_new ();
    walpe_ws_client_set_api_key (c, KEY);
    double secs = 0;

    /* the stand-in works: a name comes back as one clean line */
    char *n = author (c, M_NAME, &err, NULL);
    CHECK (n && !err && !g_strcmp0 (n, "Mock Author"));
    CHECK (Srv.hits == 1 && !g_strcmp0 (Srv.last_path, "/ISteamUser/GetPlayerSummaries/v2/"));
    g_free (n);
    g_clear_error (&err);

    /* a body without end: "too large" once the limit (1 MiB for this call)
     * is passed - not after all 64 MiB the stand-in is willing to send */
    n = author (c, M_ENDLESS, &err, NULL);
    CHECK (!n && g_error_matches (err, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE));
    CHECK (err && !strstr (err->message, KEY));
    CHECK (Srv.sent < ENDLESS_MAX / 2);
    g_print ("wsnet: endless body: stand-in had sent %.1f MiB of %.0f when the call ended\n",
             (double) Srv.sent / 1048576.0, (double) ENDLESS_MAX / 1048576.0);
    g_free (n);
    g_clear_error (&err);
    Srv.sent = ENDLESS_MAX;   /* a transfer still in flight ends at its next chunk */

    /* %u KiB of gzip that are 160 MiB once unpacked: the limit counts the
     * unpacked bytes while they arrive, nothing near 160 MiB is ever held */
    long rss0 = max_rss_kb ();
    GPtrArray *items = details (c, M_GZIP, &err);
    long grew = max_rss_kb () - rss0;
    CHECK (!items && g_error_matches (err, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE));
    CHECK (grew < 96 * 1024);
    g_print ("wsnet: gzip reply of %" G_GSIZE_FORMAT " bytes (160 MiB unpacked): peak RSS grew "
             "by %.1f MiB\n", g_bytes_get_size (Srv.gz), (double) grew / 1024.0);
    if (items)
        g_ptr_array_unref (items);
    g_clear_error (&err);

    /* a length announced over the limit: refused at the headers, without
     * waiting for a body that never comes */
    n = author (c, M_ANNOUNCED, &err, &secs);
    CHECK (!n && g_error_matches (err, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE));
    CHECK (secs < 10);
    g_free (n);
    g_clear_error (&err);

    /* redirects: the Web API follows none. The key is in this call's URL. */
    n = author (c, M_REDIRECT, &err, NULL);
    CHECK (!n && err && Srv.hits == 1 && Srv.hits2 == 0);
    CHECK (err && strstr (err->message, "redirect") && !strstr (err->message, KEY));
    CHECK (Srv.last_query && strstr (Srv.last_query, "key=" KEY));
    g_free (n);
    g_clear_error (&err);
    n = author (c, M_REDIRECT_SELF, &err, NULL);
    CHECK (!n && err && Srv.hits == 1);
    g_free (n);
    g_clear_error (&err);
    items = details (c, M_REDIRECT, &err);      /* a POST */
    CHECK (!items && err && Srv.hits == 1 && Srv.hits2 == 0);
    if (items)
        g_ptr_array_unref (items);
    g_clear_error (&err);

    /* 100000 x "[" as the answer: an error, not a crash */
    items = details (c, M_DEEP, &err);
    CHECK (!items && err);
    if (items)
        g_ptr_array_unref (items);
    g_clear_error (&err);
    n = author (c, M_DEEP, &err, NULL);
    CHECK (!n && err);
    g_free (n);
    g_clear_error (&err);

    /* a read-only call is tried twice on a server error ... */
    n = author (c, M_500, &err, NULL);
    CHECK (!n && err && Srv.hits == 2);
    g_free (n);
    g_clear_error (&err);
    /* ... Subscribe / Unsubscribe once: nobody knows whether the first
     * request was carried out, and the user is waiting for the answer */
    CHECK (!subscribe (c, M_500, &err, &secs));
    CHECK (err && Srv.hits == 1 && secs < 2.0 && !strstr (err->message, KEY));
    g_clear_error (&err);

    /* a server that takes the request and never answers: the call ends
     * after its total time (12 s for Subscribe; 2 s here), sent once.
     * It was 20 s + 2 s + 20 s, sent twice. */
    g_setenv ("WALPE_WS_TOTAL_TIMEOUT", "2", TRUE);
    CHECK (!subscribe (c, M_HANG, &err, &secs));
    CHECK (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_TIMED_OUT));
    CHECK (Srv.hits == 1 && secs >= 0.9 && secs < 8.0);
    g_clear_error (&err);
    /* a reply that trickles in (one byte every 0.3 s) never reaches the
     * session's 20 s limit between two reads: the total time ends it */
    n = author (c, M_TRICKLE, &err, &secs);
    CHECK (!n && g_error_matches (err, G_IO_ERROR, G_IO_ERROR_TIMED_OUT));
    CHECK (secs >= 0.9 && secs < 8.0);
    g_free (n);
    g_clear_error (&err);
    g_unsetenv ("WALPE_WS_TOTAL_TIMEOUT");

    /* and the client still works afterwards */
    n = author (c, M_NAME, &err, NULL);
    CHECK (n && !err);
    g_free (n);
    g_clear_error (&err);

    if (Srv.trickle_id)
        g_source_remove (Srv.trickle_id);
    walpe_ws_client_free (c);
    g_ptr_array_unref (Srv.paused);
    soup_server_disconnect (srv);
    soup_server_disconnect (srv2);
    g_object_unref (srv);
    g_object_unref (srv2);
    g_bytes_unref (Srv.gz);
    g_unsetenv ("WALPE_WS_API_BASE");
    g_print ("wsnet: stand-in on %s exercised\n", base);
    g_free (base);
}

/* ------------------------------------------------------------ previews */

typedef struct { GMainLoop *loop; GdkPixbuf *pb; GError *err; } PWait;

static void preview_done (GObject *src, GAsyncResult *res, gpointer user)
{
    (void) src;
    PWait *w = user;
    w->pb = walpe_ws_preview_finish (res, &w->err);
    g_main_loop_quit (w->loop);
}

/* A preview that is already in the disk cache is decoded without a
 * request: plant bytes there under the name the client uses and ask for
 * it. TRUE = a picture came back. */
static gboolean cached_preview (WalpeWsClient *c, const char *id, const char *bytes, gsize len)
{
    char *url = g_strdup_printf ("https://images.steamusercontent.com/ugc/%s/preview/", id);
    char *h = g_compute_checksum_for_string (G_CHECKSUM_SHA1, url, -1);
    char *name = g_strdup_printf ("%s-%.12s-f.img", id, h);
    char *dir = walpe_ws_cache_dir ();
    char *path = g_build_filename (dir, name, NULL);
    g_mkdir_with_parents (dir, 0700);
    CHECK (g_file_set_contents (path, bytes, (gssize) len, NULL));
    PWait w = { g_main_loop_new (NULL, FALSE), NULL, NULL };
    walpe_ws_preview_async (c, id, url, FALSE, 320, 320, NULL, preview_done, &w);
    g_main_loop_run (w.loop);
    g_main_loop_unref (w.loop);
    gboolean ok = w.pb != NULL;
    g_clear_object (&w.pb);
    g_clear_error (&w.err);
    g_free (path);
    g_free (dir);
    g_free (name);
    g_free (h);
    g_free (url);
    return ok;
}

static void test_preview_formats (void)
{
    WalpeWsClient *c = walpe_ws_client_new ();
    /* a real PNG (made here) is shown */
    GdkPixbuf *pb = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, 32, 24);
    gdk_pixbuf_fill (pb, 0x3060c0ff);
    char *png = NULL;
    gsize plen = 0;
    CHECK (gdk_pixbuf_save_to_buffer (pb, &png, &plen, "png", NULL, NULL));
    CHECK (cached_preview (c, "5001", png, plen));
    char *jpg = NULL;
    gsize jlen = 0;
    CHECK (gdk_pixbuf_save_to_buffer (pb, &jpg, &jlen, "jpeg", NULL, NULL));
    CHECK (cached_preview (c, "5002", jpg, jlen));
    /* the same picture in formats a Workshop preview never has: gdk-pixbuf
     * has loaders for them and picks one by the first bytes. Not decoded. */
    char *bmp = NULL;
    gsize blen = 0;
    if (gdk_pixbuf_save_to_buffer (pb, &bmp, &blen, "bmp", NULL, NULL))
        CHECK (!cached_preview (c, "5003", bmp, blen));
    char *tif = NULL;
    gsize tlen = 0;
    if (gdk_pixbuf_save_to_buffer (pb, &tif, &tlen, "tiff", NULL, NULL))
        CHECK (!cached_preview (c, "5004", tif, tlen));
    static const char svg[] = "<?xml version=\"1.0\"?>\n<svg xmlns=\"http://www.w3.org/2000/svg\" "
                              "width=\"64\" height=\"64\"><rect width=\"64\" height=\"64\" "
                              "fill=\"red\"/></svg>\n";
    CHECK (!cached_preview (c, "5005", svg, strlen (svg)));
    static const char xpm[] = "/* XPM */\nstatic char *x[] = {\n\"2 2 1 1\",\n\". c #FF0000\",\n"
                              "\"..\",\n\"..\"\n};\n";
    CHECK (!cached_preview (c, "5006", xpm, strlen (xpm)));
    static const char pnm[] = "P3\n2 2\n255\n255 0 0 255 0 0 255 0 0 255 0 0\n";
    CHECK (!cached_preview (c, "5007", pnm, strlen (pnm)));
    CHECK (!cached_preview (c, "5008", "not a picture at all", 20));
    g_free (png);
    g_free (jpg);
    g_free (bmp);
    g_free (tif);
    g_object_unref (pb);
    walpe_ws_client_free (c);
}

#endif /* HAVE_SOUP */

/* wsnet-test [limits] [depth] [redirect] [replies] [previews]: only those */
static gboolean want (int argc, char **argv, const char *what)
{
    if (argc < 2)
        return TRUE;
    for (int i = 1; i < argc; i++)
        if (!strcmp (argv[i], what))
            return TRUE;
    return FALSE;
}

int main (int argc, char **argv)
{
    char *tmp = g_dir_make_tmp ("walpe-wsnet-XXXXXX", NULL);
    if (!tmp)
        g_error ("no temp dir");
    char *T = realpath (tmp, NULL);
    g_free (tmp);
    /* before GLib caches any of them */
    g_setenv ("HOME", T, TRUE);
    const char *xdg[] = { "XDG_CONFIG_HOME", "cfg", "XDG_CACHE_HOME", "cache", "XDG_DATA_HOME",
                          "data", NULL };
    for (int i = 0; xdg[i]; i += 2) {
        char *d = g_build_filename (T, xdg[i + 1], NULL);
        g_setenv (xdg[i], d, TRUE);
        g_free (d);
    }
    /* the stand-in is on loopback: no proxy in between */
    const char *px[] = { "http_proxy", "HTTP_PROXY", "https_proxy", "HTTPS_PROXY", "all_proxy",
                         "ALL_PROXY", NULL };
    for (int i = 0; px[i]; i++)
        g_unsetenv (px[i]);
    g_setenv ("no_proxy", "127.0.0.1,localhost", TRUE);
    g_setenv ("GSETTINGS_BACKEND", "memory", TRUE);
    g_unsetenv ("WALPE_WS_TOTAL_TIMEOUT");

    if (want (argc, argv, "limits"))
        test_limits ();
    if (want (argc, argv, "depth"))
        test_depth ();
    if (want (argc, argv, "redirect") && walpe_ws_available ())
        test_redirect_rule ();
#ifdef HAVE_SOUP
    if (walpe_ws_available ()) {
        if (want (argc, argv, "replies"))
            test_replies ();
        if (want (argc, argv, "previews"))
            test_preview_formats ();
    }
#else
    g_printerr ("SKIP replies: built without libsoup\n");
#endif

    /* only what this test made */
    char *rm[] = { "/bin/rm", "-rf", "--", T, NULL };
    if (!failures && strstr (T, "walpe-wsnet-"))
        g_spawn_sync (NULL, rm, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, NULL, NULL, NULL);
    free (T);
    if (failures) {
        g_printerr ("wsnet: %d check(s) FAILED\n", failures);
        return 1;
    }
    g_print ("wsnet: ok\n");
    return 0;
}
