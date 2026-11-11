/* scenenode_layers.h - "Adjust layers" inside the scene renderer.
 *
 * Not a public header: scenenode.c includes it once, after its NodeObj /
 * SceneNodeContent types (the code lives here so that scenenode.c itself
 * only carries the hooks). The data comes from common/layeradj.h.
 *
 * What the scene does with a wallpaper's adjustments:
 *  - scene_layers_sync (): when layeradj's generation changed (one atomic
 *    read per frame, scenenode_gl_render), fetch this wallpaper's list,
 *    match the keys to nodes (id, then name; unmatched keys are logged
 *    once and ignored), fetch the keys to outline, note a pending request
 *    for the layer list.
 *  - scene_layers_apply (): called by node_world () for every node, but
 *    only while the scene has at least one matched adjustment. The node's
 *    world matrix - already holding its origin / angles / scale as set by
 *    the scene file, the user options, property animations and this
 *    frame's SceneScript update (), and its parents' matrices - is scaled
 *    about the node's own origin and moved by (dx, dy) scene pixels. So a
 *    script that writes the origin every frame still ends up offset, and
 *    children (their world = parent world * local) follow. The offset is
 *    in scene space whatever the parents' scale or rotation: the view
 *    (cover / crop, camera layer, general.zoom) comes after it, so it is
 *    the same on every monitor size.
 *  - particle systems simulate and project in the node's unadjusted
 *    space; scene_layers_unadjust () gives that matrix and the scene ->
 *    clip matrix with the adjustment in it, i.e. the system's picture is
 *    moved and scaled as a whole (sprites of "world space" systems and
 *    the perspective of perspective systems included).
 *  - scene_layers_outline (): thin outlines of the selected layers.
 *  - scene_layers_publish (): the layer list for "walpe-ctl layers" / the
 *    GUI, with this frame's positions. */
#pragma once
#include "common/layeradj.h"

typedef struct {
    char *key;           /* the saved key it came from */
    float dx, dy, scale;
    int vis;             /* WalpeLayerVis */
} SceneLayerAdj;

static gboolean node_quad_matrix (NodeObj *nv, WalpeMat3 *out);

static void scene_layers_clear (SceneNodeContent *s)
{
    for (guint i = 0; s->ladj && i < s->ladj->len; i++)
        g_free (g_array_index (s->ladj, SceneLayerAdj, i).key);
    if (s->ladj)
        g_array_set_size (s->ladj, 0);
    for (guint i = 0; i < s->nodes->len; i++)
        g_array_index (s->nodes, NodeObj, i).adj = 0;
    s->n_adj = 0;
}

static WalpeLayerRef *scene_layers_refs (SceneNodeContent *s)
{
    guint n = s->nodes->len;
    WalpeLayerRef *refs = g_new0 (WalpeLayerRef, n ? n : 1);
    for (guint i = 0; i < n; i++) {
        const NodeObj *nv = &g_array_index (s->nodes, NodeObj, i);
        refs[i].id = nv->id;
        refs[i].name = nv->name;
    }
    return refs;
}

static void scene_layers_sync (SceneNodeContent *s, guint gen)
{
    if (!s->lay_gen)   /* the first frame: from now on a list request is worth waiting for */
        walpe_layers_publish (s, s->origin, 0, NULL);
    s->lay_gen = gen;
    guint n = s->nodes->len;
    GPtrArray *adj = walpe_layers_active (s->origin);
    char **hk = walpe_layers_highlight (s->origin);
    WalpeLayerRef *refs = adj || hk ? scene_layers_refs (s) : NULL;

    if (adj || s->n_adj)
        scene_layers_clear (s);
    GString *miss = NULL;
    for (guint k = 0; adj && k < adj->len; k++) {
        const WalpeLayerAdj *a = adj->pdata[k];
        int i = walpe_layer_find (refs, n, a->key);
        if (i < 0) {
            if (!s->lay_missing)
                s->lay_missing = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
            if (g_hash_table_add (s->lay_missing, g_strdup (a->key))) {
                if (!miss)
                    miss = g_string_new (NULL);
                g_string_append_printf (miss, "%s\"%s\"", miss->len ? ", " : "", a->key);
            }
            continue;
        }
        NodeObj *nv = &g_array_index (s->nodes, NodeObj, i);
        SceneLayerAdj la = { g_strdup (a->key), (float) a->dx, (float) a->dy, (float) a->scale,
                             a->vis };
        if (!s->ladj)
            s->ladj = g_array_new (FALSE, FALSE, sizeof (SceneLayerAdj));
        if (nv->adj) {
            /* two saved keys mean the same layer: the later one counts */
            SceneLayerAdj *old = &g_array_index (s->ladj, SceneLayerAdj, nv->adj - 1);
            g_free (old->key);
            *old = la;
        } else {
            g_array_append_val (s->ladj, la);
            nv->adj = (int) s->ladj->len;
        }
    }
    s->n_adj = s->ladj ? s->ladj->len : 0;
    if (miss) {
        g_message ("scenenode: layer adjustment(s) for %s match no layer of %s and are ignored "
                   "(kept in the config)", miss->str, s->origin);
        g_string_free (miss, TRUE);
    }
    if (adj)
        g_ptr_array_unref (adj);

    if (s->hl)
        g_array_set_size (s->hl, 0);
    for (guint k = 0; hk && hk[k]; k++) {
        int i = walpe_layer_find (refs, n, hk[k]);
        if (i < 0)
            continue;
        if (!s->hl)
            s->hl = g_array_new (FALSE, FALSE, sizeof (int));
        g_array_append_val (s->hl, i);
    }
    g_strfreev (hk);
    g_free (refs);

    guint64 want = walpe_layers_list_serial ();
    if (want != s->lay_list_seen) {
        s->lay_list_seen = want;
        s->lay_publish = TRUE;
    }
}

/* node_world (): nv->world holds the node's own transform under its
 * parents'; pn = its parent or NULL. Only called while s->n_adj > 0. */
static inline void scene_layers_apply (SceneNodeContent *s, NodeObj *nv, const NodeObj *pn)
{
    /* accumulated user transform of the parents: x' = as * x + (atx, aty) */
    float as = 1.0f, atx = 0.0f, aty = 0.0f;
    if (pn && pn->ua[0] > 0.0f) {
        as = pn->ua[0];
        atx = pn->ua[1];
        aty = pn->ua[2];
    }
    if (nv->adj) {
        const SceneLayerAdj *a = &g_array_index (s->ladj, SceneLayerAdj, nv->adj - 1);
        float *m = nv->world.m;
        const float px = m[6], py = m[7];   /* the node's origin in the scene */
        m[0] *= a->scale;
        m[1] *= a->scale;
        m[3] *= a->scale;
        m[4] *= a->scale;
        m[6] = px + a->dx;
        m[7] = py + a->dy;
        atx = a->scale * (atx - px) + px + a->dx;
        aty = a->scale * (aty - py) + py + a->dy;
        as *= a->scale;
        if (a->vis == WALPE_LAYER_VIS_HIDDEN)
            nv->world_visible = FALSE;
        else if (a->vis == WALPE_LAYER_VIS_SHOWN)
            nv->world_visible = pn ? pn->world_visible : TRUE;
    }
    nv->ua[0] = as;
    nv->ua[1] = atx;
    nv->ua[2] = aty;
}

/* does the user transform (its own or a parent's) reach this node? */
static inline gboolean scene_layers_touched (const SceneNodeContent *s, const NodeObj *nv)
{
    return s->n_adj && nv->ua[0] > 0.0f &&
           (nv->ua[0] != 1.0f || nv->ua[1] != 0.0f || nv->ua[2] != 0.0f);
}

/* the node's world matrix without the user transform, and scene -> clip
 * with it: clip_u * world0 == clip * world */
static void scene_layers_unadjust (const NodeObj *nv, const WalpeMat3 *clip, WalpeMat3 *world0,
                                   WalpeMat3 *clip_u)
{
    const float as = nv->ua[0], atx = nv->ua[1], aty = nv->ua[2];
    *world0 = nv->world;
    world0->m[0] /= as;
    world0->m[1] /= as;
    world0->m[3] /= as;
    world0->m[4] /= as;
    world0->m[6] = (world0->m[6] - atx) / as;
    world0->m[7] = (world0->m[7] - aty) / as;
    WalpeMat3 u = { { as, 0, 0, 0, as, 0, atx, aty, 1 } };
    walpe_mat3_mul (clip_u, clip, &u);
}

/* ---------------------------------------------------------- outlines */

/* a line from a to b (pixels), `thick` pixels wide, square ends */
static void scene_layers_line (GLuint tex, const float a[2], const float b[2], float thick,
                               int w, int h, const float rgb[3], float alpha)
{
    float dx = b[0] - a[0], dy = b[1] - a[1];
    float len = hypotf (dx, dy);
    if (len < 0.01f)
        return;
    float ux = dx / len, uy = dy / len;
    float hl = len / 2.0f + thick / 2.0f, ht = thick / 2.0f;
    float cx = (a[0] + b[0]) / 2.0f, cy = (a[1] + b[1]) / 2.0f;
    /* unit quad (+-1) -> clip: column 0 along the line, column 1 across */
    const float m[9] = { ux * hl * 2.0f / w, uy * hl * 2.0f / h, 0,
                         -uy * ht * 2.0f / w, ux * ht * 2.0f / h, 0,
                         cx * 2.0f / w - 1.0f, cy * 2.0f / h - 1.0f, 1 };
    walpe_glutil_draw_quad_tint (tex, TRUE, alpha, m, WALPE_BLEND_NORMAL, rgb);
}

/* outline the layers the user has selected in "Adjust layers" (drawn last,
 * over the finished frame; nothing here runs unless a highlight is set) */
static void scene_layers_outline (SceneNodeContent *s, const WalpeMat3 *tail, int w, int h)
{
    if (!s->hl_tex) {
        const guint8 white[4] = { 0xff, 0xff, 0xff, 0xff };
        glGenTextures (1, &s->hl_tex);
        glBindTexture (GL_TEXTURE_2D, s->hl_tex);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    }
    glBindFramebuffer (GL_FRAMEBUFFER, 0);
    glViewport (0, 0, w, h);
    static const float dark[3] = { 0.0f, 0.0f, 0.0f }, lit[3] = { 1.0f, 0.80f, 0.10f };
    for (int pass = 0; pass < 2; pass++) {
        for (guint k = 0; k < s->hl->len; k++) {
            int i = g_array_index (s->hl, int, k);
            if (i < 0 || (guint) i >= s->nodes->len)
                continue;
            NodeObj *nv = &g_array_index (s->nodes, NodeObj, i);
            WalpeMat3 q, m = *tail;
            float c[4][2];
            if (nv->compose != 2 && node_quad_matrix (nv, &q)) {
                walpe_mat3_mul (&m, &m, &q);
                static const float cor[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
                for (int v = 0; v < 4; v++) {
                    c[v][0] = (m.m[0] * cor[v][0] + m.m[3] * cor[v][1] + m.m[6] + 1.0f) * w / 2.0f;
                    c[v][1] = (m.m[1] * cor[v][0] + m.m[4] * cor[v][1] + m.m[7] + 1.0f) * h / 2.0f;
                }
            } else {
                /* nothing with a rectangle (particle system, group, full-
                 * screen layer): a diamond on its origin */
                float ox = (tail->m[0] * nv->world.m[6] + tail->m[3] * nv->world.m[7] + tail->m[6] +
                            1.0f) * w / 2.0f;
                float oy = (tail->m[1] * nv->world.m[6] + tail->m[4] * nv->world.m[7] + tail->m[7] +
                            1.0f) * h / 2.0f;
                const float r = 22.0f;
                c[0][0] = ox - r; c[0][1] = oy;
                c[1][0] = ox;     c[1][1] = oy - r;
                c[2][0] = ox + r; c[2][1] = oy;
                c[3][0] = ox;     c[3][1] = oy + r;
            }
            /* a layer that is not shown right now: a fainter outline */
            float al = nv->world_visible && nv->alpha > 0 ? 1.0f : 0.55f;
            for (int e = 0; e < 4; e++)
                scene_layers_line (s->hl_tex, c[e], c[(e + 1) % 4], pass ? 2.0f : 5.0f, w, h,
                                   pass ? lit : dark, pass ? al : al * 0.75f);
        }
    }
}

/* ------------------------------------------------------- layer list */

static const char *scene_layers_type (const SceneNodeContent *s, const NodeObj *nv, guint i)
{
    if ((int) i == s->cam_node)
        return "camera";
    switch (nv->kind) {
    case NK_TEXT:
        return "text";
    case NK_PARTICLE:
        return "particles";
    case NK_IMAGE:
        return nv->compose == 2 ? "fullscreen" : nv->compose ? "compose"
             : nv->canvas ? "solid" : nv->video >= 0 ? "video" : "image";
    default:
        return "group";
    }
}

static void jb_num2 (JsonBuilder *b, const char *k, double x, double y)
{
    json_builder_set_member_name (b, k);
    json_builder_begin_array (b);
    json_builder_add_double_value (b, isfinite (x) ? round (x * 100.0) / 100.0 : 0);
    json_builder_add_double_value (b, isfinite (y) ? round (y * 100.0) / 100.0 : 0);
    json_builder_end_array (b);
}

/* this frame's layers as JSON {"size": [w, h], "layers": [...]}, handed to
 * layeradj (the daemon's "layers" request waits for it). Positions are
 * scene pixels, y up. */
static void scene_layers_publish (SceneNodeContent *s)
{
    guint n = s->nodes->len;
    WalpeLayerRef *refs = scene_layers_refs (s);
    /* nothing drawn and nothing below it (a sound, a light, an empty
     * group): listed as "none", there is nothing to place */
    gboolean *has_kid = g_new0 (gboolean, n ? n : 1);
    for (guint i = 0; i < n; i++) {
        int p = g_array_index (s->nodes, NodeObj, i).parent;
        if (p >= 0 && (guint) p < n)
            has_kid[p] = TRUE;
    }
    JsonBuilder *b = json_builder_new ();
    json_builder_begin_object (b);
    jb_num2 (b, "size", s->scene_w, s->scene_h);
    json_builder_set_member_name (b, "layers");
    json_builder_begin_array (b);
    for (guint i = 0; i < n; i++) {
        NodeObj *nv = &g_array_index (s->nodes, NodeObj, i);
        char *key = walpe_layer_key_for (refs, n, i);
        json_builder_begin_object (b);
        json_builder_set_member_name (b, "key");
        json_builder_add_string_value (b, key);
        g_free (key);
        json_builder_set_member_name (b, "id");
        json_builder_add_int_value (b, nv->id);
        json_builder_set_member_name (b, "name");
        json_builder_add_string_value (b, nv->name ? nv->name : "");
        json_builder_set_member_name (b, "type");
        json_builder_add_string_value (b, nv->kind == NK_EMPTY && !has_kid[i] &&
                                          (int) i != s->cam_node ? "none"
                                                                 : scene_layers_type (s, nv, i));
        json_builder_set_member_name (b, "parent");
        json_builder_add_int_value (b, nv->parent);
        /* visible: what the wallpaper itself says; shown: drawn this frame
         * (parents, alpha and the user's override counted in) */
        json_builder_set_member_name (b, "visible");
        json_builder_add_boolean_value (b, nv->visible);
        json_builder_set_member_name (b, "shown");
        json_builder_add_boolean_value (b, nv->world_visible && nv->alpha > 0 && !nv->fx_skip);
        jb_num2 (b, "pos", nv->world.m[6], nv->world.m[7]);
        WalpeMat3 q;
        if (nv->compose != 2 && node_quad_matrix (nv, &q)) {
            float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
            for (int v = 0; v < 4; v++) {
                float qx = (v & 1) ? 1.0f : -1.0f, qy = (v & 2) ? 1.0f : -1.0f;
                float x = q.m[0] * qx + q.m[3] * qy + q.m[6], y = q.m[1] * qx + q.m[4] * qy + q.m[7];
                x0 = fminf (x0, x); x1 = fmaxf (x1, x);
                y0 = fminf (y0, y); y1 = fmaxf (y1, y);
            }
            jb_num2 (b, "size", 2.0 * hypotf (q.m[0], q.m[1]), 2.0 * hypotf (q.m[3], q.m[4]));
            json_builder_set_member_name (b, "bounds");
            json_builder_begin_array (b);
            const float bb[4] = { x0, y0, x1, y1 };
            for (int v = 0; v < 4; v++)
                json_builder_add_double_value (b, isfinite (bb[v]) ? round (bb[v] * 100.0) / 100.0
                                                                   : 0);
            json_builder_end_array (b);
        }
        /* something to recognise it by when the name says nothing */
        if (nv->kind == NK_TEXT && nv->text_value && *nv->text_value) {
            char *t = g_utf8_substring (nv->text_value, 0,
                                        MIN (80, g_utf8_strlen (nv->text_value, -1)));
            json_builder_set_member_name (b, "text");
            json_builder_add_string_value (b, t);
            g_free (t);
        } else if (nv->tex_src) {
            char *bn = g_path_get_basename (nv->tex_src);
            json_builder_set_member_name (b, "file");
            json_builder_add_string_value (b, bn);
            g_free (bn);
        }
        if (nv->scripted) {
            json_builder_set_member_name (b, "scripted");
            json_builder_add_boolean_value (b, TRUE);
        }
        if (nv->adj) {
            const SceneLayerAdj *a = &g_array_index (s->ladj, SceneLayerAdj, nv->adj - 1);
            json_builder_set_member_name (b, "adjust");
            json_builder_begin_object (b);
            json_builder_set_member_name (b, "key");   /* as saved (may be an older spelling) */
            json_builder_add_string_value (b, a->key);
            /* (floats here: rounded so that 1.4 reads 1.4, not 1.39999998) */
            json_builder_set_member_name (b, "dx");
            json_builder_add_double_value (b, round ((double) a->dx * 100.0) / 100.0);
            json_builder_set_member_name (b, "dy");
            json_builder_add_double_value (b, round ((double) a->dy * 100.0) / 100.0);
            json_builder_set_member_name (b, "scale");
            json_builder_add_double_value (b, round ((double) a->scale * 1e4) / 1e4);
            json_builder_set_member_name (b, "visible");
            json_builder_add_string_value (b, walpe_layer_vis_name (a->vis));
            json_builder_end_object (b);
        }
        json_builder_end_object (b);
    }
    json_builder_end_array (b);
    json_builder_end_object (b);
    JsonNode *root = json_builder_get_root (b);
    char *json = json_to_string (root, FALSE);
    json_node_unref (root);
    g_object_unref (b);
    g_free (refs);
    g_free (has_kid);
    walpe_layers_publish (s, s->origin, s->lay_list_seen, json);
}

/* scenenode_free () */
static void scene_layers_free (SceneNodeContent *s)
{
    walpe_layers_unpublish (s);
    for (guint i = 0; s->ladj && i < s->ladj->len; i++)
        g_free (g_array_index (s->ladj, SceneLayerAdj, i).key);
    g_clear_pointer (&s->ladj, g_array_unref);
    g_clear_pointer (&s->hl, g_array_unref);
    g_clear_pointer (&s->lay_missing, g_hash_table_unref);
}
