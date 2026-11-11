/* resmodel_test.c - the GUI's model of the per-monitor render resolution
 * (src/ui/resmodel.c), no display, no daemon, no GTK:
 * the value text, preset sizes on monitors of several shapes, the status
 * reply and the listing read into one state, who is asked (an engine that
 * does not say "not asked" never causes a question), the choices offered
 * for a monitor, the check of a custom size, the requests, and when a
 * newly connected monitor is announced. */
#include <glib.h>
#include <json-glib/json-glib.h>
#include <stdarg.h>
#include <string.h>
#include "common/util.h"
#include "ui/resmodel.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; g_printerr ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static JsonObject *obj (const char *json)
{
    JsonNode *n = wpj_parse_data (json, -1);
    g_assert (n && JSON_NODE_HOLDS_OBJECT (n));
    JsonObject *o = json_object_ref (json_node_get_object (n));
    json_node_unref (n);
    return o;
}

static WalpeMonresState *state (const char *json)
{
    JsonObject *o = obj (json);
    WalpeMonresState *st = walpe_monres_state_new (o);
    json_object_unref (o);
    return st;
}

static gboolean text_is (char *got, const char *want)
{
    gboolean ok = !g_strcmp0 (got, want);
    if (!ok)
        g_printerr ("     got \"%s\", wanted \"%s\"\n", got ? got : "(null)", want);
    g_free (got);
    return ok;
}

/* ------------------------------------------------------------ the value */

static void test_value (void)
{
    WalpeMonres v;
    CHECK (walpe_monres_parse ("native", &v) && v.kind == WALPE_MONRES_NATIVE);
    CHECK (walpe_monres_parse (" Native ", &v) && v.kind == WALPE_MONRES_NATIVE);
    CHECK (walpe_monres_parse ("720p", &v) && v.kind == WALPE_MONRES_LINES && v.lines == 720);
    CHECK (walpe_monres_parse ("1080P", &v) && v.lines == 1080);
    CHECK (walpe_monres_parse ("1440p", &v) && v.lines == 1440);
    CHECK (walpe_monres_parse ("4k", &v) && v.kind == WALPE_MONRES_LINES && v.lines == 2160);
    CHECK (walpe_monres_parse ("4K", &v) && v.lines == 2160);
    CHECK (walpe_monres_parse ("2160p", &v) && v.lines == 2160);
    CHECK (walpe_monres_parse ("uhd", &v) && v.lines == 2160);
    CHECK (walpe_monres_parse ("2k", &v) && v.lines == 1440);
    CHECK (walpe_monres_parse ("qhd", &v) && v.lines == 1440);
    CHECK (walpe_monres_parse ("fhd", &v) && v.lines == 1080);
    CHECK (walpe_monres_parse ("hd", &v) && v.lines == 720);
    CHECK (walpe_monres_parse ("2560x1440", &v) && v.kind == WALPE_MONRES_CUSTOM && v.w == 2560 &&
           v.h == 1440);
    CHECK (walpe_monres_parse ("1920X1080", &v) && v.w == 1920 && v.h == 1080);
    CHECK (walpe_monres_parse ("1920 x 1080", &v) && v.w == 1920 && v.h == 1080);
    CHECK (walpe_monres_parse ("1920×1080", &v) && v.w == 1920 && v.h == 1080);
    /* not values: *out is native */
    static const char *const bad[] = { "", " ", "nativ", "1080", "1080i", "x1080", "1920x", "1920x-1",
                                       "0x0", "1920x0", "123456x100", "1920x1080x3", "1920 1080",
                                       "1e3x1e3", "-1920x1080", "1920x1080p", "p", "999999999999p" };
    for (guint i = 0; i < G_N_ELEMENTS (bad); i++) {
        v.kind = WALPE_MONRES_CUSTOM;
        if (walpe_monres_parse (bad[i], &v) || v.kind != WALPE_MONRES_NATIVE) {
            fails++;
            g_printerr ("FAIL \"%s\" was read as a resolution\n", bad[i]);
        }
    }
    CHECK (!walpe_monres_parse (NULL, &v) && v.kind == WALPE_MONRES_NATIVE);
    CHECK (!walpe_monres_parse ("junk", NULL));

    /* the canonical text, and back */
    static const char *const canon[] = { "native", "720p", "1080p", "1440p", "4k", "2560x1440",
                                         "320x200" };
    for (guint i = 0; i < G_N_ELEMENTS (canon); i++) {
        WalpeMonres a, b;
        CHECK (walpe_monres_parse (canon[i], &a));
        char *t = walpe_monres_text (&a);
        CHECK (walpe_monres_parse (t, &b) && walpe_monres_equal (&a, &b));
        CHECK (text_is (t, canon[i]));
    }
    CHECK (text_is (walpe_monres_text (NULL), "native"));
    WalpeMonres a = { WALPE_MONRES_LINES, 1080, 0, 0 }, b = { WALPE_MONRES_LINES, 720, 0, 0 };
    CHECK (!walpe_monres_equal (&a, &b) && walpe_monres_equal (&a, &a) && !walpe_monres_equal (&a, NULL));
    WalpeMonres c1 = { WALPE_MONRES_CUSTOM, 0, 1920, 1080 }, c2 = { WALPE_MONRES_CUSTOM, 0, 1920, 1200 };
    CHECK (!walpe_monres_equal (&c1, &c2) && walpe_monres_equal (&c1, &c1) && !walpe_monres_equal (&c1, &a));

    const char *label = NULL;
    CHECK (walpe_monres_preset (0, &label) == 720 && !strcmp (label, "720p"));
    CHECK (walpe_monres_preset (3, &label) == 2160 && !strcmp (label, "4K"));
    CHECK (walpe_monres_preset (WALPE_MONRES_N_PRESETS, &label) == 0 && label == NULL);
}

static void test_sizes (void)
{
    struct {
        int mode_w, mode_h, lines, w, h;
    } t[] = {
        { 1920, 1080, 720, 1280, 720 },
        { 1920, 1080, 1080, 1920, 1080 },
        { 1920, 1080, 2160, 3840, 2160 },
        { 3840, 2160, 1080, 1920, 1080 },
        { 2560, 1440, 1080, 1920, 1080 },
        { 3440, 1440, 1080, 2580, 1080 },       /* 21:9 keeps its shape */
        { 3440, 1440, 1440, 3440, 1440 },
        { 1920, 1200, 1080, 1728, 1080 },       /* 16:10 */
        { 1080, 1920, 720, 720, 1280 },         /* turned upright */
        { 1366, 768, 720, 1280, 720 },          /* an even number on the longer side */
        { 0, 0, 1080, 1920, 1080 },             /* unknown mode: 16:9 */
        { 5120, 1440, 2160, 7680, 2160 },
    };
    for (guint i = 0; i < G_N_ELEMENTS (t); i++) {
        WalpeMonres v = { WALPE_MONRES_LINES, t[i].lines, 0, 0 };
        int w = -1, h = -1;
        gboolean ok = walpe_monres_size (&v, t[i].mode_w, t[i].mode_h, &w, &h);
        if (!ok || w != t[i].w || h != t[i].h) {
            fails++;
            g_printerr ("FAIL %dp on %dx%d gave %dx%d, wanted %dx%d\n", t[i].lines, t[i].mode_w,
                        t[i].mode_h, w, h, t[i].w, t[i].h);
        }
        CHECK (w % 2 == 0 || w == t[i].mode_w);
    }
    WalpeMonres n = { WALPE_MONRES_NATIVE, 0, 0, 0 }, c = { WALPE_MONRES_CUSTOM, 0, 1600, 900 };
    int w = -1, h = -1;
    CHECK (walpe_monres_size (&n, 2560, 1440, &w, &h) && w == 2560 && h == 1440);
    CHECK (!walpe_monres_size (&n, 0, 0, &w, &h) && w == 0 && h == 0);
    CHECK (walpe_monres_size (&c, 2560, 1440, &w, &h) && w == 1600 && h == 900);
    CHECK (walpe_monres_size (&c, 0, 0, &w, &h) && w == 1600 && h == 900);
    CHECK (walpe_monres_size (&c, 1920, 1080, NULL, NULL));
    /* never more pixels than the engine takes */
    WalpeMonres big = { WALPE_MONRES_LINES, 2160, 0, 0 };
    CHECK (walpe_monres_size (&big, 32000, 1000, &w, &h));
    CHECK (w <= WALPE_MONRES_MAX_SIDE && (gint64) w * h <= WALPE_MONRES_MAX_PIXELS && h == 2160);
    /* a mode no monitor has (the numbers are the window system's): the
     * engine's answer, and no sum on the way that an int cannot hold */
    w = h = -1;
    CHECK (walpe_monres_size (&big, 2147483647, 1, &w, &h) && w == WALPE_MONRES_MAX_SIDE && h == 2160);
    w = h = -1;
    CHECK (walpe_monres_size (&big, 1, 2147483647, &w, &h) && w == 2160 && h == WALPE_MONRES_MAX_SIDE);
    w = h = -1;
    CHECK (walpe_monres_size (&big, 65535, 3, &w, &h) && w == WALPE_MONRES_MAX_SIDE && h == 2160);
}

static void test_limits_text (void)
{
    WalpeTexLimit t;
    CHECK (walpe_tex_limit_parse ("auto", &t) && t.kind == WALPE_LIMIT_AUTO);
    CHECK (walpe_tex_limit_parse ("none", &t) && t.kind == WALPE_LIMIT_NONE);
    CHECK (walpe_tex_limit_parse ("Off", &t) && t.kind == WALPE_LIMIT_NONE);
    CHECK (walpe_tex_limit_parse ("4096", &t) && t.kind == WALPE_LIMIT_VALUE && t.px == 4096);
    CHECK (walpe_tex_limit_parse ("2k", &t) && t.kind == WALPE_LIMIT_VALUE && t.px == 2048);
    CHECK (walpe_tex_limit_parse ("16K", &t) && t.px == 16384);
    CHECK (!walpe_tex_limit_parse ("big", &t) && t.kind == WALPE_LIMIT_AUTO);
    CHECK (!walpe_tex_limit_parse ("40 96", &t));
    CHECK (!walpe_tex_limit_parse (NULL, &t));
    t.kind = WALPE_LIMIT_VALUE;
    t.px = 2048;
    CHECK (text_is (walpe_tex_limit_text (&t), "2048"));
    t.kind = WALPE_LIMIT_NONE;
    CHECK (text_is (walpe_tex_limit_text (&t), "none"));
    CHECK (text_is (walpe_tex_limit_text (NULL), "auto"));
    WalpeTexLimit a = { WALPE_LIMIT_VALUE, 2048 }, b = { WALPE_LIMIT_VALUE, 4096 }, c = { WALPE_LIMIT_AUTO, 7 },
                  d = { WALPE_LIMIT_AUTO, 0 };
    CHECK (!walpe_tex_limit_equal (&a, &b) && walpe_tex_limit_equal (&c, &d) && !walpe_tex_limit_equal (&a, &c));

    WalpeVideoLimit v;
    CHECK (walpe_video_limit_parse ("auto", &v) && v.kind == WALPE_LIMIT_AUTO);
    CHECK (walpe_video_limit_parse ("native", &v) && v.kind == WALPE_LIMIT_AUTO);
    CHECK (walpe_video_limit_parse ("none", &v) && v.kind == WALPE_LIMIT_NONE);
    CHECK (walpe_video_limit_parse ("1080p", &v) && v.kind == WALPE_LIMIT_VALUE &&
           v.size.kind == WALPE_MONRES_LINES && v.size.lines == 1080);
    CHECK (walpe_video_limit_parse ("2560x1440", &v) && v.kind == WALPE_LIMIT_VALUE && v.size.w == 2560);
    CHECK (!walpe_video_limit_parse ("large", &v) && v.kind == WALPE_LIMIT_AUTO);
    CHECK (walpe_video_limit_parse ("1080p", &v));
    CHECK (text_is (walpe_video_limit_text (&v), "1080p"));
    CHECK (walpe_video_limit_parse ("none", &v));
    CHECK (text_is (walpe_video_limit_text (&v), "none"));
}

/* ------------------------------------------------------------ the state */

/* what an engine of the last release sends: no word about resolutions */
#define STATUS_OLD \
    "{\"path\":\"/w/a\",\"paused\":false,\"fullscreen_pause\":false,\"monitors\":[" \
    "{\"index\":0,\"name\":\"HDMI-0\",\"geometry\":[0,0,1920,1080],\"refresh\":143.9}," \
    "{\"index\":1,\"name\":\"DP-2\",\"geometry\":[1920,0,1920,1080],\"refresh\":60}]," \
    "\"monitor_count\":2}"

/* an engine with the feature, one monitor not asked about */
#define STATUS_NEW \
    "{\"resolution_prompt\":true,\"resolution_unasked\":1,\"fullscreen_pause\":false,\"monitors\":[" \
    "{\"index\":0,\"name\":\"HDMI-0\",\"geometry\":[0,0,1920,1080],\"mode\":[1920,1080]," \
    "\"refresh\":143.9,\"resolution\":\"native\",\"render_size\":[1920,1080]," \
    "\"resolution_asked\":true,\"texture_limit\":\"auto\"}," \
    "{\"index\":1,\"name\":\"DP-2\",\"geometry\":[1920,0,3840,2160],\"mode\":[3840,2160]," \
    "\"refresh\":59.94,\"resolution\":\"1080p\",\"render_size\":[1920,1080]," \
    "\"resolution_asked\":false,\"texture_limit\":\"2048\"}]}"

#define LISTING \
    "{\"ok\":true,\"prompt\":true,\"unasked\":1," \
    "\"limits\":{\"min_width\":320,\"min_height\":200,\"max_side\":8192,\"max_pixels\":67108864}," \
    "\"scale_filter\":\"bilinear\",\"video_resolution\":\"1080p\",\"video_box\":[1920,1080]," \
    "\"note\":\"DP-9 is not connected\",\"monitors\":[" \
    "{\"index\":0,\"name\":\"HDMI-0\",\"connected\":true,\"mode\":[1920,1080],\"refresh\":143.9," \
    "\"resolution\":\"native\",\"render_size\":[1920,1080],\"asked\":true,\"texture_limit\":\"auto\"," \
    "\"texture_limit_px\":0,\"choices\":[{\"value\":\"native\",\"size\":[1920,1080],\"percent\":100}," \
    "{\"value\":\"720p\",\"size\":[1280,720],\"percent\":44}," \
    "{\"value\":\"1080p\",\"size\":[1920,1080],\"percent\":100}]}," \
    "{\"index\":1,\"name\":\"DP-2\",\"connected\":true,\"mode\":[3440,1440],\"refresh\":100," \
    "\"resolution\":\"2000x900\",\"render_size\":[2000,900],\"asked\":false,\"texture_limit\":\"4096\"," \
    "\"texture_limit_px\":4096,\"choices\":[{\"value\":\"native\",\"size\":[3440,1440]}," \
    "{\"value\":\"720p\",\"size\":[1720,720]},{\"value\":\"1080p\",\"size\":[2580,1080]}," \
    "{\"value\":\"1440p\",\"size\":[3440,1440]}]}," \
    "{\"name\":\"DP-3\",\"connected\":false,\"resolution\":\"1440p\",\"texture_limit\":\"auto\"," \
    "\"asked\":true}]}"

static void test_state_status (void)
{
    /* no engine */
    WalpeMonresState *st = walpe_monres_state_new (NULL);
    CHECK (st && !st->supported && st->mons && st->mons->len == 0 && st->prompt);
    CHECK (walpe_monres_state_unasked (st, NULL) == 0);
    CHECK (st->min_w == WALPE_MONRES_MIN_W && st->max_side == WALPE_MONRES_MAX_SIDE);
    walpe_monres_state_free (st);
    walpe_monres_state_free (NULL);

    /* an engine without the feature: its monitors are listed, nobody is asked */
    st = state (STATUS_OLD);
    CHECK (!st->supported && st->mons->len == 2);
    CHECK (walpe_monres_state_unasked (st, NULL) == 0);
    const WalpeMonresMon *m = walpe_monres_state_find (st, "HDMI-0");
    CHECK (m && !m->knows && m->asked && m->mode_w == 1920 && m->mode_h == 1080);
    CHECK (m && m->choice.kind == WALPE_MONRES_NATIVE && !m->tex_known);
    CHECK (!walpe_monres_state_find (st, "hdmi-0") && !walpe_monres_state_find (st, NULL));
    walpe_monres_state_free (st);

    st = state (STATUS_NEW);
    CHECK (st->supported && !st->full && st->prompt && !st->fullscreen && st->mons->len == 2);
    m = walpe_monres_state_find (st, "DP-2");
    CHECK (m && m->knows && !m->asked && m->index == 1);
    CHECK (m && m->choice.kind == WALPE_MONRES_LINES && m->choice.lines == 1080 && !m->choice_raw);
    CHECK (m && m->mode_w == 3840 && m->mode_h == 2160 && m->render_w == 1920 && m->render_h == 1080);
    CHECK (m && m->refresh > 59.9 && m->refresh < 60.0);
    CHECK (m && m->tex_known && m->tex.kind == WALPE_LIMIT_VALUE && m->tex.px == 2048);
    GPtrArray *names = g_ptr_array_new_with_free_func (g_free);
    CHECK (walpe_monres_state_unasked (st, names) == 1 && names->len == 1 &&
           !strcmp (names->pdata[0], "DP-2"));
    g_ptr_array_unref (names);
    walpe_monres_state_free (st);

    /* asking turned off: the monitor stays "not asked", nobody is asked */
    st = state ("{\"resolution_prompt\":false,\"monitors\":[{\"name\":\"A\",\"mode\":[1920,1080],"
                "\"resolution\":\"native\",\"resolution_asked\":false}]}");
    CHECK (st->supported && !st->prompt && walpe_monres_state_unasked (st, NULL) == 0);
    CHECK (!((WalpeMonresMon *) st->mons->pdata[0])->asked);
    walpe_monres_state_free (st);

    /* "not asked" is only a plain false next to a resolution: everything
     * else counts as asked, so nothing of this opens a question */
    static const char *const never[] = {
        "{\"monitors\":[{\"name\":\"A\",\"resolution\":\"native\"}]}",
        "{\"monitors\":[{\"name\":\"A\",\"resolution\":\"native\",\"resolution_asked\":null}]}",
        "{\"monitors\":[{\"name\":\"A\",\"resolution\":\"native\",\"resolution_asked\":0}]}",
        "{\"monitors\":[{\"name\":\"A\",\"resolution\":\"native\",\"resolution_asked\":\"no\"}]}",
        "{\"monitors\":[{\"name\":\"A\",\"resolution\":\"native\",\"resolution_asked\":[false]}]}",
        "{\"monitors\":[{\"name\":\"A\",\"resolution_asked\":false}]}",     /* no resolution reported */
        "{\"monitors\":[{\"name\":\"A\",\"asked\":false}]}",
        "{\"monitors\":[{\"name\":\"\",\"resolution\":\"native\",\"resolution_asked\":false}]}",
        /* a name the engine cannot store an answer under (a tab in it) */
        "{\"monitors\":[{\"name\":\"a\\tb\",\"resolution\":\"native\",\"resolution_asked\":false}]}",
        "{\"monitors\":2}",
        "{\"monitors\":[3,\"x\",null,[]]}",
        "{}",
    };
    for (guint i = 0; i < G_N_ELEMENTS (never); i++) {
        st = state (never[i]);
        if (walpe_monres_state_unasked (st, NULL) != 0) {
            fails++;
            g_printerr ("FAIL a question would be asked for %s\n", never[i]);
        }
        walpe_monres_state_free (st);
    }

    /* the names the engine takes: 1 to 64 bytes, no control characters */
    {
        char *n64 = g_strnfill (64, 'x'), *n65 = g_strnfill (65, 'x');
        CHECK (walpe_monres_name_ok ("HDMI-0") && walpe_monres_name_ok ("Büro links") &&
               walpe_monres_name_ok (n64));
        CHECK (!walpe_monres_name_ok (n65) && !walpe_monres_name_ok ("") && !walpe_monres_name_ok (NULL));
        CHECK (!walpe_monres_name_ok ("a\tb") && !walpe_monres_name_ok ("a\nb") &&
               !walpe_monres_name_ok ("a\x7f") && !walpe_monres_name_ok ("a\xff"));
        /* such a monitor is not announced either, and a usable one next to it is */
        char *js = g_strdup_printf ("{\"resolution_prompt\":true,\"monitors\":["
                                    "{\"name\":\"OK\",\"resolution\":\"native\",\"resolution_asked\":true},"
                                    "{\"name\":\"%s\",\"resolution\":\"native\",\"resolution_asked\":false},"
                                    "{\"name\":\"NEW\",\"resolution\":\"native\",\"resolution_asked\":false}]}",
                                    n65);
        WalpeMonresTracker *t = walpe_monres_tracker_new ();
        WalpeMonresState *first = state ("{\"resolution_prompt\":true,\"monitors\":[{\"name\":\"OK\","
                                         "\"resolution\":\"native\",\"resolution_asked\":true}]}");
        CHECK (walpe_monres_tracker_feed (t, first, NULL, NULL) == NULL);
        walpe_monres_state_free (first);
        st = state (js);
        GPtrArray *news = walpe_monres_tracker_feed (t, st, NULL, NULL);
        CHECK (news && news->len == 1 && !strcmp (news->pdata[0], "NEW"));
        CHECK (walpe_monres_state_unasked (st, NULL) == 1);
        if (news)
            g_ptr_array_unref (news);
        walpe_monres_state_free (st);
        walpe_monres_tracker_free (t);
        g_free (js);
        g_free (n64);
        g_free (n65);
    }

    /* fullscreen: the status member, else the older one */
    st = state ("{\"fullscreen\":true,\"fullscreen_pause\":false}");
    CHECK (st->fullscreen);
    walpe_monres_state_free (st);
    st = state ("{\"fullscreen\":false,\"fullscreen_pause\":true}");
    CHECK (!st->fullscreen);
    walpe_monres_state_free (st);
    st = state ("{\"fullscreen_pause\":true}");
    CHECK (st->fullscreen);
    walpe_monres_state_free (st);

    /* a value this build cannot read is kept as text, the row is native */
    st = state ("{\"monitors\":[{\"name\":\"A\",\"mode\":[1920,1080],\"resolution\":\"8k-ish\"}]}");
    m = st->mons->pdata[0];
    CHECK (m->knows && m->choice.kind == WALPE_MONRES_NATIVE && !g_strcmp0 (m->choice_raw, "8k-ish"));
    walpe_monres_state_free (st);

    /* nonsense in the numbers does no harm */
    st = state ("{\"monitors\":[{\"index\":-5,\"name\":\"A\",\"mode\":[1e30,-3],\"geometry\":[0,0,\"w\",9],"
                "\"refresh\":1e99,\"resolution\":7,\"render_size\":[0,0],\"texture_limit\":-1,"
                "\"texture_limit_px\":1e30,\"choices\":{\"a\":1}}],\"resolution_limits\":3,"
                "\"limits\":{\"min_width\":9000,\"min_height\":1,\"max_side\":10,\"max_pixels\":-1},"
                "\"video_box\":[\"a\",2],\"video_resolution\":5}");
    m = st->mons->pdata[0];
    CHECK (m->index == 0 && m->mode_w == 0 && m->mode_h == 0 && m->refresh == 0);
    CHECK (m->knows && m->choice_raw && m->render_w == 0 && !m->tex_known && m->tex_now == 0);
    CHECK (m->offered == NULL);
    CHECK (st->min_w == WALPE_MONRES_MIN_W && st->min_h == WALPE_MONRES_MIN_H &&
           st->max_side == WALPE_MONRES_MAX_SIDE && st->max_pixels == WALPE_MONRES_MAX_PIXELS);
    CHECK (!st->video_now_known && !st->video_known);
    walpe_monres_state_free (st);

    /* a reply with hundreds of "monitors" is cut off */
    GString *many = g_string_new ("{\"monitors\":[");
    for (int i = 0; i < 500; i++)
        g_string_append_printf (many, "%s{\"name\":\"M%d\",\"resolution\":\"native\"}", i ? "," : "", i);
    g_string_append (many, "]}");
    st = state (many->str);
    CHECK (st->mons->len > 0 && st->mons->len <= 64);
    g_string_free (many, TRUE);
    walpe_monres_state_free (st);
}

static void test_state_listing (void)
{
    WalpeMonresState *st = state (LISTING);
    CHECK (st->supported && st->full && st->prompt);
    CHECK (st->mons->len == 2 && st->stored_only == 1);     /* DP-3 is not connected */
    CHECK (!walpe_monres_state_find (st, "DP-3"));
    CHECK (st->min_w == 320 && st->min_h == 200 && st->max_side == 8192 && st->max_pixels == 67108864);
    CHECK (st->video_known && st->video.kind == WALPE_LIMIT_VALUE && st->video.size.lines == 1080);
    CHECK (st->video_now_known && st->video_now_w == 1920 && st->video_now_h == 1080);
    CHECK (!g_strcmp0 (st->scale_filter, "bilinear"));
    CHECK (!g_strcmp0 (st->note, "DP-9 is not connected"));
    const WalpeMonresMon *a = walpe_monres_state_find (st, "HDMI-0");
    const WalpeMonresMon *b = walpe_monres_state_find (st, "DP-2");
    CHECK (a && a->asked && a->offered && a->offered->len == 3);
    CHECK (b && !b->asked && b->choice.kind == WALPE_MONRES_CUSTOM && b->choice.w == 2000 &&
           b->choice.h == 900);
    CHECK (b && b->tex_known && b->tex.px == 4096 && b->tex_now == 4096);
    CHECK (walpe_monres_state_unasked (st, NULL) == 1);

    /* the size of a value on a monitor: the engine's where it sent one */
    WalpeMonres v = { WALPE_MONRES_LINES, 720, 0, 0 };
    int w = 0, h = 0;
    CHECK (walpe_monres_mon_size (b, &v, &w, &h) && w == 1720 && h == 720);
    v.lines = 2160;         /* not in its list: computed */
    CHECK (walpe_monres_mon_size (b, &v, &w, &h) && w == 5160 && h == 2160);
    CHECK (walpe_monres_mon_size (NULL, &v, &w, &h) && w == 3840 && h == 2160);

    /* the texture limit as one setting */
    WalpeTexLimit common = { WALPE_LIMIT_NONE, 0 };
    gboolean any = FALSE;
    CHECK (!walpe_monres_state_texture (st, &common, &any) && any);     /* auto and 4096 */
    walpe_monres_state_free (st);
    st = state ("{\"monitors\":[{\"name\":\"A\",\"resolution\":\"native\",\"texture_limit\":\"2048\"},"
                "{\"name\":\"B\",\"resolution\":\"native\",\"texture_limit\":2048},"
                "{\"name\":\"C\",\"resolution\":\"native\"}]}");
    CHECK (walpe_monres_state_texture (st, &common, &any) && any && common.kind == WALPE_LIMIT_VALUE &&
           common.px == 2048);
    walpe_monres_state_free (st);
    st = state (STATUS_OLD);
    CHECK (!walpe_monres_state_texture (st, &common, &any) && !any);
    walpe_monres_state_free (st);
    CHECK (!walpe_monres_state_texture (NULL, &common, &any) && !any);

    /* "video_box": [0, 0] = no limit, and that is known */
    st = state ("{\"prompt\":true,\"limits\":{},\"video_resolution\":\"none\",\"video_box\":[0,0]}");
    CHECK (st->full && st->video_known && st->video.kind == WALPE_LIMIT_NONE);
    CHECK (st->video_now_known && st->video_now_w == 0 && st->video_now_h == 0);
    walpe_monres_state_free (st);

    /* signatures: the same reply twice is the same, a changed choice is not */
    WalpeMonresState *s1 = state (LISTING), *s2 = state (LISTING), *s3 = state (STATUS_NEW);
    char *g1 = walpe_monres_state_signature (s1), *g2 = walpe_monres_state_signature (s2),
         *g3 = walpe_monres_state_signature (s3);
    CHECK (!strcmp (g1, g2) && strcmp (g1, g3));
    ((WalpeMonresMon *) s2->mons->pdata[0])->choice.kind = WALPE_MONRES_LINES;
    ((WalpeMonresMon *) s2->mons->pdata[0])->choice.lines = 720;
    g_free (g2);
    g2 = walpe_monres_state_signature (s2);
    CHECK (strcmp (g1, g2));
    ((WalpeMonresMon *) s1->mons->pdata[1])->asked = TRUE;
    g_free (g3);
    g3 = walpe_monres_state_signature (s1);
    CHECK (strcmp (g1, g3));
    CHECK (text_is (walpe_monres_state_signature (NULL), ""));
    g_free (g1);
    g_free (g2);
    g_free (g3);
    walpe_monres_state_free (s1);
    walpe_monres_state_free (s2);
    walpe_monres_state_free (s3);
}

/* ------------------------------------------------- what a row offers */

/* "Native (1920 × 1080)|720p (1280 × 720)|..." */
static char *labels (GPtrArray *o)
{
    GString *s = g_string_new (NULL);
    for (guint i = 0; i < o->len; i++)
        g_string_append_printf (s, "%s%s", i ? "|" : "", ((WalpeMonresOption *) o->pdata[i])->label);
    g_ptr_array_unref (o);
    return g_string_free (s, FALSE);
}

static void test_options (void)
{
    /* without a list from the engine: the presets that are not larger */
    WalpeMonresState *st = state ("{\"monitors\":["
        "{\"name\":\"FHD\",\"mode\":[1920,1080],\"resolution\":\"native\"},"
        "{\"name\":\"UHD\",\"mode\":[3840,2160],\"resolution\":\"native\"},"
        "{\"name\":\"WIDE\",\"mode\":[3440,1440],\"resolution\":\"native\"},"
        "{\"name\":\"TALL\",\"mode\":[1080,1920],\"resolution\":\"native\"},"
        "{\"name\":\"SMALL\",\"mode\":[1280,720],\"resolution\":\"native\"},"
        "{\"name\":\"TINY\",\"mode\":[800,600],\"resolution\":\"native\"},"
        "{\"name\":\"ODD\",\"resolution\":\"native\"},"
        "{\"name\":\"OVER\",\"mode\":[1920,1080],\"resolution\":\"4k\"},"
        "{\"name\":\"CUST\",\"mode\":[1920,1080],\"resolution\":\"1600x900\"}]}");
    const WalpeMonresMon *m = walpe_monres_state_find (st, "FHD");
    CHECK (text_is (labels (walpe_monres_options (m, FALSE)),
                    "Native (1920 × 1080)|720p (1280 × 720)|1080p (1920 × 1080)|Custom size"));
    CHECK (text_is (labels (walpe_monres_options (m, TRUE)),
                    "Native (1920 × 1080)|720p (1280 × 720)|1080p (1920 × 1080)|"
                    "1440p (2560 × 1440), larger than the monitor|"
                    "4K (3840 × 2160), larger than the monitor|Custom size"));
    m = walpe_monres_state_find (st, "UHD");
    CHECK (text_is (labels (walpe_monres_options (m, FALSE)),
                    "Native (3840 × 2160)|720p (1280 × 720)|1080p (1920 × 1080)|1440p (2560 × 1440)|"
                    "4K (3840 × 2160)|Custom size"));
    /* nothing is larger than a 4K monitor: the check box changes nothing */
    CHECK (text_is (labels (walpe_monres_options (m, TRUE)),
                    "Native (3840 × 2160)|720p (1280 × 720)|1080p (1920 × 1080)|1440p (2560 × 1440)|"
                    "4K (3840 × 2160)|Custom size"));
    m = walpe_monres_state_find (st, "WIDE");
    CHECK (text_is (labels (walpe_monres_options (m, FALSE)),
                    "Native (3440 × 1440)|720p (1720 × 720)|1080p (2580 × 1080)|1440p (3440 × 1440)|"
                    "Custom size"));
    m = walpe_monres_state_find (st, "TALL");
    CHECK (text_is (labels (walpe_monres_options (m, FALSE)),
                    "Native (1080 × 1920)|720p (720 × 1280)|1080p (1080 × 1920)|Custom size"));
    m = walpe_monres_state_find (st, "SMALL");
    CHECK (text_is (labels (walpe_monres_options (m, FALSE)),
                    "Native (1280 × 720)|720p (1280 × 720)|Custom size"));
    m = walpe_monres_state_find (st, "TINY");       /* every preset is larger */
    CHECK (text_is (labels (walpe_monres_options (m, FALSE)), "Native (800 × 600)|Custom size"));
    GPtrArray *o = walpe_monres_options (m, TRUE);
    CHECK (o->len == 6 && ((WalpeMonresOption *) o->pdata[1])->exceeds &&
           !((WalpeMonresOption *) o->pdata[0])->exceeds && !((WalpeMonresOption *) o->pdata[5])->exceeds);
    g_ptr_array_unref (o);
    m = walpe_monres_state_find (st, "ODD");        /* size unknown: every preset, none "larger" */
    CHECK (text_is (labels (walpe_monres_options (m, FALSE)),
                    "Native|720p (1280 × 720)|1080p (1920 × 1080)|1440p (2560 × 1440)|"
                    "4K (3840 × 2160)|Custom size"));
    /* the stored choice is in the list, also a preset larger than the monitor */
    m = walpe_monres_state_find (st, "OVER");
    CHECK (text_is (labels (walpe_monres_options (m, FALSE)),
                    "Native (1920 × 1080)|720p (1280 × 720)|1080p (1920 × 1080)|"
                    "4K (3840 × 2160), larger than the monitor|Custom size"));
    o = walpe_monres_options (m, FALSE);
    CHECK (walpe_monres_options_find (o, &m->choice) == 3);
    WalpeMonres v = { WALPE_MONRES_NATIVE, 0, 0, 0 };
    CHECK (walpe_monres_options_find (o, &v) == 0);
    v.kind = WALPE_MONRES_LINES;
    v.lines = 1440;
    CHECK (walpe_monres_options_find (o, &v) == -1);
    v.kind = WALPE_MONRES_CUSTOM;
    v.w = 123;
    v.h = 456;
    CHECK (walpe_monres_options_find (o, &v) == 4);         /* any custom size is "Custom size" */
    CHECK (walpe_monres_options_find (o, NULL) == -1 && walpe_monres_options_find (NULL, &v) == -1);
    g_ptr_array_unref (o);
    m = walpe_monres_state_find (st, "CUST");
    o = walpe_monres_options (m, FALSE);
    CHECK (walpe_monres_options_find (o, &m->choice) == (int) o->len - 1);
    g_ptr_array_unref (o);
    walpe_monres_state_free (st);

    /* with the engine's list: its entries and its sizes */
    st = state (LISTING);
    m = walpe_monres_state_find (st, "DP-2");
    CHECK (text_is (labels (walpe_monres_options (m, FALSE)),
                    "Native (3440 × 1440)|720p (1720 × 720)|1080p (2580 × 1080)|1440p (3440 × 1440)|"
                    "Custom size"));
    CHECK (text_is (labels (walpe_monres_options (m, TRUE)),
                    "Native (3440 × 1440)|720p (1720 × 720)|1080p (2580 × 1080)|1440p (3440 × 1440)|"
                    "4K (5160 × 2160), larger than the monitor|Custom size"));
    walpe_monres_state_free (st);
    /* an engine that offers less than the built-in rule would */
    st = state ("{\"monitors\":[{\"name\":\"A\",\"mode\":[2560,1440],\"resolution\":\"native\","
                "\"choices\":[{\"value\":\"native\",\"size\":[2560,1440]},"
                "{\"value\":\"1080p\",\"size\":[1920,1080]},{\"value\":\"bogus\"},7]}]}");
    m = st->mons->pdata[0];
    CHECK (text_is (labels (walpe_monres_options (m, FALSE)),
                    "Native (2560 × 1440)|1080p (1920 × 1080)|Custom size"));
    walpe_monres_state_free (st);
    /* no monitor at all */
    CHECK (text_is (labels (walpe_monres_options (NULL, FALSE)),
                    "Native|720p (1280 × 720)|1080p (1920 × 1080)|1440p (2560 × 1440)|"
                    "4K (3840 × 2160)|Custom size"));
}

static void test_texts (void)
{
    WalpeMonresState *st = state (
        "{\"monitors\":["
        "{\"index\":0,\"name\":\"HDMI-0\",\"mode\":[1920,1080],\"refresh\":143.9,\"resolution\":\"native\"},"
        "{\"index\":1,\"name\":\"DP-2\",\"connector\":\"DP-2\",\"description\":\"ACME Wide 34\","
        "\"mode\":[3440,1440],\"refresh\":100,\"resolution\":\"native\"},"
        "{\"index\":2,\"name\":\"card0-eDP-1\",\"connector\":\"eDP-1\",\"mode\":[1920,1200],"
        "\"refresh\":59.94,\"resolution\":\"native\"},"
        "{\"index\":3,\"name\":\"\",\"resolution\":\"native\"},"
        "{\"index\":4,\"name\":\"evil\\u202e\\n\\tname\\u0007\",\"mode\":[1280,720],\"resolution\":\"native\"}"
        "]}");
    const WalpeMonresMon *m = st->mons->pdata[0];
    CHECK (text_is (walpe_monres_mon_title (m), "1 · HDMI-0"));
    CHECK (text_is (walpe_monres_mon_detail (m), "1920 × 1080 · 143.9 Hz"));
    m = st->mons->pdata[1];
    CHECK (text_is (walpe_monres_mon_title (m), "2 · ACME Wide 34"));
    CHECK (text_is (walpe_monres_mon_detail (m), "DP-2 · 3440 × 1440 · 100 Hz"));
    m = st->mons->pdata[2];
    CHECK (text_is (walpe_monres_mon_title (m), "3 · card0-eDP-1"));
    CHECK (text_is (walpe_monres_mon_detail (m), "eDP-1 · 1920 × 1200 · 59.94 Hz"));
    m = st->mons->pdata[3];
    CHECK (text_is (walpe_monres_mon_title (m), "4 · Monitor"));
    CHECK (text_is (walpe_monres_mon_detail (m), ""));
    /* a name is text from outside: one line, no control or direction characters */
    m = st->mons->pdata[4];
    char *t = walpe_monres_mon_title (m);
    CHECK (!strchr (t, '\n') && !strchr (t, '\t') && !strchr (t, 7) && !strstr (t, "\xe2\x80\xae"));
    CHECK (g_str_has_prefix (t, "5 · evil"));
    g_free (t);
    CHECK (text_is (walpe_monres_mon_title (NULL), ""));
    CHECK (text_is (walpe_monres_mon_detail (NULL), ""));
    /* make and model stand in for a description; a description wins */
    WalpeMonresState *mm = state (
        "{\"monitors\":[{\"index\":0,\"name\":\"DP-1\",\"make\":\"ACME\",\"model\":\"P27\","
        "\"resolution\":\"native\"},{\"index\":1,\"name\":\"DP-2\",\"model\":\"P24\","
        "\"resolution\":\"native\"},{\"index\":2,\"name\":\"DP-3\",\"make\":\"ACME\","
        "\"model\":\"P27\",\"description\":\"Left screen\",\"resolution\":\"native\"}]}");
    CHECK (text_is (walpe_monres_mon_title (mm->mons->pdata[0]), "1 · ACME P27"));
    CHECK (text_is (walpe_monres_mon_detail (mm->mons->pdata[0]), "DP-1"));
    CHECK (text_is (walpe_monres_mon_title (mm->mons->pdata[1]), "2 · P24"));
    CHECK (text_is (walpe_monres_mon_title (mm->mons->pdata[2]), "3 · Left screen"));
    walpe_monres_state_free (mm);

    m = st->mons->pdata[0];
    CHECK (text_is (walpe_monres_cost_note (m, 1920, 1080), "The monitor's own size."));
    CHECK (text_is (walpe_monres_cost_note (m, 1280, 720), "44 % of the monitor's pixels, scaled up."));
    CHECK (text_is (walpe_monres_cost_note (m, 3840, 2160), "400 % of the monitor's pixels, scaled down."));
    CHECK (text_is (walpe_monres_cost_note (m, 1080, 1920), "As many pixels as the monitor has."));
    CHECK (text_is (walpe_monres_cost_note (m, 0, 0), ""));
    CHECK (text_is (walpe_monres_cost_note (NULL, 1280, 720), ""));
    CHECK (text_is (walpe_monres_cost_note (st->mons->pdata[3], 1280, 720), ""));

    /* a name of a megabyte does not end up in a label */
    GString *big = g_string_new ("{\"monitors\":[{\"name\":\"");
    for (int i = 0; i < 100000; i++)
        g_string_append (big, "WWWWWWWWWW");
    g_string_append (big, "\",\"resolution\":\"native\"}]}");
    WalpeMonresState *bs = state (big->str);
    t = walpe_monres_mon_title (bs->mons->pdata[0]);
    CHECK (g_utf8_strlen (t, -1) < 80);
    g_free (t);
    g_string_free (big, TRUE);
    walpe_monres_state_free (bs);

    /* the announcement */
    GPtrArray *names = g_ptr_array_new_with_free_func (g_free);
    g_ptr_array_add (names, g_strdup ("DP-2"));
    char *title = NULL, *body = NULL;
    walpe_monres_announce_text (st, names, &title, &body);
    CHECK (text_is (title, "New monitor: 2 · ACME Wide 34"));
    CHECK (body && strstr (body, "3440 × 1440") && strstr (body, "own resolution"));
    g_free (body);
    g_ptr_array_add (names, g_strdup ("HDMI-0"));
    g_ptr_array_add (names, g_strdup ("GONE-1"));
    walpe_monres_announce_text (st, names, &title, &body);
    CHECK (text_is (title, "New monitors connected"));
    CHECK (body && strstr (body, "2 · ACME Wide 34, 1 · HDMI-0, GONE-1 are rendered"));
    g_free (body);
    walpe_monres_announce_text (st, names, NULL, NULL);     /* nothing asked for: nothing done */
    g_ptr_array_unref (names);
    walpe_monres_state_free (st);

    /* a notification's body is markup to most services: a name starts no
     * tag and no entity in it */
    st = state ("{\"monitors\":[{\"index\":0,\"name\":\"<b>A&amp;\",\"resolution\":\"native\"},"
                "{\"index\":1,\"name\":\"B\",\"resolution\":\"native\"}]}");
    names = g_ptr_array_new_with_free_func (g_free);
    g_ptr_array_add (names, g_strdup ("<b>A&amp;"));
    g_ptr_array_add (names, g_strdup ("B"));
    walpe_monres_announce_text (st, names, &title, &body);
    CHECK (body && !strpbrk (body, "<>&") && strstr (body, "1 · (b)A+amp;, 2 · B are rendered"));
    g_free (title);
    g_free (body);
    g_ptr_array_unref (names);
    walpe_monres_state_free (st);
}

/* ----------------------------------------------------- a custom size */

static void test_custom (void)
{
    WalpeMonresState *st = state (LISTING);     /* limits 320 x 200 .. 8192 a side, 64 megapixels */
    const WalpeMonresMon *m = walpe_monres_state_find (st, "HDMI-0");      /* 1920 x 1080 */
    int w = -1, h = -1;
    char *msg = NULL;
#define BAD(wt, ht) (walpe_monres_check_custom (st, m, wt, ht, &w, &h, &msg) == WALPE_MONRES_CHECK_BAD)
    CHECK (BAD ("", "") && msg && w == 0 && h == 0);
    g_clear_pointer (&msg, g_free);
    CHECK (BAD ("1920", ""));
    CHECK (w == 1920 && h == 0);
    g_clear_pointer (&msg, g_free);
    CHECK (BAD (NULL, NULL));
    g_clear_pointer (&msg, g_free);
    CHECK (BAD ("abc", "1080") && w == 0 && h == 1080);
    CHECK (msg && strstr (msg, "whole numbers"));
    g_clear_pointer (&msg, g_free);
    static const char *const junk[] = { "19 20", "1920px", "-1920", "+1920", "1e3", "1920.5", "0x780",
                                        "１９２０", "999999", "0" };
    for (guint i = 0; i < G_N_ELEMENTS (junk); i++) {
        if (!BAD (junk[i], "1080")) {
            fails++;
            g_printerr ("FAIL \"%s\" was taken as a width\n", junk[i]);
        }
        g_clear_pointer (&msg, g_free);
    }
    CHECK (BAD ("319", "1080") && msg && strstr (msg, "320 × 200"));
    g_clear_pointer (&msg, g_free);
    CHECK (BAD ("1920", "199"));
    g_clear_pointer (&msg, g_free);
    CHECK (BAD ("8193", "1080") && msg && strstr (msg, "8192"));
    g_clear_pointer (&msg, g_free);
    CHECK (BAD ("1920", "8193"));
    g_clear_pointer (&msg, g_free);
    CHECK (BAD ("8192", "8192") == FALSE);      /* exactly the pixel limit */
    g_clear_pointer (&msg, g_free);
#undef BAD
    /* the limits at their edge, blanks around the numbers */
    CHECK (walpe_monres_check_custom (st, NULL, " 320 ", "\t200", &w, &h, &msg) == WALPE_MONRES_CHECK_OK);
    CHECK (w == 320 && h == 200 && msg == NULL);
    /* smaller, same shape: fine, nothing to remark */
    CHECK (walpe_monres_check_custom (st, m, "1280", "720", &w, &h, &msg) == WALPE_MONRES_CHECK_OK);
    CHECK (w == 1280 && h == 720 && msg == NULL);
    CHECK (walpe_monres_check_custom (st, m, "1920", "1080", &w, &h, &msg) == WALPE_MONRES_CHECK_OK);
    CHECK (msg == NULL);
    /* more pixels than the monitor: allowed, said */
    CHECK (walpe_monres_check_custom (st, m, "3840", "2160", &w, &h, &msg) == WALPE_MONRES_CHECK_NOTE);
    CHECK (msg && strstr (msg, "400 %") && !strstr (msg, "shape"));
    g_clear_pointer (&msg, g_free);
    /* another shape: allowed, said */
    CHECK (walpe_monres_check_custom (st, m, "1280", "1024", &w, &h, &msg) == WALPE_MONRES_CHECK_NOTE);
    CHECK (msg && strstr (msg, "shape") && strstr (msg, "1920 × 1080"));
    g_clear_pointer (&msg, g_free);
    /* a pixel of rounding is not "another shape" */
    CHECK (walpe_monres_check_custom (st, m, "1366", "768", &w, &h, &msg) == WALPE_MONRES_CHECK_OK);
    CHECK (msg == NULL);
    /* msg not wanted */
    CHECK (walpe_monres_check_custom (st, m, "1", "1", NULL, NULL, NULL) == WALPE_MONRES_CHECK_BAD);
    walpe_monres_state_free (st);

    /* without a state: the built-in limits */
    CHECK (walpe_monres_check_custom (NULL, NULL, "320", "200", &w, &h, NULL) == WALPE_MONRES_CHECK_OK);
    CHECK (walpe_monres_check_custom (NULL, NULL, "320", "199", &w, &h, NULL) == WALPE_MONRES_CHECK_BAD);
    CHECK (walpe_monres_check_custom (NULL, NULL, "16384", "2000", &w, &h, NULL) == WALPE_MONRES_CHECK_OK);
    CHECK (walpe_monres_check_custom (NULL, NULL, "16385", "2000", &w, &h, NULL) == WALPE_MONRES_CHECK_BAD);
    CHECK (walpe_monres_check_custom (NULL, NULL, "16384", "16384", &w, &h, &msg) ==
           WALPE_MONRES_CHECK_BAD);
    CHECK (msg && strstr (msg, "million pixels"));
    g_clear_pointer (&msg, g_free);
}

/* ------------------------------------------------------------- requests */

static void test_requests (void)
{
    char *s = walpe_monres_request_list ();
    JsonObject *o = obj (s);
    CHECK (!g_strcmp0 (wpj_str (o, "cmd", ""), "resolution") && json_object_get_size (o) == 1);
    json_object_unref (o);
    g_free (s);

    /* names are sent as they are, whatever is in them */
    WalpeMonresAssign a[4] = {
        { .name = "HDMI-0", .set_value = TRUE, .value = { WALPE_MONRES_NATIVE, 0, 0, 0 }, .asked = -1 },
        { .name = "we\"ird\\na\tme é", .set_value = TRUE, .value = { WALPE_MONRES_CUSTOM, 0, 2560, 1440 },
          .set_tex = TRUE, .tex = { WALPE_LIMIT_VALUE, 4096 }, .asked = 0 },
        { .name = "", .set_value = TRUE, .asked = -1 },              /* no name: left out */
        { .name = "DP-3", .asked = -1 },                             /* nothing to set: left out */
    };
    s = walpe_monres_request_apply (a, 4);
    CHECK (s && !strchr (s, '\n'));
    o = obj (s);
    CHECK (!g_strcmp0 (wpj_str (o, "cmd", ""), "resolution"));
    JsonArray *set = wpj_arr (o, "set");
    CHECK (wpj_arr_len (set) == 2);
    JsonObject *e0 = wpj_arr_obj (set, 0), *e1 = wpj_arr_obj (set, 1);
    CHECK (!g_strcmp0 (wpj_str (e0, "monitor", ""), "HDMI-0") && !g_strcmp0 (wpj_str (e0, "value", ""), "native"));
    CHECK (!json_object_has_member (e0, "asked") && !json_object_has_member (e0, "texture_limit"));
    CHECK (!g_strcmp0 (wpj_str (e1, "monitor", ""), "we\"ird\\na\tme é"));
    CHECK (!g_strcmp0 (wpj_str (e1, "value", ""), "2560x1440"));
    CHECK (!g_strcmp0 (wpj_str (e1, "texture_limit", ""), "4096"));
    CHECK (json_object_has_member (e1, "asked") && !wpj_bool (e1, "asked", TRUE));
    json_object_unref (o);
    g_free (s);
    CHECK (walpe_monres_request_apply (a + 2, 2) == NULL);
    CHECK (walpe_monres_request_apply (NULL, 0) == NULL);
    /* "asked" alone is a request: keep what it has, do not ask again */
    WalpeMonresAssign k = { .name = "DP-2", .asked = 1 };
    s = walpe_monres_request_apply (&k, 1);
    o = obj (s);
    e0 = wpj_arr_obj (wpj_arr (o, "set"), 0);
    CHECK (e0 && wpj_bool (e0, "asked", FALSE) && !json_object_has_member (e0, "value"));
    json_object_unref (o);
    g_free (s);

    s = walpe_monres_request_prompt (FALSE);
    o = obj (s);
    CHECK (!g_strcmp0 (wpj_str (o, "cmd", ""), "resolution") && json_object_has_member (o, "prompt") &&
           !wpj_bool (o, "prompt", TRUE));
    json_object_unref (o);
    g_free (s);

    WalpeVideoLimit v = { WALPE_LIMIT_NONE, { WALPE_MONRES_NATIVE, 0, 0, 0 } };
    s = walpe_monres_request_video (&v);
    o = obj (s);
    CHECK (!g_strcmp0 (wpj_str (o, "cmd", ""), "set") &&
           !g_strcmp0 (wpj_str (o, "video_resolution", ""), "none"));
    json_object_unref (o);
    g_free (s);
    v.kind = WALPE_LIMIT_VALUE;
    v.size.kind = WALPE_MONRES_LINES;
    v.size.lines = 1440;
    s = walpe_monres_request_video (&v);
    o = obj (s);
    CHECK (!g_strcmp0 (wpj_str (o, "video_resolution", ""), "1440p"));
    json_object_unref (o);
    g_free (s);

    s = walpe_monres_request_filter ("nearest");
    o = obj (s);
    CHECK (!g_strcmp0 (wpj_str (o, "cmd", ""), "set") && !g_strcmp0 (wpj_str (o, "scale_filter", ""), "nearest"));
    json_object_unref (o);
    g_free (s);
    CHECK (walpe_monres_request_filter (NULL) == NULL && walpe_monres_request_filter ("") == NULL);
}

/* ------------------------------------------------- new monitors (hot-plug) */

/* a status with the monitors "NAME" (asked) or "?NAME" (not asked) */
static WalpeMonresState *mons (gboolean prompt, gboolean fullscreen, const char *first, ...)
{
    GString *s = g_string_new (NULL);
    g_string_append_printf (s, "{\"resolution_prompt\":%s,\"fullscreen\":%s,\"monitors\":[",
                            prompt ? "true" : "false", fullscreen ? "true" : "false");
    va_list ap;
    va_start (ap, first);
    int i = 0;
    for (const char *n = first; n; n = va_arg (ap, const char *), i++) {
        gboolean un = *n == '?';
        g_string_append_printf (s, "%s{\"index\":%d,\"name\":\"%s\",\"mode\":[1920,1080],"
                                "\"resolution\":\"native\",\"resolution_asked\":%s}",
                                i ? "," : "", i, un ? n + 1 : n, un ? "false" : "true");
    }
    va_end (ap);
    g_string_append (s, "]}");
    WalpeMonresState *st = state (s->str);
    g_string_free (s, TRUE);
    return st;
}

/* feed one state; "A,B" = announced now, "" = nothing. *startup and
 * *withdrawn ("A,B") as the tracker reports them */
static char *feed (WalpeMonresTracker *t, WalpeMonresState *st, guint *startup, char **withdrawn)
{
    GPtrArray *back = NULL;
    GPtrArray *news = walpe_monres_tracker_feed (t, st, startup, &back);
    GString *s = g_string_new (NULL), *b = g_string_new (NULL);
    for (guint i = 0; news && i < news->len; i++)
        g_string_append_printf (s, "%s%s", i ? "," : "", (char *) news->pdata[i]);
    for (guint i = 0; back && i < back->len; i++)
        g_string_append_printf (b, "%s%s", i ? "," : "", (char *) back->pdata[i]);
    if (news)
        g_ptr_array_unref (news);
    if (back)
        g_ptr_array_unref (back);
    if (withdrawn)
        *withdrawn = g_string_free (b, FALSE);
    else
        g_string_free (b, TRUE);
    walpe_monres_state_free (st);
    return g_string_free (s, FALSE);
}

static void test_tracker (void)
{
    guint startup = 99;
    char *back = NULL;

    /* a fresh installation: two monitors, nobody asked. They are the
     * start-up question, never an announcement. */
    WalpeMonresTracker *t = walpe_monres_tracker_new ();
    CHECK (text_is (feed (t, walpe_monres_state_new (NULL), &startup, NULL), "") && startup == 0);
    CHECK (text_is (feed (t, mons (TRUE, FALSE, "?A", "?B", NULL), &startup, NULL), "") && startup == 2);
    CHECK (text_is (feed (t, mons (TRUE, FALSE, "?A", "?B", NULL), &startup, NULL), "") && startup == 0);
    /* the user applied: both asked. Nothing was announced, nothing is taken back. */
    CHECK (text_is (feed (t, mons (TRUE, FALSE, "A", "B", NULL), &startup, &back), "") && startup == 0);
    CHECK (text_is (back, ""));
    /* a third monitor is plugged in (the engine restarts in between) */
    CHECK (text_is (feed (t, walpe_monres_state_new (NULL), &startup, NULL), ""));
    CHECK (text_is (feed (t, mons (TRUE, FALSE, "A", "B", "?C", NULL), &startup, &back), "C"));
    CHECK (startup == 0);
    CHECK (text_is (back, ""));
    /* ... once */
    CHECK (text_is (feed (t, mons (TRUE, FALSE, "A", "B", "?C", NULL), NULL, NULL), ""));
    /* unplugged: the announcement is taken back; plugged in again: not announced again */
    CHECK (text_is (feed (t, mons (TRUE, FALSE, "A", "B", NULL), NULL, &back), ""));
    CHECK (text_is (back, "C"));
    CHECK (text_is (feed (t, mons (TRUE, FALSE, "A", "B", "?C", NULL), NULL, &back), ""));
    CHECK (text_is (back, ""));
    walpe_monres_tracker_free (t);

    /* an upgraded installation: every monitor counts as asked. Nothing, ever. */
    t = walpe_monres_tracker_new ();
    CHECK (text_is (feed (t, mons (TRUE, FALSE, "A", "B", NULL), &startup, NULL), "") && startup == 0);
    CHECK (text_is (feed (t, mons (TRUE, FALSE, "B", "A", NULL), &startup, NULL), "") && startup == 0);
    CHECK (text_is (feed (t, state (STATUS_OLD), &startup, NULL), "") && startup == 0);
    walpe_monres_tracker_free (t);

    /* an engine without the feature never primes the tracker: when the
     * new engine takes over, its unasked monitors are the start-up ones */
    t = walpe_monres_tracker_new ();
    CHECK (text_is (feed (t, state (STATUS_OLD), &startup, NULL), "") && startup == 0);
    CHECK (text_is (feed (t, state ("{\"resolution_prompt\":true,\"monitors\":[]}"), &startup, NULL), ""));
    CHECK (startup == 0);
    CHECK (text_is (feed (t, mons (TRUE, FALSE, "?A", NULL), &startup, NULL), "") && startup == 1);
    walpe_monres_tracker_free (t);

    /* a fullscreen application has the focus: the announcement waits */
    t = walpe_monres_tracker_new ();
    CHECK (text_is (feed (t, mons (TRUE, TRUE, "A", NULL), &startup, NULL), "") && startup == 0);
    CHECK (text_is (feed (t, mons (TRUE, TRUE, "A", "?B", NULL), NULL, NULL), ""));
    CHECK (walpe_monres_tracker_waiting (t) == 1);
    CHECK (text_is (feed (t, mons (TRUE, TRUE, "A", "?B", "?C", NULL), NULL, NULL), ""));
    CHECK (walpe_monres_tracker_waiting (t) == 2);
    /* one of them goes away while it waits: it is never announced */
    CHECK (text_is (feed (t, mons (TRUE, TRUE, "A", "?B", NULL), NULL, &back), ""));
    CHECK (text_is (back, "") && walpe_monres_tracker_waiting (t) == 1);
    /* fullscreen over: now */
    CHECK (text_is (feed (t, mons (TRUE, FALSE, "A", "?B", NULL), NULL, NULL), "B"));
    CHECK (walpe_monres_tracker_waiting (t) == 0);
    /* asked about meanwhile (walpe-ctl, the dialog): taken back */
    CHECK (text_is (feed (t, mons (TRUE, FALSE, "A", "B", NULL), NULL, &back), ""));
    CHECK (text_is (back, "B"));
    walpe_monres_tracker_free (t);

    /* asked about while it waited behind a fullscreen application: no announcement */
    t = walpe_monres_tracker_new ();
    g_free (feed (t, mons (TRUE, FALSE, "A", NULL), NULL, NULL));
    CHECK (text_is (feed (t, mons (TRUE, TRUE, "A", "?B", NULL), NULL, NULL), ""));
    CHECK (text_is (feed (t, mons (TRUE, TRUE, "A", "B", NULL), NULL, NULL), ""));
    CHECK (text_is (feed (t, mons (TRUE, FALSE, "A", "B", NULL), NULL, NULL), ""));
    CHECK (walpe_monres_tracker_waiting (t) == 0);
    walpe_monres_tracker_free (t);

    /* asking turned off: nothing at start-up, nothing later; turned off
     * while an announcement is up: it is taken back */
    t = walpe_monres_tracker_new ();
    CHECK (text_is (feed (t, mons (FALSE, FALSE, "?A", NULL), &startup, NULL), "") && startup == 0);
    CHECK (text_is (feed (t, mons (FALSE, FALSE, "?A", "?B", NULL), &startup, NULL), "") && startup == 0);
    CHECK (text_is (feed (t, mons (TRUE, FALSE, "?A", "?B", NULL), &startup, NULL), "") && startup == 0);
    CHECK (text_is (feed (t, mons (TRUE, FALSE, "?A", "?B", "?C", NULL), NULL, NULL), "C"));
    CHECK (text_is (feed (t, mons (FALSE, FALSE, "?A", "?B", "?C", NULL), NULL, &back), ""));
    CHECK (text_is (back, "C"));
    walpe_monres_tracker_free (t);

    /* two at once, in the engine's order */
    t = walpe_monres_tracker_new ();
    g_free (feed (t, mons (TRUE, FALSE, "A", NULL), NULL, NULL));
    CHECK (text_is (feed (t, mons (TRUE, FALSE, "?Z", "A", "?B", NULL), NULL, NULL), "Z,B"));
    walpe_monres_tracker_free (t);

    /* an announcement that was not shown (a dialog lists the monitor,
     * another program announces it): nothing is taken back for it later,
     * and it is not returned a second time */
    t = walpe_monres_tracker_new ();
    g_free (feed (t, mons (TRUE, FALSE, "A", NULL), NULL, NULL));
    {
        WalpeMonresState *s2 = mons (TRUE, FALSE, "A", "?B", NULL);
        GPtrArray *news = walpe_monres_tracker_feed (t, s2, NULL, NULL);
        CHECK (news && news->len == 1);
        walpe_monres_tracker_unshown (t, news);
        if (news)
            g_ptr_array_unref (news);
        walpe_monres_state_free (s2);
    }
    CHECK (text_is (feed (t, mons (TRUE, FALSE, "A", "?B", NULL), NULL, &back), ""));
    CHECK (text_is (back, ""));
    CHECK (text_is (feed (t, mons (TRUE, FALSE, "A", "B", NULL), NULL, &back), ""));
    CHECK (text_is (back, ""));
    walpe_monres_tracker_unshown (NULL, NULL);
    walpe_monres_tracker_unshown (t, NULL);
    walpe_monres_tracker_free (t);

    /* nothing to feed */
    CHECK (walpe_monres_tracker_feed (NULL, NULL, &startup, NULL) == NULL && startup == 0);
    CHECK (walpe_monres_tracker_waiting (NULL) == 0);
    walpe_monres_tracker_free (NULL);
}

int main (void)
{
    test_value ();
    test_sizes ();
    test_limits_text ();
    test_state_status ();
    test_state_listing ();
    test_options ();
    test_texts ();
    test_custom ();
    test_requests ();
    test_tracker ();
    if (fails) {
        g_printerr ("resmodel_test: %d failure(s)\n", fails);
        return 1;
    }
    g_print ("resmodel_test: all passed\n");
    return 0;
}
