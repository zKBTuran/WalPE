/* icon.c - the WalPE tray icon, drawn with cairo at any size
 *
 * Same picture as data/icons/walpe.svg (64 x 64 design grid): a rounded
 * blue-violet tile with a sun and two hills ("a wallpaper"). State shows
 * as a badge in the lower right corner, like WE's tray icon:
 *   paused      whole icon desaturated and dimmed + pause badge
 *   autopaused  (fullscreen app / covered) small amber pause badge
 *   dead        grey + red badge (engine not running)
 */
#include "tray.h"
#include <math.h>

static void rounded_rect (cairo_t *cr, double x, double y, double w, double h, double r)
{
    cairo_new_sub_path (cr);
    cairo_arc (cr, x + w - r, y + r, r, -G_PI / 2, 0);
    cairo_arc (cr, x + w - r, y + h - r, r, 0, G_PI / 2);
    cairo_arc (cr, x + r, y + h - r, r, G_PI / 2, G_PI);
    cairo_arc (cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path (cr);
}

/* the picture on the 64-unit grid */
static void draw_tile (cairo_t *cr)
{
    cairo_pattern_t *g = cairo_pattern_create_linear (4, 4, 60, 60);
    cairo_pattern_add_color_stop_rgb (g, 0.0, 0x2f / 255.0, 0x80 / 255.0, 0xed / 255.0);
    cairo_pattern_add_color_stop_rgb (g, 1.0, 0x8e / 255.0, 0x3b / 255.0, 0xc9 / 255.0);
    rounded_rect (cr, 3, 3, 58, 58, 13);
    cairo_set_source (cr, g);
    cairo_fill_preserve (cr);
    cairo_pattern_destroy (g);
    cairo_set_source_rgba (cr, 1, 1, 1, 0.22);
    cairo_set_line_width (cr, 2);
    cairo_stroke (cr);

    cairo_save (cr);
    rounded_rect (cr, 3, 3, 58, 58, 13);
    cairo_clip (cr);
    /* sun */
    cairo_arc (cr, 44, 19, 7, 0, 2 * G_PI);
    cairo_set_source_rgb (cr, 1.0, 0.83, 0.42);
    cairo_fill (cr);
    /* back hill */
    cairo_move_to (cr, 0, 50);
    cairo_line_to (cr, 20, 26);
    cairo_line_to (cr, 31, 38);
    cairo_line_to (cr, 39, 31);
    cairo_line_to (cr, 64, 54);
    cairo_line_to (cr, 64, 64);
    cairo_line_to (cr, 0, 64);
    cairo_close_path (cr);
    cairo_set_source_rgba (cr, 1, 1, 1, 0.55);
    cairo_fill (cr);
    /* front wave */
    cairo_move_to (cr, 0, 50);
    cairo_curve_to (cr, 14, 42, 26, 56, 40, 47);
    cairo_curve_to (cr, 48, 42, 56, 44, 64, 48);
    cairo_line_to (cr, 64, 64);
    cairo_line_to (cr, 0, 64);
    cairo_close_path (cr);
    cairo_set_source_rgba (cr, 1, 1, 1, 0.92);
    cairo_fill (cr);
    cairo_restore (cr);
}

/* in-place: grey = luma, then scale alpha (premultiplied ARGB32) */
static void desaturate (cairo_surface_t *s, double alpha)
{
    cairo_surface_flush (s);
    unsigned char *d = cairo_image_surface_get_data (s);
    int w = cairo_image_surface_get_width (s), h = cairo_image_surface_get_height (s);
    int stride = cairo_image_surface_get_stride (s);
    for (int y = 0; y < h; y++) {
        guint32 *p = (guint32 *) (d + y * stride);
        for (int x = 0; x < w; x++) {
            guint32 v = p[x];
            double a = (v >> 24) & 255, r = (v >> 16) & 255, g = (v >> 8) & 255, b = v & 255;
            double l = 0.299 * r + 0.587 * g + 0.114 * b;
            a *= alpha;
            l *= alpha;
            guint32 L = (guint32) CLAMP (l + 0.5, 0, 255), A = (guint32) CLAMP (a + 0.5, 0, 255);
            p[x] = (A << 24) | (L << 16) | (L << 8) | L;
        }
    }
    cairo_surface_mark_dirty (s);
}

/* badge circle at the lower right (64-unit grid), r in units */
static void badge (cairo_t *cr, double r, double R, double G, double B)
{
    double cx = 64 - r - 1, cy = 64 - r - 1;
    cairo_arc (cr, cx, cy, r + 2.5, 0, 2 * G_PI);
    cairo_set_source_rgba (cr, 0, 0, 0, 0.55);   /* separates it from any bar colour */
    cairo_fill (cr);
    cairo_arc (cr, cx, cy, r, 0, 2 * G_PI);
    cairo_set_source_rgb (cr, R, G, B);
    cairo_fill (cr);
}

static void pause_bars (cairo_t *cr, double r)
{
    double cx = 64 - r - 1, cy = 64 - r - 1;
    double bw = r * 0.32, bh = r * 1.05, gap = r * 0.22;
    cairo_set_source_rgb (cr, 1, 1, 1);
    cairo_rectangle (cr, cx - gap - bw, cy - bh / 2, bw, bh);
    cairo_rectangle (cr, cx + gap, cy - bh / 2, bw, bh);
    cairo_fill (cr);
}

cairo_surface_t *tray_icon_render (int size, TrayIconState st)
{
    size = MAX (size, 8);
    cairo_surface_t *s = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, size, size);
    cairo_t *cr = cairo_create (s);
    cairo_scale (cr, size / 64.0, size / 64.0);
    draw_tile (cr);
    cairo_destroy (cr);

    if (st == TRAY_ICON_PAUSED)
        desaturate (s, 0.6);
    else if (st == TRAY_ICON_DEAD)
        desaturate (s, 0.45);

    cr = cairo_create (s);
    cairo_scale (cr, size / 64.0, size / 64.0);
    /* badges stay readable at 16 px: never smaller than ~7 px across */
    double minr = 3.6 * 64.0 / size;
    switch (st) {
    case TRAY_ICON_PAUSED: {
        double r = MAX (14, minr);
        badge (cr, r, 0.16, 0.16, 0.18);
        pause_bars (cr, r);
        break;
    }
    case TRAY_ICON_AUTOPAUSED: {
        double r = MAX (11, minr);
        badge (cr, r, 0.93, 0.62, 0.13);
        pause_bars (cr, r);
        break;
    }
    case TRAY_ICON_DEAD: {
        double r = MAX (13, minr);
        badge (cr, r, 0.85, 0.18, 0.18);
        double cx = 64 - r - 1, cy = 64 - r - 1, k = r * 0.45;
        cairo_set_source_rgb (cr, 1, 1, 1);
        cairo_set_line_width (cr, MAX (r * 0.3, 2.2));
        cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
        cairo_move_to (cr, cx - k, cy - k);
        cairo_line_to (cr, cx + k, cy + k);
        cairo_move_to (cr, cx + k, cy - k);
        cairo_line_to (cr, cx - k, cy + k);
        cairo_stroke (cr);
        break;
    }
    default:
        break;
    }
    cairo_destroy (cr);
    cairo_surface_flush (s);
    return s;
}

cairo_surface_t *tray_icon_render_in (int w, int h, TrayIconState st)
{
    w = MAX (w, 1);
    h = MAX (h, 1);
    int side = MIN (w, h);
    /* trays hand out the bar height; a 1-2 px margin like other icons */
    int margin = side >= 20 ? MAX (1, side / 12) : 0;
    int isz = MAX (side - 2 * margin, 8);
    cairo_surface_t *icon = tray_icon_render (isz, st);
    cairo_surface_t *s = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create (s);
    cairo_set_source_surface (cr, icon, (w - isz) / 2, (h - isz) / 2);
    cairo_paint (cr);
    cairo_destroy (cr);
    cairo_surface_destroy (icon);
    cairo_surface_flush (s);
    return s;
}

gboolean tray_icon_export_png (const char *path, int size, TrayIconState st)
{
    cairo_surface_t *s = tray_icon_render (size, st);
    gboolean ok = cairo_surface_write_to_png (s, path) == CAIRO_STATUS_SUCCESS;
    cairo_surface_destroy (s);
    return ok;
}
