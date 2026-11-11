/* scenetext - WE text layer rasterizer (see scenetext.h) */
#include "scenetext.h"
#include "common/util.h"
#include <fontconfig/fontconfig.h>
#include <pango/pangocairo.h>
#include <pango/pangofc-fontmap.h>
#include <math.h>
#include <string.h>
#include <sys/mman.h>

/* WE lays text out at 300 dpi: every saved text box in the test scenes is
 * 2 * padding + (ascent + descent) * pointsize * 300 / 72 high
 * (Monofur 45 pt: 188 + 64 = 252; Comic Sans 32 pt: 186 + 64 = 250) */
#define WE_TEXT_DPI 300.0

static GMutex text_lock;
static FcConfig *text_fc;
static PangoFontMap *text_map;
static GHashTable *text_files;   /* font path -> PangoFontDescription* (family/style) */

void walpe_text_style_clear (WalpeTextStyle *st)
{
    g_clear_pointer (&st->font_path, g_free);
    g_clear_pointer (&st->font_ref, g_free);
}

/* WE's built-in "systemfont_*" choices are Windows fonts: the real family
 * first, then metric-compatible / look-alike Linux families */
static const struct { const char *key, *families; } sysfonts[] = {
    { "arial",          "Arial,Liberation Sans,Arimo,Helvetica,DejaVu Sans" },
    { "comicsans",      "Comic Sans MS,Comic Neue,Comic Relief,Comic Mono,DejaVu Sans" },
    { "couriernew",     "Courier New,Liberation Mono,Cousine,DejaVu Sans Mono" },
    { "georgia",        "Georgia,Gelasio,DejaVu Serif" },
    { "impact",         "Impact,Anton,Oswald,DejaVu Sans Condensed" },
    { "segoeui",        "Segoe UI,Selawik,Open Sans,Noto Sans,DejaVu Sans" },
    { "tahoma",         "Tahoma,DejaVu Sans" },
    { "timesnewroman",  "Times New Roman,Liberation Serif,Tinos,DejaVu Serif" },
    { "trebuchetms",    "Trebuchet MS,Fira Sans,DejaVu Sans" },
    { "verdana",        "Verdana,DejaVu Sans" },
    { "consolas",       "Consolas,Inconsolata,DejaVu Sans Mono" },
    { "lucidaconsole",  "Lucida Console,DejaVu Sans Mono" },
    { "segoeprint",     "Segoe Print,Comic Neue,DejaVu Sans" },
    { "segoescript",    "Segoe Script,DejaVu Sans" },
};

const char *walpe_text_system_family (const char *ref)
{
    if (!ref || !g_str_has_prefix (ref, "systemfont_"))
        return NULL;
    const char *k = ref + strlen ("systemfont_");
    for (guint i = 0; i < G_N_ELEMENTS (sysfonts); i++)
        if (!g_ascii_strcasecmp (k, sysfonts[i].key))
            return sysfonts[i].families;
    return "Arial,Liberation Sans,DejaVu Sans";
}

static int text_users;            /* scenes holding text layers (text_lock) */

void walpe_text_hold (void)
{
    g_mutex_lock (&text_lock);
    text_users++;
    g_mutex_unlock (&text_lock);
}

/* the last scene with text layers is gone: fontconfig's config + font set
 * and pango's font map/glyph caches are freed (rebuilt by the next text) */
void walpe_text_release (void)
{
    g_mutex_lock (&text_lock);
    if (text_users > 0 && --text_users == 0 && text_map) {
        /* the font map holds its own reference on the config
         * (pango_fc_font_map_set_config) and Pango does not drop it when
         * the map is finalized: without this the whole FcConfig - font
         * set, patterns, parsed fonts.conf - stayed allocated after every
         * text scene (about 250 kB per wallpaper switch or option change) */
        if (PANGO_IS_FC_FONT_MAP (text_map))
            pango_fc_font_map_set_config (PANGO_FC_FONT_MAP (text_map), NULL);
        g_clear_object (&text_map);
        g_clear_pointer (&text_files, g_hash_table_unref);
        if (text_fc)
            FcConfigDestroy (text_fc);
        text_fc = NULL;
        g_message ("scenetext: no text layers left, font map freed");
    }
    g_mutex_unlock (&text_lock);
}

static void text_init_locked (void)
{
    if (text_map)
        return;
    text_fc = FcInitLoadConfigAndFonts ();   /* the system fonts + app fonts */
    text_map = pango_cairo_font_map_new_for_font_type (CAIRO_FONT_TYPE_FT);
    if (text_map && PANGO_IS_FC_FONT_MAP (text_map) && text_fc)
        pango_fc_font_map_set_config (PANGO_FC_FONT_MAP (text_map), text_fc);
    text_files = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                        (GDestroyNotify) pango_font_description_free);
}

/* the description (family + the file's own weight/slant/stretch) that
 * selects exactly this font file; NULL if fontconfig can't read it */
static PangoFontDescription *text_file_desc_locked (const char *path)
{
    PangoFontDescription *fd = g_hash_table_lookup (text_files, path);
    if (fd || g_hash_table_contains (text_files, path))
        return fd;
    if (text_fc && FcConfigAppFontAddFile (text_fc, (const FcChar8 *) path)) {
        FcFontSet *fs = FcConfigGetFonts (text_fc, FcSetApplication);
        for (int i = 0; fs && i < fs->nfont; i++) {
            FcChar8 *file = NULL;
            if (FcPatternGetString (fs->fonts[i], FC_FILE, 0, &file) == FcResultMatch &&
                !strcmp ((const char *) file, path)) {
                fd = pango_fc_font_description_from_pattern (fs->fonts[i], FALSE);
                break;
            }
        }
        /* new app font: drop the font map's caches */
        if (PANGO_IS_FC_FONT_MAP (text_map))
            pango_fc_font_map_config_changed (PANGO_FC_FONT_MAP (text_map));
    }
    if (!fd)
        g_message ("scenetext: font %s not usable, falling back to a system font", path);
    else {
        char *desc = pango_font_description_to_string (fd);
        g_debug ("scenetext: font %s -> %s", path, desc);
        g_free (desc);
    }
    g_hash_table_insert (text_files, g_strdup (path), fd);
    return fd;
}

static PangoLayout *text_layout_locked (PangoContext *ctx, const WalpeTextStyle *st,
                                        const char *text)
{
    PangoFontDescription *fd = NULL;
    PangoFontDescription *file = st->font_path ? text_file_desc_locked (st->font_path) : NULL;
    if (file) {
        fd = pango_font_description_copy (file);
    } else {
        const char *fam = walpe_text_system_family (st->font_ref);
        fd = pango_font_description_new ();
        pango_font_description_set_family (fd, fam ? fam : "Arial,Liberation Sans,DejaVu Sans");
        pango_font_description_set_weight (fd, PANGO_WEIGHT_NORMAL);
    }
    float pt = st->pointsize > 0 ? st->pointsize : 32.0f;
    pango_font_description_set_absolute_size (fd, pt * WE_TEXT_DPI / 72.0 * PANGO_SCALE);

    PangoLayout *pl = pango_layout_new (ctx);
    pango_layout_set_font_description (pl, fd);
    pango_font_description_free (fd);
    pango_layout_set_alignment (pl, st->halign == 1 ? PANGO_ALIGN_CENTER
                                    : st->halign == 2 ? PANGO_ALIGN_RIGHT : PANGO_ALIGN_LEFT);
    if (st->limitwidth && st->maxwidth > 1) {
        pango_layout_set_width (pl, walpe_d2i (st->maxwidth * PANGO_SCALE));
        if (st->limitrows && st->maxrows == 1)
            pango_layout_set_ellipsize (pl, st->ellipsis ? PANGO_ELLIPSIZE_END : PANGO_ELLIPSIZE_NONE);
        else
            pango_layout_set_wrap (pl, PANGO_WRAP_WORD_CHAR);
        if (st->limitrows && st->maxrows > 0 && st->ellipsis) {
            pango_layout_set_ellipsize (pl, PANGO_ELLIPSIZE_END);
            pango_layout_set_height (pl, -st->maxrows);
        }
    }
    pango_layout_set_text (pl, text, -1);
    return pl;
}

cairo_surface_t *walpe_text_render (const WalpeTextStyle *st, const char *text, float quad[4])
{
    return walpe_text_render_part (st, text, quad, NULL, NULL, NULL);
}

/* The surface a text is drawn on: W x H transparent pixels. A box of
 * 2500 x 1900 is 19 MB, of which a clock writes a few hundred kB. From
 * cairo_image_surface_create that memory comes from calloc, and glibc
 * hands out the block it was given back a frame ago and clears all of it
 * again (its mmap threshold follows the last block freed): 19 MB of
 * memset for every new string. Pages mapped from the system are zero
 * without anyone writing them and only the ones the glyphs touch ever
 * exist; the pixels cairo draws are the same either way (same format,
 * size and stride). That only pays when most of the surface stays
 * untouched - a page fault costs more than clearing a page - so it is
 * done for a large surface of which the caller will read a small part
 * (`sparse`); everything else stays with cairo's own allocation. */
#define TEXT_MAP_MIN (1024u * 1024u)
typedef struct { void *p; gsize len; } TextPx;
static const cairo_user_data_key_t text_px_key;

static void text_px_free (void *data)
{
    TextPx *m = data;
    munmap (m->p, m->len);
    g_free (m);
}

static cairo_surface_t *text_surface_new (int W, int H, gboolean sparse)
{
    int stride = cairo_format_stride_for_width (CAIRO_FORMAT_ARGB32, W);
    gsize len = stride > 0 ? (gsize) stride * (gsize) H : 0;
    if (sparse && len >= TEXT_MAP_MIN) {
        void *p = mmap (NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p != MAP_FAILED) {
#ifdef MADV_NOHUGEPAGE
            /* (a huge page would be cleared whole for one glyph) */
            madvise (p, len, MADV_NOHUGEPAGE);
#endif
            cairo_surface_t *sf = cairo_image_surface_create_for_data (p, CAIRO_FORMAT_ARGB32, W, H,
                                                                       stride);
            TextPx *m = g_new (TextPx, 1);
            m->p = p;
            m->len = len;
            if (cairo_surface_status (sf) == CAIRO_STATUS_SUCCESS &&
                cairo_surface_set_user_data (sf, &text_px_key, m, text_px_free) ==
                    CAIRO_STATUS_SUCCESS)
                return sf;
            cairo_surface_destroy (sf);
            munmap (p, len);
            g_free (m);
        }
    }
    return cairo_image_surface_create (CAIRO_FORMAT_ARGB32, W, H);
}

/* WALPE_TEXT_PART=0: always the whole texture (to compare) */
static gboolean text_part_enabled (void)
{
    static gsize once;
    static gboolean on = TRUE;
    if (g_once_init_enter (&once)) {
        const char *e = g_getenv ("WALPE_TEXT_PART");
        on = !(e && *e == '0');
        g_once_init_leave (&once, 1);
    }
    return on;
}

cairo_surface_t *walpe_text_render_part (const WalpeTextStyle *st, const char *text,
                                         float quad[4], const WalpeTextInk *prev,
                                         WalpeTextInk *now, int part[4])
{
    if (!text || !*text)
        return NULL;
    g_mutex_lock (&text_lock);
    text_init_locked ();
    if (!text_map) {
        g_mutex_unlock (&text_lock);
        return NULL;
    }
    PangoContext *ctx = pango_font_map_create_context (text_map);
    cairo_font_options_t *fo = cairo_font_options_create ();
    /* unhinted outlines at their real advances (hinting at these sizes
     * snaps widths and makes strokes heavier than WE's) */
    cairo_font_options_set_antialias (fo, CAIRO_ANTIALIAS_GRAY);
    cairo_font_options_set_hint_style (fo, CAIRO_HINT_STYLE_NONE);
    cairo_font_options_set_hint_metrics (fo, CAIRO_HINT_METRICS_OFF);
    pango_cairo_context_set_font_options (ctx, fo);
    cairo_font_options_destroy (fo);
    pango_cairo_context_set_resolution (ctx, 72.0);  /* sizes are absolute pixels */
#if PANGO_VERSION_CHECK(1, 44, 0)
    pango_context_set_round_glyph_positions (ctx, FALSE);
#endif

    PangoLayout *pl = text_layout_locked (ctx, st, text);
    /* rows beyond maxrows are dropped when no ellipsis is wanted */
    int nlines = pango_layout_get_line_count (pl);
    PangoRectangle ink, log;
    pango_layout_get_pixel_extents (pl, &ink, &log);
    double lw = log.width, lh = log.height;
    if (st->limitrows && st->maxrows > 0 && nlines > st->maxrows && !st->ellipsis) {
        PangoLayoutIter *it = pango_layout_get_iter (pl);
        for (int i = 0; i < st->maxrows - 1; i++)
            pango_layout_iter_next_line (it);
        int y0, y1;
        pango_layout_iter_get_line_yrange (it, &y0, &y1);
        pango_layout_iter_free (it);
        lh = y1 / (double) PANGO_SCALE;
    }
    double pad = st->padding > 0 ? st->padding : 0;
    double bw = st->box_w >= 1 ? st->box_w : lw + 2 * pad;
    double bh = st->box_h >= 1 ? st->box_h : lh + 2 * pad;
    /* the texture: the box, grown to the content, away from the aligned
     * edge (left-aligned text grows right, top-aligned grows down) */
    double tw = MAX (bw, ceil (lw + 2 * pad)), th = MAX (bh, ceil (lh + 2 * pad));
    /* WE keeps a little ink outside the logical box (italics, swashes) */
    double ix0 = MIN (0, ink.x - log.x), ix1 = MAX (lw, ink.x + ink.width - log.x);
    (void) ix0; (void) ix1;
    int W = walpe_d2i (ceil (tw)), H = walpe_d2i (ceil (th));
    if (W > 8192 || H > 8192 || W < 1 || H < 1) {
        g_object_unref (pl);
        g_object_unref (ctx);
        g_mutex_unlock (&text_lock);
        return NULL;
    }
    /* WE anchors aligned text at the layer origin: left-aligned text starts
     * AT the origin (3163060610's two clocks, boxes 406 and 656 wide, share
     * their left edge only this way and sit inside their pill as in the
     * Workshop preview), right-aligned ends there, top-aligned hangs from
     * it; centred text is centred on it */
    quad[0] = st->halign == 0 ? tw / 2 - pad : st->halign == 2 ? -(tw / 2 - pad) : 0;
    quad[1] = st->valign == 0 ? -(th / 2 - pad) : st->valign == 2 ? th / 2 - pad : 0;
    (void) bw; (void) bh;
    quad[2] = W;
    quad[3] = H;

    /* layout origin inside the texture */
    double ox, oy;
    double inner_w = tw - 2 * pad, inner_h = th - 2 * pad;
    double layout_w = pango_layout_get_width (pl) > 0 ? pango_layout_get_width (pl) / (double) PANGO_SCALE : lw;
    /* PangoLayout aligns lines within its own width (or the widest
     * line): place that block by the horizontal alignment */
    switch (st->halign) {
    case 1: ox = pad + (inner_w - layout_w) / 2; break;
    case 2: ox = pad + inner_w - layout_w; break;
    default: ox = pad; break;
    }
    if (pango_layout_get_width (pl) > 0)
        ox -= 0;  /* lines already aligned within the wrap width */
    else
        ox -= log.x;
    switch (st->valign) {
    case 0: oy = pad; break;
    case 2: oy = pad + inner_h - lh; break;
    default: oy = pad + (inner_h - lh) / 2; break;
    }
    oy -= log.y;

    /* where the ink can be: the layout's ink bounds at its place in the
     * texture, grown by the margin (see scenetext.h) */
    WalpeTextInk ink_now = { W, H, 0, 0, 0, 0 };
    if (ink.width > 0 && ink.height > 0) {
        double ix = ox + ink.x, iy = oy + ink.y;
        ink_now.x0 = (int) CLAMP (floor (ix) - WALPE_TEXT_INK_MARGIN, 0.0, (double) W);
        ink_now.y0 = (int) CLAMP (floor (iy) - WALPE_TEXT_INK_MARGIN, 0.0, (double) H);
        ink_now.x1 = (int) CLAMP (ceil (ix + ink.width) + WALPE_TEXT_INK_MARGIN, 0.0, (double) W);
        ink_now.y1 = (int) CLAMP (ceil (iy + ink.height) + WALPE_TEXT_INK_MARGIN, 0.0, (double) H);
    }
    /* the part the texture needs: everything, or - a texture of the same
     * size that holds earlier text - where the old or the new ink is */
    int px = 0, py = 0, pw = W, ph = H;
    if (prev && part && prev->w == W && prev->h == H && text_part_enabled ()) {
        gboolean have_old = prev->x1 > prev->x0 && prev->y1 > prev->y0;
        gboolean have_new = ink_now.x1 > ink_now.x0 && ink_now.y1 > ink_now.y0;
        int x0 = 0, y0 = 0, x1 = 1, y1 = 1;   /* no ink before or after: one (transparent) pixel */
        if (have_old && have_new) {
            x0 = MIN (prev->x0, ink_now.x0); y0 = MIN (prev->y0, ink_now.y0);
            x1 = MAX (prev->x1, ink_now.x1); y1 = MAX (prev->y1, ink_now.y1);
        } else if (have_old) {
            x0 = prev->x0; y0 = prev->y0; x1 = prev->x1; y1 = prev->y1;
        } else if (have_new) {
            x0 = ink_now.x0; y0 = ink_now.y0; x1 = ink_now.x1; y1 = ink_now.y1;
        }
        x0 = CLAMP (x0, 0, W - 1); y0 = CLAMP (y0, 0, H - 1);
        x1 = CLAMP (x1, x0 + 1, W); y1 = CLAMP (y1, y0 + 1, H);
        px = x0; py = y0; pw = x1 - x0; ph = y1 - y0;
    }
    if (now)
        *now = ink_now;
    if (part) {
        part[0] = px; part[1] = py; part[2] = pw; part[3] = ph;
    }

    /* (sparse: the part is at most a quarter of the texture) */
    cairo_surface_t *sf = text_surface_new (W, H, (gint64) pw * ph * 4 <= (gint64) W * H);
    cairo_t *cr = cairo_create (sf);
    if (st->limitrows && st->maxrows > 0 && nlines > st->maxrows && !st->ellipsis) {
        cairo_rectangle (cr, 0, 0, W, oy + log.y + lh);
        cairo_clip (cr);
    }
    cairo_move_to (cr, ox, oy);
    cairo_set_source_rgba (cr, 1, 1, 1, 1);
    pango_cairo_update_context (cr, ctx);
    pango_layout_context_changed (pl);
    pango_cairo_show_layout (cr, pl);
    cairo_destroy (cr);
    cairo_surface_flush (sf);
    g_object_unref (pl);
    g_object_unref (ctx);
    g_mutex_unlock (&text_lock);
    return sf;
}
