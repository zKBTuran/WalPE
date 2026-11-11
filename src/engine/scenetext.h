/* scenetext - Wallpaper Engine text layers rasterized like WE does it:
 * the layer's own font file (scene.pkg, project dir or the WE install's
 * assets/fonts) or a mapped Windows "systemfont_*", pointsize at 300 dpi
 * (the box heights WE saves = padding*2 + line height at 300 dpi), regular
 * weight, laid out inside the layer's box with its padding and alignment. */
#pragma once
#include <glib.h>
#include <cairo.h>

typedef struct {
    char *font_path;     /* resolved font file, or NULL */
    char *font_ref;      /* the scene's "font" value (systemfont_* / path) */
    float pointsize;     /* WE points; pixels = pointsize * 300 / 72 */
    float box_w, box_h;  /* the object's "size" (texture size in WE) */
    float padding;
    int halign;          /* 0 left, 1 center, 2 right */
    int valign;          /* 0 top, 1 center, 2 bottom */
    float maxwidth;      /* wrap width (px) when limitwidth */
    gboolean limitwidth;
    int maxrows;         /* rows kept when limitrows */
    gboolean limitrows;
    gboolean ellipsis;   /* limituseellipsis */
} WalpeTextStyle;

void walpe_text_style_clear (WalpeTextStyle *st);

/* Renders white text (premultiplied ARGB32). quad = {cx, cy, w, h}: the
 * texture's rectangle in the layer's local space (Y up, box centred on 0):
 * the box itself unless the text needs more room, then grown away from the
 * aligned edge. NULL for empty text. Thread-safe. */
cairo_surface_t *walpe_text_render (const WalpeTextStyle *st, const char *text, float quad[4]);

/* ---- a text layer whose string changes while it is shown (a clock, a
 * counter, typed text): only the part of its texture that can differ is
 * recoloured and sent to the GPU again.
 * WalpeTextInk describes what a texture holds: its size and the rectangle
 * (texture pixels, y down like the surface rows; x1 <= x0 = empty) outside
 * of which every pixel is transparent - the layout's ink bounds grown by
 * WALPE_TEXT_INK_MARGIN pixels. A glyph is rasterised within one pixel of
 * its ink bounds (outline bounds rounded outwards, origin on a pixel or a
 * quarter of one); the margin is four.
 * walpe_text_render_part renders exactly as walpe_text_render does - the
 * same surface of the whole texture, by the same calls (rendering into a
 * smaller surface is NOT the same: cairo takes another path for a glyph
 * near a surface edge and rounds its position differently) - and fills
 * *now. part[] = {x, y, w, h} is what of the surface the texture needs:
 * all of it, or, when prev describes a texture of the same size, the union
 * of the two ink rectangles (the old ink is transparent there now, the new
 * ink drawn). Every pixel outside it is transparent before and after, so
 * copying that part into the texture gives the texture a full upload
 * gives. The surface's memory comes zeroed from the system and only the
 * pages the glyphs touch are ever written, so a caller that reads and
 * uploads only part[] pays for the ink, not for the box.
 * WALPE_TEXT_PART=0 in the environment: part[] is always the whole. */
#define WALPE_TEXT_INK_MARGIN 4
typedef struct {
    int w, h;            /* texture size */
    int x0, y0, x1, y1;  /* ink rectangle, margin included, within the texture */
} WalpeTextInk;
cairo_surface_t *walpe_text_render_part (const WalpeTextStyle *st, const char *text,
                                         float quad[4], const WalpeTextInk *prev,
                                         WalpeTextInk *now, int part[4]);

/* a scene with text layers holds the font machinery (fontconfig + pango
 * font map, built lazily by the first walpe_text_render); the last release
 * frees it */
void walpe_text_hold (void);
void walpe_text_release (void);

/* "systemfont_comicsans" -> fontconfig family list ("Comic Sans MS,...") */
const char *walpe_text_system_family (const char *ref);
