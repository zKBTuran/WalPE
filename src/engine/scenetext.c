/* scenetext - WE text layer rasterizer (see scenetext.h) */
#include "scenetext.h"
#include "common/util.h"
#include <fontconfig/fontconfig.h>
#include <pango/pangocairo.h>
#include <pango/pangofc-fontmap.h>
#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* WE lays text out at 300 dpi: every saved text box in the test scenes is
 * 2 * padding + (ascent + descent) * pointsize * 300 / 72 high
 * (Monofur 45 pt: 188 + 64 = 252; Comic Sans 32 pt: 186 + 64 = 250) */
#define WE_TEXT_DPI 300.0
/* the largest point size handed to Pango (text_layout_locked): 62500
 * pixels, below the 65535 pixels per em FreeType takes */
#define TEXT_MAX_POINTS 15000.0f

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

/* cairo keeps a scaled font that nothing uses any more in a list of its
 * own (256 "holdovers", the oldest goes when a new one comes) in case it is
 * asked for again - and with it every glyph it has rasterised. Text layers
 * are drawn at 300 dpi: a glyph of a 32 point layer is a bitmap 133 pixels
 * high, a line of text some hundred kB, and every font and size of a
 * wallpaper is one such font. They stayed when the wallpaper was closed, and
 * when the font map was freed: half a megabyte and more per wallpaper with
 * text that had been shown (16 wallpapers with a clock, a date and a small
 * line each, one after the other: 6 MB; one line in 120 sizes: 49 MB), up
 * to 256 fonts, for the life of the process. cairo has no call that empties
 * the list (the one that resets its static data must not be used while any
 * cairo object lives). So the list is turned over once: as many scaled
 * fonts of a user font without glyphs as it has places are made and
 * released; each pushes out the oldest holdover. What is left in the list
 * is those, a few hundred bytes each. Fonts that are in use are not in the
 * list and are not touched; a font pushed out is made again when a text
 * asks for it. */
#define TEXT_CAIRO_HOLDOVERS 256   /* CAIRO_SCALED_FONT_MAX_HOLDOVERS */
static void text_forget_unused_fonts (void)
{
    cairo_font_face_t *face = cairo_user_font_face_create ();
    cairo_font_options_t *fo = cairo_font_options_create ();
    cairo_matrix_t ctm, fm;
    cairo_matrix_init_identity (&ctm);
    /* (+ 2: the font cairo remembers as the last one used joins the list
     * when the first of these takes its place, and the last of these stays
     * in that place) */
    for (int i = 1; i <= TEXT_CAIRO_HOLDOVERS + 2; i++) {
        cairo_matrix_init_scale (&fm, i, i);
        cairo_scaled_font_destroy (cairo_scaled_font_create (face, &fm, &ctm, fo));
    }
    cairo_font_options_destroy (fo);
    cairo_font_face_destroy (face);
}

/* a scene with text layers is gone. The last one: fontconfig's config +
 * font set and pango's font map/glyph caches are freed (rebuilt by the next
 * text). Not the last one: the font map stays for the others, without the
 * fonts it has cached - those of the scene that left among them, each with
 * its cairo font and glyphs (Pango keeps the last 256 font sets it was
 * asked for). The scenes that stay get theirs again with their next text. */
void walpe_text_release (void)
{
    g_mutex_lock (&text_lock);
    if (text_users > 1 && text_map) {
        text_users--;
        /* the font files of the wallpaper that left are no fonts of the
         * ones that stay: they leave the configuration with it. (Each
         * stayed registered, a pattern per face, until the last wallpaper
         * with text went; and a later wallpaper whose font has the family
         * and style of one of them was drawn with the file of the
         * wallpaper that was gone.) A file that is still in use is
         * registered again by the next render that names it
         * (text_file_desc_locked); the textures that exist are not
         * touched */
        g_hash_table_remove_all (text_files);
        if (text_fc)
            FcConfigAppFontClear (text_fc);
        if (PANGO_IS_FC_FONT_MAP (text_map))
            pango_fc_font_map_config_changed (PANGO_FC_FONT_MAP (text_map));
        text_forget_unused_fonts ();
    } else if (text_users > 0 && --text_users == 0 && text_map) {
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
        text_forget_unused_fonts ();
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

/* ---- what a wallpaper may hand to fontconfig and FreeType as a font ----
 * The file a text layer names goes to FcConfigAppFontAddFile, which opens
 * it with whichever FreeType driver takes its bytes, reads every face of it
 * and keeps a pattern for each face and for each named instance of a
 * variable face. Nothing bounded that. A collection header that names one
 * font 32000 times (1 MiB of file, nearly all zeros) was 32000 patterns:
 * 2.9 s and 86 MiB. 4000 named instances in each of 16 faces (36 KiB of
 * file) were 64000 patterns: 2.1 s and 152 MiB - four kilobytes of memory
 * for a byte of file, as far as the file goes, with the text lock held.
 * And a "font" could be a PCF or BDF bitmap font, a Type 1 or PFR file, a
 * compressed WOFF: each with a parser of its own behind it.
 * So the headers are read here first. Taken: a regular file that is a
 * TrueType / OpenType font or a collection of them, with at most
 * TEXT_FONT_MAX_FACES faces and named instances in all (what fontconfig
 * will make patterns of). The fonts in 91 Workshop scenes and in Wallpaper
 * Engine's assets are single TrueType / OpenType fonts, one pattern each.
 * Anything else is not handed over: the layer is drawn with a system font,
 * as for a file fontconfig cannot read. */
#define TEXT_FONT_MAX_FACES 256
#define TEXT_FONT_MAX_TABLES 512    /* directory entries of one face (real fonts: under 40) */

static gboolean font_read (int fd, guint64 size, guint64 off, guint8 *buf, gsize n)
{
    if (off > size || n > size - off)
        return FALSE;
    for (gsize got = 0; got < n;) {
        ssize_t r = pread (fd, buf + got, n - got, (off_t) (off + got));
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return FALSE;
        got += (gsize) r;
    }
    return TRUE;
}

static guint32 font_be32 (const guint8 *p)
{
    return (guint32) p[0] << 24 | (guint32) p[1] << 16 | (guint32) p[2] << 8 | p[3];
}

/* the patterns fontconfig makes of the face whose table directory is at
 * off: the face, and for a variable one its named instances and the
 * variable font itself. 0: not taken (must_be_font: no TrueType / OpenType
 * table directory there; always: a directory of absurd length). */
static guint font_face_patterns (int fd, guint64 size, guint64 off, gboolean must_be_font)
{
    guint8 h[12];
    if (!font_read (fd, size, off, h, sizeof h))
        return must_be_font ? 0 : 1;
    guint32 tag = font_be32 (h);
    if (must_be_font && tag != 0x00010000u && tag != 0x4F54544Fu /* OTTO */ &&
        tag != 0x74727565u /* true */)
        return 0;
    guint ntab = (guint) h[4] << 8 | h[5], n = 1;
    if (ntab > TEXT_FONT_MAX_TABLES)
        return 0;
    for (guint i = 0; i < ntab; i++) {
        guint8 r[16], v[16];
        if (!font_read (fd, size, off + 12 + 16 * (guint64) i, r, sizeof r))
            break;      /* a directory that leaves the file: FreeType's to refuse */
        if (font_be32 (r) != 0x66766172u /* fvar */)
            continue;
        /* the variations table: its instance count is at byte 12 */
        guint inst = font_be32 (r + 12) >= sizeof v && font_read (fd, size, font_be32 (r + 8), v, sizeof v)
                         ? (guint) v[12] << 8 | v[13] : 0;
        n = MAX (n, inst + 2);
    }
    return n;
}

static gboolean text_font_file_ok (const char *path)
{
    gboolean ok = FALSE;
    int fd = g_open (path, O_RDONLY | O_CLOEXEC | O_NONBLOCK, 0);
    struct stat sb;
    guint8 h[12];
    if (fd >= 0 && fstat (fd, &sb) == 0 && S_ISREG (sb.st_mode) &&
        font_read (fd, (guint64) sb.st_size, 0, h, sizeof h)) {
        guint64 size = (guint64) sb.st_size;
        if (font_be32 (h) == 0x74746366u /* ttcf: a collection */) {
            guint32 faces = font_be32 (h + 8);
            guint total = 0;
            ok = faces >= 1 && faces <= TEXT_FONT_MAX_FACES;
            for (guint32 i = 0; ok && i < faces; i++) {
                guint8 o[4];
                /* the first face is the one a text layer is drawn with */
                guint n = font_read (fd, size, 12 + 4 * (guint64) i, o, sizeof o)
                              ? font_face_patterns (fd, size, font_be32 (o), i == 0) : 0;
                total += n;
                ok = n > 0 && total <= TEXT_FONT_MAX_FACES;
            }
        } else {
            guint n = font_face_patterns (fd, size, 0, TRUE);
            ok = n > 0 && n <= TEXT_FONT_MAX_FACES;
        }
    }
    if (fd >= 0)
        close (fd);
    if (!ok)
        g_message ("scenetext: %s is not handed to the font library (not a TrueType / OpenType "
                   "font, or more than %d faces and named instances)", walpe_log_str (path), TEXT_FONT_MAX_FACES);
    return ok;
}

/* the description (family + the file's own weight/slant/stretch) that
 * selects exactly this font file; NULL if fontconfig can't read it */
static PangoFontDescription *text_file_desc_locked (const char *path)
{
    PangoFontDescription *fd = g_hash_table_lookup (text_files, path);
    if (fd || g_hash_table_contains (text_files, path))
        return fd;
    if (text_fc && text_font_file_ok (path) &&
        FcConfigAppFontAddFile (text_fc, (const FcChar8 *) path)) {
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
        g_message ("scenetext: font %s not usable, falling back to a system font", walpe_log_str (path));
    else {
        char *desc = pango_font_description_to_string (fd);
        g_debug ("scenetext: font %s -> %s", walpe_log_str (path), walpe_log_str (desc));
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
    /* Pango keeps the size as an int of 1/1024 pixels: a "pointsize" above
     * 503316 (1e9, 1e30, infinity from the scene file or from a user's
     * option) did not fit, the size came out negative and went on into the
     * font lookup (Pango-CRITICAL "size >= 0" when it was laid out; what was
     * laid out then is undefined). Letters of 15000 points are 62500
     * pixels high, far above what a texture holds: the layer is refused
     * by its size, as every such layer was meant to be. The limit stays
     * below 65535 pixels: FreeType refuses a larger size, and cairo then
     * drops the font it had begun without freeing its glyph table (648
     * bytes for every layout of such a layer). */
    if (pt > TEXT_MAX_POINTS)
        pt = TEXT_MAX_POINTS;
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
    if (W > 8192 || H > 8192 || W < 1 || H < 1 ||
        (st->max_bytes > 0 && (gint64) W * H * 4 > st->max_bytes)) {
        quad[2] = (float) MAX (W, 0);
        quad[3] = (float) MAX (H, 0);
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
    if (pango_layout_get_width (pl) > 0) {
        /* lines already aligned within the wrap width - at the end the
         * layer asked for, unless Pango turned the alignment round: lines
         * of a right-to-left script (Hebrew, Arabic) are aligned at the
         * other end of the wrap width. The block then lay where the layer
         * did not want it, and outside the texture when the wrap width is
         * larger than the box (a left-aligned Hebrew title with "maxwidth"
         * 4000 in a 2000 px box: nothing was drawn). It is moved to the
         * end it belongs to. Text Pango aligns as asked sits within a
         * pixel of that place (its extents are rounded) and is not
         * touched. */
        double want_x = st->halign == 1 ? (layout_w - lw) / 2 : st->halign == 2 ? layout_w - lw : 0;
        if (fabs (want_x - log.x) > 1.5)
            ox += want_x - log.x;
    } else {
        ox -= log.x;
    }
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
