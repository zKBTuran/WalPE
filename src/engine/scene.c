/* scene.c - Wallpaper Engine scene wallpapers.
 *
 * Tier-1 scene core: recursive LAYER TREE (compose groups with inherited
 * transforms), camera (zoom/shift), particles as LAYERS, text layers
 * (pangocairo -> premultiplied BGRA texture), sound/light layers ignored.
 * Layer transforms: every layer maps its +-1 unit quad into its PARENT's
 * local px space; the parent's matrix (root: scene px -> clip) carries it
 * the rest of the way — groups therefore compose by matrix multiplication,
 * matching WE's coordinate system.
 *
 * Tier-2 (effects: the 46 builtin pass-graphs with their shipped GLSL)
 * attaches to the per-layer draw call once this tree is solid. */
#include "scene.h"
#include "glutil.h"
#include "particles.h"
#include "scenenode.h"
#include "common/util.h"
#include <json-glib/json-glib.h>
#include <pango/pangocairo.h>
#include <string.h>
#include <math.h>

#define SCENE_MAX_DEPTH 8
/* text layers: as scenenode.c (TEXT_MAX_BYTES) and scenetext.c (8192 px) */
#define SCENE_TEXT_MAX_BYTES (16 * 1024)
#define SCENE_TEXT_MAX_PX 8192

typedef struct {
    gboolean visible;
    GLuint tex;
    gboolean tex_ready, premult;
    int tex_w, tex_h;
    int blend;
    float scale, opacity, angle; /* opacity 0..1, angle degrees */
    float pivot[2], translation[2];
    float par_amount[2], par_depth;
    char *path;

    /* tier-1 additions */
    GArray *children;        /* compose group: SceneLayer tree */
    gboolean is_group;
    gboolean is_text;        /* text layer: rendered via pango */
    char *text;
    float text_color[4];
    double font_size;        /* px */
    char *font;
    WalpeParticles *parts;   /* particle system attached to this layer */
} SceneLayer;

typedef struct {
    WalpeContent base;
    JsonNode *root;          /* owns scene json */
    float scene_w, scene_h;
    float camera_zoom;
    float camera_shift[2];
    GArray *layers;          /* SceneLayer tree (root level) */
    char *dir;
    int fps_cap;
    gint64 due_us;       /* frame clock phase */
    gboolean paused;
    double t0;
    guint effect_count;      /* tier-2 preview: how many effect instances */
    gboolean warned_nested_particles, warned_light_sound;
} SceneContent;

/* a file the scene json names: below dir (or dir/assets), a regular file */
static char *resolve_asset (const char *dir, const char *file)
{
    if (!file || !*file)
        return NULL;
    char *a = walpe_asset_path (dir, file);
    if (!a) {
        char *b = g_build_filename ("assets", file, NULL);
        a = walpe_asset_path (dir, b);
        g_free (b);
    }
    return a;
}

/* ------------------------------------------------------------- parsing */

static void parse_layers (JsonArray *arr, GArray *out, const char *dir, SceneContent *s);

static void parse_layer (SceneLayer *l, JsonObject *o, const char *dir, SceneContent *s)
{
    memset (l, 0, sizeof (*l));
    l->visible = wpj_bool (o, "visible", TRUE);
    l->blend = walpe_d2i (wpj_num (o, "blendmode", WALPE_BLEND_NORMAL));
    if (l->blend < 0 || l->blend > 5)
        l->blend = WALPE_BLEND_NORMAL;
    l->scale = (float) wpj_num (o, "scale", 1.0);
    l->opacity = (float) (wpj_num (o, "opacity", 255.0) / 255.0);
    l->angle = (float) wpj_num (o, "angle", 0.0);
    double px = 0, py = 0, tx = 0, ty = 0;
    wpj_num2 (o, "pivot", &px, &py);
    wpj_num2 (o, "translation", &tx, &ty);
    l->pivot[0] = (float) px;
    l->pivot[1] = (float) py;
    l->translation[0] = (float) tx;
    l->translation[1] = (float) ty;
    JsonObject *par = wpj_obj (o, "parallax");
    if (par) {
        double ax = 0, ay = 0;
        wpj_num2 (par, "amount", &ax, &ay);
        l->par_amount[0] = (float) ax;
        l->par_amount[1] = (float) ay;
        l->par_depth = (float) wpj_num (par, "depth", 0.0);
    }

    /* effects: tier-2 will execute these; count + name them for now */
    JsonArray *fx = wpj_arr (o, "effects");
    guint nfx = wpj_arr_len (fx);
    if (nfx > 0) {
        s->effect_count += nfx;
        for (guint i = 0; i < nfx; i++) {
            JsonObject *e = wpj_arr_obj (fx, i);
            if (!e)
                continue;
            const char *name = wpj_str (e, "file", wpj_str (e, "id", "?"));
            if (i == 0 && nfx == 1)
                g_debug ("scene: layer has effect %s (tier-2: not applied yet)", name);
        }
    }

    const char *file = wpj_str (o, "file", wpj_str (o, "image", NULL));
    if (file)
        l->path = resolve_asset (dir, file);

    /* compose group: recursive layer tree */
    JsonObject *compose = wpj_obj (o, "compose");
    if (compose) {
        l->is_group = TRUE;
        l->children = g_array_new (FALSE, FALSE, sizeof (SceneLayer));
        parse_layers (wpj_arr (compose, "layers"), l->children, dir, s);
    }

    /* text layer */
    JsonObject *tobj = wpj_obj (o, "text");
    if (tobj) {
        l->is_text = TRUE;
        /* bounded like the text of the current scene format (scenenode.c):
         * megabytes of text are megabytes for Pango to lay out, twice, on
         * the render thread, and nothing wider than a texture is drawn */
        const char *txt = wpj_str (tobj, "text", "");
        gsize tl = strnlen (txt, SCENE_TEXT_MAX_BYTES + 1);
        if (tl > SCENE_TEXT_MAX_BYTES) {
            tl = SCENE_TEXT_MAX_BYTES;
            while (tl > 0 && ((guchar) txt[tl] & 0xC0) == 0x80)
                tl--;
        }
        l->text = g_strndup (txt, tl);
        l->font_size = wpj_num (tobj, "size", wpj_num (tobj, "fontsize", 48.0));
        if (!(l->font_size >= 2))   /* also not-a-number */
            l->font_size = 48;
        if (l->font_size > SCENE_TEXT_MAX_PX)
            l->font_size = SCENE_TEXT_MAX_PX;
        l->font = g_strdup (wpj_str (tobj, "font", "Sans"));
        JsonArray *ca = wpj_arr (tobj, "color");
        for (int i = 0; i < 4; i++) {
            l->text_color[i] = 1.0f;
            if (ca && i < (int) json_array_get_length (ca)) {
                GValue v = G_VALUE_INIT;
                JsonNode *cn = json_array_get_element (ca, i);
                if (cn && json_node_get_value_type (cn) == G_TYPE_DOUBLE) {
                    json_node_get_value (cn, &v);
                    l->text_color[i] = (float) g_value_get_double (&v);
                    g_value_unset (&v);
                }
            }
        }
    }

    /* particle system attached to this layer */
    JsonNode *pjn = json_object_get_member (o, "particles");
    if (pjn && !l->is_group) {
        if (json_node_get_node_type (pjn) == JSON_NODE_ARRAY)
            l->parts = walpe_particles_new (json_node_get_array (pjn), dir);
        else if (json_node_get_node_type (pjn) == JSON_NODE_OBJECT) {
            JsonArray *one = json_array_new ();
            json_array_add_object_element (one, json_object_ref (json_node_get_object (pjn)));
            l->parts = walpe_particles_new (one, dir);
            json_array_unref (one);
        }
        if (!l->parts || !walpe_particles_usable (l->parts))
            g_clear_pointer (&l->parts, walpe_particles_free);
    }

    /* layers without drawable content: sound / light -> skip cleanly */
    if (!l->path && !l->is_group && !l->is_text && !l->parts) {
        if (json_object_has_member (o, "sound") || json_object_has_member (o, "light")) {
            if (!s->warned_light_sound) {
                s->warned_light_sound = TRUE;
                g_debug ("scene: sound/light layers present (ignored: silent renderer)");
            }
            l->visible = FALSE;
        }
    }
}

static void parse_layers (JsonArray *arr, GArray *out, const char *dir, SceneContent *s)
{
    guint n = wpj_arr_len (arr);
    for (guint i = 0; i < n; i++) {
        JsonObject *lo = wpj_arr_obj (arr, i);
        if (!lo)
            continue;
        SceneLayer l;
        parse_layer (&l, lo, dir, s);
        g_array_append_val (out, l);
    }
}

/* --------------------------------------------------------- text render */

/* render the layer's text via pangocairo into a premultiplied BGRA
 * texture (cairo ARGB32 is exactly premultiplied BGRA on little-endian) */
static void make_text_texture (SceneLayer *l)
{
    if (!l->text || !*l->text)
        return;
    PangoFontDescription *fd = pango_font_description_from_string (
        l->font ? l->font : "Sans");
    pango_font_description_set_absolute_size (fd, l->font_size * PANGO_SCALE);

    /* measure */
    cairo_surface_t *m = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *cr = cairo_create (m);
    PangoLayout *pl = pango_cairo_create_layout (cr);
    pango_layout_set_text (pl, l->text, -1);
    pango_layout_set_font_description (pl, fd);
    int tw = 0, th = 0;
    pango_layout_get_pixel_size (pl, &tw, &th);
    g_object_unref (pl);
    cairo_destroy (cr);
    cairo_surface_destroy (m);
    if (tw < 1 || th < 1 || tw > SCENE_TEXT_MAX_PX || th > SCENE_TEXT_MAX_PX) {
        if (tw > SCENE_TEXT_MAX_PX || th > SCENE_TEXT_MAX_PX)
            g_warning ("scene: a text layer of %d x %d pixels is not drawn (the limit is %d)",
                       tw, th, SCENE_TEXT_MAX_PX);
        pango_font_description_free (fd);
        return;
    }

    cairo_surface_t *sf = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, tw, th);
    cr = cairo_create (sf);
    pl = pango_cairo_create_layout (cr);
    pango_layout_set_text (pl, l->text, -1);
    pango_layout_set_font_description (pl, fd);
    cairo_set_source_rgba (cr, l->text_color[0], l->text_color[1],
                           l->text_color[2], l->text_color[3]);
    pango_cairo_show_layout (cr, pl);
    g_object_unref (pl);   /* (was never released: one layout, with its fonts, per text layer and load) */
    cairo_destroy (cr);
    pango_font_description_free (fd);
    cairo_surface_flush (sf);

    GLuint tex = 0;
    glGenTextures (1, &tex);
    glBindTexture (GL_TEXTURE_2D, tex);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei (GL_UNPACK_ROW_LENGTH, cairo_image_surface_get_stride (sf) / 4);
    glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, tw, th, 0, GL_BGRA, GL_UNSIGNED_BYTE,
                  cairo_image_surface_get_data (sf));
    glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei (GL_UNPACK_ALIGNMENT, 4);
    cairo_surface_destroy (sf);

    if (glGetError () == GL_NO_ERROR) {
        l->tex = tex;
        l->premult = TRUE; /* cairo ARGB32 is premultiplied */
        l->tex_w = tw;
        l->tex_h = th;
        l->tex_ready = TRUE;
    }
}

/* ------------------------------------------------------------- gl init */

static void layers_gl_init (GArray *layers)
{
    for (guint i = 0; i < layers->len; i++) {
        SceneLayer *l = &g_array_index (layers, SceneLayer, i);
        if (l->children)
            layers_gl_init (l->children);
        if (l->parts) {
            walpe_particles_gl_init (l->parts);
            if (!walpe_particles_usable (l->parts))
                g_clear_pointer (&l->parts, walpe_particles_free);
        }
        if (l->is_text) {
            make_text_texture (l);
            continue;
        }
        if (!l->path || l->is_group)
            continue;
        WalpeGlTex t = walpe_gl_tex_from_file (l->path);
        if (!t.tex) {
            g_warning ("scene: cannot load layer texture %s", l->path);
            continue;
        }
        l->tex = t.tex;
        l->premult = t.premult;
        l->tex_w = t.width;
        l->tex_h = t.height;
        l->tex_ready = TRUE;
    }
}

/* does any layer move with the pointer (parallax) or hold particles
 * (rendered with the pointer position)? */
static gboolean layers_use_pointer (GArray *layers)
{
    for (guint i = 0; layers && i < layers->len; i++) {
        SceneLayer *l = &g_array_index (layers, SceneLayer, i);
        if (l->parts || ((l->par_amount[0] != 0.0f || l->par_amount[1] != 0.0f) && l->par_depth != 0.0f) ||
            (l->children && layers_use_pointer (l->children)))
            return TRUE;
    }
    return FALSE;
}

static gboolean scene_gl_init (WalpeContent *c, GError **err)
{
    SceneContent *s = (SceneContent *) c;
    walpe_glutil_init ();
    {
        /* the texture budget of the current scene format (scenenode.c):
         * 1024 MiB unless WALPE_TEX_BUDGET_MB says otherwise, 0 = no limit.
         * This format had none: every layer picture was stored at file size */
        const char *tb = g_getenv ("WALPE_TEX_BUDGET_MB");
        double mb = tb && *tb ? g_ascii_strtod (tb, NULL) : 1024.0;
        walpe_gl_tex_scope_begin (isfinite (mb) && mb > 0 ? (gint64) (mb * 1048576.0) : 0);
        layers_gl_init (s->layers);
        walpe_gl_tex_scope_end ();
    }
    /* the daemon polls the pointer (and keeps one rendering per monitor)
     * only for content that says it draws with it */
    c->uses_pointer_pos = layers_use_pointer (s->layers);
    if (s->effect_count > 0)
        g_message ("scene: %u effect instances found (tier-2 effect pipeline pending)",
                   s->effect_count);
    (void) err;
    return TRUE;
}

/* arm the render thread's frame clock while something animates */
static void scene_schedule (SceneContent *s)
{
    if (s->paused)
        return;
    /* particles need animation frames even if no other layer animates */
    gboolean animates = FALSE;
    GPtrArray *q = g_ptr_array_new ();
    g_ptr_array_add (q, s->layers);
    for (guint qi = 0; qi < q->len && !animates; qi++) {
        GArray *layers = q->pdata[qi];
        for (guint i = 0; i < layers->len && !animates; i++) {
            SceneLayer *l = &g_array_index (layers, SceneLayer, i);
            if (l->parts)
                animates = TRUE;
            if (l->children)
                g_ptr_array_add (q, l->children);
        }
    }
    g_ptr_array_free (q, TRUE);
    if (!animates)
        return;
    s->base.next_frame_us = walpe_frame_clock_next (&s->due_us,
                                                    1000000 / (s->fps_cap > 0 ? s->fps_cap : 30));
}

/* --------------------------------------------------------------- render */

/* per-layer local matrix: +-1 unit quad -> the layer's local px space,
 * transformed by pivot/rotation/scale/translation/parallax (call order =
 * point order, premultiplied helpers) */
static void layer_local_matrix (SceneLayer *l, float parx, float pary, WalpeMat3 *m)
{
    walpe_mat3_identity (m);
    walpe_mat3_scale (m, 0.5f, -0.5f);
    walpe_mat3_translate (m, 0.5f, 0.5f);
    if (!l->is_group) /* groups have no texture: children live in their px space */
        walpe_mat3_scale (m, (float) l->tex_w, (float) l->tex_h);
    walpe_mat3_scale (m, l->scale, l->scale);
    walpe_mat3_translate (m, -l->pivot[0] * l->tex_w * l->scale,
                          -l->pivot[1] * l->tex_h * l->scale);
    walpe_mat3_rotate (m, l->angle * (float) G_PI / 180.0f);
    walpe_mat3_translate (m, l->pivot[0] * l->tex_w * l->scale,
                          l->pivot[1] * l->tex_h * l->scale);
    walpe_mat3_translate (m, l->translation[0] + parx, l->translation[1] + pary);
}

static void render_layers (GArray *layers, const WalpeMat3 *parent_clip,
                           int w, int h, float px, float py, int depth,
                           SceneContent *s)
{
    if (depth > SCENE_MAX_DEPTH)
        return;
    /* WE exports layers top-first; render bottom-first (reverse order) */
    for (guint i = layers->len; i-- > 0;) {
        SceneLayer *l = &g_array_index (layers, SceneLayer, i);
        if (!l->visible || l->opacity <= 0)
            continue;
        float parx = px * l->par_amount[0] * (l->par_depth / 100.0f) * 60.0f;
        float pary = py * l->par_amount[1] * (l->par_depth / 100.0f) * 60.0f;

        WalpeMat3 m = *parent_clip; /* parent local px -> clip */
        WalpeMat3 local;
        layer_local_matrix (l, parx, pary, &local);
        walpe_mat3_mul (&m, &local, &m); /* m = local * parent_clip */

        if (l->tex_ready)
            walpe_glutil_draw_quad (l->tex, l->premult, l->opacity, m.m, l->blend);

        if (l->parts) {
            if (depth == 0) {
                /* particles.c builds its own scene->clip matrix: only
                 * correct for top-level layers */
                walpe_particles_render (l->parts, s->scene_w, s->scene_h, w, h, px, py);
            } else if (!s->warned_nested_particles) {
                s->warned_nested_particles = TRUE;
                g_debug ("scene: particles inside compose groups not supported yet");
            }
        }

        if (l->children)
            render_layers (l->children, &m, w, h, px, py, depth + 1, s);
    }
}

static gboolean scene_gl_render (WalpeContent *c, int w, int h, float px, float py)
{
    SceneContent *s = (SceneContent *) c;
    glClearColor (0, 0, 0, 1);
    glClear (GL_COLOR_BUFFER_BIT);

    float k = fmaxf ((float) w / s->scene_w, (float) h / s->scene_h);
    /* scene px -> clip (the root parent matrix), with camera applied */
    WalpeMat3 tail;
    walpe_mat3_identity (&tail);
    walpe_mat3_scale (&tail, k, k);
    walpe_mat3_translate (&tail, -(s->scene_w * k - (float) w) / 2.0f,
                          -(s->scene_h * k - (float) h) / 2.0f);
    if (s->camera_zoom != 1.0f) { /* zoom about the scene center */
        walpe_mat3_translate (&tail, s->scene_w * k / 2.0f, s->scene_h * k / 2.0f);
        walpe_mat3_scale (&tail, s->camera_zoom, s->camera_zoom);
        walpe_mat3_translate (&tail, -s->scene_w * k / 2.0f, -s->scene_h * k / 2.0f);
    }
    if (s->camera_shift[0] != 0.0f || s->camera_shift[1] != 0.0f)
        walpe_mat3_translate (&tail, s->camera_shift[0] * s->scene_w * k,
                              s->camera_shift[1] * s->scene_h * k);
    walpe_mat3_scale (&tail, 2.0f / (float) w, -2.0f / (float) h);
    walpe_mat3_translate (&tail, -1.0f, 1.0f);

    render_layers (s->layers, &tail, w, h, px, py, 0, s);
    scene_schedule (s); /* particle/text animation needs the next frame */
    return TRUE;
}

/* ------------------------------------------------------------- teardown */

static void layers_gl_free (GArray *layers)
{
    for (guint i = 0; i < layers->len; i++) {
        SceneLayer *l = &g_array_index (layers, SceneLayer, i);
        if (l->children)
            layers_gl_free (l->children);
        if (l->tex)
            glDeleteTextures (1, &l->tex);
        l->tex = 0;
        l->tex_ready = FALSE;
        g_clear_pointer (&l->parts, walpe_particles_free);
    }
}

/* The list itself, as parse_layers () made it: the groups' own lists and
 * every layer's strings stayed behind per load of an old-format scene.
 * Particle systems that never reached gl_init hold no GL object. */
static void layers_free (GArray *layers)
{
    for (guint i = 0; i < layers->len; i++) {
        SceneLayer *l = &g_array_index (layers, SceneLayer, i);
        if (l->children)
            layers_free (l->children);
        g_free (l->path);
        g_free (l->text);
        g_free (l->font);
        g_clear_pointer (&l->parts, walpe_particles_free);
    }
    g_array_free (layers, TRUE);
}

static void scene_gl_free (WalpeContent *c)
{
    SceneContent *s = (SceneContent *) c;
    if (s->layers) {
        layers_gl_free (s->layers);
        layers_free (s->layers);
        s->layers = NULL;
    }
}

static void scene_set_paused (WalpeContent *c, gboolean p)
{
    SceneContent *s = (SceneContent *) c;
    s->paused = p;
    s->due_us = 0;
}

/* render thread: live fps cap change (next frame uses the new interval) */
static void scene_set_fps (WalpeContent *c, int fps)
{
    SceneContent *s = (SceneContent *) c;
    s->fps_cap = fps > 0 ? fps : 30;
    s->due_us = 0;
}

/* render thread, synchronous: no main-loop source references content */
static void scene_free (WalpeContent *c)
{
    SceneContent *s = (SceneContent *) c;
    if (s->layers)   /* loaded, never drawn: gl_free was not called */
        layers_free (s->layers);
    if (s->root)
        json_node_unref (s->root);
    g_free (s->dir);
    g_free (s->base.path);
    g_free (s->base.title);
    g_free (s);
}

WalpeContent *walpe_scene_new (const char *project_dir, const char *scene_file,
                               const WalpeLoadOpts *opts, GError **err)
{
    /* content.c passes the scene file as a full path below project_dir */
    char *sp = scene_file && g_path_is_absolute (scene_file)
        ? (walpe_path_is_inside (scene_file, project_dir) &&
           g_file_test (scene_file, G_FILE_TEST_IS_REGULAR) ? g_strdup (scene_file) : NULL)
        : resolve_asset (project_dir, scene_file ? scene_file : "scene.json");
    if (!sp) {
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "scene.json not found in %s", project_dir);
        return NULL;
    }
    JsonNode *root = wpj_parse_file (sp);
    char *scene_path = sp;
    if (!root) {
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "bad scene json: %s", scene_path);
        g_free (scene_path);
        return NULL;
    }
    JsonObject *rooto = json_node_get_object (root);
    /* modern node-graph format ("objects" array) delegates to scenenode.c,
     * which takes ownership of the parsed root */
    if (json_object_has_member (rooto, "objects")) {
        g_free (scene_path);
        return walpe_scenenode_new_from_root (root, project_dir, opts, err);
    }
    JsonObject *obj = wpj_obj (rooto, "object");

    SceneContent *s = g_new0 (SceneContent, 1);
    s->root = root;
    s->dir = g_path_get_dirname (scene_path);
    s->fps_cap = opts && opts->fps > 0 ? opts->fps : 30;
    s->base.type = WALPE_TYPE_SCENE;
    s->base.path = g_strdup (project_dir);
    s->base.gl_init = scene_gl_init;
    s->base.gl_render = scene_gl_render;
    s->base.gl_free = scene_gl_free;
    s->base.set_paused = scene_set_paused;
    s->base.set_fps = scene_set_fps;
    s->base.free = scene_free;

    s->scene_w = (float) wpj_num (obj, "width", 1920.0);
    s->scene_h = (float) wpj_num (obj, "height", 1080.0);

    /* camera */
    s->camera_zoom = 1.0f;
    JsonObject *cam = wpj_obj (obj, "camera");
    if (cam) {
        s->camera_zoom = (float) wpj_num (cam, "zoom", 1.0);
        if (s->camera_zoom < 0.05f)
            s->camera_zoom = 1.0f;
        double sx = 0, sy = 0;
        wpj_num2 (cam, "shift", &sx, &sy);
        s->camera_shift[0] = (float) sx;
        s->camera_shift[1] = (float) sy;
    }

    s->layers = g_array_new (FALSE, FALSE, sizeof (SceneLayer));
    parse_layers (wpj_arr (obj, "layers"), s->layers, s->dir, s);

    JsonObject *general = wpj_obj (rooto, "general");
    s->base.props = wpj_obj (general, "properties");
    g_free (scene_path);
    return &s->base;
}
