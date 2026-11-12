/* propnotes.c - see propnotes.h */
#include "propnotes.h"
#include "content.h"
#include "common/util.h"
#include <string.h>

/* a particle file's numbers that decide whether "count" can change anything */
typedef struct {
    double maxcount;     /* live particles the file allows */
    double rate;         /* particles a second of its emitters */
    double life;         /* mean lifetime, seconds */
    gboolean unknown;    /* audio-driven / timed / burst emitters: not judged */
} PtFile;

/* an option bound to the "count" of a particle object */
typedef struct {
    char *prop, *file;
    double lifetime;     /* the object's "lifetime" factor (1: none, 0: bound - not judged) */
} PtCount;

typedef struct {
    GHashTable *ok;      /* prop -> how many bindings WalPE resolves */
    GHashTable *lacking; /* prop -> feature name (static string) */
    GString *scripts;    /* every script source, concatenated */
    GHashTable *ptfiles; /* particle file name -> PtFile */
    GArray *counts;      /* PtCount */
    const char *root;    /* scan_dir: the wallpaper's folder (names are relative to it) */
} Scan;

/* scene objects with "sound" met by the scan that runs on this thread
 * (walpe_prop_notes_sound sets it to 0 and reads it) */
static _Thread_local guint scan_sound_layers;

/* general.* settings WalPE reads (scenenode.c) / the features behind the
 * ones it does not */
static const char *general_feature (const char *k)
{
    if (!strcmp (k, "clearcolor") || !strcmp (k, "zoom"))
        return NULL;
    if (g_str_has_prefix (k, "cameraparallax"))
        return NULL;   /* camera parallax (scenenode.c) */
    /* the standard bloom and the camera shake are drawn (scenenode_post.h);
     * Wallpaper Engine's HDR bloom (hdr, bloomhdr*) is not */
    if (!strcmp (k, "bloom") || !strcmp (k, "bloomstrength") || !strcmp (k, "bloomthreshold") ||
        !strcmp (k, "bloomtint") || g_str_has_prefix (k, "camerashake"))
        return NULL;
    if (g_str_has_prefix (k, "bloom") || !strcmp (k, "hdr"))
        return "HDR bloom";
    if (g_str_has_prefix (k, "camerafade"))
        return "camera fade";
    return "this scene setting";
}

static void mark (Scan *sc, const char *prop, const char *feature)
{
    if (!prop || !*prop)
        return;
    if (!feature)
        g_hash_table_replace (sc->ok, g_strdup (prop),
                              GINT_TO_POINTER (GPOINTER_TO_INT (g_hash_table_lookup (sc->ok, prop)) + 1));
    else if (!g_hash_table_contains (sc->lacking, prop))
        g_hash_table_insert (sc->lacking, g_strdup (prop), (gpointer) feature);
}

static double jdouble (JsonObject *o, const char *k, double d)
{
    JsonNode *n = o ? json_object_get_member (o, k) : NULL;
    if (!n || !JSON_NODE_HOLDS_VALUE (n))
        return d;
    if (json_node_get_value_type (n) == G_TYPE_STRING) {
        const char *str = json_node_get_string (n);
        char *end = NULL;
        double v = g_ascii_strtod (str, &end);
        return end != str ? v : d;
    }
    if (json_node_get_value_type (n) == G_TYPE_BOOLEAN)
        return json_node_get_boolean (n) ? 1.0 : 0.0;
    return json_node_get_double (n);
}

/* a WE particle file (it has "emitter"): what count has to fill */
static void note_particle_file (Scan *sc, const char *name, JsonNode *j)
{
    if (!name || !JSON_NODE_HOLDS_OBJECT (j))
        return;
    JsonObject *o = json_node_get_object (j);
    JsonArray *em = wpj_arr (o, "emitter");
    if (!em)
        return;
    PtFile *f = g_new0 (PtFile, 1);
    f->maxcount = jdouble (o, "maxcount", 100);
    for (guint i = 0; i < json_array_get_length (em); i++) {
        JsonObject *e = wpj_arr_obj (em, i);
        if (!e)
            continue;
        if (jdouble (e, "audioprocessingmode", 0) > 0 || jdouble (e, "duration", 0) > 0 ||
            jdouble (e, "instantaneous", 0) > 0)
            f->unknown = TRUE;
        f->rate += MAX (jdouble (e, "rate", 5), 0.0);
    }
    f->life = 1.0;
    JsonArray *in = wpj_arr (o, "initializer");
    for (guint i = 0; in && i < json_array_get_length (in); i++) {
        JsonObject *e = wpj_arr_obj (in, i);
        if (e && !g_strcmp0 (wpj_str (e, "name", ""), "lifetimerandom"))
            f->life = (jdouble (e, "min", 0) + jdouble (e, "max", 1)) / 2.0;
    }
    g_hash_table_replace (sc->ptfiles, g_strdup (name), f);
}

/* a scene object with a particle system whose "count" is an option */
static void note_count_binding (Scan *sc, JsonObject *o)
{
    const char *file = wpj_str (o, "particle", NULL);
    JsonObject *io = wpj_obj (o, "instanceoverride");
    JsonObject *cb = io ? wpj_obj (io, "count") : NULL;
    const char *prop = cb ? wpj_str (cb, "user", NULL) : NULL;
    if (!file || !prop || json_object_has_member (cb, "script"))
        return;
    JsonNode *lt = json_object_get_member (io, "lifetime");
    PtCount c = { g_strdup (prop), g_strdup (file),
                  !lt ? 1.0 : JSON_NODE_HOLDS_VALUE (lt) ? jdouble (io, "lifetime", 1.0) : 0.0 };
    g_array_append_val (sc->counts, c);
}

/* feature = what every binding below this node needs (NULL = resolved) */
static void walk (Scan *sc, JsonNode *n, const char *feature, int depth)
{
    if (!n || depth > 64)
        return;
    if (JSON_NODE_HOLDS_ARRAY (n)) {
        JsonArray *a = json_node_get_array (n);
        for (guint i = 0; i < json_array_get_length (a); i++)
            walk (sc, json_array_get_element (a, i), feature, depth + 1);
        return;
    }
    if (!JSON_NODE_HOLDS_OBJECT (n))
        return;
    JsonObject *o = json_node_get_object (n);
    if (!feature && json_object_has_member (o, "particle"))
        note_count_binding (sc, o);
    JsonNode *u = json_object_get_member (o, "user");
    if (u && JSON_NODE_HOLDS_VALUE (u) && json_node_get_value_type (u) == G_TYPE_STRING)
        mark (sc, json_node_get_string (u), feature);
    else if (u && JSON_NODE_HOLDS_OBJECT (u))
        mark (sc, wpj_str (json_node_get_object (u), "name", NULL), feature);
    /* a sound object: whatever is bound on it does something only while
     * wallpaper sound is switched on (off by default) */
    const char *sub = feature;
    if (json_object_has_member (o, "sound") && json_object_has_member (o, "id")) {
        sub = "scene sound";
        scan_sound_layers++;
    }
    GList *ks = json_object_get_members (o);
    for (GList *k = ks; k; k = k->next) {
        JsonNode *v = json_object_get_member (o, k->data);
        const char *key = k->data;
        if (!strcmp (key, "user"))
            continue;
        if (!strcmp (key, "script") && v && JSON_NODE_HOLDS_VALUE (v) &&
            json_node_get_value_type (v) == G_TYPE_STRING) {
            g_string_append (sc->scripts, json_node_get_string (v));
            g_string_append_c (sc->scripts, '\n');
            continue;
        }
        if (!strcmp (key, "usertextures") && v && JSON_NODE_HOLDS_ARRAY (v)) {
            JsonArray *a = json_node_get_array (v);
            for (guint i = 0; i < json_array_get_length (a); i++) {
                JsonNode *e = json_array_get_element (a, i);
                if (e && JSON_NODE_HOLDS_VALUE (e) && json_node_get_value_type (e) == G_TYPE_STRING)
                    mark (sc, json_node_get_string (e), sub);
            }
            continue;
        }
        const char *f = sub;
        if (!strcmp (key, "parallaxDepth"))
            f = NULL;   /* read by the camera parallax */
        walk (sc, v, f, depth + 1);
    }
    g_list_free (ks);
}

static void scan_scene_root (Scan *sc, JsonNode *root)
{
    if (!root || !JSON_NODE_HOLDS_OBJECT (root))
        return;
    JsonObject *ro = json_node_get_object (root);
    JsonObject *gen = wpj_obj (ro, "general");
    GList *ks = gen ? json_object_get_members (gen) : NULL;
    for (GList *k = ks; k; k = k->next)
        walk (sc, json_object_get_member (gen, k->data), general_feature (k->data), 0);
    g_list_free (ks);
    ks = json_object_get_members (ro);
    for (GList *k = ks; k; k = k->next)
        if (strcmp (k->data, "general"))
            walk (sc, json_object_get_member (ro, k->data), NULL, 0);
    g_list_free (ks);
}

/* an image layer's own material ("materials/x.json", not an effect's) that
 * names a shader other than WE's generic image ones: WalPE draws layers
 * with its own quad shader, so constants bound in such a material reach
 * nothing (3671993849's audio bars: shader "workshop/.../tint") */
static const char *layer_material_feature (const char *name, JsonNode *j)
{
    if (!g_str_has_prefix (name, "materials/") || strstr (name, "/effects/") ||
        !JSON_NODE_HOLDS_OBJECT (j))
        return NULL;
    JsonNode *pn = json_object_get_member (json_node_get_object (j), "passes");
    JsonArray *pa = pn && JSON_NODE_HOLDS_ARRAY (pn) ? json_node_get_array (pn) : NULL;
    JsonNode *p0 = pa && json_array_get_length (pa) ? json_array_get_element (pa, 0) : NULL;
    const char *sh = p0 && JSON_NODE_HOLDS_OBJECT (p0)
        ? wpj_str (json_node_get_object (p0), "shader", NULL) : NULL;
    if (!sh || g_str_has_prefix (sh, "generic") || g_str_has_prefix (sh, "effects/"))
        return NULL;
    return "custom layer shaders";
}

static guint32 rd32 (const guint8 *p)
{
    return (guint32) p[0] | (p[1] << 8) | (p[2] << 16) | ((guint32) p[3] << 24);
}

/* the .json entries of a scene.pkg (pkg.c documents the layout), parsed
 * in place: nothing is written */
static gboolean scan_pkg (Scan *sc, const char *pkg)
{
    gboolean found_scene = FALSE;
    GMappedFile *map = g_mapped_file_new (pkg, FALSE, NULL);
    if (!map)
        return FALSE;
    const guint8 *buf = (const guint8 *) g_mapped_file_get_contents (map);
    gsize len = g_mapped_file_get_length (map), off = 0;
    if (!buf || len < 24 || rd32 (buf) != 8 || memcmp (buf + 4, "PKGV", 4)) {
        g_mapped_file_unref (map);
        return FALSE;
    }
    off = 12;
    guint32 n = rd32 (buf + off);
    off += 4;
    typedef struct { const char *name; guint32 nl, foff, fsz; } Ent;
    GArray *ents = g_array_new (FALSE, FALSE, sizeof (Ent));
    for (guint32 i = 0; i < n && n <= 100000 && off + 4 <= len; i++) {
        guint32 pl = rd32 (buf + off);
        off += 4;
        if (pl > 1024 || off + pl + 8 > len)
            break;
        Ent e = { (const char *) buf + off, pl, rd32 (buf + off + pl), rd32 (buf + off + pl + 4) };
        off += pl + 8;
        g_array_append_val (ents, e);
    }
    gsize base = off;
    /* this runs on the GUI thread when a wallpaper is selected: at most
     * 8 MiB of JSON is parsed per package. Entries may name the same bytes
     * (300 entries for one 1 MB file froze the window for a minute); the
     * JSON of a real scene adds up to 1.5 MB at most (89 Workshop scenes). */
    gsize json_left = 8u << 20;
    for (guint i = 0; i < ents->len; i++) {
        Ent *e = &g_array_index (ents, Ent, i);
        if (e->nl < 5 || g_ascii_strncasecmp (e->name + e->nl - 5, ".json", 5) ||
            e->fsz > json_left || base > len || e->foff > len - base || e->fsz > len - base - e->foff)
            continue;
        json_left -= e->fsz;
        JsonNode *j = wpj_parse_data ((const char *) buf + base + e->foff, e->fsz);
        if (!j)
            continue;
        if (e->nl == 10 && !strncmp (e->name, "scene.json", 10)) {
            scan_scene_root (sc, j);
            found_scene = TRUE;
        } else {
            char *nm = g_strndup (e->name, e->nl);
            walk (sc, j, layer_material_feature (nm, j), 0);
            note_particle_file (sc, nm, j);
            g_free (nm);
        }
        json_node_unref (j);
    }
    g_array_free (ents, TRUE);
    g_mapped_file_unref (map);
    return found_scene;
}

/* an unpacked scene: scene.json and the .json files next to it. Only what
 * is the wallpaper's own: a symbolic link in the folder that leads out of it
 * was followed like any other entry - a link to a directory was searched
 * three levels deep and every .json found there parsed, on the GUI thread
 * (a link to "/" or to the home directory: thousands of files that are
 * nobody's wallpaper). Links that stay inside the folder are followed. */
static void scan_dir (Scan *sc, const char *dir, const char *scene_file, int depth, guint *budget)
{
    GDir *d = g_dir_open (dir, 0, NULL);
    if (!d)
        return;
    const char *n;
    while ((n = g_dir_read_name (d)) && *budget) {
        char *p = g_build_filename (dir, n, NULL);
        gsize l = strlen (n);
        if (sc->root && g_file_test (p, G_FILE_TEST_IS_SYMLINK) && !walpe_path_is_inside (p, sc->root)) {
            /* not the wallpaper's */
        } else if (g_file_test (p, G_FILE_TEST_IS_DIR)) {
            if (depth < 3 && n[0] != '.')
                scan_dir (sc, p, scene_file, depth + 1, budget);
        } else if (l > 5 && !g_ascii_strcasecmp (n + l - 5, ".json") && strcmp (n, "project.json")) {
            (*budget)--;
            JsonNode *j = wpj_parse_file (p);
            if (j) {
                if (depth == 0 && !g_strcmp0 (n, scene_file)) {
                    scan_scene_root (sc, j);
                } else {
                    walk (sc, j, NULL, 0);
                    if (sc->root && g_str_has_prefix (p, sc->root) && p[strlen (sc->root)] == '/')
                        note_particle_file (sc, p + strlen (sc->root) + 1, j);
                }
                json_node_unref (j);
            }
        }
        g_free (p);
    }
    g_dir_close (d);
}

/* key as a whole identifier in text. access: only where a script would
 * read a property - `x.key`, 'key', "key" (userProperties.key,
 * hasOwnProperty('key')) - not the bare word in a comment or a local name
 * ("parallax", "bloom", "music" are both option keys and plain words) */
static gboolean mentions (const char *text, const char *key, gboolean access)
{
    gsize kl = strlen (key);
    if (!kl)
        return FALSE;
    for (const char *p = text; (p = strstr (p, key)); p += kl) {
        gboolean lb = p == text || !(g_ascii_isalnum (p[-1]) || p[-1] == '_' || p[-1] == '$');
        gboolean rb = !(g_ascii_isalnum (p[kl]) || p[kl] == '_' || p[kl] == '$');
        if (access && lb)
            lb = p > text && (p[-1] == '.' || p[-1] == '\'' || p[-1] == '"' || p[-1] == '`');
        if (lb && rb)
            return TRUE;
    }
    return FALSE;
}

/* The two questions asked of every option - does a script read it, does
 * another option's condition name it - were one search of all the script
 * text and of every condition PER OPTION: options x text, both the
 * wallpaper's to choose. 5000 options with a condition each (a project.json
 * of half a megabyte) held the GUI thread for seconds, 20000 for minutes.
 * The identifiers of the text are collected once instead: a key made of
 * identifier characters is mentioned exactly where it is one of them (a
 * longest run of letters, digits, '_' and '$'; for a script read: right
 * behind . ' " or `). */
typedef struct {
    guint n;       /* texts it was found in */
    guint last;    /* 1 + the last of them */
} IdRef;

static gboolean ident_char (char c)
{
    return g_ascii_isalnum (c) || c == '_' || c == '$';
}

static gboolean ident_only (const char *key)
{
    if (!*key)
        return FALSE;
    for (const char *p = key; *p; p++)
        if (!ident_char (*p))
            return FALSE;
    return TRUE;
}

static void idents_add (GHashTable *ids, const char *text, gboolean access, guint owner)
{
    for (const char *p = text; *p;) {
        if (!ident_char (*p)) {
            p++;
            continue;
        }
        const char *b = p;
        while (ident_char (*p))
            p++;
        if (access && !(b > text && (b[-1] == '.' || b[-1] == '\'' || b[-1] == '"' || b[-1] == '`')))
            continue;
        char *id = g_strndup (b, (gsize) (p - b));
        IdRef *r = g_hash_table_lookup (ids, id);
        if (!r) {
            r = g_new0 (IdRef, 1);
            g_hash_table_insert (ids, id, r);
            id = NULL;
        }
        if (r->last != owner + 1) {
            r->last = owner + 1;
            r->n++;
        }
        g_free (id);
    }
}

/* a key with other characters in it ("my-option") is searched for as
 * before, up to this much text for all such keys together; past it the
 * answer is "mentioned" (no hint is given, never a wrong one) */
#define NOTES_SCAN_MAX (G_GUINT64_CONSTANT (1) << 30)

static gboolean is_option_type (const char *t)
{
    return t && *t && strcmp (t, "group") && strcmp (t, "text");
}

static void pt_count_clear (gpointer p)
{
    PtCount *c = p;
    g_free (c->prop);
    g_free (c->file);
}

/* Is the option `key` (project.json definition d) bound to nothing but the
 * "count" of particle systems that are full whatever it is set to? count
 * scales what a system emits; the particle file's maxcount caps what is
 * alive (weparticles.h). A system whose emitters would keep more than
 * maxcount alive at a tenth of the option's maximum already (or at its
 * minimum) shows the same picture over the option's range: 3163060610's
 * "Count" slider (0.01 .. 4) feeds five dust systems of maxcount 50 that
 * emit 100 a second for 10 - 25 s. Returns that maxcount, 0 = no. */
static int count_saturated (Scan *sc, const char *key, JsonObject *d)
{
    int n = 0, cap = 0;
    double pmax = jdouble (d, "max", 0), pmin = jdouble (d, "min", 0);
    if (!(pmax > 0))
        return 0;
    double low = MAX (pmin, pmax * 0.1);   /* a setting near the option's low end */
    for (guint i = 0; i < sc->counts->len; i++) {
        PtCount *c = &g_array_index (sc->counts, PtCount, i);
        if (strcmp (c->prop, key))
            continue;
        PtFile *f = g_hash_table_lookup (sc->ptfiles, c->file);
        if (!f || f->unknown || !(c->lifetime > 0) || !(f->maxcount >= 1))
            return 0;   /* not judged: say nothing */
        if (!(f->rate * f->life * c->lifetime * low >= f->maxcount))
            return 0;   /* the option does thin this system out */
        n++;
        cap = MAX (cap, (int) MIN (f->maxcount, 1e6));
    }
    /* every binding of the option is such a count */
    return n > 0 && n == GPOINTER_TO_INT (g_hash_table_lookup (sc->ok, key)) ? cap : 0;
}

JsonObject *walpe_prop_notes (const char *path)
{
    return walpe_prop_notes_sound (path, NULL);
}

JsonObject *walpe_prop_notes_sound (const char *path, guint *sound_layers)
{
    if (sound_layers)
        *sound_layers = 0;
    char *abs = walpe_expand_path (path);
    if (!abs)
        return NULL;
    WalpeType t = walpe_detect_type (abs, NULL);
    JsonNode *defs = NULL;
    JsonObject *out = NULL;
    if (t == WALPE_TYPE_SCENE || t == WALPE_TYPE_VIDEO || t == WALPE_TYPE_GIF || t == WALPE_TYPE_IMAGE)
        defs = walpe_content_property_defs (abs, NULL);
    if (!defs && sound_layers && t == WALPE_TYPE_SCENE) {
        /* a scene without options can have sound layers all the same: its
         * files are read for the count alone */
        defs = json_node_new (JSON_NODE_OBJECT);
        json_node_take_object (defs, json_object_new ());
    }
    if (!defs) {
        g_free (abs);
        return NULL;
    }
    JsonObject *dp = json_node_get_object (defs);
    Scan sc = { g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL),
                g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL),
                g_string_new (NULL),
                g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free),
                g_array_new (FALSE, FALSE, sizeof (PtCount)), NULL };
    scan_sound_layers = 0;
    g_array_set_clear_func (sc.counts, pt_count_clear);
    gboolean scanned = FALSE;
    if (t == WALPE_TYPE_SCENE) {
        char *base = walpe_preset_base_dir (abs, NULL);
        const char *dir = base ? base : abs;
        if (g_file_test (dir, G_FILE_TEST_IS_DIR)) {
            char *pkg = g_build_filename (dir, "scene.pkg", NULL);
            if (g_file_test (pkg, G_FILE_TEST_IS_REGULAR) && walpe_path_is_inside (pkg, dir)) {
                scanned = scan_pkg (&sc, pkg);
            } else {
                char *pjp = g_build_filename (dir, "project.json", NULL);
                JsonNode *pj = wpj_parse_file (pjp);
                char *sf = g_strdup (pj && JSON_NODE_HOLDS_OBJECT (pj)
                                         ? wpj_str (json_node_get_object (pj), "file", "scene.json")
                                         : "scene.json");
                char *sp = g_build_filename (dir, sf, NULL);
                if (g_file_test (sp, G_FILE_TEST_IS_REGULAR)) {
                    guint budget = 3000;
                    sc.root = dir;
                    scan_dir (&sc, dir, sf, 0, &budget);
                    sc.root = NULL;
                    scanned = TRUE;
                }
                g_free (sp);
                g_free (sf);
                g_free (pjp);
                if (pj)
                    json_node_unref (pj);
            }
            g_free (pkg);
        }
        g_free (base);
    } else {
        scanned = TRUE;   /* flat media: no project property is read */
    }
    if (scanned) {
        out = json_object_new ();
        GList *ks = json_object_get_members (dp);
        GHashTable *script_ids = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
        GHashTable *cond_ids = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
        guint64 scan_work = 0, cond_len = 0;
        guint owner = 0;
        idents_add (script_ids, sc.scripts->str, TRUE, 0);
        for (GList *k = ks; k; k = k->next, owner++) {
            const char *c = wpj_str (wpj_obj (dp, k->data), "condition", NULL);
            if (c) {
                idents_add (cond_ids, c, FALSE, owner);
                cond_len += strlen (c);
            }
            cond_len += 64;   /* (looking at one definition costs about that many bytes of search) */
        }
        for (GList *k = ks; k; k = k->next) {
            const char *key = k->data;
            JsonObject *d = wpj_obj (dp, key);
            if (!d || !is_option_type (wpj_str (d, "type", "")) || g_str_has_prefix (key, "walpe_"))
                continue;
            gboolean plain = ident_only (key);
            if (plain ? g_hash_table_contains (script_ids, key)
                      : (scan_work += sc.scripts->len + 64) > NOTES_SCAN_MAX ||
                        mentions (sc.scripts->str, key, TRUE))
                continue;
            if (g_hash_table_contains (sc.ok, key)) {
                /* bound and resolved - but to nothing that can show it? */
                int cap = count_saturated (&sc, key, d);
                if (cap > 0) {
                    char *nt = g_strdup_printf ("full:%d", cap);
                    json_object_set_string_member (out, key, nt);
                    g_free (nt);
                }
                continue;
            }
            const char *feat = g_hash_table_lookup (sc.lacking, key);
            if (feat) {
                char *nt = g_strconcat ("needs:", feat, NULL);
                json_object_set_string_member (out, key, nt);
                g_free (nt);
                continue;
            }
            /* a switch that shows / hides other options does its job */
            gboolean cond = FALSE;
            if (plain) {
                /* named by the condition of an option other than itself */
                const IdRef *r = g_hash_table_lookup (cond_ids, key);
                const char *own = wpj_str (d, "condition", NULL);
                guint n = r ? r->n : 0;
                if (n && own && mentions (own, key, FALSE))
                    n--;
                cond = n > 0;
            } else if ((scan_work += cond_len) > NOTES_SCAN_MAX) {
                cond = TRUE;
            } else {
                for (GList *k2 = ks; k2 && !cond; k2 = k2->next) {
                    JsonObject *d2 = k2 != k ? wpj_obj (dp, k2->data) : NULL;
                    const char *c = d2 ? wpj_str (d2, "condition", NULL) : NULL;
                    cond = c && mentions (c, key, FALSE);
                }
            }
            if (!cond)
                json_object_set_string_member (out, key, "unused");
        }
        g_hash_table_unref (script_ids);
        g_hash_table_unref (cond_ids);
        g_list_free (ks);
        if (!json_object_get_size (out))
            g_clear_pointer (&out, json_object_unref);
    }
    if (sound_layers)
        *sound_layers = scan_sound_layers;
    g_hash_table_unref (sc.ok);
    g_hash_table_unref (sc.lacking);
    g_string_free (sc.scripts, TRUE);
    g_hash_table_unref (sc.ptfiles);
    g_array_free (sc.counts, TRUE);
    json_node_unref (defs);
    g_free (abs);
    return out;
}

char *walpe_prop_note_text (const char *note)
{
    if (!note)
        return NULL;
    if (!strcmp (note, "needs:scene sound"))
        return g_strdup ("Belongs to the wallpaper's sound: no effect until sound is switched on "
                         "(Engine settings: \"Wallpaper sound\") and the volume is above 0.");
    if (g_str_has_prefix (note, "needs:"))
        return g_strdup_printf ("Needs %s, which WalPE does not support yet: no effect.", note + 6);
    if (!strcmp (note, "unused"))
        return g_strdup ("This wallpaper does not use this option: no effect.");
    if (g_str_has_prefix (note, "full:"))
        return g_strdup_printf ("Sets how many particles are emitted, but the particle systems it feeds "
                                "are full at any setting (their files allow %s at a time): little or "
                                "no visible effect.", note + 5);
    return g_strdup (note);
}
