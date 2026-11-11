/* resolution_test - the render resolution per output (src/common/resolution.c)
 * and its part of config.json (src/common/config.c)
 *
 * No display, no daemon, no GL; a private XDG_CONFIG_HOME:
 *  1. values: what is read, what is refused (with a sentence that names
 *     the value), the canonical spelling;
 *  2. sizes: native, presets by the output's shape, custom sizes;
 *  3. the texture limit: values, the rule for one wallpaper on several
 *     monitors (the least strict wins), the level a cap asks for;
 *  4. videos: the box by the monitors that show the video, "none", and a
 *     fixed "1080p" that gives exactly the sizes of the limit that was
 *     built in before (1920 x 1080);
 *  5. NAME:VALUE lists of `walpe-ctl set`;
 *  6. config.json: a new installation asks about every output; a file of
 *     format 3 comes back with the same settings, its outputs entered as
 *     "asked, native" by the daemon's step and videos at 1080p; an output
 *     that appears later has no entry ("not asked yet") until it is given
 *     one; the upgrade mark survives a save by a process that is not the
 *     daemon; hand-written nonsense in "monitors" costs only that entry. */
#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include "common/config.h"
#include "common/resolution.h"
#include "common/util.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; g_printerr ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static char *cfg_dir, *cfg_path;

static void put_config (const char *json)
{
    g_assert (g_file_set_contents (cfg_path, json, -1, NULL));
}

static JsonObject *read_config (JsonNode **root)
{
    *root = wpj_parse_file (cfg_path);
    return *root && JSON_NODE_HOLDS_OBJECT (*root) ? json_node_get_object (*root) : NULL;
}

/* parses, and reads back as `canon` */
static gboolean res_is (const char *text, const char *canon)
{
    WalpeRes r = { 99, 99, 99, 99 };
    char *err = NULL;
    if (!walpe_res_parse (text, &r, &err)) {
        g_free (err);
        return FALSE;
    }
    char *f = walpe_res_format (&r);
    gboolean ok = !g_strcmp0 (f, canon) && err == NULL;
    g_free (f);
    return ok;
}

/* refused, with a sentence holding `part`, and the output untouched */
static gboolean res_refused (const char *text, const char *part)
{
    WalpeRes r = { 99, 98, 97, 96 };
    char *err = NULL;
    gboolean ok = !walpe_res_parse (text, &r, &err) && err && strstr (err, part) &&
                  r.kind == 99 && r.lines == 98 && r.w == 97 && r.h == 96;
    if (!ok)
        g_printerr ("  ('%s' -> %s)\n", text ? text : "(null)", err ? err : "(accepted)");
    g_free (err);
    return ok;
}

static gboolean size_is (const char *text, int mw, int mh, int w, int h)
{
    WalpeRes r;
    int ow = -1, oh = -1;
    if (!walpe_res_parse (text, &r, NULL))
        return FALSE;
    walpe_res_size (&r, mw, mh, &ow, &oh);
    if (ow != w || oh != h)
        g_printerr ("  (%s on %dx%d -> %dx%d, expected %dx%d)\n", text, mw, mh, ow, oh, w, h);
    return ow == w && oh == h;
}

static void test_values (void)
{
    CHECK (res_is ("native", "native") && res_is ("NATIVE", "native") && res_is (" native ", "native"));
    CHECK (res_is ("720p", "720p") && res_is ("1080p", "1080p") && res_is ("1440p", "1440p") &&
           res_is ("4k", "4k"));
    CHECK (res_is ("4K", "4k") && res_is ("2160p", "4k") && res_is ("UHD", "4k") && res_is ("2k", "1440p") &&
           res_is ("qhd", "1440p") && res_is ("FHD", "1080p") && res_is ("hd", "720p"));
    CHECK (res_is ("2560x1440", "2560x1440") && res_is ("2560X1440", "2560x1440") &&
           res_is ("2560 x 1440", "2560x1440") && res_is ("1920*1200", "1920x1200"));
    CHECK (res_is ("320x200", "320x200") && res_is ("16384x4096", "16384x4096") &&
           res_is ("8192x8192", "8192x8192"));
    /* nonsense, each with its reason */
    CHECK (res_refused ("", "is not one of") && res_refused (NULL, "is not one of") &&
           res_refused ("big", "is not one of") && res_refused ("1080", "is not one of") &&
           res_refused ("1080i", "is not one of") && res_refused ("-1920x1080", "is not one of") &&
           res_refused ("1920x", "is not one of") && res_refused ("x1080", "is not one of") &&
           res_refused ("1920x1080x3", "is not one of") && res_refused ("1920x1080 ; rm", "is not one of") &&
           res_refused ("1e3x1e3", "is not one of") && res_refused ("0x0", "too small") &&
           res_refused ("1920.5x1080", "is not one of"));
    CHECK (res_refused ("319x200", "too small") && res_refused ("320x199", "too small") &&
           res_refused ("16x16", "too small"));
    CHECK (res_refused ("16385x1080", "too large") && res_refused ("1080x16385", "too large") &&
           res_refused ("16384x8192", "too large") &&            /* 134 MP: past the pixel limit */
           res_refused ("99999999999x1", "is not one of"));     /* not a size at all */
    {
        /* the value shows up in the sentence, cut and cleaned */
        WalpeRes r;
        char *err = NULL;
        CHECK (!walpe_res_parse ("a\001b\nc", &r, &err) && err && strstr (err, "'a?b?c'"));
        g_free (err);
        err = NULL;
        char *lng = g_strnfill (500, 'z');
        CHECK (!walpe_res_parse (lng, &r, &err) && err && strlen (err) < 200);
        g_free (err);
        g_free (lng);
        /* ... at a whole character: 14 characters of 3 bytes are cut after
         * the 13th, not inside the 14th (the sentence goes into a JSON reply) */
        err = NULL;
        CHECK (!walpe_res_parse ("\xef\xbc\x91\xef\xbc\x90\xef\xbc\x98\xef\xbc\x90\xef\xbc\x91\xef\xbc\x90\xef\xbc\x98"
                                 "\xef\xbc\x90\xef\xbc\x91\xef\xbc\x90\xef\xbc\x98\xef\xbc\x90\xef\xbc\x91\xef\xbc\x90",
                                 &r, &err) && err && g_utf8_validate (err, -1, NULL));
        g_free (err);
        err = NULL;
        CHECK (!walpe_res_parse ("ab\xff\xfe", &r, &err) && err && g_utf8_validate (err, -1, NULL) &&
               strstr (err, "'ab'"));
        g_free (err);
        /* err may be NULL */
        CHECK (!walpe_res_parse ("bad", &r, NULL));
    }
    guint n = 0;
    const WalpeResPreset *p = walpe_res_presets (&n);
    CHECK (n == 4 && p[0].lines == 720 && p[3].lines == 2160 && !strcmp (p[1].name, "1080p"));
    for (guint i = 0; i < n; i++)       /* every preset's name reads back as itself */
        CHECK (res_is (p[i].name, p[i].name));
}

static void test_sizes (void)
{
    /* native is the mode, whatever it is */
    CHECK (size_is ("native", 1920, 1080, 1920, 1080) && size_is ("native", 1366, 768, 1366, 768) &&
           size_is ("native", 1080, 1920, 1080, 1920) && size_is ("native", 5120, 1440, 5120, 1440));
    /* 16:9: the familiar sizes */
    CHECK (size_is ("720p", 1920, 1080, 1280, 720) && size_is ("1080p", 1920, 1080, 1920, 1080) &&
           size_is ("1440p", 1920, 1080, 2560, 1440) && size_is ("4k", 1920, 1080, 3840, 2160));
    CHECK (size_is ("1080p", 3840, 2160, 1920, 1080) && size_is ("720p", 3840, 2160, 1280, 720) &&
           size_is ("1440p", 2560, 1440, 2560, 1440));
    /* other shapes: the lines on the shorter side, the output's aspect */
    CHECK (size_is ("1080p", 3440, 1440, 2580, 1080) && size_is ("1080p", 2560, 1600, 1728, 1080) &&
           size_is ("720p", 1366, 768, 1280, 720) && size_is ("1080p", 5120, 1440, 3840, 1080) &&
           size_is ("1080p", 1280, 1024, 1350, 1080));
    /* upright outputs: the preset is the shorter side there too */
    CHECK (size_is ("1080p", 2160, 3840, 1080, 1920) && size_is ("720p", 1080, 1920, 720, 1280));
    /* a preset that is the mode's own shorter side is the mode, exactly */
    CHECK (size_is ("1080p", 1920, 1080, 1920, 1080) && size_is ("1080p", 2580, 1080, 2580, 1080));
    /* custom: as written, the mode does not matter */
    CHECK (size_is ("2560x1440", 1920, 1080, 2560, 1440) && size_is ("1000x1000", 3840, 2160, 1000, 1000));
    /* a mode nobody knows: 16:9 */
    CHECK (size_is ("1080p", 0, 0, 1920, 1080) && size_is ("4k", -5, 7, 3840, 2160));
    {
        /* both sides even, within the limits, on any mode */
        guint n = 0;
        const WalpeResPreset *p = walpe_res_presets (&n);
        static const int modes[][2] = { { 1024, 768 }, { 1280, 800 }, { 1600, 900 }, { 3840, 1080 },
                                        { 7680, 1080 }, { 768, 1366 }, { 1, 1 }, { 16384, 1 },
                                        { 3, 16384 }, { 2147483647, 1 } };
        for (guint i = 0; i < n; i++)
            for (guint m = 0; m < G_N_ELEMENTS (modes); m++) {
                WalpeRes r = { WALPE_RES_LINES, p[i].lines, 0, 0 };
                int w = 0, h = 0;
                walpe_res_size (&r, modes[m][0], modes[m][1], &w, &h);
                CHECK (w >= 2 && h >= 2 && w <= WALPE_RES_MAX_SIDE && h <= WALPE_RES_MAX_SIDE &&
                       (gint64) w * h <= WALPE_RES_MAX_PIXELS && MIN (w, h) <= p[i].lines);
            }
    }
    CHECK (walpe_res_percent (1920, 1080, 1920, 1080) == 100 && walpe_res_percent (1280, 720, 1920, 1080) == 44 &&
           walpe_res_percent (1920, 1080, 3840, 2160) == 25 && walpe_res_percent (3840, 2160, 1920, 1080) == 400 &&
           walpe_res_percent (0, 0, 1920, 1080) == 100 && walpe_res_percent (16, 16, 16384, 16384) == 1);
    {
        WalpeRes r;
        walpe_res_parse ("1080p", &r, NULL);
        char *l = walpe_res_label (&r, 3440, 1440);
        CHECK (!g_strcmp0 (l, "1080p (2580 x 1080)"));
        g_free (l);
        walpe_res_parse ("native", &r, NULL);
        l = walpe_res_label (&r, 1920, 1080);
        CHECK (!g_strcmp0 (l, "native (1920 x 1080)"));
        g_free (l);
        walpe_res_parse ("1600x900", &r, NULL);
        l = walpe_res_label (&r, 1920, 1080);
        CHECK (!g_strcmp0 (l, "1600 x 900"));
        g_free (l);
        /* the three kinds of sentence */
        char *a = walpe_res_cost_line (1920, 1080, 1920, 1080), *b = walpe_res_cost_line (1280, 720, 1920, 1080);
        char *c = walpe_res_cost_line (3840, 2160, 1920, 1080);
        CHECK (a && strstr (a, "own resolution") && b && strstr (b, "44 %") && strstr (b, "softer") &&
               c && strstr (c, "4.0 times") && strstr (c, "smoother"));
        g_free (a); g_free (b); g_free (c);
    }
    {
        WalpeRes a, b;
        walpe_res_parse ("4k", &a, NULL);
        walpe_res_parse ("2160p", &b, NULL);
        CHECK (walpe_res_equal (&a, &b));
        walpe_res_parse ("3840x2160", &b, NULL);
        CHECK (!walpe_res_equal (&a, &b));      /* a preset follows the output's shape, a size does not */
        walpe_res_parse ("native", &a, NULL);
        walpe_res_parse ("native", &b, NULL);
        CHECK (walpe_res_equal (&a, &b));
    }
}

static gboolean tl_is (const char *text, int v)
{
    int out = -77;
    char *err = NULL;
    gboolean ok = walpe_texlimit_parse (text, &out, &err) && out == v && !err;
    g_free (err);
    return ok;
}

static gboolean tl_refused (const char *text)
{
    int out = -77;
    char *err = NULL;
    gboolean ok = !walpe_texlimit_parse (text, &out, &err) && out == -77 && err &&
                  strstr (err, "texture_limit");
    g_free (err);
    return ok;
}

static void test_texlimit (void)
{
    CHECK (tl_is ("auto", WALPE_TEXLIMIT_AUTO) && tl_is ("AUTO", WALPE_TEXLIMIT_AUTO) &&
           tl_is ("none", WALPE_TEXLIMIT_NONE) && tl_is ("off", WALPE_TEXLIMIT_NONE) &&
           tl_is ("full", WALPE_TEXLIMIT_NONE));
    CHECK (tl_is ("2048", 2048) && tl_is ("64", 64) && tl_is ("16384", 16384) && tl_is ("3000", 3000) &&
           tl_is ("4k", 4096) && tl_is ("2K", 2048) && tl_is ("16k", 16384) && tl_is ("1k", 1024));
    CHECK (tl_refused ("") && tl_refused (NULL) && tl_refused ("63") && tl_refused ("16385") &&
           tl_refused ("0") && tl_refused ("-2048") && tl_refused ("2048px") && tl_refused ("17k") &&
           tl_refused ("big") && tl_refused ("2048.5") && tl_refused ("99999999999"));
    {
        char *a = walpe_texlimit_format (WALPE_TEXLIMIT_AUTO), *n = walpe_texlimit_format (WALPE_TEXLIMIT_NONE);
        char *p = walpe_texlimit_format (4096);
        CHECK (!g_strcmp0 (a, "auto") && !g_strcmp0 (n, "none") && !g_strcmp0 (p, "4096"));
        g_free (a); g_free (n); g_free (p);
    }
    /* one wallpaper on two monitors: its textures are shared, the least
     * strict limit holds for both. none > auto > the larger number. */
    const int A = WALPE_TEXLIMIT_AUTO, N = WALPE_TEXLIMIT_NONE;
    CHECK (walpe_texlimit_merge (1024, 4096) == 4096 && walpe_texlimit_merge (4096, 1024) == 4096 &&
           walpe_texlimit_merge (2048, 2048) == 2048);
    CHECK (walpe_texlimit_merge (A, 1024) == A && walpe_texlimit_merge (1024, A) == A &&
           walpe_texlimit_merge (A, A) == A);
    CHECK (walpe_texlimit_merge (N, 1024) == N && walpe_texlimit_merge (A, N) == N &&
           walpe_texlimit_merge (N, N) == N);
    {
        /* three monitors, any order: the same answer */
        static const int v[3] = { 1024, WALPE_TEXLIMIT_AUTO, 8192 };
        static const int perm[6][3] = { { 0, 1, 2 }, { 0, 2, 1 }, { 1, 0, 2 }, { 1, 2, 0 }, { 2, 0, 1 }, { 2, 1, 0 } };
        for (int i = 0; i < 6; i++)
            CHECK (walpe_texlimit_merge (walpe_texlimit_merge (v[perm[i][0]], v[perm[i][1]]), v[perm[i][2]]) == A);
    }
    /* what a load gets: auto leaves everything to tex_downscale, exactly
     * as before the limit existed */
    gboolean nd = TRUE;
    int cap = -1;
    walpe_texlimit_effective (A, TRUE, &nd, &cap);
    CHECK (!nd && cap == 0);
    walpe_texlimit_effective (A, FALSE, &nd, &cap);
    CHECK (nd && cap == 0);
    walpe_texlimit_effective (N, TRUE, &nd, &cap);
    CHECK (nd && cap == 0);
    walpe_texlimit_effective (2048, TRUE, &nd, &cap);
    CHECK (!nd && cap == 2048);
    walpe_texlimit_effective (2048, FALSE, &nd, &cap);
    CHECK (nd && cap == 2048);
    walpe_texlimit_effective (2048, TRUE, NULL, NULL);      /* either may be NULL */
    /* halvings a cap asks for: the longer side within it, not below 64 */
    CHECK (walpe_texlimit_level (4096, 4096, 0) == 0 && walpe_texlimit_level (4096, 2048, 4096) == 0 &&
           walpe_texlimit_level (4096, 2048, 2048) == 1 && walpe_texlimit_level (2048, 4096, 2048) == 1 &&
           walpe_texlimit_level (8192, 100, 1024) == 3 && walpe_texlimit_level (4097, 4097, 4096) == 1 &&
           walpe_texlimit_level (5760, 3240, 2048) == 2 && walpe_texlimit_level (0, 0, 1024) == 0);
    CHECK (walpe_texlimit_level (200, 200, 64) == 1 &&      /* 100, and 50 would be below 64 */
           walpe_texlimit_level (16384, 16384, 64) == 8);
}

static gboolean box_is (const char *setting, const int *sizes, guint n, int w, int h)
{
    WalpeVideoRes v;
    WalpeVideoBox b = { -1, -1 };
    if (!walpe_vidres_parse (setting, &v, NULL))
        return FALSE;
    walpe_video_box (&v, sizes, n, &b);
    if (b.w != w || b.h != h)
        g_printerr ("  (video_resolution %s -> box %dx%d, expected %dx%d)\n", setting, b.w, b.h, w, h);
    return b.w == w && b.h == h;
}

static gboolean fit_is (int bw, int bh, double w, double h, int ow, int oh)
{
    const WalpeVideoBox b = { bw, bh };
    int rw = -1, rh = -1;
    walpe_video_box_fit (&b, w, h, &rw, &rh);
    if (rw != ow || rh != oh)
        g_printerr ("  (%gx%g in %dx%d -> %dx%d, expected %dx%d)\n", w, h, bw, bh, rw, rh, ow, oh);
    return rw == ow && rh == oh;
}

static void test_video (void)
{
    static const int one_hd[] = { 1920, 1080 };
    static const int hd_uhd[] = { 1920, 1080, 3840, 2160 };
    static const int hd_up[] = { 1920, 1080, 1080, 1920 };
    static const int three[] = { 1280, 720, 2560, 1440, 1920, 1200 };
    /* auto: the largest chosen size among the monitors that show it */
    CHECK (box_is ("auto", one_hd, 1, 1920, 1080) && box_is ("auto", hd_uhd, 2, 3840, 2160) &&
           box_is ("auto", three, 3, 2560, 1440));
    /* a lying and an upright monitor: wide enough and tall enough for both */
    CHECK (box_is ("auto", hd_up, 2, 1920, 1920));
    /* nobody shows it and no monitor is known: no limit */
    CHECK (box_is ("auto", NULL, 0, 0, 0) && box_is ("native", one_hd, 1, 1920, 1080));
    /* none: no limit, whatever the monitors */
    CHECK (box_is ("none", hd_uhd, 2, 0, 0) && box_is ("off", one_hd, 1, 0, 0) && box_is ("source", NULL, 0, 0, 0));
    /* fixed: the monitors do not matter; 1080p is the limit that was built in */
    CHECK (box_is ("1080p", hd_uhd, 2, 1920, 1080) && box_is ("720p", hd_uhd, 2, 1280, 720) &&
           box_is ("1440p", NULL, 0, 2560, 1440) && box_is ("4k", one_hd, 1, 3840, 2160) &&
           box_is ("2000x1000", one_hd, 1, 2000, 1000));
    {
        WalpeVideoRes v, w2;
        char *err = NULL;
        CHECK (!walpe_vidres_parse ("huge", &v, &err) && err && strstr (err, "video_resolution"));
        g_free (err);
        err = NULL;
        CHECK (!walpe_vidres_parse ("10x10", &v, &err) && err && strstr (err, "too small"));
        g_free (err);
        CHECK (!walpe_vidres_parse ("", &v, NULL) && !walpe_vidres_parse (NULL, &v, NULL));
        static const char *const rt[] = { "auto", "none", "720p", "1080p", "1440p", "4k", "2000x1000" };
        for (guint i = 0; i < G_N_ELEMENTS (rt); i++) {
            CHECK (walpe_vidres_parse (rt[i], &v, NULL));
            char *f = walpe_vidres_format (&v);
            CHECK (!g_strcmp0 (f, rt[i]) && walpe_vidres_parse (f, &w2, NULL) && walpe_vidres_equal (&v, &w2));
            g_free (f);
        }
        walpe_vidres_parse ("auto", &v, NULL);
        walpe_vidres_parse ("none", &w2, NULL);
        CHECK (!walpe_vidres_equal (&v, &w2));
    }
    /* fitting: inside the box, never enlarged, even sides. The 1920 x 1080
     * rows are the sizes the importer gave while that box was fixed. */
    CHECK (fit_is (1920, 1080, 1920, 1080, 1920, 1080) && fit_is (1920, 1080, 640, 360, 640, 360) &&
           fit_is (1920, 1080, 3840, 2160, 1920, 1080) && fit_is (1920, 1080, 3440, 1440, 1920, 802) &&
           fit_is (1920, 1080, 1080, 1920, 606, 1080) && fit_is (1920, 1080, 1279, 719, 1278, 718) &&
           fit_is (1920, 1080, 1920, 1440, 1440, 1080));
    CHECK (fit_is (3840, 2160, 3840, 2160, 3840, 2160) && fit_is (3840, 2160, 7680, 4320, 3840, 2160) &&
           fit_is (2560, 1440, 3840, 2160, 2560, 1440) && fit_is (1280, 720, 1920, 1080, 1280, 720));
    CHECK (fit_is (0, 0, 7680, 4320, 7680, 4320) && fit_is (0, 0, 1279, 719, 1278, 718) &&
           fit_is (0, 0, 1, 1, 2, 2) && fit_is (1920, 1080, 0, 0, 2, 2) && fit_is (1920, 1080, 1e12, 1, 1920, 2));
    /* no box is not "any size": a header (times a pixel aspect ratio) that
     * says more than a picture can be is brought within 16384 a side and
     * 8192 x 8192 pixels - nothing a real video reaches */
    CHECK (fit_is (0, 0, 1e12, 1, 16384, 2) && fit_is (0, 0, 384000, 2160, 16384, 92) &&
           fit_is (0, 0, 1e12, 1e12, 8192, 8192) && fit_is (0, 0, 18000, 6000, 14188, 4728));
    {
        int w = 0, h = 0;
        walpe_video_box_fit (NULL, 3000, 2000, &w, &h);
        CHECK (w == 3000 && h == 2000);
        const WalpeVideoBox hd = { 1920, 1080 }, none = { 0, 0 };
        CHECK (walpe_video_box_holds (&hd, 1920, 1080) && !walpe_video_box_holds (&hd, 1921, 1080) &&
               !walpe_video_box_holds (&hd, 1920, 1082) && walpe_video_box_holds (&none, 9999, 9999) &&
               walpe_video_box_holds (NULL, 9999, 9999));
    }
}

static void test_lists (void)
{
    char *err = NULL;
    GPtrArray *a = walpe_res_assign_parse ("1080p", &err);
    CHECK (a && a->len == 1 && !err);
    if (a) {
        WalpeResAssign *x = g_ptr_array_index (a, 0);
        CHECK (x->monitor == NULL && !strcmp (x->value, "1080p"));
        g_ptr_array_unref (a);
    }
    a = walpe_res_assign_parse ("HDMI-0:1080p, DP-2 : native", &err);
    CHECK (a && a->len == 2);
    if (a && a->len == 2) {
        WalpeResAssign *x = g_ptr_array_index (a, 0), *y = g_ptr_array_index (a, 1);
        CHECK (!g_strcmp0 (x->monitor, "HDMI-0") && !strcmp (x->value, "1080p") &&
               !g_strcmp0 (y->monitor, "DP-2") && !strcmp (y->value, "native"));
    }
    if (a)
        g_ptr_array_unref (a);
    /* an output name may hold a ':' (the value never does); "all" is no name */
    a = walpe_res_assign_parse ("usb:1.2:2560x1440", &err);
    CHECK (a && a->len == 1 && !g_strcmp0 (((WalpeResAssign *) g_ptr_array_index (a, 0))->monitor, "usb:1.2"));
    if (a)
        g_ptr_array_unref (a);
    a = walpe_res_assign_parse ("all:720p", &err);
    CHECK (a && a->len == 1 && ((WalpeResAssign *) g_ptr_array_index (a, 0))->monitor == NULL);
    if (a)
        g_ptr_array_unref (a);
    /* not of that form */
    static const char *const bad[] = { "", ",", "HDMI-0:", ":1080p", "HDMI-0:1080p,", "1080p,720p",
                                       "HDMI-0:1080p,720p" };
    for (guint i = 0; i < G_N_ELEMENTS (bad); i++) {
        err = NULL;
        a = walpe_res_assign_parse (bad[i], &err);
        if (a) {
            g_printerr ("  ('%s' was taken)\n", bad[i]);
            g_ptr_array_unref (a);
        }
        CHECK (!a && err && strstr (err, "<monitor>:<value>"));
        g_free (err);
    }
    CHECK (walpe_res_assign_parse (NULL, NULL) == NULL);
    CHECK (walpe_monitor_name_ok ("HDMI-0") && walpe_monitor_name_ok ("mon0") && walpe_monitor_name_ok ("DP-1-2") &&
           walpe_monitor_name_ok ("Bildschirm \303\274ber Dock"));
    char *lng = g_strnfill (65, 'm');
    CHECK (!walpe_monitor_name_ok ("") && !walpe_monitor_name_ok (NULL) && !walpe_monitor_name_ok ("a\nb") &&
           !walpe_monitor_name_ok ("\377\376") && !walpe_monitor_name_ok (lng));
    g_free (lng);
    CHECK (walpe_scale_filter_parse ("bilinear") == WALPE_SCALE_BILINEAR &&
           walpe_scale_filter_parse ("NEAREST") == WALPE_SCALE_NEAREST &&
           walpe_scale_filter_parse ("bicubic") == -1 && walpe_scale_filter_parse (NULL) == -1 &&
           !strcmp (walpe_scale_filter_name (WALPE_SCALE_NEAREST), "nearest") &&
           !strcmp (walpe_scale_filter_name (0), "bilinear") && !strcmp (walpe_scale_filter_name (99), "bilinear"));
}

/* ------------------------------------------------------------ config.json */

/* how many of the outputs have not been asked about (what the daemon
 * counts for "resolution_unasked") */
static int unasked (WalpeConfig *c, const char *const *names, guint n)
{
    int u = 0;
    for (guint i = 0; i < n; i++) {
        const WalpeMonitorConf *mc = walpe_config_monitor (c, names[i]);
        u += !(mc && mc->asked);
    }
    return u;
}

static void test_config_fresh (void)
{
    g_unlink (cfg_path);
    WalpeConfig *c = walpe_config_load ();
    /* a new installation: nothing chosen, everything to be asked, videos
     * as large as the monitors show them */
    CHECK (c->config_version == 4 && !c->monitors && c->resolution_prompt && !c->resolution_migrate &&
           c->scale_filter == WALPE_SCALE_BILINEAR && c->video_res.kind == WALPE_VIDRES_AUTO);
    static const char *const outs[] = { "HDMI-0", "DP-2" };
    CHECK (unasked (c, outs, 2) == 2 && walpe_config_monitor (c, "HDMI-0") == NULL);
    WalpeMonitorConf mc;
    walpe_config_monitor_get (c, "HDMI-0", &mc);
    CHECK (mc.res.kind == WALPE_RES_NATIVE && mc.tex_limit == WALPE_TEXLIMIT_AUTO && !mc.asked);
    /* the upgrade step does nothing here: the outputs stay to be asked */
    CHECK (!walpe_config_monitors_migrate (c, outs, 2) && unasked (c, outs, 2) == 2);
    /* a save of an untouched config writes none of the optional members;
     * "video_resolution" is always there (it marks the file as format 4) */
    walpe_config_save (c);
    JsonNode *root = NULL;
    JsonObject *o = read_config (&root);
    CHECK (o && wpj_num (o, "config_version", 0) == 4 && !json_object_has_member (o, "monitors") &&
           !json_object_has_member (o, "resolution_prompt") && !json_object_has_member (o, "resolution_migrate") &&
           !json_object_has_member (o, "scale_filter") && !g_strcmp0 (wpj_str (o, "video_resolution", ""), "auto"));
    walpe_config_free (c);

    /* That file through a build of format 3 and back: such a build keeps
     * the members it does not know and writes its own version number. It
     * is not an upgrade: the outputs are still to be asked about, videos
     * still follow the monitors. */
    if (o) {
        json_object_set_int_member (o, "config_version", 3);
        char *txt = json_to_string (root, TRUE);
        put_config (txt);
        g_free (txt);
    }
    if (root)
        json_node_unref (root);
    c = walpe_config_load ();
    CHECK (!c->resolution_migrate && c->video_res.kind == WALPE_VIDRES_AUTO && !c->extra);
    CHECK (!walpe_config_monitors_migrate (c, outs, 2) && unasked (c, outs, 2) == 2);
    walpe_config_save (c);
    walpe_config_free (c);
    o = read_config (&root);
    CHECK (o && wpj_num (o, "config_version", 0) == 4 && !json_object_has_member (o, "monitors") &&
           !json_object_has_member (o, "resolution_migrate") &&
           !g_strcmp0 (wpj_str (o, "video_resolution", ""), "auto"));
    if (root)
        json_node_unref (root);
}

/* config.json as format 3 wrote it (the keys that matter here) */
static const char v3_config[] =
    "{\"config_version\":3,\"wallpapers_dir\":\"/home/u/lib\",\"current\":\"/lib/2901582518\","
    "\"fps\":60,\"monitor_current\":{\"DP-2\":\"/lib/3292662040\"},\"tex_downscale\":true,"
    "\"texture_slack\":2,\"render_scale\":100,\"quality_preset\":\"high\",\"video_limit\":false,"
    "\"render_shared\":true,\"parallax\":true}";

static void test_config_upgrade (void)
{
    static const char *const outs[] = { "HDMI-0", "DP-2" };
    put_config (v3_config);
    WalpeConfig *c = walpe_config_load ();
    /* the settings it had */
    CHECK (!g_strcmp0 (c->current, "/lib/2901582518") && c->fps == 60 && c->tex_downscale &&
           c->texture_slack == 2 && c->render_scale == 100 && c->render_shared && c->parallax &&
           !g_strcmp0 (walpe_config_monitor_path (c, "DP-2"), "/lib/3292662040") && !c->extra);
    /* what this format adds, as that version behaved: nobody is asked,
     * videos within 1920 x 1080 */
    CHECK (c->resolution_migrate && c->resolution_prompt && !c->monitors &&
           c->scale_filter == WALPE_SCALE_BILINEAR && c->video_res.kind == WALPE_VIDRES_FIXED);
    {
        WalpeVideoBox b;
        static const int uhd[] = { 3840, 2160 };
        walpe_video_box (&c->video_res, uhd, 1, &b);
        CHECK (b.w == 1920 && b.h == 1080);     /* also on a 4K monitor: as it was */
    }
    /* a process that is not the daemon saves (it does not know the
     * outputs): the mark stays in the file */
    walpe_config_save (c);
    walpe_config_free (c);
    JsonNode *root = NULL;
    JsonObject *o = read_config (&root);
    CHECK (o && wpj_num (o, "config_version", 0) == 4 && wpj_bool (o, "resolution_migrate", FALSE) &&
           !g_strcmp0 (wpj_str (o, "video_resolution", ""), "1080p") && !json_object_has_member (o, "monitors"));
    if (root)
        json_node_unref (root);
    c = walpe_config_load ();
    CHECK (c->resolution_migrate && c->video_res.kind == WALPE_VIDRES_FIXED && !c->extra);

    /* the daemon's step: the connected outputs are entered, once */
    CHECK (walpe_config_monitors_migrate (c, outs, 2) && !c->resolution_migrate);
    CHECK (unasked (c, outs, 2) == 0);
    for (int i = 0; i < 2; i++) {
        const WalpeMonitorConf *mc = walpe_config_monitor (c, outs[i]);
        CHECK (mc && mc->asked && mc->res.kind == WALPE_RES_NATIVE && mc->tex_limit == WALPE_TEXLIMIT_AUTO);
        /* native on its mode is the mode; auto leaves textures to
         * tex_downscale: the numbers the renderer gets are the old ones */
        int w = 0, h = 0, cap = -1;
        gboolean nd = TRUE;
        if (mc) {
            walpe_res_size (&mc->res, 1920, 1080, &w, &h);
            walpe_texlimit_effective (mc->tex_limit, c->tex_downscale, &nd, &cap);
        }
        CHECK (w == 1920 && h == 1080 && !nd && cap == 0);
    }
    CHECK (!walpe_config_monitors_migrate (c, outs, 2));    /* not twice */
    walpe_config_save (c);
    walpe_config_free (c);
    o = read_config (&root);
    JsonObject *ms = o ? wpj_obj (o, "monitors") : NULL;
    CHECK (ms && json_object_get_size (ms) == 2 && !json_object_has_member (o, "resolution_migrate"));
    JsonObject *h0 = ms ? wpj_obj (ms, "HDMI-0") : NULL;
    CHECK (h0 && !g_strcmp0 (wpj_str (h0, "resolution", ""), "native") &&
           !g_strcmp0 (wpj_str (h0, "texture_limit", ""), "auto") && wpj_bool (h0, "asked", FALSE));
    if (root)
        json_node_unref (root);

    /* the next start: nothing pending, nobody to ask. Then a monitor is
     * plugged in that was never there: it has no entry - native, to be
     * asked - and the others are as they were. */
    c = walpe_config_load ();
    static const char *const outs3[] = { "HDMI-0", "DP-2", "DP-3" };
    CHECK (!c->resolution_migrate && unasked (c, outs, 2) == 0);
    CHECK (!walpe_config_monitors_migrate (c, outs3, 3));   /* the step is over: DP-3 is not entered */
    CHECK (unasked (c, outs3, 3) == 1 && walpe_config_monitor (c, "DP-3") == NULL);
    /* unplugged and back, a daemon restart in between: still to be asked */
    walpe_config_save (c);
    walpe_config_free (c);
    c = walpe_config_load ();
    CHECK (unasked (c, outs3, 3) == 1 && unasked (c, outs, 2) == 0);
    /* the user answers for it */
    WalpeMonitorConf mc;
    walpe_config_monitor_get (c, "DP-3", &mc);
    CHECK (!mc.asked && mc.res.kind == WALPE_RES_NATIVE);
    walpe_res_parse ("1440p", &mc.res, NULL);
    mc.tex_limit = 4096;
    mc.asked = TRUE;
    walpe_config_set_monitor (c, "DP-3", &mc);
    walpe_config_save (c);
    walpe_config_free (c);
    c = walpe_config_load ();
    const WalpeMonitorConf *d3 = walpe_config_monitor (c, "DP-3");
    CHECK (d3 && d3->asked && d3->res.kind == WALPE_RES_LINES && d3->res.lines == 1440 && d3->tex_limit == 4096);
    CHECK (unasked (c, outs3, 3) == 0);
    /* its entry stays while it is unplugged (the other outputs' too) */
    CHECK (g_hash_table_size (c->monitors) == 3);
    /* "ask again" and "forget" */
    walpe_config_monitor_get (c, "DP-3", &mc);
    mc.asked = FALSE;
    walpe_config_set_monitor (c, "DP-3", &mc);
    CHECK (unasked (c, outs3, 3) == 1 && walpe_config_monitor (c, "DP-3")->res.lines == 1440);
    walpe_config_set_monitor (c, "DP-3", NULL);
    CHECK (walpe_config_monitor (c, "DP-3") == NULL && g_hash_table_size (c->monitors) == 2);
    walpe_config_set_monitor (c, "HDMI-0", NULL);
    walpe_config_set_monitor (c, "DP-2", NULL);
    walpe_config_set_monitor (c, "DP-2", NULL);             /* (not there any more) */
    walpe_config_save (c);
    walpe_config_free (c);
    o = read_config (&root);
    CHECK (o && !json_object_has_member (o, "monitors"));   /* no empty object is left, and none comes back */
    if (root)
        json_node_unref (root);
    c = walpe_config_load ();
    CHECK (!c->monitors && !c->extra && !c->resolution_migrate);
    walpe_config_free (c);
}

static void test_config_keys (void)
{
    /* every key through save + load with a value that is not its default */
    g_unlink (cfg_path);
    WalpeConfig *c = walpe_config_load ();
    WalpeMonitorConf mc = { { WALPE_RES_CUSTOM, 0, 2560, 1080 }, WALPE_TEXLIMIT_NONE, FALSE };
    walpe_config_set_monitor (c, "eDP-1", &mc);
    c->resolution_prompt = FALSE;
    c->scale_filter = WALPE_SCALE_NEAREST;
    walpe_vidres_parse ("none", &c->video_res, NULL);
    walpe_config_save (c);
    walpe_config_free (c);
    c = walpe_config_load ();
    const WalpeMonitorConf *e = walpe_config_monitor (c, "eDP-1");
    CHECK (e && e->res.kind == WALPE_RES_CUSTOM && e->res.w == 2560 && e->res.h == 1080 &&
           e->tex_limit == WALPE_TEXLIMIT_NONE && !e->asked);
    CHECK (!c->resolution_prompt && c->scale_filter == WALPE_SCALE_NEAREST &&
           c->video_res.kind == WALPE_VIDRES_NONE && !c->extra);
    /* back to the defaults: the optional members leave the file, nothing
     * brings them back ("video_resolution" stays, as "auto") */
    c->resolution_prompt = TRUE;
    c->scale_filter = WALPE_SCALE_BILINEAR;
    walpe_vidres_parse ("auto", &c->video_res, NULL);
    walpe_config_save (c);
    walpe_config_free (c);
    JsonNode *root = NULL;
    JsonObject *o = read_config (&root);
    CHECK (o && !json_object_has_member (o, "resolution_prompt") && !json_object_has_member (o, "scale_filter") &&
           !g_strcmp0 (wpj_str (o, "video_resolution", ""), "auto") && json_object_has_member (o, "monitors"));
    if (root)
        json_node_unref (root);
    c = walpe_config_load ();
    CHECK (c->resolution_prompt && c->scale_filter == WALPE_SCALE_BILINEAR &&
           c->video_res.kind == WALPE_VIDRES_AUTO && !c->extra);
    walpe_config_free (c);

    /* written by hand: what is not a value costs that part only */
    put_config ("{\"config_version\":4,\"fps\":50,\"monitors\":{"
                "\"A\":{\"resolution\":\"1080p\"},"
                "\"B\":{\"resolution\":\"enormous\",\"texture_limit\":\"2048\",\"asked\":false},"
                "\"C\":{\"resolution\":\"99999x99999\",\"texture_limit\":7,\"asked\":\"yes\"},"
                "\"D\":{\"resolution\":7,\"texture_limit\":4096},"
                "\"E\":\"1080p\",\"F\":[1],\"\":{\"resolution\":\"720p\"},"
                "\"G\":{}},"
                "\"scale_filter\":\"cubic\",\"video_resolution\":\"gigantic\",\"resolution_prompt\":\"no\","
                "\"future_key\":1}");
    c = walpe_config_load ();
    CHECK (c->fps == 50 && c->config_version == 4 && !c->resolution_migrate);
    const WalpeMonitorConf *a = walpe_config_monitor (c, "A"), *b = walpe_config_monitor (c, "B");
    const WalpeMonitorConf *cc = walpe_config_monitor (c, "C"), *d = walpe_config_monitor (c, "D");
    const WalpeMonitorConf *g = walpe_config_monitor (c, "G");
    CHECK (a && a->res.kind == WALPE_RES_LINES && a->res.lines == 1080 && a->tex_limit == WALPE_TEXLIMIT_AUTO &&
           a->asked);                           /* an entry without "asked" is a choice */
    CHECK (b && b->res.kind == WALPE_RES_NATIVE && b->tex_limit == 2048 && !b->asked);
    CHECK (cc && cc->res.kind == WALPE_RES_NATIVE && cc->tex_limit == WALPE_TEXLIMIT_AUTO && cc->asked);
    CHECK (d && d->res.kind == WALPE_RES_NATIVE && d->tex_limit == 4096);
    CHECK (g && g->res.kind == WALPE_RES_NATIVE && g->asked);
    CHECK (!walpe_config_monitor (c, "E") && !walpe_config_monitor (c, "F") && !walpe_config_monitor (c, ""));
    CHECK (c->scale_filter == WALPE_SCALE_BILINEAR && c->video_res.kind == WALPE_VIDRES_AUTO &&
           c->resolution_prompt);
    /* a member of a newer version is kept; the members of this feature
     * are not mistaken for one */
    CHECK (c->extra && json_object_get_size (c->extra) == 1 && json_object_has_member (c->extra, "future_key"));
    walpe_config_free (c);

    {
        /* a file with thousands of entries (not one the daemon wrote): read
         * up to the bound, and the settings around them are read as usual */
        GString *big = g_string_new ("{\"config_version\":4,\"fps\":50,\"monitors\":{");
        for (int i = 0; i < WALPE_MONITORS_READ_MAX + 300; i++)
            g_string_append_printf (big, "%s\"OUT-%d\":{\"resolution\":\"720p\"}", i ? "," : "", i);
        g_string_append (big, "},\"scale_filter\":\"nearest\"}");
        put_config (big->str);
        g_string_free (big, TRUE);
        c = walpe_config_load ();
        CHECK (c->monitors && g_hash_table_size (c->monitors) == WALPE_MONITORS_READ_MAX &&
               c->fps == 50 && c->scale_filter == WALPE_SCALE_NEAREST && !c->extra);
        const WalpeMonitorConf *first = walpe_config_monitor (c, "OUT-0");
        CHECK (first && first->res.kind == WALPE_RES_LINES && first->res.lines == 720);
        walpe_config_free (c);
        put_config ("{\"config_version\":4,\"fps\":50,\"monitors\":{\"A\":{\"resolution\":\"1080p\"}},"
                    "\"future_key\":1}");
    }
    /* a name that cannot be a key is not stored */
    c = walpe_config_load ();
    WalpeMonitorConf ok = { { WALPE_RES_NATIVE, 0, 0, 0 }, WALPE_TEXLIMIT_AUTO, TRUE };
    guint before = g_hash_table_size (c->monitors);
    walpe_config_set_monitor (c, "", &ok);
    walpe_config_set_monitor (c, NULL, &ok);
    walpe_config_set_monitor (c, "a\tb", &ok);
    CHECK (g_hash_table_size (c->monitors) == before && walpe_config_monitor (c, NULL) == NULL);
    walpe_config_free (c);

    /* format 1 and 2 files take the same step as format 3 */
    put_config ("{\"fps\":144,\"current\":\"/lib/1\"}");
    c = walpe_config_load ();
    CHECK (c->resolution_migrate && c->video_res.kind == WALPE_VIDRES_FIXED);
    walpe_config_free (c);
    /* ... unless the file has "video_resolution": it has been of format 4
     * (an older build saved it since). Its value holds and nothing is
     * migrated; a mark that was still pending in it stays pending. */
    put_config ("{\"config_version\":2,\"fps\":144,\"video_resolution\":\"none\"}");
    c = walpe_config_load ();
    CHECK (!c->resolution_migrate && c->video_res.kind == WALPE_VIDRES_NONE);
    walpe_config_free (c);
    put_config ("{\"config_version\":3,\"video_resolution\":\"1080p\",\"resolution_migrate\":true}");
    c = walpe_config_load ();
    CHECK (c->resolution_migrate && c->video_res.kind == WALPE_VIDRES_FIXED);
    walpe_config_free (c);
}

static void test_config_newer (void)
{
    /* a file of a later version: what this build does not know, at the top
     * of the file and inside an output's entry, is still there after this
     * build loaded, changed and saved it */
    put_config ("{\"config_version\":5,\"wallpapers_dir\":\"/lib\",\"fps\":50,"
                "\"video_resolution\":\"auto\",\"later_top\":{\"a\":[1,2]},\"monitors\":{"
                "\"DP-1\":{\"resolution\":\"1440p\",\"texture_limit\":\"auto\",\"asked\":true,"
                "\"later_text\":\"nearest\",\"later_obj\":{\"serial\":\"X1\",\"n\":[1,2,3]}},"
                "\"HDMI-0\":{\"resolution\":\"native\",\"later_num\":7},"
                "\"DP-2\":{\"resolution\":\"720p\"}}}");
    WalpeConfig *c = walpe_config_load ();
    const WalpeMonitorConf *e = walpe_config_monitor (c, "DP-1");
    CHECK (e && e->res.kind == WALPE_RES_LINES && e->res.lines == 1440 && e->asked &&
           !c->resolution_migrate);
    /* this build changes an entry, drops one and adds one */
    WalpeMonitorConf mc = { { WALPE_RES_CUSTOM, 0, 1600, 900 }, WALPE_TEXLIMIT_AUTO, TRUE };
    walpe_config_set_monitor (c, "DP-1", &mc);
    walpe_config_set_monitor (c, "HDMI-0", NULL);
    walpe_config_set_monitor (c, "DP-3", &mc);
    walpe_config_save (c);
    walpe_config_free (c);
    JsonNode *root = NULL;
    JsonObject *o = read_config (&root);
    JsonObject *mo = o ? wpj_obj (o, "monitors") : NULL;
    JsonObject *d1 = mo ? wpj_obj (mo, "DP-1") : NULL, *d2 = mo ? wpj_obj (mo, "DP-2") : NULL;
    JsonObject *d3 = mo ? wpj_obj (mo, "DP-3") : NULL;
    JsonObject *lo = d1 ? wpj_obj (d1, "later_obj") : NULL;
    CHECK (o && json_object_has_member (o, "later_top"));
    CHECK (d1 && !g_strcmp0 (wpj_str (d1, "resolution", ""), "1600x900") &&
           !g_strcmp0 (wpj_str (d1, "later_text", ""), "nearest"));
    CHECK (lo && !g_strcmp0 (wpj_str (lo, "serial", ""), "X1") && json_object_has_member (lo, "n") &&
           json_array_get_length (json_object_get_array_member (lo, "n")) == 3);
    CHECK (mo && !json_object_has_member (mo, "HDMI-0"));   /* a dropped entry is gone whole */
    CHECK (d2 && json_object_get_size (d2) == 3 && d3 && json_object_get_size (d3) == 3);
    if (root)
        json_node_unref (root);
    /* loaded and saved once more: the same file; an entry that was dropped
     * and is made again starts without the members it once had */
    char *t1 = NULL, *t2 = NULL;
    g_file_get_contents (cfg_path, &t1, NULL, NULL);
    c = walpe_config_load ();
    walpe_config_save (c);
    g_file_get_contents (cfg_path, &t2, NULL, NULL);
    CHECK (t1 && t2 && !strcmp (t1, t2));
    g_free (t1);
    g_free (t2);
    walpe_config_set_monitor (c, "DP-1", NULL);
    walpe_config_set_monitor (c, "DP-1", &mc);
    walpe_config_save (c);
    walpe_config_free (c);
    o = read_config (&root);
    mo = o ? wpj_obj (o, "monitors") : NULL;
    d1 = mo ? wpj_obj (mo, "DP-1") : NULL;
    CHECK (d1 && json_object_get_size (d1) == 3);
    if (root)
        json_node_unref (root);
}

static gboolean path_is (WalpeConfig *c, const char *output, const char *path)
{
    return !g_strcmp0 (walpe_config_monitor_path (c, output), path);
}

static void test_config_other_spelling (void)
{
    /* a file as an X11-only version wrote it: the monitors' own wallpapers
     * under RandR names */
    put_config ("{\"config_version\":3,\"wallpapers_dir\":\"/lib\",\"current\":\"/lib/a\","
                "\"monitor_current\":{\"HDMI-1\":\"/lib/b\",\"DP-1\":\"/lib/c\",\"DVI-I-1\":\"/lib/d\","
                "\"eDP1\":\"/lib/e\"}}");
    static const char *const wl[] = { "HDMI-A-1", "DP-1", "eDP-1", "DVI-D-1" };
    static const char *const dvi[] = { "DVI-I-1", "DVI-D-1" };
    WalpeConfig *c = walpe_config_load ();
    /* nobody said otherwise, or X11: the exact name only, as it always was */
    CHECK (path_is (c, "HDMI-A-1", "/lib/a") && path_is (c, "HDMI-1", "/lib/b"));
    walpe_config_set_outputs (c, wl, G_N_ELEMENTS (wl), FALSE);
    CHECK (path_is (c, "HDMI-A-1", "/lib/a") && path_is (c, "eDP-1", "/lib/a") && path_is (c, "DP-1", "/lib/c"));
    /* Wayland: the same connectors under their other spelling */
    walpe_config_set_outputs (c, wl, G_N_ELEMENTS (wl), TRUE);
    CHECK (path_is (c, "HDMI-A-1", "/lib/b") && path_is (c, "DP-1", "/lib/c") &&
           path_is (c, "eDP-1", "/lib/e") && path_is (c, "DVI-D-1", "/lib/d"));
    CHECK (path_is (c, "HDMI-A-2", "/lib/a") && path_is (c, "DP-2", "/lib/a"));   /* other connectors */
    /* the entry of an output that is connected itself is not lent */
    walpe_config_set_outputs (c, dvi, G_N_ELEMENTS (dvi), TRUE);
    CHECK (path_is (c, "DVI-I-1", "/lib/d") && path_is (c, "DVI-D-1", "/lib/a"));
    walpe_config_set_outputs (c, wl, G_N_ELEMENTS (wl), TRUE);
    /* the upgrade step: an output with an entry under its other spelling
     * has one; a choice made now is stored under the name of now and holds */
    WalpeMonitorConf mc = { { WALPE_RES_LINES, 720, 0, 0 }, WALPE_TEXLIMIT_AUTO, TRUE };
    g_hash_table_remove_all (c->monitors ? c->monitors : (c->monitors = g_hash_table_new_full (
                                 g_str_hash, g_str_equal, g_free, g_free)));
    g_hash_table_insert (c->monitors, g_strdup ("HDMI-1"), g_memdup2 (&mc, sizeof (mc)));
    const WalpeMonitorConf *e = walpe_config_monitor (c, "HDMI-A-1");
    CHECK (e && e->res.kind == WALPE_RES_LINES && e->res.lines == 720);
    CHECK (c->resolution_migrate && walpe_config_monitors_migrate (c, wl, G_N_ELEMENTS (wl)) &&
           !g_hash_table_contains (c->monitors, "HDMI-A-1") && g_hash_table_contains (c->monitors, "DP-1"));
    walpe_config_set_monitor_path (c, "HDMI-A-1", "/lib/g");
    CHECK (path_is (c, "HDMI-A-1", "/lib/g") &&
           !g_strcmp0 (g_hash_table_lookup (c->monitor_current, "HDMI-1"), "/lib/b"));
    /* the choice is dropped: under both spellings, the default shows */
    walpe_config_set_monitor_path (c, "HDMI-A-1", NULL);
    CHECK (path_is (c, "HDMI-A-1", "/lib/a") && !g_hash_table_contains (c->monitor_current, "HDMI-1"));
    walpe_config_set_monitor (c, "HDMI-A-1", NULL);
    CHECK (!walpe_config_monitor (c, "HDMI-A-1") && !g_hash_table_contains (c->monitors, "HDMI-1"));
    walpe_config_free (c);
    /* two entries of one connector: not guessed */
    put_config ("{\"config_version\":3,\"wallpapers_dir\":\"/lib\",\"current\":\"/lib/a\","
                "\"monitor_current\":{\"DVI-I-1\":\"/lib/b\",\"DVI-A-1\":\"/lib/c\"}}");
    c = walpe_config_load ();
    walpe_config_set_outputs (c, wl, G_N_ELEMENTS (wl), TRUE);
    CHECK (path_is (c, "DVI-D-1", "/lib/a"));
    walpe_config_free (c);
}

int main (void)
{
    /* before any GLib call that caches the config dir */
    char *tmp = g_dir_make_tmp ("walpe-resolution-test-XXXXXX", NULL);
    g_assert (tmp);
    g_setenv ("XDG_CONFIG_HOME", tmp, TRUE);
    g_setenv ("HOME", tmp, TRUE);
    cfg_dir = g_build_filename (tmp, "walpe", NULL);
    g_mkdir_with_parents (cfg_dir, 0700);
    cfg_path = g_build_filename (cfg_dir, "config.json", NULL);

    test_values ();
    test_sizes ();
    test_texlimit ();
    test_video ();
    test_lists ();
    test_config_fresh ();
    test_config_upgrade ();
    test_config_keys ();
    test_config_newer ();
    test_config_other_spelling ();

    g_unlink (cfg_path);
    g_rmdir (cfg_dir);
    g_rmdir (tmp);
    g_free (cfg_dir);
    g_free (cfg_path);
    g_free (tmp);
    if (fails)
        g_printerr ("%d check(s) failed\n", fails);
    else
        g_print ("resolution: all checks passed\n");
    return fails ? 1 : 0;
}
