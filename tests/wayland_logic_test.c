/* wayland_logic_test.c - the rules of the Wayland backend
 * (platform/wayland_logic.h) against made-up values: no compositor, no
 * libwayland. Scale arithmetic at fractional scales and odd sizes, the
 * output rectangles and their order, buffer size / buffer scale / viewport
 * of a surface, the pointer mapping, wheel clicks, the frame-callback
 * visibility test, the life cycle of a layer surface and the layer a
 * wallpaper surface goes on. */
#include <glib.h>
#include <math.h>
#include <string.h>
#include "platform/wayland_logic.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; g_printerr ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void test_scale (void)
{
    /* 1.25 = 150/120, 1.5 = 180/120, 1.75 = 210/120 */
    CHECK (wl_scale120_apply (2048, 150) == 2560);
    CHECK (wl_scale120_apply (1152, 150) == 1440);
    CHECK (wl_scale120_apply (1280, 180) == 1920);
    CHECK (wl_scale120_apply (1920, 120) == 1920);
    CHECK (wl_scale120_apply (1920, 240) == 3840);
    /* odd sizes: 1366 x 768 at 1.25 is 1092.8 x 614.4 units, which a
     * compositor rounds to 1093 x 614; back to pixels with the protocol's
     * rounding (half away from zero) */
    CHECK (wl_scale120_apply (1093, 150) == 1366);   /* 1366.25 */
    CHECK (wl_scale120_apply (614, 150) == 768);     /* 767.5 rounds up */
    CHECK (wl_scale120_apply (853, 180) == 1280);    /* 1279.5 rounds up */
    CHECK (wl_scale120_apply (1463, 210) == 2560);   /* 2560.25 */
    CHECK (wl_scale120_apply (1, 150) == 1);         /* 1.25 */
    CHECK (wl_scale120_apply (2, 150) == 3);         /* 2.5 rounds up */
    CHECK (wl_scale120_apply (0, 150) == 0);
    CHECK (wl_scale120_apply (-5, 150) == 0);
    CHECK (wl_scale120_apply (100, 0) == 0);

    CHECK (wl_scale120_from_sizes (1920, 1920) == 120);
    CHECK (wl_scale120_from_sizes (2560, 2048) == 150);
    CHECK (wl_scale120_from_sizes (1920, 1280) == 180);
    CHECK (wl_scale120_from_sizes (3840, 1920) == 240);
    CHECK (wl_scale120_from_sizes (1366, 1093) == 150);   /* 1.2498 on the grid: 1.25 */
    CHECK (wl_scale120_from_sizes (2560, 1463) == 210);
    CHECK (wl_scale120_from_sizes (0, 100) == 0);
    CHECK (wl_scale120_from_sizes (100, 0) == 0);

    /* every pixel width, odd ones included: the logical width a compositor
     * derives gives the scale back exactly, and the scale gives the pixel
     * width back to within one pixel */
    static const int scales[] = { 120, 150, 180, 210, 240 };
    for (guint i = 0; i < G_N_ELEMENTS (scales); i++)
        for (int px = 1280; px <= 3841; px += 7) {
            int logical = (int) floor ((double) px * 120.0 / scales[i] + 0.5);
            int back = wl_scale120_apply (logical, scales[i]);
            CHECK (wl_scale120_from_sizes (px, logical) == scales[i]);
            CHECK (ABS (back - px) <= 1);
        }

    CHECK (!wl_transform_swaps (0) && wl_transform_swaps (1) && !wl_transform_swaps (2) &&
           wl_transform_swaps (3));
    CHECK (!wl_transform_swaps (4) && wl_transform_swaps (5) && !wl_transform_swaps (6) &&
           wl_transform_swaps (7));
}

/* text from the compositor: what the daemon stores is valid UTF-8 without
 * control characters and of bounded length */
static void check_clean (const char *in, gsize max, const char *want)
{
    char *got = wl_text_clean (in, max);
    if (g_strcmp0 (got, want)) {
        fails++;
        g_printerr ("FAIL wl_text_clean: got \"%s\", wanted \"%s\"\n", got ? got : "(null)",
                    want ? want : "(null)");
    }
    if (got)
        CHECK (g_utf8_validate (got, -1, NULL) && strlen (got) <= max);
    g_free (got);
}

static void test_text (void)
{
    check_clean ("DP-1", WL_NAME_MAX, "DP-1");
    check_clean ("HDMI-A-1", WL_NAME_MAX, "HDMI-A-1");
    check_clean (NULL, WL_NAME_MAX, NULL);
    check_clean ("", WL_NAME_MAX, NULL);
    /* UTF-8 stays as it is */
    check_clean ("Bildschirm \xc3\xa4 \xe2\x82\xac", WL_TEXT_MAX, "Bildschirm \xc3\xa4 \xe2\x82\xac");
    /* control characters, also the ones that would start a new log line */
    check_clean ("DP-1\nfake: line", WL_TEXT_MAX, "DP-1?fake: line");
    check_clean ("a\tb\x7f" "c\x1b[0m", WL_TEXT_MAX, "a?b?c?[0m");
    check_clean ("x\xc2\x85y\xe2\x80\xa8z", WL_TEXT_MAX, "x?y?z");   /* U+0085, U+2028 */
    /* bytes that are not UTF-8: Latin-1, a cut sequence, an overlong form */
    check_clean ("Monit\xf6r", WL_TEXT_MAX, "Monit?r");
    check_clean ("abc\xe2\x82", WL_TEXT_MAX, "abc??");
    check_clean ("\xc0\xaf", WL_TEXT_MAX, "??");
    check_clean ("\xff\xfe", WL_TEXT_MAX, "??");
    /* the length is a number of bytes and never cuts a character in two */
    check_clean ("abcdef", 4, "abcd");
    check_clean ("ab\xe2\x82\xac", 4, "ab");
    check_clean ("ab\xe2\x82\xac", 5, "ab\xe2\x82\xac");
    check_clean ("\xe2\x82\xac", 2, NULL);
    check_clean ("abc", 0, NULL);
    char big[4000];
    memset (big, 'x', sizeof big - 1);
    big[sizeof big - 1] = 0;
    char *c = wl_text_clean (big, WL_NAME_MAX);
    CHECK (c && strlen (c) == WL_NAME_MAX);
    g_free (c);
}

static WlOutProps out_plain (guint32 id, const char *name, int x, int y, int w, int h)
{
    WlOutProps p;
    memset (&p, 0, sizeof p);
    p.id = id;
    p.name = name;
    p.geo_x = x;
    p.geo_y = y;
    p.mode_w = w;
    p.mode_h = h;
    p.refresh_mhz = 60000;
    p.int_scale = 1;
    return p;
}

/* an output with a scale: the mode, and what xdg-output says */
static WlOutProps out_scaled (guint32 id, const char *name, int lx, int ly, int lw, int lh, int mw,
                              int mh, int int_scale)
{
    WlOutProps p = out_plain (id, name, lx, ly, mw, mh);
    p.int_scale = int_scale;
    p.have_xdg_pos = p.have_xdg_size = TRUE;
    p.xdg_x = lx;
    p.xdg_y = ly;
    p.xdg_w = lw;
    p.xdg_h = lh;
    return p;
}

static void test_output_sizes (void)
{
    int w, h, lx, ly, lw, lh;
    WlOutProps p = out_plain (1, "DP-1", 0, 0, 1920, 1080);
    CHECK (wl_out_usable (&p));
    wl_out_logical (&p, &lx, &ly, &lw, &lh);
    CHECK (lx == 0 && ly == 0 && lw == 1920 && lh == 1080);
    wl_out_pixels (&p, TRUE, &w, &h);
    CHECK (w == 1920 && h == 1080);
    wl_out_pixels (&p, FALSE, &w, &h);
    CHECK (w == 1920 && h == 1080);

    /* 2560 x 1440 at 1.25: wl_output.scale says 2, xdg-output 2048 x 1152 */
    p = out_scaled (2, "DP-2", 1920, 0, 2048, 1152, 2560, 1440, 2);
    wl_out_logical (&p, &lx, &ly, &lw, &lh);
    CHECK (lx == 1920 && ly == 0 && lw == 2048 && lh == 1152);
    wl_out_pixels (&p, TRUE, &w, &h);       /* with a viewport: the panel's pixels */
    CHECK (w == 2560 && h == 1440);
    wl_out_pixels (&p, FALSE, &w, &h);      /* without: logical x integer scale */
    CHECK (w == 4096 && h == 2304);

    /* rotated by 90 degrees: the mode is the panel's, the layout has it upright */
    p = out_plain (3, "HDMI-A-1", 0, 0, 1920, 1080);
    p.transform = 1;
    wl_out_mode_transformed (&p, &w, &h);
    CHECK (w == 1080 && h == 1920);
    wl_out_logical (&p, &lx, &ly, &lw, &lh);
    CHECK (lw == 1080 && lh == 1920);
    wl_out_pixels (&p, TRUE, &w, &h);
    CHECK (w == 1080 && h == 1920);
    p.transform = 2;                        /* upside down: nothing is exchanged */
    wl_out_pixels (&p, TRUE, &w, &h);
    CHECK (w == 1920 && h == 1080);

    /* integer scale 2 without xdg-output: logical = mode / 2 */
    p = out_plain (4, "eDP-1", 0, 0, 3840, 2160);
    p.int_scale = 2;
    wl_out_logical (&p, &lx, &ly, &lw, &lh);
    CHECK (lw == 1920 && lh == 1080);
    wl_out_pixels (&p, FALSE, &w, &h);
    CHECK (w == 3840 && h == 2160);

    /* no mode told (a virtual output): the logical size has to do */
    p = out_scaled (5, "HEADLESS-1", 0, 0, 1280, 720, 0, 0, 1);
    CHECK (wl_out_usable (&p));
    wl_out_pixels (&p, TRUE, &w, &h);
    CHECK (w == 1280 && h == 720);
    /* nothing told at all: not listed */
    p = out_plain (6, NULL, 0, 0, 0, 0);
    CHECK (!wl_out_usable (&p));
    /* a scale of 0 or less is read as 1 */
    p = out_plain (7, "X", 0, 0, 800, 600);
    p.int_scale = 0;
    CHECK (wl_out_int_scale (&p) == 1);
    wl_out_pixels (&p, FALSE, &w, &h);
    CHECK (w == 800 && h == 600);
}

static void test_output_layout (void)
{
    /* (set: the first call hands the array over with a count of 0, and a
     * build with the sanitizers warns about an array nobody wrote) */
    WlOutProps p[4] = { 0 };
    WlOutRect r[4];

    memset (p, 0, sizeof p);                /* (a build without LTO warns about handing it over unset) */
    wl_out_layout (p, 0, TRUE, r);          /* no output: nothing to do, no crash */

    /* two monitors side by side, announced right one first: the list is
     * ordered by position, not by arrival */
    p[0] = out_plain (40, "HDMI-A-1", 1920, 0, 2560, 1440);
    p[0].refresh_mhz = 143998;
    p[1] = out_plain (30, "DP-1", 0, 0, 1920, 1080);
    p[1].refresh_mhz = 59951;
    wl_out_layout (p, 2, TRUE, r);
    CHECK (r[0].src == &p[1] && r[1].src == &p[0]);
    CHECK (r[0].x == 0 && r[0].y == 0 && r[0].width == 1920 && r[0].height == 1080);
    CHECK (r[1].x == 1920 && r[1].y == 0 && r[1].width == 2560 && r[1].height == 1440);
    CHECK (r[0].scale120 == 120 && r[1].scale120 == 120);
    CHECK (fabs (r[0].refresh_hz - 59.951) < 1e-9 && fabs (r[1].refresh_hz - 143.998) < 1e-9);
    CHECK (!wl_rects_overlap (&r[0], &r[1]));

    /* stacked: same x, ordered by y */
    p[0] = out_plain (1, "B", 0, 1080, 1920, 1080);
    p[1] = out_plain (2, "A", 0, 0, 1920, 1080);
    wl_out_layout (p, 2, TRUE, r);
    CHECK (r[0].src == &p[1] && r[1].src == &p[0] && r[1].y == 1080);

    /* mirrored (same position): by name, then by id; and they overlap, so
     * the daemon gets them in a row */
    p[0] = out_plain (9, "DP-2", 0, 0, 1920, 1080);
    p[1] = out_plain (8, "DP-1", 0, 0, 1920, 1080);
    p[2] = out_plain (7, "DP-2", 0, 0, 1920, 1080);
    wl_out_layout (p, 3, TRUE, r);
    CHECK (r[0].src == &p[1] && r[1].src == &p[2] && r[2].src == &p[0]);
    CHECK (r[0].x == 0 && r[1].x == 1920 && r[2].x == 3840 && r[2].y == 0);
    /* an output without a name sorts first and breaks nothing */
    p[0] = out_plain (9, NULL, 0, 0, 800, 600);
    p[1] = out_plain (8, "DP-1", 0, 0, 800, 600);
    wl_out_layout (p, 2, TRUE, r);
    CHECK (r[0].src == &p[0] && r[1].src == &p[1]);

    /* left of the origin */
    p[0] = out_plain (1, "A", 0, 0, 1920, 1080);
    p[1] = out_plain (2, "B", -1920, 0, 1920, 1080);
    wl_out_layout (p, 2, TRUE, r);
    CHECK (r[0].src == &p[1] && r[0].x == -1920 && r[1].x == 0);

    /* 1.25 right of a scale-1 monitor: its origin in its own pixels,
     * 1920 units x 1.25 = 2400; the two do not overlap, the gap is fine */
    p[0] = out_plain (1, "DP-1", 0, 0, 1920, 1080);
    p[1] = out_scaled (2, "DP-2", 1920, 0, 2048, 1152, 2560, 1440, 2);
    wl_out_layout (p, 2, TRUE, r);
    CHECK (r[1].scale120 == 150 && r[1].lw == 2048 && r[1].width == 2560);
    CHECK (r[0].x == 0 && r[1].x == 2400 && r[1].y == 0);
    CHECK (!wl_rects_overlap (&r[0], &r[1]));
    /* the same pair without wp_viewporter: buffers are logical x 2 */
    wl_out_layout (p, 2, FALSE, r);
    CHECK (r[1].width == 4096 && r[1].height == 2304 && r[1].scale120 == 240);
    CHECK (r[1].x == 3840);

    /* a scale-2 panel LEFT of a scale-1 monitor: 0..3840 and 1920.. would
     * overlap, so: one row in list order */
    p[0] = out_scaled (1, "eDP-1", 0, 0, 1920, 1080, 3840, 2160, 2);
    p[1] = out_plain (2, "HDMI-A-1", 1920, 0, 1920, 1080);
    wl_out_layout (p, 2, TRUE, r);
    CHECK (r[0].src == &p[0] && r[0].x == 0 && r[0].width == 3840);
    CHECK (r[1].x == 3840 && r[1].y == 0);
    CHECK (!wl_rects_overlap (&r[0], &r[1]));
    /* the fallback moves every output, also one that sat below */
    p[2] = out_plain (3, "DP-3", 0, 1080, 1920, 1080);
    wl_out_layout (p, 3, TRUE, r);
    for (int i = 0; i < 3; i++)
        for (int k = i + 1; k < 3; k++)
            CHECK (!wl_rects_overlap (&r[i], &r[k]));
    CHECK (r[0].y == 0 && r[1].y == 0 && r[2].y == 0);

    /* 1.5 with a logical origin that is not a whole pixel count at 1:1 */
    p[0] = out_scaled (1, "A", 0, 0, 1280, 720, 1920, 1080, 2);
    p[1] = out_scaled (2, "B", 1280, 0, 1280, 720, 1920, 1080, 2);
    wl_out_layout (p, 2, TRUE, r);
    CHECK (r[0].scale120 == 180 && r[1].x == 1920 && !wl_rects_overlap (&r[0], &r[1]));

    /* two reads of the same properties give the same list */
    WlOutRect again[4];
    wl_out_layout (p, 2, TRUE, again);
    for (int i = 0; i < 2; i++)
        CHECK (r[i].src == again[i].src && r[i].x == again[i].x && r[i].y == again[i].y &&
               r[i].width == again[i].width && r[i].height == again[i].height &&
               r[i].scale120 == again[i].scale120 && r[i].refresh_hz == again[i].refresh_hz);
}

static void test_surface_sizes (void)
{
    WlSizeIn in;
    WlSizeOut o;

    /* a wallpaper at 1.25 with a viewport: the output's pixels in the
     * buffer, the configured size as the destination */
    memset (&in, 0, sizeof in);
    in.viewporter = TRUE;
    in.out_w = 2560;
    in.out_h = 1440;
    in.conf_w = 2048;
    in.conf_h = 1152;
    in.int_scale = 2;
    wl_surface_sizes (&in, &o);
    CHECK (o.buf_w == 2560 && o.buf_h == 1440 && o.buffer_scale == 1 && o.viewport);
    CHECK (o.dst_w == 2048 && o.dst_h == 1152);

    /* a render size set from outside: smaller buffers, the same destination */
    in.user_w = 1280;
    in.user_h = 720;
    wl_surface_sizes (&in, &o);
    CHECK (o.buf_w == 1280 && o.buf_h == 720 && o.dst_w == 2048 && o.dst_h == 1152 && o.viewport);
    /* only one of the two given: not a size */
    in.user_h = 0;
    wl_surface_sizes (&in, &o);
    CHECK (o.buf_w == 2560 && o.buf_h == 1440);
    in.user_w = 0;

    /* a later configure (the output's scale changed before the daemon
     * restarts): the buffer stays, the destination follows */
    in.conf_w = 1707;
    in.conf_h = 960;
    wl_surface_sizes (&in, &o);
    CHECK (o.buf_w == 2560 && o.buf_h == 1440 && o.dst_w == 1707 && o.dst_h == 960);

    /* the compositor leaves the size to the client (0 x 0): the output in
     * layout units, by the fractional scale when told, else the integer one */
    in.conf_w = in.conf_h = 0;
    in.scale120 = 150;
    wl_surface_sizes (&in, &o);
    CHECK (o.dst_w == 2048 && o.dst_h == 1152);
    in.scale120 = 0;
    wl_surface_sizes (&in, &o);
    CHECK (o.dst_w == 1280 && o.dst_h == 720);

    /* odd output at 1.25: 1366 x 768 in 1093 x 614 units */
    memset (&in, 0, sizeof in);
    in.viewporter = TRUE;
    in.out_w = 1366;
    in.out_h = 768;
    in.conf_w = 1093;
    in.conf_h = 614;
    in.int_scale = 2;
    wl_surface_sizes (&in, &o);
    CHECK (o.buf_w == 1366 && o.buf_h == 768 && o.dst_w == 1093 && o.dst_h == 614);

    /* no viewporter: an integer multiple of the configured size, whatever
     * it is - the protocol refuses a buffer that is not */
    in.viewporter = FALSE;
    wl_surface_sizes (&in, &o);
    CHECK (!o.viewport && o.buffer_scale == 2 && o.buf_w == 2186 && o.buf_h == 1228);
    CHECK (o.buf_w % o.buffer_scale == 0 && o.buf_h % o.buffer_scale == 0);
    in.user_w = 640;                        /* cannot be honoured without a viewport */
    in.user_h = 360;
    wl_surface_sizes (&in, &o);
    CHECK (o.buf_w == 2186 && o.buf_h == 1228);
    in.user_w = in.user_h = 0;
    in.int_scale = 1;
    in.out_w = 1920;
    in.out_h = 1080;
    in.conf_w = 1920;
    in.conf_h = 1080;
    wl_surface_sizes (&in, &o);
    CHECK (o.buffer_scale == 1 && o.buf_w == 1920 && o.buf_h == 1080);
    for (int sc = 1; sc <= 3; sc++)
        for (int lw = 799; lw <= 803; lw++) {
            in.int_scale = sc;
            in.conf_w = lw;
            in.conf_h = 601;
            wl_surface_sizes (&in, &o);
            CHECK (o.buf_w == lw * sc && o.buf_h == 601 * sc && o.buffer_scale == sc);
        }

    /* the plain window: the size asked for; a tiling compositor's own size
     * only changes how large it is shown */
    memset (&in, 0, sizeof in);
    in.viewporter = TRUE;
    in.asked_w = 960;
    in.asked_h = 540;
    in.int_scale = 1;
    wl_surface_sizes (&in, &o);
    CHECK (o.buf_w == 960 && o.buf_h == 540 && o.dst_w == 960 && o.dst_h == 540 && o.viewport);
    in.conf_w = 1200;
    in.conf_h = 700;
    wl_surface_sizes (&in, &o);
    CHECK (o.buf_w == 960 && o.buf_h == 540 && o.dst_w == 1200 && o.dst_h == 700);
    in.viewporter = FALSE;
    wl_surface_sizes (&in, &o);
    CHECK (o.buf_w == 960 && o.buf_h == 540 && o.buffer_scale == 1 && !o.viewport);

    /* the probe surface: 64 x 64, nothing else known */
    memset (&in, 0, sizeof in);
    in.asked_w = in.asked_h = 64;
    wl_surface_sizes (&in, &o);
    CHECK (o.buf_w == 64 && o.buf_h == 64 && o.buffer_scale == 1);

    /* limits: never 0, never beyond what a texture can be */
    memset (&in, 0, sizeof in);
    wl_surface_sizes (&in, &o);
    CHECK (o.buf_w == 1 && o.buf_h == 1 && o.dst_w == 1 && o.dst_h == 1);
    in.viewporter = TRUE;
    in.out_w = 1920;
    in.out_h = 1080;
    in.user_w = 100000;
    in.user_h = 50000;
    wl_surface_sizes (&in, &o);
    CHECK (o.buf_w == WL_BUFFER_MAX && o.buf_h == WL_BUFFER_MAX);
}

static void test_pointer (void)
{
    int x = -1, y = -1;
    /* scale 1: the surface position is the pixel */
    wl_pointer_to_output (100.5, 200.9, 1920, 1080, 0, 0, 1920, 1080, &x, &y);
    CHECK (x == 100 && y == 200);
    /* the second monitor: its rectangle's origin is added */
    wl_pointer_to_output (100.5, 200.9, 1920, 1080, 1920, 0, 1920, 1080, &x, &y);
    CHECK (x == 2020 && y == 200);
    /* 1.25: 2048 x 1152 units on 2560 x 1440 pixels at (2400, 0) */
    wl_pointer_to_output (1024.0, 576.0, 2048, 1152, 2400, 0, 2560, 1440, &x, &y);
    CHECK (x == 2400 + 1280 && y == 720);
    wl_pointer_to_output (2047.9, 1151.9, 2048, 1152, 2400, 0, 2560, 1440, &x, &y);
    CHECK (x == 2400 + 2559 && y == 1439);
    wl_pointer_to_output (0.0, 0.0, 2048, 1152, 2400, 0, 2560, 1440, &x, &y);
    CHECK (x == 2400 && y == 0);
    /* on the far edge and beyond (a drag that left the surface): clamped
     * into the rectangle */
    wl_pointer_to_output (2048.0, 1152.0, 2048, 1152, 2400, 0, 2560, 1440, &x, &y);
    CHECK (x == 2400 + 2559 && y == 1439);
    wl_pointer_to_output (-5.0, 3000.0, 2048, 1152, 2400, 0, 2560, 1440, &x, &y);
    CHECK (x == 2400 && y == 1439);
    /* odd sizes at 1.25: the last unit maps into the last pixels */
    wl_pointer_to_output (1092.99, 613.99, 1093, 614, 0, 0, 1366, 768, &x, &y);
    CHECK (x == 1365 && y == 767);
    /* the pointer mapping uses the output's rectangle, not the buffer: a
     * lower render size changes nothing here */
    wl_pointer_to_output (960.0, 540.0, 1920, 1080, 0, 0, 1920, 1080, &x, &y);
    CHECK (x == 960 && y == 540);
    /* a rotated output: units and pixels both upright */
    wl_pointer_to_output (540.0, 960.0, 1080, 1920, 0, 0, 1080, 1920, &x, &y);
    CHECK (x == 540 && y == 960);
    /* no logical size known yet: one to one, no division by zero */
    wl_pointer_to_output (10.0, 20.0, 0, 0, 5, 6, 100, 100, &x, &y);
    CHECK (x == 15 && y == 26);
    wl_pointer_to_output (10.0, 20.0, 100, 100, 0, 0, 0, 0, &x, &y);
    CHECK (x == 0 && y == 0);

    CHECK (wl_button_number (0x110) == 1);
    CHECK (wl_button_number (0x111) == 3);
    CHECK (wl_button_number (0x112) == 2);
    CHECK (wl_button_number (0x113) == 0 && wl_button_number (0) == 0);
}

static void test_axis (void)
{
    WlAxisAcc a;
    memset (&a, 0, sizeof a);
    /* a wheel: 120 per detent; down is positive */
    CHECK (wl_axis_clicks (&a, 0, 15.0, 0, 120, FALSE) == 1);
    CHECK (wl_axis_clicks (&a, 0, -30.0, 0, -240, FALSE) == -2);
    /* a high-resolution wheel: two half steps are one click */
    CHECK (wl_axis_clicks (&a, 0, 7.5, 0, 60, FALSE) == 0);
    CHECK (wl_axis_clicks (&a, 0, 7.5, 0, 60, FALSE) == 1);
    CHECK (wl_axis_clicks (&a, 0, -7.5, 0, -60, FALSE) == 0);
    CHECK (wl_axis_clicks (&a, 0, -7.5, 0, -60, FALSE) == -1);
    /* the older discrete count */
    CHECK (wl_axis_clicks (&a, 1, 45.0, 3, 0, FALSE) == 3);
    CHECK (wl_axis_clicks (&a, 1, -15.0, -1, 0, FALSE) == -1);

    /* a touchpad: distance only, 15 units a click, the rest is carried */
    memset (&a, 0, sizeof a);
    CHECK (wl_axis_clicks (&a, 0, 7.0, 0, 0, FALSE) == 0);
    CHECK (wl_axis_clicks (&a, 0, 7.0, 0, 0, FALSE) == 0);
    CHECK (wl_axis_clicks (&a, 0, 2.0, 0, 0, FALSE) == 1);
    CHECK (fabs (a.dist[0] - 1.0) < 1e-9);
    CHECK (wl_axis_clicks (&a, 0, 44.0, 0, 0, FALSE) == 3);
    /* the axes do not mix */
    CHECK (wl_axis_clicks (&a, 1, 14.0, 0, 0, FALSE) == 0);
    CHECK (wl_axis_clicks (&a, 0, 14.0, 0, 0, FALSE) == 0);
    CHECK (wl_axis_clicks (&a, 1, 1.0, 0, 0, FALSE) == 1);
    /* turning round starts from nothing */
    memset (&a, 0, sizeof a);
    CHECK (wl_axis_clicks (&a, 0, 14.0, 0, 0, FALSE) == 0);
    CHECK (wl_axis_clicks (&a, 0, -14.0, 0, 0, FALSE) == 0);
    CHECK (wl_axis_clicks (&a, 0, -2.0, 0, 0, FALSE) == -1);
    /* the fingers left: the remainder is forgotten */
    memset (&a, 0, sizeof a);
    CHECK (wl_axis_clicks (&a, 0, 10.0, 0, 0, TRUE) == 0);
    CHECK (a.dist[0] == 0.0);
    CHECK (wl_axis_clicks (&a, 0, 10.0, 0, 0, FALSE) == 0);
    CHECK (wl_axis_clicks (&a, 0, 0.0, 0, 0, TRUE) == 0 && a.dist[0] == 0.0);
    /* an axis that does not exist */
    CHECK (wl_axis_clicks (&a, 2, 100.0, 5, 600, FALSE) == 0);
    CHECK (wl_axis_clicks (&a, -1, 100.0, 5, 600, FALSE) == 0);

    CHECK (wl_axis_button (0, -1) == 4 && wl_axis_button (0, 3) == 5);
    CHECK (wl_axis_button (1, -2) == 6 && wl_axis_button (1, 1) == 7);
    CHECK (wl_axis_button (0, 0) == 0 && wl_axis_button (1, 0) == 0);
}

static void test_frames (void)
{
    /* four refresh periods, at least 100 ms, at most 250 ms */
    CHECK (wl_frame_limit_us (60.0) == 100000);
    CHECK (wl_frame_limit_us (144.0) == 100000);
    CHECK (wl_frame_limit_us (30.0) == 133333);
    CHECK (wl_frame_limit_us (20.0) == 200000);
    CHECK (wl_frame_limit_us (10.0) == 250000);
    CHECK (wl_frame_limit_us (0.0) == 100000);

    WlFrameState f;
    memset (&f, 0, sizeof f);
    /* shown, interval 1: one callback per frame, the swap waits for it */
    CHECK (wl_frame_pre_swap (&f, 1000));
    CHECK (f.cb_pending && f.cb_since_us == 1000);
    CHECK (wl_frame_wait_us (&f, 1, 60.0) == 100000);
    CHECK (!wl_frame_done (&f));            /* it was not hidden */
    CHECK (!f.cb_pending && f.callbacks == 1);
    CHECK (wl_frame_wait_us (&f, 1, 60.0) == 0);      /* nothing outstanding */
    CHECK (!wl_frame_after_swap (&f, FALSE, 2000, 60.0));
    CHECK (wl_frame_pre_swap (&f, 17000));
    /* a second swap before the callback: no second request */
    CHECK (!wl_frame_pre_swap (&f, 18000));
    CHECK (f.cb_since_us == 17000 && f.commits == 3);
    wl_frame_done (&f);

    /* the callback stays away: the wait ends, the surface is hidden, and
     * later swaps do not wait again */
    memset (&f, 0, sizeof f);
    CHECK (wl_frame_pre_swap (&f, 0));
    CHECK (wl_frame_wait_us (&f, 1, 60.0) == 100000);
    CHECK (wl_frame_after_swap (&f, TRUE, 100000, 60.0));
    CHECK (f.hidden && f.timeouts == 1);
    CHECK (!wl_frame_pre_swap (&f, 116000));          /* the old callback is still out */
    CHECK (wl_frame_wait_us (&f, 1, 60.0) == 0);
    CHECK (!wl_frame_after_swap (&f, FALSE, 500000, 60.0));   /* said once */
    CHECK (f.timeouts == 1);
    /* it arrives: shown again, and the next frame is paced as before */
    CHECK (wl_frame_done (&f));
    CHECK (!f.hidden && !f.cb_pending);
    CHECK (wl_frame_pre_swap (&f, 600000));
    CHECK (wl_frame_wait_us (&f, 1, 60.0) == 100000);

    /* interval 0: never a wait; the age of the outstanding callback tells */
    memset (&f, 0, sizeof f);
    CHECK (wl_frame_pre_swap (&f, 0));
    CHECK (wl_frame_wait_us (&f, 0, 60.0) == 0);
    CHECK (!wl_frame_after_swap (&f, FALSE, 50000, 60.0));
    CHECK (!wl_frame_pre_swap (&f, 60000));
    CHECK (!wl_frame_after_swap (&f, FALSE, 100000, 60.0));   /* exactly the limit: not yet */
    CHECK (wl_frame_after_swap (&f, FALSE, 100001, 60.0));
    CHECK (f.hidden && f.timeouts == 0);
    CHECK (wl_frame_done (&f) && !f.hidden);
    /* a slow frame clock (one frame a second) with a compositor that
     * answers in time is not "hidden": the callback of frame 1 has
     * arrived long before frame 2 looks */
    memset (&f, 0, sizeof f);
    CHECK (wl_frame_pre_swap (&f, 0));
    CHECK (!wl_frame_after_swap (&f, FALSE, 100, 60.0));
    wl_frame_done (&f);
    CHECK (wl_frame_pre_swap (&f, 1000000));
    CHECK (!wl_frame_after_swap (&f, FALSE, 1000100, 60.0));

    /* a swap that failed after it was announced: the callback's age starts
     * again, so a late next frame does not read as "not shown" - and a
     * callback that then does stay away is still noticed */
    memset (&f, 0, sizeof f);
    CHECK (wl_frame_pre_swap (&f, 0));
    wl_frame_swap_missed (&f, 500000);
    CHECK (f.cb_pending && f.cb_since_us == 500000);
    CHECK (!wl_frame_pre_swap (&f, 500000));
    CHECK (!wl_frame_after_swap (&f, FALSE, 500100, 60.0));
    CHECK (wl_frame_after_swap (&f, FALSE, 700000, 60.0));
    memset (&f, 0, sizeof f);
    wl_frame_swap_missed (&f, 5);           /* nothing outstanding: nothing to do */
    CHECK (!f.cb_pending && f.cb_since_us == 0);

    /* the role went away: nothing is outstanding, the count stays */
    memset (&f, 0, sizeof f);
    wl_frame_pre_swap (&f, 5);
    wl_frame_reset (&f);
    CHECK (!f.cb_pending && f.commits == 1);
    CHECK (wl_frame_wait_us (&f, 1, 60.0) == 0);

    CHECK (wl_presented_us (0, 5, 500000000) == 5500000);
    CHECK (wl_presented_us (1, 0, 0) == G_GINT64_CONSTANT (4294967296000000));
    CHECK (wl_presented_us (0, 0, 999) == 0);
    CHECK (fabs (wl_refresh_hz_from_ns (16666666) - 60.0) < 0.001);
    CHECK (fabs (wl_refresh_hz_from_ns (6944444) - 144.0) < 0.001);
    CHECK (wl_refresh_hz_from_ns (0) == 0.0);
}

/* NONE -> READY, as surface_new does it */
static void layer_ready (WlLayerState *s, int recreate_max)
{
    wl_ls_init (s, recreate_max);
    wl_ls_event (s, WL_LS_EV_CREATE, 0, 0, 0, 0);
    wl_ls_event (s, WL_LS_EV_CONFIGURE, 1, 1920, 1080, 0);
}

static void test_layer (void)
{
    WlLayerState s;
    const gint64 sec = G_USEC_PER_SEC;

    /* the normal start */
    wl_ls_init (&s, 3);
    CHECK (s.state == WL_LS_NONE && !wl_ls_can_present (&s) && s.output_alive);
    CHECK (!wl_ls_wants_create (&s, 0));
    CHECK (wl_ls_event (&s, WL_LS_EV_CONFIGURE, 9, 1, 1, 0) == 0 && s.state == WL_LS_NONE);
    CHECK (wl_ls_event (&s, WL_LS_EV_CREATE, 0, 0, 0, 0) == 0);
    CHECK (s.state == WL_LS_WAIT && !wl_ls_can_present (&s) && !s.ack_due);
    CHECK (wl_ls_event (&s, WL_LS_EV_CONFIGURE, 7, 1920, 1080, 10) == 0);
    CHECK (s.state == WL_LS_READY && wl_ls_can_present (&s));
    CHECK (s.ack_due && s.serial == 7 && s.conf_w == 1920 && s.conf_h == 1080 && s.was_ready);
    CHECK (s.recreated == 0);
    /* a second create changes nothing */
    CHECK (wl_ls_event (&s, WL_LS_EV_CREATE, 0, 0, 0, 20) == 0 && s.state == WL_LS_READY);

    /* further configures: the newest serial is the one to acknowledge */
    s.ack_due = FALSE;
    CHECK (wl_ls_event (&s, WL_LS_EV_CONFIGURE, 8, 1920, 1080, 30) == 0);
    CHECK (s.ack_due && s.serial == 8 && s.state == WL_LS_READY);
    CHECK (wl_ls_event (&s, WL_LS_EV_CONFIGURE, 9, 2560, 1440, 40) == WL_LS_ACT_SIZE_CHANGED);
    CHECK (s.serial == 9 && s.conf_w == 2560 && s.conf_h == 1440);
    /* an older configure delivered late (it sat on the other queue): out of date */
    CHECK (wl_ls_event (&s, WL_LS_EV_CONFIGURE, 8, 1920, 1080, 41) == 0);
    CHECK (s.serial == 9 && s.conf_w == 2560 && s.conf_h == 1440);
    /* the same serial again is taken (nothing changes), and serials wrap */
    CHECK (wl_ls_event (&s, WL_LS_EV_CONFIGURE, 9, 2560, 1440, 42) == 0 && s.serial == 9);
    s.serial = 0xfffffff0u;
    CHECK (wl_ls_event (&s, WL_LS_EV_CONFIGURE, 5, 2560, 1440, 43) == 0 && s.serial == 5);
    CHECK (wl_ls_event (&s, WL_LS_EV_CONFIGURE, 0xfffffff8u, 1, 1, 44) == 0 && s.serial == 5);
    s.serial = 9;

    /* closed while the output is there: the role goes, a frame is asked
     * for, and the next swap makes it again */
    CHECK (wl_ls_event (&s, WL_LS_EV_CLOSED, 0, 0, 0, sec) ==
           (WL_LS_ACT_DESTROY_ROLE | WL_LS_ACT_ASK_FRAME));
    CHECK (s.state == WL_LS_LOST && !wl_ls_can_present (&s) && !s.ack_due);
    /* not in the first moment: the output may be on its way out */
    CHECK (!wl_ls_wants_create (&s, sec));
    CHECK (!wl_ls_wants_create (&s, sec + WL_LS_RECREATE_GRACE_US - 1));
    CHECK (wl_ls_wants_create (&s, sec + WL_LS_RECREATE_GRACE_US));
    /* events that were on their way to the destroyed role are ignored */
    CHECK (wl_ls_event (&s, WL_LS_EV_CONFIGURE, 10, 5, 5, sec) == 0 && s.state == WL_LS_LOST);
    CHECK (wl_ls_event (&s, WL_LS_EV_CLOSED, 0, 0, 0, sec) == 0 && s.state == WL_LS_LOST);
    CHECK (wl_ls_event (&s, WL_LS_EV_CREATE, 0, 0, 0, sec) == 0);
    CHECK (s.state == WL_LS_WAIT && s.attempts == 1 && s.recreating);
    CHECK (wl_ls_event (&s, WL_LS_EV_CONFIGURE, 11, 2560, 1440, sec + 1000) == WL_LS_ACT_RECREATED);
    CHECK (s.state == WL_LS_READY && s.recreated == 1 && s.ack_due && s.serial == 11);
    CHECK (!s.closed_reported);

    /* three re-creations within ten seconds, then it is final */
    layer_ready (&s, 3);
    for (int i = 0; i < 3; i++) {
        gint64 t = (1 + i) * sec;
        CHECK (wl_ls_event (&s, WL_LS_EV_CLOSED, 0, 0, 0, t) ==
               (WL_LS_ACT_DESTROY_ROLE | WL_LS_ACT_ASK_FRAME));
        wl_ls_event (&s, WL_LS_EV_CREATE, 0, 0, 0, t);
        CHECK (wl_ls_event (&s, WL_LS_EV_CONFIGURE, 2, 1920, 1080, t) == WL_LS_ACT_RECREATED);
    }
    CHECK (s.attempts == 3 && s.recreated == 3);
    CHECK (wl_ls_event (&s, WL_LS_EV_CLOSED, 0, 0, 0, 4 * sec) ==
           (WL_LS_ACT_DESTROY_ROLE | WL_LS_ACT_REPORT_CLOSED));
    CHECK (s.state == WL_LS_DEAD && s.closed_reported && !wl_ls_wants_create (&s, 4 * sec));
    /* dead is dead */
    CHECK (wl_ls_event (&s, WL_LS_EV_CREATE, 0, 0, 0, 5 * sec) == 0 && s.state == WL_LS_DEAD);
    CHECK (wl_ls_event (&s, WL_LS_EV_OUTPUT_GONE, 0, 0, 0, 5 * sec) == 0);
    CHECK (wl_ls_event (&s, WL_LS_EV_SHELL_GONE, 0, 0, 0, 5 * sec) == 0);

    /* ... but a close long after the last ones starts a new count */
    layer_ready (&s, 3);
    for (int i = 0; i < 3; i++) {
        wl_ls_event (&s, WL_LS_EV_CLOSED, 0, 0, 0, sec);
        wl_ls_event (&s, WL_LS_EV_CREATE, 0, 0, 0, sec);
        wl_ls_event (&s, WL_LS_EV_CONFIGURE, 2, 1920, 1080, sec);
    }
    CHECK (wl_ls_event (&s, WL_LS_EV_CLOSED, 0, 0, 0, 12 * sec) ==
           (WL_LS_ACT_DESTROY_ROLE | WL_LS_ACT_ASK_FRAME));
    CHECK (s.state == WL_LS_LOST && wl_ls_wants_create (&s, 12 * sec + WL_LS_RECREATE_GRACE_US));
    wl_ls_event (&s, WL_LS_EV_CREATE, 0, 0, 0, 12 * sec);
    CHECK (s.attempts == 1 && s.window_start_us == 12 * sec);

    /* re-creation switched off (also: the plain window): closed is final */
    layer_ready (&s, 0);
    CHECK (wl_ls_event (&s, WL_LS_EV_CLOSED, 0, 0, 0, sec) ==
           (WL_LS_ACT_DESTROY_ROLE | WL_LS_ACT_REPORT_CLOSED));
    CHECK (s.state == WL_LS_DEAD);

    /* the output is unplugged: final at once, whatever the budget */
    layer_ready (&s, 3);
    CHECK (wl_ls_event (&s, WL_LS_EV_OUTPUT_GONE, 0, 0, 0, sec) ==
           (WL_LS_ACT_DESTROY_ROLE | WL_LS_ACT_REPORT_CLOSED));
    CHECK (s.state == WL_LS_DEAD && !s.output_alive);
    /* the compositor's own closed for it comes after: nothing more */
    CHECK (wl_ls_event (&s, WL_LS_EV_CLOSED, 0, 0, 0, sec) == 0);
    /* closed first, then the output goes: reported once, no role left to destroy */
    layer_ready (&s, 3);
    wl_ls_event (&s, WL_LS_EV_CLOSED, 0, 0, 0, sec);
    CHECK (s.state == WL_LS_LOST);
    CHECK (wl_ls_event (&s, WL_LS_EV_OUTPUT_GONE, 0, 0, 0, sec) == WL_LS_ACT_REPORT_CLOSED);
    CHECK (s.state == WL_LS_DEAD && !wl_ls_wants_create (&s, sec));
    /* the output goes while its surface was closed and the budget is not
     * the reason: no re-creation is tried on a dead output */
    layer_ready (&s, 3);
    s.output_alive = FALSE;
    CHECK (wl_ls_event (&s, WL_LS_EV_CLOSED, 0, 0, 0, sec) ==
           (WL_LS_ACT_DESTROY_ROLE | WL_LS_ACT_REPORT_CLOSED));

    /* the layer shell itself goes away */
    layer_ready (&s, 3);
    CHECK (wl_ls_event (&s, WL_LS_EV_SHELL_GONE, 0, 0, 0, sec) ==
           (WL_LS_ACT_DESTROY_ROLE | WL_LS_ACT_REPORT_CLOSED));

    /* no configure after the very first create: the role is destroyed (a
     * buffer must never reach an unconfigured surface); nothing is
     * reported, because the surface was never handed out */
    wl_ls_init (&s, 3);
    wl_ls_event (&s, WL_LS_EV_CREATE, 0, 0, 0, 0);
    CHECK (wl_ls_event (&s, WL_LS_EV_TIMEOUT, 0, 0, 0, 3 * sec) == WL_LS_ACT_DESTROY_ROLE);
    CHECK (s.state == WL_LS_DEAD && !s.closed_reported);
    /* the same when the compositor closes it before configuring */
    wl_ls_init (&s, 3);
    wl_ls_event (&s, WL_LS_EV_CREATE, 0, 0, 0, 0);
    CHECK (wl_ls_event (&s, WL_LS_EV_CLOSED, 0, 0, 0, 1) == WL_LS_ACT_DESTROY_ROLE);
    CHECK (s.state == WL_LS_DEAD);
    /* a timeout that comes when the configure has arrived: nothing */
    layer_ready (&s, 3);
    CHECK (wl_ls_event (&s, WL_LS_EV_TIMEOUT, 0, 0, 0, sec) == 0 && s.state == WL_LS_READY);

    /* no configure after a re-creation: tried again while the budget lasts */
    layer_ready (&s, 3);
    wl_ls_event (&s, WL_LS_EV_CLOSED, 0, 0, 0, sec);
    wl_ls_event (&s, WL_LS_EV_CREATE, 0, 0, 0, sec);
    CHECK (wl_ls_event (&s, WL_LS_EV_TIMEOUT, 0, 0, 0, sec + 200000) ==
           (WL_LS_ACT_DESTROY_ROLE | WL_LS_ACT_ASK_FRAME));
    CHECK (s.state == WL_LS_LOST && !s.recreating && !wl_ls_wants_create (&s, sec + 200000));
    CHECK (wl_ls_wants_create (&s, sec + 200000 + WL_LS_RECREATE_GRACE_US));
    wl_ls_event (&s, WL_LS_EV_CREATE, 0, 0, 0, sec + 300000);
    CHECK (s.attempts == 2);
    wl_ls_event (&s, WL_LS_EV_TIMEOUT, 0, 0, 0, sec + 500000);
    wl_ls_event (&s, WL_LS_EV_CREATE, 0, 0, 0, sec + 600000);
    CHECK (s.attempts == 3);
    CHECK (wl_ls_event (&s, WL_LS_EV_TIMEOUT, 0, 0, 0, sec + 800000) ==
           (WL_LS_ACT_DESTROY_ROLE | WL_LS_ACT_REPORT_CLOSED));
    CHECK (s.state == WL_LS_DEAD);

    /* lost and nobody draws on it: given up, reported, no role to destroy */
    layer_ready (&s, 3);
    CHECK (wl_ls_event (&s, WL_LS_EV_GIVE_UP, 0, 0, 0, sec) == 0);   /* not lost: nothing */
    wl_ls_event (&s, WL_LS_EV_CLOSED, 0, 0, 0, sec);
    CHECK (wl_ls_event (&s, WL_LS_EV_GIVE_UP, 0, 0, 0, 6 * sec) == WL_LS_ACT_REPORT_CLOSED);
    CHECK (s.state == WL_LS_DEAD && s.closed_reported);
    CHECK (wl_ls_event (&s, WL_LS_EV_GIVE_UP, 0, 0, 0, 7 * sec) == 0);

    CHECK (!strcmp (wl_ls_state_name (WL_LS_NONE), "none"));
    CHECK (!strcmp (wl_ls_state_name (WL_LS_READY), "ready"));
    CHECK (!strcmp (wl_ls_state_name (WL_LS_DEAD), "dead"));
    CHECK (!strcmp (wl_ls_state_name ((WlLsState) 99), "?"));
}

/* numbers no real compositor sends: cut to the bounds before anything is
 * computed from them - no overflow, no size of 0 for an output that is
 * listed, positions inside the rectangle */
static void test_limits (void)
{
    int w, h, lx, ly, lw, lh;
    CHECK (wl_dim (-1) == 0 && wl_dim (0) == 0 && wl_dim (1920) == 1920);
    CHECK (wl_dim (G_MAXINT) == WL_DIM_MAX && wl_dim (G_MININT) == 0);
    CHECK (wl_pos (-1920) == -1920 && wl_pos (G_MAXINT) == WL_POS_MAX &&
           wl_pos (G_MININT) == -WL_POS_MAX);

    /* a mode, a position and a scale at the end of the number range */
    WlOutProps p = out_plain (1, "X", G_MAXINT, G_MININT, G_MAXINT, G_MAXINT);
    p.int_scale = G_MAXINT;                 /* read as 16 */
    CHECK (wl_out_usable (&p));
    wl_out_logical (&p, &lx, &ly, &lw, &lh);
    CHECK (lx == WL_POS_MAX && ly == -WL_POS_MAX);
    CHECK (lw == (WL_DIM_MAX + 8) / 16 && lh == lw);
    wl_out_pixels (&p, TRUE, &w, &h);
    CHECK (w == WL_DIM_MAX && h == WL_DIM_MAX);
    wl_out_pixels (&p, FALSE, &w, &h);       /* 4096 x 16 is one more than the bound */
    CHECK (w == WL_DIM_MAX && h == WL_DIM_MAX);
    /* the same from xdg-output */
    p = out_scaled (2, "Y", G_MININT, G_MAXINT, G_MAXINT, G_MAXINT, 1920, 1080, 16);
    wl_out_logical (&p, &lx, &ly, &lw, &lh);
    CHECK (lx == -WL_POS_MAX && ly == WL_POS_MAX && lw == WL_DIM_MAX && lh == WL_DIM_MAX);
    wl_out_pixels (&p, FALSE, &w, &h);
    CHECK (w == WL_DIM_MAX && h == WL_DIM_MAX);
    /* a negative mode is no mode */
    p = out_plain (3, "N", 0, 0, -1920, -1080);
    CHECK (!wl_out_usable (&p));
    wl_out_mode_transformed (&p, &w, &h);
    CHECK (w == 0 && h == 0);
    /* a mode smaller than half the scale: one unit, and pixels to render */
    p = out_plain (4, "T", 0, 0, 7, 5);
    p.int_scale = 16;
    wl_out_logical (&p, &lx, &ly, &lw, &lh);
    CHECK (lw == 1 && lh == 1);
    wl_out_pixels (&p, FALSE, &w, &h);
    CHECK (w == 16 && h == 16);
    wl_out_pixels (&p, TRUE, &w, &h);
    CHECK (w == 7 && h == 5);

    /* the layout of such outputs stays inside what an int holds */
    WlOutProps q[2];
    WlOutRect r[2];
    q[0] = out_plain (1, "A", G_MAXINT, G_MININT, G_MAXINT, G_MAXINT);
    q[0].int_scale = G_MAXINT;
    q[1] = out_scaled (2, "B", G_MININT, G_MAXINT, G_MAXINT, G_MAXINT, G_MAXINT, G_MAXINT, 16);
    for (int vp = 0; vp < 2; vp++) {
        wl_out_layout (q, 2, vp, r);
        CHECK (r[0].src == &q[1] && r[1].src == &q[0]);     /* by position: B is far left */
        for (int i = 0; i < 2; i++) {
            /* the ranges the platform layer keeps an output list to */
            CHECK (r[i].width >= 1 && r[i].width <= WL_DIM_MAX);
            CHECK (r[i].height >= 1 && r[i].height <= WL_DIM_MAX);
            CHECK (ABS (r[i].x) <= WL_POS_MAX && ABS (r[i].y) <= WL_POS_MAX);
            CHECK (r[i].lw <= WL_DIM_MAX && r[i].lh <= WL_DIM_MAX);
            CHECK (ABS (r[i].lx) <= WL_POS_MAX && ABS (r[i].ly) <= WL_POS_MAX);
            CHECK (r[i].scale120 >= WL_SCALE_DEN / 4 && r[i].scale120 <= WL_SCALE_DEN * 16);
        }
        CHECK (!wl_rects_overlap (&r[0], &r[1]));
    }

    /* a transform that is none of the eight: not turned; a refresh rate
     * that is none: unknown */
    CHECK (wl_transform_sane (0) == 0 && wl_transform_sane (7) == 7 && wl_transform_sane (3) == 3);
    CHECK (wl_transform_sane (8) == 0 && wl_transform_sane (-1) == 0 &&
           wl_transform_sane (G_MAXINT) == 0);
    p = out_plain (5, "R", 0, 0, 1920, 1080);
    p.transform = 9;                        /* odd, but not a quarter turn */
    wl_out_mode_transformed (&p, &w, &h);
    CHECK (w == 1920 && h == 1080);
    CHECK (wl_refresh_hz_from_mhz (59951) > 59.95 && wl_refresh_hz_from_mhz (59951) < 59.96);
    CHECK (wl_refresh_hz_from_mhz (1000000) == 1000.0);
    CHECK (wl_refresh_hz_from_mhz (0) == 0.0 && wl_refresh_hz_from_mhz (-60000) == 0.0 &&
           wl_refresh_hz_from_mhz (1000001) == 0.0 && wl_refresh_hz_from_mhz (G_MAXINT) == 0.0);
    q[0] = out_plain (1, "A", 0, 0, 1920, 1080);
    q[0].refresh_mhz = G_MAXINT;
    wl_out_layout (q, 1, TRUE, r);
    CHECK (r[0].refresh_hz == 0.0);

    /* a configure of any size: the buffer stays within its limit */
    WlSizeIn in;
    WlSizeOut o;
    memset (&in, 0, sizeof in);
    in.out_w = 1920;
    in.out_h = 1080;
    in.conf_w = G_MAXINT;
    in.conf_h = G_MAXINT;
    in.int_scale = G_MAXINT;
    wl_surface_sizes (&in, &o);
    CHECK (o.dst_w == WL_DIM_MAX && o.dst_h == WL_DIM_MAX && o.buffer_scale == 16);
    CHECK (o.buf_w == WL_BUFFER_MAX && o.buf_h == WL_BUFFER_MAX);
    in.viewporter = TRUE;
    wl_surface_sizes (&in, &o);
    CHECK (o.buf_w == 1920 && o.buf_h == 1080 && o.dst_w == WL_DIM_MAX);
    /* a configure the compositor sent as a number above 2^31 reads as negative: "your choice" */
    in.conf_w = in.conf_h = -1;
    in.int_scale = 1;
    wl_surface_sizes (&in, &o);
    CHECK (o.dst_w == 1920 && o.dst_h == 1080);

    /* a pointer position at the ends of wl_fixed on a logical size of 1 */
    int x = -1, y = -1;
    wl_pointer_to_output (8388607.0, -8388608.0, 1, 1, 10, 20, WL_DIM_MAX * 16, WL_DIM_MAX * 16, &x, &y);
    CHECK (x == 10 + WL_DIM_MAX * 16 - 1 && y == 20);

    /* wheel numbers */
    WlAxisAcc a;
    memset (&a, 0, sizeof a);
    CHECK (wl_axis_clicks (&a, 0, 0.0, G_MAXINT, 0, FALSE) == WL_AXIS_STEPS_MAX);
    CHECK (wl_axis_clicks (&a, 0, 0.0, G_MININT, 0, FALSE) == -WL_AXIS_STEPS_MAX);
    CHECK (wl_axis_clicks (&a, 1, 0.0, 0, G_MAXINT, FALSE) == WL_AXIS_STEPS_MAX);
    CHECK (wl_axis_clicks (&a, 1, 0.0, 0, G_MININT, FALSE) == -WL_AXIS_STEPS_MAX);
    CHECK (a.v120[1] == 0);
    CHECK (wl_axis_clicks (&a, 0, 1e300, 0, 0, FALSE) == WL_AXIS_STEPS_MAX);
    CHECK (wl_axis_clicks (&a, 0, -1e300, 0, 0, FALSE) == -WL_AXIS_STEPS_MAX);

    /* a presentation time stamp that is no time: bounded, still positive */
    CHECK (wl_presented_us (0xffffffffu, 0xffffffffu, 999999999) > 0);
    CHECK (wl_presented_us (0xffffffffu, 0xffffffffu, 999999999) <
           G_MAXINT64 / 4);
}

/* which layer a wallpaper surface goes on: "background", except on KWin */
static void test_layer_choice (void)
{
    WlLayerWhy why = WL_LAYER_WHY_ASKED;
    /* the numbers are the protocol's (zwlr_layer_shell_v1.layer) */
    CHECK (WL_LAYER_BACKGROUND == 0 && WL_LAYER_BOTTOM == 1);

    /* nothing asked for */
    CHECK (wl_layer_choose (NULL, FALSE, &why) == WL_LAYER_BACKGROUND && why == WL_LAYER_WHY_DEFAULT);
    CHECK (wl_layer_choose ("", FALSE, &why) == WL_LAYER_BACKGROUND && why == WL_LAYER_WHY_DEFAULT);
    CHECK (wl_layer_choose ("auto", FALSE, &why) == WL_LAYER_BACKGROUND && why == WL_LAYER_WHY_DEFAULT);
    CHECK (wl_layer_choose (NULL, TRUE, &why) == WL_LAYER_BOTTOM && why == WL_LAYER_WHY_KWIN);
    CHECK (wl_layer_choose ("", TRUE, &why) == WL_LAYER_BOTTOM && why == WL_LAYER_WHY_KWIN);
    CHECK (wl_layer_choose ("Auto", TRUE, &why) == WL_LAYER_BOTTOM && why == WL_LAYER_WHY_KWIN);

    /* a layer by name beats the compositor */
    CHECK (wl_layer_choose ("background", TRUE, &why) == WL_LAYER_BACKGROUND && why == WL_LAYER_WHY_ASKED);
    CHECK (wl_layer_choose ("bottom", FALSE, &why) == WL_LAYER_BOTTOM && why == WL_LAYER_WHY_ASKED);
    CHECK (wl_layer_choose ("BOTTOM", TRUE, &why) == WL_LAYER_BOTTOM && why == WL_LAYER_WHY_ASKED);
    CHECK (wl_layer_choose ("bottom", TRUE, NULL) == WL_LAYER_BOTTOM);

    /* no value this program knows: as if nothing was asked for. "top" and
     * "overlay" are layers of the protocol and none a wallpaper goes on */
    static const char *const bad[] = { "top", "overlay", "0", "1", "bottom ", " background", "bg" };
    for (guint i = 0; i < G_N_ELEMENTS (bad); i++) {
        WlLayer l = WL_LAYER_BOTTOM;
        CHECK (wl_layer_parse (bad[i], &l) == -1 && l == WL_LAYER_BOTTOM);
        CHECK (wl_layer_choose (bad[i], FALSE, &why) == WL_LAYER_BACKGROUND && why == WL_LAYER_WHY_DEFAULT);
        CHECK (wl_layer_choose (bad[i], TRUE, &why) == WL_LAYER_BOTTOM && why == WL_LAYER_WHY_KWIN);
    }
    WlLayer l = WL_LAYER_BOTTOM;
    CHECK (wl_layer_parse (NULL, &l) == 0 && wl_layer_parse ("", &l) == 0 &&
           wl_layer_parse ("auto", &l) == 0 && l == WL_LAYER_BOTTOM);
    CHECK (wl_layer_parse ("background", &l) == 1 && l == WL_LAYER_BACKGROUND);
    CHECK (wl_layer_parse ("bottom", &l) == 1 && l == WL_LAYER_BOTTOM);

    /* KWin is told by globals only it has; what other compositors and KWin
     * share (the layer shell itself, KDE's decoration protocol, which
     * wlroots compositors offer too) says nothing */
    CHECK (wl_global_is_kwin ("org_kde_plasma_shell"));
    CHECK (wl_global_is_kwin ("org_kde_plasma_virtual_desktop_management"));
    CHECK (!wl_global_is_kwin ("org_kde_kwin_server_decoration_manager"));
    CHECK (!wl_global_is_kwin ("zwlr_layer_shell_v1"));
    CHECK (!wl_global_is_kwin ("org_kde_plasma_shell_v2"));
    CHECK (!wl_global_is_kwin ("") && !wl_global_is_kwin (NULL));

    CHECK (!strcmp (wl_layer_name (WL_LAYER_BACKGROUND), "background"));
    CHECK (!strcmp (wl_layer_name (WL_LAYER_BOTTOM), "bottom"));
}

/* The add-on for KDE Plasma: what the files say, and the one line for
 * the user. The daemon's layer does not depend on any of it. */
static void test_plasma_addon (void)
{
    static const char applets_sel[] = "[Containments][1]\nactivityId=a\nlastScreen=0\n"
                                      "plugin=org.kde.plasma.folder\n"
                                      "wallpaperplugin=io.github.walpe.plasma\r\n\n"
                                      "[Containments][1][Wallpaper][io.github.walpe.plasma][General]\n"
                                      "Under=true\n";
    /* the type was selected once: its settings group is still there */
    static const char applets_not[] = "[Containments][1]\nwallpaperplugin=org.kde.image\n\n"
                                      "[Containments][1][Wallpaper][io.github.walpe.plasma][General]\n"
                                      "Previous=org.kde.image\n";
    /* the key as a prefix of another key, with another value, after text */
    static const char applets_else[] = "[Containments][1]\nwallpaperplugin=io.github.walpe.plasma2\n"
                                       "wallpaperpluginX=io.github.walpe.plasma\n"
                                       "xwallpaperplugin=io.github.walpe.plasma\n";

    CHECK (wl_kconfig_has (applets_sel, NULL, "wallpaperplugin", "io.github.walpe.plasma"));
    CHECK (wl_kconfig_has (applets_sel, "Containments][1", "wallpaperplugin", "io.github.walpe.plasma"));
    CHECK (!wl_kconfig_has (applets_sel, "Containments][2", "wallpaperplugin", "io.github.walpe.plasma"));
    CHECK (!wl_kconfig_has (applets_sel, "Containments", "wallpaperplugin", "io.github.walpe.plasma"));
    CHECK (!wl_kconfig_has (applets_else, NULL, "wallpaperplugin", "io.github.walpe.plasma"));
    CHECK (!wl_kconfig_has (NULL, "Plugins", "a", "b") && !wl_kconfig_has ("", NULL, "a", "b"));
    CHECK (wl_kconfig_has ("a=b", NULL, "a", "b") && wl_kconfig_has ("[g]\na=b  \n", "g", "a", "b"));
    CHECK (!wl_kconfig_has ("a=b\n[g]\n", "g", "a", "b") && !wl_kconfig_has ("[h]\na=b\n[g]\n", "g", "a", "b"));

    CHECK (wl_plasma_addon_state (FALSE, applets_sel) == WL_PLASMA_ADDON_NONE);
    CHECK (wl_plasma_addon_state (FALSE, NULL) == WL_PLASMA_ADDON_NONE);
    CHECK (wl_plasma_addon_state (TRUE, NULL) == WL_PLASMA_ADDON_UNSELECTED);
    CHECK (wl_plasma_addon_state (TRUE, applets_not) == WL_PLASMA_ADDON_UNSELECTED);
    CHECK (wl_plasma_addon_state (TRUE, applets_else) == WL_PLASMA_ADDON_UNSELECTED);
    CHECK (wl_plasma_addon_state (TRUE, applets_sel) == WL_PLASMA_ADDON_SELECTED);

    /* a line for the user on KWin unless the add-on is in use; never elsewhere */
    for (int a = WL_PLASMA_ADDON_NONE; a <= WL_PLASMA_ADDON_SELECTED; a++) {
        const char *hint = wl_plasma_hint (TRUE, (WlPlasmaAddon) a);
        CHECK (!wl_plasma_hint (FALSE, (WlPlasmaAddon) a));
        CHECK ((hint == NULL) == (a == WL_PLASMA_ADDON_SELECTED));
        CHECK (!hint || (strstr (hint, "hidden") && strstr (hint, "contrib/plasma/install.sh") &&
                         strstr (hint, "docs/WAYLAND.md")));
        CHECK (*wl_plasma_addon_name ((WlPlasmaAddon) a));
    }
    CHECK (strstr (wl_plasma_hint (TRUE, WL_PLASMA_ADDON_NONE), "install.sh install"));
    CHECK (strstr (wl_plasma_hint (TRUE, WL_PLASMA_ADDON_UNSELECTED), "install.sh select"));
    /* an installed WalPE has no contrib/plasma: both lines name Plasma's own
     * dialog, which is there on every system, and say where install.sh is;
     * the one for "not installed" names the package as a source too */
    CHECK (strstr (wl_plasma_hint (TRUE, WL_PLASMA_ADDON_NONE), "\"Desktop and Wallpaper\""));
    CHECK (strstr (wl_plasma_hint (TRUE, WL_PLASMA_ADDON_UNSELECTED), "\"Desktop and Wallpaper\""));
    CHECK (strstr (wl_plasma_hint (TRUE, WL_PLASMA_ADDON_NONE), "source tree") &&
           strstr (wl_plasma_hint (TRUE, WL_PLASMA_ADDON_NONE), "package"));
    CHECK (strstr (wl_plasma_hint (TRUE, WL_PLASMA_ADDON_UNSELECTED), "source tree"));
    /* the id is the package's (contrib/plasma/wallpaper/metadata.json) */
    CHECK (!strcmp (WL_PLASMA_TYPE_ID, "io.github.walpe.plasma"));
}

int main (void)
{
    test_scale ();
    test_text ();
    test_output_sizes ();
    test_output_layout ();
    test_surface_sizes ();
    test_pointer ();
    test_axis ();
    test_frames ();
    test_layer ();
    test_layer_choice ();
    test_plasma_addon ();
    test_limits ();
    if (fails) {
        g_printerr ("wayland-logic-test: %d check(s) failed\n", fails);
        return 1;
    }
    g_print ("wayland-logic-test: all checks passed\n");
    return 0;
}
