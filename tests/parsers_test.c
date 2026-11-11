/* parsers_test.c - hostile wallpaper files, second set. A
 * wallpaper from the Workshop is input somebody else wrote; what its files
 * must not be able to do:
 *   json     - the nesting limit holds when the text hides its brackets from
 *              the count behind what json-glib also accepts (comments,
 *              'single-quoted' strings); a json file of tens of MiB is not
 *              read; ordinary files, also ones with a comment, still parse
 *   links    - a symbolic link inside a wallpaper that points out of it is
 *              not opened as the wallpaper's asset; links that stay inside
 *              and a wallpaper folder reached through a link work
 *   tex      - a texture file is decoded as a picture only by the decoders
 *              of the formats it may wrap (not a Windows cursor behind
 *              "RIFF"); a file full of picture signatures costs a bounded
 *              number of decode attempts
 *   pkg      - directory chains and entry counts are bounded, the cache's
 *              bookkeeping names cannot be written by a package, a refused
 *              package leaves no copy in the cache; a normal one extracts
 *   gl       - (needs a surfaceless GL 3.3 context, else skipped) layer
 *              pictures of the old scene format: only picture formats a
 *              wallpaper is made of, counted against the texture budget;
 *              its text layers are bounded like those of the current format;
 *              a particle sprite sheet with a frame rectangle far outside
 *              the picture is not measured (it was read out of bounds)
 *              numbers of a scene / text / particle file that no int holds
 *              ("id": 1e30, "maxcount": 1e999, a size of "nan nan") are
 *              converted in a defined way
 * All files are generated here. Private temp dir = HOME and the XDG dirs.
 *
 *   model    - a puppet model of hundreds of MiB is not read into memory
 *   mp4      - the video reader (a video wallpaper's file, the video inside
 *              a .tex): a sample table that names millions of frames without
 *              storing them is refused, a timescale the player cannot hold is
 *              refused, an empty tkhd box at the very end of a file that
 *              fills its last page is not read past the file; an ordinary
 *              file opens
 * Run: meson test -C build parsers -v   (parsers-test json|links|tex|model|mp4|pkg|gl = one group) */
#include "engine/content.h"
#include "engine/glutil.h"
#include "engine/mp4demux.h"
#include "engine/pkg.h"
#include "engine/puppet.h"
#include "common/tex.h"
#include "common/util.h"
#include <epoxy/gl.h>
#include <epoxy/egl.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gstdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures;
static char *T;      /* temp root (realpath) */

#define CHECK(c) do { if (!(c)) { g_printerr ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                                 failures++; } } while (0)

static char *P (const char *rel)
{
    return g_build_filename (T, rel, NULL);
}

static void mkfile (const char *rel, const void *data, gssize len)
{
    char *p = P (rel);
    char *d = g_path_get_dirname (p);
    g_mkdir_with_parents (d, 0755);
    if (!g_file_set_contents (p, data, len, NULL))
        g_error ("cannot write %s", p);
    g_free (d);
    g_free (p);
}

static void put16 (GByteArray *a, guint16 v)
{
    guint8 b[2] = { (guint8) v, (guint8) (v >> 8) };
    g_byte_array_append (a, b, 2);
}

static void put32 (GByteArray *a, guint32 v)
{
    guint8 b[4] = { (guint8) v, (guint8) (v >> 8), (guint8) (v >> 16), (guint8) (v >> 24) };
    g_byte_array_append (a, b, 4);
}

static double cpu_now (void)
{
    return (double) clock () / CLOCKS_PER_SEC;
}

/* messages are counted, not printed: the hostile files produce many */
static int warnings, criticals;
static GString *logged;     /* message texts (bounded), for the checks that look for one */
static GLogWriterOutput log_writer (GLogLevelFlags level, const GLogField *fields, gsize n,
                                    gpointer user)
{
    (void) user;
    if (level & (G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL))
        warnings++;
    if (level & G_LOG_LEVEL_CRITICAL)
        criticals++;
    for (gsize i = 0; i < n && logged && logged->len < 1024 * 1024; i++)
        if (!g_strcmp0 (fields[i].key, "MESSAGE") && fields[i].value) {
            if (fields[i].length < 0)
                g_string_append (logged, fields[i].value);
            else
                g_string_append_len (logged, fields[i].value, MIN (fields[i].length, 4096));
            g_string_append_c (logged, '\n');
        }
    return G_LOG_WRITER_HANDLED;
}

/* ------------------------------------------------------------------- json */

/* {"a": <prefix> [[[[ ... ]]]] } nested `depth` deep */
static char *nested (const char *prefix, int depth, gsize *len)
{
    GString *g = g_string_new ("{\"a\":");
    g_string_append (g, prefix);
    for (int i = 0; i < depth; i++)
        g_string_append_c (g, '[');
    for (int i = 0; i < depth; i++)
        g_string_append_c (g, ']');
    g_string_append_c (g, '}');
    *len = g->len;
    return g_string_free (g, FALSE);
}

/* does this json-glib take the text at all (it decides what is "lax") */
static gboolean jsonglib_takes (const char *txt)
{
    JsonParser *p = json_parser_new ();
    gboolean ok = json_parser_load_from_data (p, txt, -1, NULL);
    g_object_unref (p);
    return ok;
}

static void test_json (void)
{
    gsize len = 0;
    /* plain nesting: 400 levels parse, 600 do not (as before) */
    char *t = nested ("", 400, &len);
    JsonNode *n = wpj_parse_data (t, (gssize) len);
    CHECK (n != NULL);
    g_clear_pointer (&n, json_node_unref);
    g_free (t);
    t = nested ("", 600, &len);
    CHECK (wpj_parse_data (t, (gssize) len) == NULL);
    g_free (t);

    /* 300000 levels behind something that holds one '"' and that json-glib
     * (up to 1.8) skips or reads as a string of its own: the count used to
     * see an open string from there on, json-glib saw the brackets and
     * recursed until the stack ended. Each of these must come back NULL -
     * and come back. */
    static const char *const hide[] = {
        "/*\"*/",             /* C comment */
        "/\"/",               /* json-glib <= 1.8: a comment from '/' to '/' */
        "//\"\n",             /* a line comment, where a scanner has them */
        "/*'*/ /*\"*/",       /* a quote of the other kind first */
        "#\"\n",              /* a hash comment, where a scanner has them */
    };
    for (guint i = 0; i < G_N_ELEMENTS (hide); i++) {
        t = nested (hide[i], 300000, &len);
        CHECK (wpj_parse_data (t, (gssize) len) == NULL);
        g_free (t);
    }
    /* ... and as a member before the nested one: {'"':1,"a":[[[ ... */
    {
        GString *g = g_string_new ("{'\"':1,\"a\":");
        for (int i = 0; i < 300000; i++)
            g_string_append_c (g, '[');
        for (int i = 0; i < 300000; i++)
            g_string_append_c (g, ']');
        g_string_append_c (g, '}');
        CHECK (wpj_parse_data (g->str, (gssize) g->len) == NULL);
        g_string_free (g, TRUE);
    }
    /* "/" "*" "/" is a whole comment for json-glib <= 1.8 and only the
     * start of one for a reader of C comments, which then looks for the
     * end inside the member name (found by the random texts below, against
     * a first version of the fix that knew fewer readings) */
    {
        GString *g = g_string_new ("{/*/ /\"/ \"a*/\":");
        for (int i = 0; i < 300000; i++)
            g_string_append_c (g, '[');
        for (int i = 0; i < 300000; i++)
            g_string_append_c (g, ']');
        g_string_append_c (g, '}');
        CHECK (wpj_parse_data (g->str, (gssize) g->len) == NULL);
        g_string_free (g, TRUE);
    }
    /* the file reader takes the same way */
    t = nested ("/*\"*/", 300000, &len);
    mkfile ("json/deep.json", t, (gssize) len);
    g_free (t);
    char *f = P ("json/deep.json");
    CHECK (wpj_parse_file (f) == NULL);
    g_free (f);

    /* what parsed before still parses: quotes and slashes inside strings,
     * and, where this json-glib reads them, a comment or a 'string' */
    n = wpj_parse_data ("{\"path\":\"materials/a's \\\"b\\\".tex\",\"u\":\"http://x/y\","
                        "\"l\":[[1,2],[3,[4]]]}", -1);
    CHECK (n && json_object_has_member (json_node_get_object (n), "l"));
    g_clear_pointer (&n, json_node_unref);
    static const char *const lax[] = {
        "/* made by hand */ {\"a\":[1,2,{\"b\":[3]}]}",
        "{'a':[1,2,{'b':[3]}]}",
        "{\"a\":[1, /* two */ 2]}",
    };
    for (guint i = 0; i < G_N_ELEMENTS (lax); i++) {
        n = wpj_parse_data (lax[i], -1);
        CHECK ((n != NULL) == jsonglib_takes (lax[i]));
        g_clear_pointer (&n, json_node_unref);
    }
    /* many brackets inside strings are no nesting */
    {
        GString *g = g_string_new ("{\"s\":\"");
        for (int i = 0; i < 5000; i++)
            g_string_append (g, "[{");
        g_string_append (g, "\",\"t\":'x'}");
        n = wpj_parse_data (g->str, (gssize) g->len);
        CHECK ((n != NULL) == jsonglib_takes (g->str));
        g_clear_pointer (&n, json_node_unref);
        g_string_free (g, TRUE);
    }

    /* The limit against the installed json-glib, with random texts: member
     * names and values in either kind of quotes, comments of every kind,
     * all filled with quotes, slashes, stars, backslashes and brackets,
     * then nesting around the limit. Whatever comes back as a tree must not
     * nest deeper than the limit - however this json-glib reads the rest. */
    {
        static const char *const junk[] = { "\"", "'", "/", "*", "\\", "[", "]", "{", "}", "x", " ",
                                            "\n", "/*", "*/", "//", "\\\"", "\\'", "\\\\", ":",
                                            ",", "1", "#", "//\n", "/*/", "*//" };
        GRand *r = g_rand_new_with_seed (12);
        int parsed = 0, too_deep = 0;
        for (int it = 0; it < 12000; it++) {
            GString *g = g_string_new ("{");
            int members = g_rand_int_range (r, 0, 3);
            for (int m = 0; m <= members; m++) {
                for (int k = g_rand_int_range (r, 0, 4); k > 0; k--) {
                    int kind = g_rand_int_range (r, 0, 5);
                    const char *open = kind == 0 ? "/*" : kind == 1 ? "/" : kind == 2 ? "//"
                                     : kind == 3 ? "#" : " ";
                    const char *close = kind == 0 ? "*/" : kind == 1 ? "/" : kind < 4 ? "\n" : " ";
                    g_string_append (g, open);
                    for (int j = g_rand_int_range (r, 0, 6); j > 0 && kind < 4; j--) {
                        const char *w = junk[g_rand_int_range (r, 0, G_N_ELEMENTS (junk))];
                        if (!strstr (w, close) && !(kind == 1 && strchr (w, '*')))
                            g_string_append (g, w);
                    }
                    g_string_append (g, close);
                }
                for (int part = 0; part < (m < members ? 2 : 1); part++) {
                    const char *q = g_rand_boolean (r) ? "'" : "\"";
                    g_string_append_printf (g, "%sk%d", q, m);
                    for (int j = g_rand_int_range (r, 0, 6); j > 0; j--) {
                        const char *w = junk[g_rand_int_range (r, 0, G_N_ELEMENTS (junk))];
                        if (!strstr (w, q) && !strchr (w, '\\'))
                            g_string_append (g, w);
                    }
                    g_string_append_printf (g, "%s%s", q, part ? "," : ":");
                }
                if (m == members) {
                    int depth = g_rand_int_range (r, 495, 540);
                    for (int i = 0; i < depth; i++)
                        g_string_append_c (g, '[');
                    for (int i = 0; i < depth; i++)
                        g_string_append_c (g, ']');
                }
            }
            g_string_append_c (g, '}');
            n = wpj_parse_data (g->str, (gssize) g->len);
            if (n) {
                /* the arrays below the last member: the tree's depth */
                int depth = 1;
                JsonNode *v = n;
                GList *ks = json_object_get_members (json_node_get_object (n));
                for (GList *k = ks; k; k = k->next) {
                    JsonNode *e = json_object_get_member (json_node_get_object (n), k->data);
                    if (JSON_NODE_HOLDS_ARRAY (e))
                        v = e;
                }
                g_list_free (ks);
                while (v != n && JSON_NODE_HOLDS_ARRAY (v) && json_array_get_length (json_node_get_array (v))) {
                    v = json_array_get_element (json_node_get_array (v), 0);
                    depth++;
                }
                if (v != n && JSON_NODE_HOLDS_ARRAY (v))
                    depth++;
                parsed++;
                if (depth > 512 && !too_deep++)
                    g_printerr ("FAIL a tree %d deep came back; the text starts: %.100s\n", depth, g->str);
                json_node_unref (n);
            }
            g_string_free (g, TRUE);
        }
        g_rand_free (r);
        CHECK (too_deep == 0);
        CHECK (parsed > 100);       /* the generator still makes texts json-glib takes */
    }

    /* size: 1 MiB of json is read, 40 MiB is not (it was: the text, then a
     * tree of many times its size) */
    {
        GString *g = g_string_new ("{\"a\":1}");
        for (int i = 0; i < 1024 * 1024; i++)
            g_string_append_c (g, ' ');
        mkfile ("json/mib.json", g->str, (gssize) g->len);
        g_string_free (g, TRUE);
        f = P ("json/mib.json");
        n = wpj_parse_file (f);
        CHECK (n != NULL);
        g_clear_pointer (&n, json_node_unref);
        g_free (f);
        f = P ("json/big.json");
        FILE *fp = fopen (f, "wb");
        CHECK (fp != NULL);
        if (fp) {
            char *blank = g_malloc (1024 * 1024);
            memset (blank, ' ', 1024 * 1024);
            fputs ("{\"a\":1}", fp);
            for (int i = 0; i < 40; i++)
                fwrite (blank, 1, 1024 * 1024, fp);
            fclose (fp);
            g_free (blank);
        }
        n = wpj_parse_file (f);
        CHECK (n == NULL);
        g_clear_pointer (&n, json_node_unref);
        g_unlink (f);
        g_free (f);
    }
}

/* ------------------------------------------------------------------ links */

static void test_links (void)
{
    mkfile ("ln/wp/materials/a.tex", "texture", -1);
    mkfile ("ln/wp/scene.json", "{}", -1);
    mkfile ("ln/outside/secret.tex", "secret", -1);
    mkfile ("ln/up.tex", "also not the wallpaper's", -1);
    char *wp = P ("ln/wp"), *out = P ("ln/outside/secret.tex"), *outdir = P ("ln/outside");
    char *l1 = P ("ln/wp/materials/leak.tex"), *l2 = P ("ln/wp/materials/alias.tex");
    char *l3 = P ("ln/wp/linkdir"), *l4 = P ("ln/wp/inner"), *l5 = P ("ln/wp/materials/gone.tex");
    char *via = P ("ln/via");
    if (symlink (out, l1) != 0 || symlink ("a.tex", l2) != 0 || symlink (outdir, l3) != 0 ||
        symlink ("materials", l4) != 0 || symlink ("/nonexistent/x", l5) != 0 ||
        symlink (wp, via) != 0) {
        g_printerr ("parsers-test: no symbolic links here, links part skipped\n");
    } else {
        char *got;
        /* out of the wallpaper: by a file link, through a directory link */
        CHECK (walpe_asset_path (wp, "materials/leak.tex") == NULL);
        CHECK (walpe_asset_path (wp, "linkdir/secret.tex") == NULL);
        /* ".." after a link: by its text "linkdir/../up.tex" is the
         * wallpaper's own up.tex, the kernel goes through the link's target
         * to the file next to "outside" */
        CHECK (walpe_asset_path (wp, "linkdir/../up.tex") == NULL);
        char *abs1 = g_build_filename (wp, "materials", "leak.tex", NULL);
        CHECK (!walpe_path_is_inside (abs1, wp));
        g_free (abs1);
        /* inside it: allowed as before */
        got = walpe_asset_path (wp, "materials/alias.tex");
        CHECK (got != NULL);
        g_free (got);
        got = walpe_asset_path (wp, "inner/a.tex");
        CHECK (got != NULL);
        g_free (got);
        got = walpe_asset_path (wp, "materials/a.tex");
        CHECK (got != NULL);
        g_free (got);
        /* the wallpaper folder itself reached through a link */
        got = walpe_asset_path (via, "materials/a.tex");
        CHECK (got != NULL);
        g_free (got);
        CHECK (walpe_asset_path (via, "materials/leak.tex") == NULL);
        /* a link to nothing is no asset; a name that does not exist is
         * still "inside" by its text (callers report it as missing) */
        CHECK (walpe_asset_path (wp, "materials/gone.tex") == NULL);
        char *miss = g_build_filename (wp, "materials", "missing.tex", NULL);
        CHECK (walpe_path_is_inside (miss, wp));
        g_free (miss);
    }
    g_free (wp); g_free (out); g_free (outdir);
    g_free (l1); g_free (l2); g_free (l3); g_free (l4); g_free (l5); g_free (via);
}

/* -------------------------------------------------------------------- tex */

/* a 2x2 Windows icon inside an animated cursor (RIFF ACON): what gdk-pixbuf's
 * "ani" decoder reads, here only to see that the texture loader does not
 * ask it */
static GByteArray *ani_bytes (void)
{
    GByteArray *ico = g_byte_array_new ();
    put16 (ico, 0); put16 (ico, 1); put16 (ico, 1);            /* ICONDIR */
    guint8 ent[4] = { 2, 2, 0, 0 };
    g_byte_array_append (ico, ent, 4);
    put16 (ico, 1); put16 (ico, 32);
    put32 (ico, 40 + 16 + 8); put32 (ico, 22);
    put32 (ico, 40); put32 (ico, 2); put32 (ico, 4);           /* BITMAPINFOHEADER */
    put16 (ico, 1); put16 (ico, 32);
    for (int i = 0; i < 6; i++)
        put32 (ico, 0);
    for (int i = 0; i < 4; i++)
        put32 (ico, 0xff1e140a);
    put32 (ico, 0); put32 (ico, 0);                            /* AND mask */
    GByteArray *a = g_byte_array_new ();
    g_byte_array_append (a, (const guint8 *) "RIFF", 4);
    put32 (a, 4 + 8 + 36 + 12 + 8 + ico->len);
    g_byte_array_append (a, (const guint8 *) "ACONanih", 8);
    put32 (a, 36);
    const guint32 anih[9] = { 36, 1, 1, 0, 0, 0, 0, 10, 1 };
    for (int i = 0; i < 9; i++)
        put32 (a, anih[i]);
    g_byte_array_append (a, (const guint8 *) "LIST", 4);
    put32 (a, 4 + 8 + ico->len);
    g_byte_array_append (a, (const guint8 *) "framicon", 8);
    put32 (a, ico->len);
    g_byte_array_append (a, ico->data, ico->len);
    g_byte_array_unref (ico);
    return a;
}

static void test_tex (void)
{
    /* an animated cursor as a texture file: gdk-pixbuf would decode it (when
     * its "ani" loader is installed), the texture loader must not hand it over */
    GByteArray *ani = ani_bytes ();
    mkfile ("tex/cursor.tex", ani->data, ani->len);
    g_byte_array_unref (ani);
    char *f = P ("tex/cursor.tex");
    GdkPixbuf *direct = gdk_pixbuf_new_from_file (f, NULL);
    if (!direct)
        g_printerr ("parsers-test: this gdk-pixbuf reads no .ani (the check below cannot fail here)\n");
    g_clear_object (&direct);
    WalpeTex *t = walpe_tex_load (f);
    CHECK (t == NULL);
    walpe_tex_free (t);
    g_free (f);

    /* pictures a texture file may be: still loaded */
    {
        GdkPixbuf *pb = gdk_pixbuf_new (GDK_COLORSPACE_RGB, TRUE, 8, 8, 6);
        gdk_pixbuf_fill (pb, 0x336699ff);
        static const char *const kinds[] = { "png", "jpeg", "bmp", "tiff" };
        for (guint i = 0; i < G_N_ELEMENTS (kinds); i++) {
            char *rel = g_strdup_printf ("tex/pic.%s.tex", kinds[i]);
            f = P (rel);
            char *d = g_path_get_dirname (f);
            g_mkdir_with_parents (d, 0755);
            g_free (d);
            if (gdk_pixbuf_save (pb, f, kinds[i], NULL, NULL)) {
                t = walpe_tex_load (f);
                CHECK (t && t->width == 8 && t->height == 6);
                walpe_tex_free (t);
            }
            g_free (f);
            g_free (rel);
        }
        g_object_unref (pb);
    }

    /* 600 JPEG starts, each in the comment segment of the one before, then
     * 4 MiB of zeros: every start was a decode attempt that read to the end
     * of the file (17 s of CPU for this one; the time grew with the file).
     * Now a few attempts. */
    {
        const int N = 600, end = 6 * N + 2;
        GByteArray *a = g_byte_array_new ();
        for (int i = 0; i < N; i++) {
            int seg = end - (6 * i + 4);
            const guint8 h[6] = { 0xff, 0xd8, 0xff, 0xfe, (guint8) (seg >> 8), (guint8) seg };
            g_byte_array_append (a, h, 6);
        }
        guint8 *zeros = g_malloc0 (4 * 1024 * 1024 + 2);
        g_byte_array_append (a, zeros, 4 * 1024 * 1024 + 2);
        g_free (zeros);
        mkfile ("tex/nest.tex", a->data, a->len);
        g_byte_array_unref (a);
        f = P ("tex/nest.tex");
        double c0 = cpu_now ();
        t = walpe_tex_load (f);
        double cpu = cpu_now () - c0;
        CHECK (t == NULL);
        walpe_tex_free (t);
        if (cpu > 3.0) {
            g_printerr ("FAIL a file of nested JPEG starts: %.1f s of CPU\n", cpu);
            failures++;
        }
        g_free (f);
    }
}

/* ------------------------------------------------------------------ mp4 */

static void be32 (GByteArray *a, guint32 v)
{
    guint8 b[4] = { (guint8) (v >> 24), (guint8) (v >> 16), (guint8) (v >> 8), (guint8) v };
    g_byte_array_append (a, b, 4);
}

/* appends the box `type` with the given body; the body array is freed */
static void mp4_box (GByteArray *out, const char *type, GByteArray *body)
{
    be32 (out, 8 + body->len);
    g_byte_array_append (out, (const guint8 *) type, 4);
    g_byte_array_append (out, body->data, body->len);
    g_byte_array_free (body, TRUE);
}

static GByteArray *zeros (guint n)
{
    GByteArray *a = g_byte_array_new ();
    g_byte_array_set_size (a, n);
    memset (a->data, 0, n);
    return a;
}

/* a moov box with one H.264 track: one chunk at file offset 0 that holds
 * all samples. fixed != 0: every sample has that size and `count` is only a
 * number in the header; fixed == 0: `count` sizes of 8 bytes are stored.
 * empty_tkhd: the track ends with a tkhd box that has no body. */
static GByteArray *mp4_moov (guint32 fixed, guint32 count, guint32 timescale, gboolean empty_tkhd)
{
    static const guint8 avcc[] = { 1, 0x64, 0, 0x1f, 0xff, 0xe1, 0, 4, 0x67, 0x64, 0, 0x1f,
                                   1, 0, 2, 0x68, 0xee };
    GByteArray *entry = zeros (78);     /* visual sample entry, 16 x 16 */
    entry->data[25] = 16;
    entry->data[27] = 16;
    GByteArray *cc = g_byte_array_new ();
    g_byte_array_append (cc, avcc, sizeof avcc);
    mp4_box (entry, "avcC", cc);
    GByteArray *stsd = g_byte_array_new ();
    be32 (stsd, 0);
    be32 (stsd, 1);
    mp4_box (stsd, "avc1", entry);
    GByteArray *stts = g_byte_array_new ();
    be32 (stts, 0);
    be32 (stts, 1);
    be32 (stts, 0xffffffffu);
    be32 (stts, 10);
    GByteArray *stsc = g_byte_array_new ();
    be32 (stsc, 0);
    be32 (stsc, 1);
    be32 (stsc, 1);
    be32 (stsc, 0xffffffffu);
    be32 (stsc, 1);
    GByteArray *stsz = g_byte_array_new ();
    be32 (stsz, 0);
    be32 (stsz, fixed);
    be32 (stsz, count);
    if (!fixed)
        for (guint32 i = 0; i < count; i++)
            be32 (stsz, 8);
    GByteArray *stco = g_byte_array_new ();
    be32 (stco, 0);
    be32 (stco, 1);
    be32 (stco, 0);
    GByteArray *stbl = g_byte_array_new ();
    mp4_box (stbl, "stsd", stsd);
    mp4_box (stbl, "stts", stts);
    mp4_box (stbl, "stsc", stsc);
    mp4_box (stbl, "stsz", stsz);
    mp4_box (stbl, "stco", stco);
    GByteArray *minf = g_byte_array_new ();
    mp4_box (minf, "stbl", stbl);
    GByteArray *mdhd = zeros (24);
    mdhd->data[12] = (guint8) (timescale >> 24);
    mdhd->data[13] = (guint8) (timescale >> 16);
    mdhd->data[14] = (guint8) (timescale >> 8);
    mdhd->data[15] = (guint8) timescale;
    GByteArray *mdia = g_byte_array_new ();
    mp4_box (mdia, "mdhd", mdhd);
    mp4_box (mdia, "minf", minf);
    GByteArray *trak = g_byte_array_new ();
    mp4_box (trak, "mdia", mdia);
    if (empty_tkhd)
        mp4_box (trak, "tkhd", g_byte_array_new ());
    GByteArray *moov = g_byte_array_new ();
    mp4_box (moov, "trak", trak);
    GByteArray *out = g_byte_array_new ();
    mp4_box (out, "moov", moov);
    return out;
}

/* writes rel: ftyp, the moov, then zeros up to `size` bytes (a hole in the
 * file, no disk space) */
static char *mp4_file (const char *rel, GByteArray *moov, goffset size)
{
    GByteArray *f = g_byte_array_new ();
    GByteArray *ftyp = g_byte_array_new ();
    g_byte_array_append (ftyp, (const guint8 *) "isom\0\0\0\0isom", 12);
    mp4_box (f, "ftyp", ftyp);
    g_byte_array_append (f, moov->data, moov->len);
    g_byte_array_free (moov, TRUE);
    mkfile (rel, f->data, f->len);
    g_byte_array_free (f, TRUE);
    char *p = P (rel);
    if ((goffset) 0 < size && truncate (p, size) != 0)
        g_error ("truncate %s", p);
    return p;
}

static void test_mp4 (void)
{
    GError *err = NULL;
    /* 1. an ordinary file: three stored sizes */
    char *ok = mp4_file ("mp4/ok.mp4", mp4_moov (0, 3, 600, FALSE), 4096);
    Mp4Demux *d = mp4_demux_open (ok, &err);
    CHECK (d && d->nsamples == 3 && d->timescale == 600);
    if (d) {
        const Mp4Sample *s = mp4_demux_next (d);
        CHECK (s && s->size == 8 && s->data && !memcmp (s->data + 4, "ftyp", 4));
        mp4_demux_free (d);
    }
    g_clear_error (&err);

    /* 2. one fixed sample size: 100000 frames in 200 KiB is a file; "as many
     * frames as the file has bytes" for 5 MiB of zeros is not (it was
     * opened: 5.2 million samples, 240 MiB of tables) */
    char *many = mp4_file ("mp4/many.mp4", mp4_moov (1, 100000, 600, FALSE), 200 * 1024);
    d = mp4_demux_open (many, &err);
    CHECK (d && d->nsamples == 100000);
    mp4_demux_free (d);
    g_clear_error (&err);
    char *bomb = mp4_file ("mp4/bomb.mp4", mp4_moov (1, 0xffffffffu, 600, FALSE),
                           5 * 1024 * 1024);
    d = mp4_demux_open (bomb, &err);
    CHECK (d == NULL && err != NULL);
    if (d)
        g_printerr ("  a table of %u samples was built from a file with %d bytes of data\n",
                    d->nsamples, 4096);
    mp4_demux_free (d);
    g_clear_error (&err);

    /* 3. a timescale of 2^31: the player hands it to the decoder as an int */
    char *ts = mp4_file ("mp4/timescale.mp4", mp4_moov (0, 3, 0x80000000u, FALSE), 4096);
    d = mp4_demux_open (ts, &err);
    CHECK (d == NULL && err != NULL);
    mp4_demux_free (d);
    g_clear_error (&err);

    /* 4. the file ends with an empty tkhd box and is exactly one page long.
     * In a child: single free pages are filled with no-access pages until
     * the next mapping of one page lands right below one of them, then the
     * file is opened: the byte after the box is the byte after the mapping
     * (it was read: SIGSEGV; under ASan a SEGV report). Exit 0 = opened
     * without a fault, 77 = the pages could not be arranged (not counted). */
    long ps = sysconf (_SC_PAGESIZE);
    GByteArray *moov = mp4_moov (0, 3, 600, TRUE);
    GByteArray *f = g_byte_array_new (), *ftyp = g_byte_array_new ();
    g_byte_array_append (ftyp, (const guint8 *) "isom\0\0\0\0isom", 12);
    mp4_box (f, "ftyp", ftyp);
    mp4_box (f, "free", zeros ((guint) (ps - 20 - 8 - moov->len)));
    g_byte_array_append (f, moov->data, moov->len);
    g_byte_array_free (moov, TRUE);
    CHECK ((long) f->len == ps);
    mkfile ("mp4/tkhd.mp4", f->data, f->len);
    g_byte_array_free (f, TRUE);
    char *tk = P ("mp4/tkhd.mp4");
    fflush (NULL);
    pid_t pid = fork ();
    if (pid == 0) {
        void *prev = NULL, *last = NULL;
        int run = 0;
        for (int i = 0; i < 4096 && run < 2; i++) {
            void *pg = mmap (NULL, (size_t) ps, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (pg == MAP_FAILED)
                break;
            run = prev && (char *) pg + ps == (char *) prev ? run + 1 : 0;
            prev = last = pg;
        }
        void *probe = mmap (NULL, (size_t) ps, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        gboolean placed = probe != MAP_FAILED && last && (char *) probe + ps == (char *) last;
        if (probe != MAP_FAILED)
            munmap (probe, (size_t) ps);
        Mp4Demux *cd = mp4_demux_open (tk, NULL);
        _exit (!cd ? 3 : placed ? 0 : 77);
    }
    int st = 0;
    CHECK (pid > 0 && waitpid (pid, &st, 0) == pid);
    if (WIFEXITED (st) && WEXITSTATUS (st) == 77)
        g_printerr ("  note: the page after the file could not be made a no-access page here; "
                    "the read past an empty tkhd box was not tested\n");
    else
        CHECK (WIFEXITED (st) && WEXITSTATUS (st) == 0);

    g_free (ok);
    g_free (many);
    g_free (bomb);
    g_free (ts);
    g_free (tk);
}

/* ------------------------------------------------------------------ model */

/* the smallest puppet model the loader takes: one 80-byte vertex and two
 * triangles' worth of indices */
static void test_model (void)
{
    GByteArray *a = g_byte_array_new ();
    g_byte_array_append (a, (const guint8 *) "MDLV0019\0", 9);
    put32 (a, 0); put32 (a, 1); put32 (a, 1);
    g_byte_array_append (a, (const guint8 *) "m\0", 2);
    put32 (a, 0x0180000F);
    put32 (a, 80);
    for (int i = 0; i < 20; i++)
        put32 (a, 0);
    put32 (a, 6);
    for (int i = 0; i < 3; i++)
        put16 (a, 0);
    for (int i = 0; i < 16; i++)
        put32 (a, 0);
    mkfile ("mdl/small.mdl", a->data, a->len);
    mkfile ("mdl/huge.mdl", a->data, a->len);
    g_byte_array_unref (a);
    char *f = P ("mdl/small.mdl");
    WalpePuppet *p = walpe_puppet_load (f);
    CHECK (p != NULL);
    walpe_puppet_free (p);
    g_free (f);
    /* the same model with 300 MiB behind it (a hole: no disk is used). It
     * was read into memory whole, whatever its size */
    f = P ("mdl/huge.mdl");
    if (truncate (f, (off_t) 300 * 1024 * 1024) == 0) {
        p = walpe_puppet_load (f);
        CHECK (p == NULL);
        walpe_puppet_free (p);
    }
    g_unlink (f);
    g_free (f);
}

/* -------------------------------------------------------------------- pkg */

typedef struct { char *name; guint32 off, size; } Ent;

static void write_pkg (const char *rel, GArray *ents, gsize blob)
{
    GByteArray *a = g_byte_array_new ();
    put32 (a, 8);
    g_byte_array_append (a, (const guint8 *) "PKGV0016", 8);
    put32 (a, ents->len);
    for (guint i = 0; i < ents->len; i++) {
        Ent *e = &g_array_index (ents, Ent, i);
        put32 (a, (guint32) strlen (e->name));
        g_byte_array_append (a, (const guint8 *) e->name, (guint) strlen (e->name));
        put32 (a, e->off);
        put32 (a, e->size);
        g_free (e->name);
    }
    guint8 *data = g_malloc (blob);
    for (gsize i = 0; i < blob; i++)
        data[i] = (guint8) ('a' + i % 26);
    g_byte_array_append (a, data, (guint) blob);
    g_free (data);
    mkfile (rel, a->data, a->len);
    g_byte_array_unref (a);
    g_array_set_size (ents, 0);
}

static void ent (GArray *ents, char *name, guint32 off, guint32 size)
{
    Ent e = { name, off, size };
    g_array_append_val (ents, e);
}

/* files and directories below dir (not following links) */
static guint tree_objects (const char *dir, int depth)
{
    guint n = 0;
    GDir *d = depth < 600 ? g_dir_open (dir, 0, NULL) : NULL;
    const char *name;
    while (d && (name = g_dir_read_name (d))) {
        char *p = g_build_filename (dir, name, NULL);
        GStatBuf st;
        n++;
        if (g_lstat (p, &st) == 0 && S_ISDIR (st.st_mode))
            n += tree_objects (p, depth + 1);
        g_free (p);
    }
    if (d)
        g_dir_close (d);
    return n;
}

static void test_pkg (void)
{
    GArray *ents = g_array_new (FALSE, FALSE, sizeof (Ent));
    GError *err = NULL;

    /* a normal package, as deep as real ones go and a little deeper */
    ent (ents, g_strdup ("scene.json"), 0, 10);
    ent (ents, g_strdup ("materials/effects/workshop/123/masks/a/b/c.tex"), 10, 20);
    ent (ents, g_strdup ("shaders/x.frag"), 30, 0);
    write_pkg ("pkg/ok.pkg", ents, 64);
    char *pkg = P ("pkg/ok.pkg"), *out = P ("pkg/ok-out");
    CHECK (walpe_pkg_extract (pkg, out, &err) && !err);
    g_clear_error (&err);
    char *f = g_build_filename (out, "materials/effects/workshop/123/masks/a/b/c.tex", NULL);
    CHECK (g_file_test (f, G_FILE_TEST_IS_REGULAR));
    g_free (f);
    g_free (pkg);
    g_free (out);

    /* Directory chains. One megabyte of table could hold 1000 empty
     * entries, each at the end of 500 directories of its own: half a
     * million directories, 2 GiB of blocks, none of it bytes the budget
     * counted, and deeper than the cache's own cleanup went. Here: three
     * entries 40 levels deep are refused, 32 levels are accepted. */
    for (int i = 0; i < 3; i++) {
        GString *g = g_string_new (NULL);
        g_string_append_printf (g, "%d", i);
        for (int k = 0; k < 39; k++)
            g_string_append (g, "/d");
        g_string_append (g, "/f");
        ent (ents, g_string_free (g, FALSE), 0, 0);
    }
    write_pkg ("pkg/deep.pkg", ents, 16);
    pkg = P ("pkg/deep.pkg");
    out = P ("pkg/deep-out");
    CHECK (!walpe_pkg_extract (pkg, out, &err) && err);
    g_clear_error (&err);
    CHECK (tree_objects (out, 0) == 0);     /* refused before anything is written */
    g_free (pkg);
    g_free (out);
    {
        GString *g = g_string_new ("top");
        for (int k = 0; k < 30; k++)
            g_string_append (g, "/d");
        g_string_append (g, "/f");              /* 32 components */
        ent (ents, g_string_free (g, FALSE), 0, 4);
    }
    write_pkg ("pkg/limit.pkg", ents, 16);
    pkg = P ("pkg/limit.pkg");
    out = P ("pkg/limit-out");
    CHECK (walpe_pkg_extract (pkg, out, &err) && !err);
    g_clear_error (&err);
    CHECK (tree_objects (out, 0) == 32);
    g_free (pkg);
    g_free (out);

    /* 33000 empty files: more objects than a package may create (the
     * largest of 88 Workshop packages has 507 files and directories) */
    for (int i = 0; i < 33000; i++)
        ent (ents, g_strdup_printf ("%d/f%d", i % 100, i), 0, 0);
    write_pkg ("pkg/many.pkg", ents, 16);
    pkg = P ("pkg/many.pkg");
    out = P ("pkg/many-out");
    CHECK (!walpe_pkg_extract (pkg, out, &err) && err);
    g_clear_error (&err);
    CHECK (tree_objects (out, 0) == 0);
    g_free (pkg);
    g_free (out);

    /* the cache's own files are not the package's to write */
    ent (ents, g_strdup ("scene.json"), 0, 2);
    ent (ents, g_strdup (".walpe-pkg-stamp"), 0, 8);
    ent (ents, g_strdup (".walpe-pkg-src"), 0, 8);
    ent (ents, g_strdup ("sub/.walpe-pkg-stamp"), 0, 8);    /* only a name, below the top */
    write_pkg ("pkg/stamp.pkg", ents, 16);
    pkg = P ("pkg/stamp.pkg");
    out = P ("pkg/stamp-out");
    CHECK (walpe_pkg_extract (pkg, out, &err) && !err);
    g_clear_error (&err);
    char *s1 = g_build_filename (out, ".walpe-pkg-stamp", NULL);
    char *s2 = g_build_filename (out, ".walpe-pkg-src", NULL);
    char *s3 = g_build_filename (out, "sub", ".walpe-pkg-stamp", NULL);
    CHECK (!g_file_test (s1, G_FILE_TEST_EXISTS) && !g_file_test (s2, G_FILE_TEST_EXISTS));
    CHECK (g_file_test (s3, G_FILE_TEST_IS_REGULAR));
    g_free (s1); g_free (s2); g_free (s3);
    g_free (pkg);
    g_free (out);

    /* through the cache: a package whose extraction fails half way (here a
     * name that is a file and a directory at once) leaves no copy behind.
     * It stayed - stamped by the package itself, if it liked - until some
     * other package was extracted. */
    ent (ents, g_strdup ("scene.json"), 0, 2);
    ent (ents, g_strdup (".walpe-pkg-stamp"), 0, 8);
    ent (ents, g_strdup ("big/a.bin"), 0, 1024);
    ent (ents, g_strdup ("clash"), 0, 4);
    ent (ents, g_strdup ("clash/inside"), 0, 4);
    write_pkg ("cache/item1/scene.pkg", ents, 2048);
    mkfile ("cache/item1/project.json", "{\"type\":\"scene\",\"file\":\"scene.json\"}", -1);
    pkg = P ("cache/item1/scene.pkg");
    char *dir = P ("cache/item1");
    char *cache = walpe_pkg_extract_cached (pkg, dir, &err);
    CHECK (cache == NULL && err);
    g_clear_error (&err);
    g_free (cache);
    {
        char *root = g_build_filename (g_get_user_cache_dir (), "walpe", "pkg", NULL);
        CHECK (tree_objects (root, 0) == 0);
        g_free (root);
    }
    g_free (pkg);
    g_free (dir);

    /* ... and a good one is cached and reused as before */
    ent (ents, g_strdup ("scene.json"), 0, 2);
    ent (ents, g_strdup ("materials/a.tex"), 2, 30);
    write_pkg ("cache/item2/scene.pkg", ents, 64);
    mkfile ("cache/item2/project.json", "{\"type\":\"scene\",\"file\":\"scene.json\"}", -1);
    pkg = P ("cache/item2/scene.pkg");
    dir = P ("cache/item2");
    cache = walpe_pkg_extract_cached (pkg, dir, &err);
    CHECK (cache && !err);
    g_clear_error (&err);
    if (cache) {
        f = g_build_filename (cache, "materials", "a.tex", NULL);
        CHECK (g_file_test (f, G_FILE_TEST_IS_REGULAR));
        g_free (f);
        /* the copy is the user's alone */
        GStatBuf st;
        CHECK (g_stat (cache, &st) == 0 && (st.st_mode & 077) == 0);
        char *again = walpe_pkg_extract_cached (pkg, dir, &err);
        CHECK (again && !strcmp (again, cache));
        walpe_pkg_cache_release (again);
        g_free (again);
        walpe_pkg_cache_release (cache);
        /* the package is replaced by one without materials/a.tex: the new
         * copy does not keep the old file */
        ent (ents, g_strdup ("scene.json"), 0, 3);
        ent (ents, g_strdup ("materials/b.tex"), 3, 30);
        write_pkg ("cache/item2/scene.pkg", ents, 80);
        again = walpe_pkg_extract_cached (pkg, dir, &err);
        CHECK (again && !err);
        g_clear_error (&err);
        if (again) {
            char *olds = g_build_filename (again, "materials", "a.tex", NULL);
            char *news = g_build_filename (again, "materials", "b.tex", NULL);
            CHECK (!g_file_test (olds, G_FILE_TEST_EXISTS));
            CHECK (g_file_test (news, G_FILE_TEST_IS_REGULAR));
            g_free (olds);
            g_free (news);
            walpe_pkg_cache_release (again);
        }
        g_free (again);
    }
    g_free (cache);
    g_free (pkg);
    g_free (dir);
    g_array_free (ents, TRUE);
}

/* --------------------------------------------------------------------- gl */

static void save_png (const char *rel, int w, int h)
{
    char *p = P (rel);
    char *d = g_path_get_dirname (p);
    g_mkdir_with_parents (d, 0755);
    GdkPixbuf *pb = gdk_pixbuf_new (GDK_COLORSPACE_RGB, TRUE, 8, w, h);
    gdk_pixbuf_fill (pb, 0x8040c0ff);
    if (!gdk_pixbuf_save (pb, p, "png", NULL, "compression", "1", NULL))
        g_error ("cannot write %s", p);
    g_object_unref (pb);
    g_free (d);
    g_free (p);
}

/* bytes of every texture the context holds */
static gint64 gl_tex_bytes (void)
{
    int count = 0;
    gint64 bytes = 0, comp = 0;
    walpe_gl_tex_totals (&count, &bytes, &comp);
    return bytes;
}

static void gl_part (void)
{
    walpe_glutil_init ();
    /* 1. direct loads: only picture formats a wallpaper is made of */
    save_png ("gl/a.png", 16, 16);
    mkfile ("gl/vector.png",
            "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"8\" height=\"8\">"
            "<rect width=\"8\" height=\"8\" fill=\"red\"/></svg>", -1);
    mkfile ("gl/pixmap.png", "/* XPM */\nstatic char *a[] = {\n\"4 4 1 1\",\n\"a c #102030\",\n"
            "\"aaaa\",\n\"aaaa\",\n\"aaaa\",\n\"aaaa\"};\n", -1);
    char *f = P ("gl/a.png");
    WalpeGlTex t = walpe_gl_tex_from_file (f);
    CHECK (t.tex && t.width == 16 && t.height == 16);
    if (t.tex)
        glDeleteTextures (1, &t.tex);
    g_free (f);
    static const char *const odd[] = { "gl/vector.png", "gl/pixmap.png" };
    for (guint i = 0; i < G_N_ELEMENTS (odd); i++) {
        f = P (odd[i]);
        GdkPixbuf *direct = gdk_pixbuf_new_from_file (f, NULL);
        if (!direct)
            g_printerr ("parsers-test: this gdk-pixbuf does not read %s (check cannot fail here)\n",
                        odd[i]);
        g_clear_object (&direct);
        t = walpe_gl_tex_from_file (f);
        CHECK (t.tex == 0);
        if (t.tex)
            glDeleteTextures (1, &t.tex);
        g_free (f);
    }

    /* 2. direct loads inside a texture scope are counted and held to its
     * budget: five 1024 x 1024 pictures (5.3 MiB each with a mip chain)
     * under 8 MiB */
    save_png ("gl/big.png", 1024, 1024);
    f = P ("gl/big.png");
    GLuint held[5] = { 0 };
    walpe_gl_tex_scope_begin (8 * 1024 * 1024);
    for (int i = 0; i < 5; i++) {
        t = walpe_gl_tex_from_file (f);
        CHECK (t.tex && t.width == 1024);      /* the picture's size, whatever is stored */
        held[i] = t.tex;
    }
    gint64 counted = walpe_gl_tex_scope_end ();
    CHECK (counted > 4 * 1024 * 1024 && counted <= 9 * 1024 * 1024);
    glDeleteTextures (5, held);
    g_free (f);

    /* 3. the old scene format ("object" with "layers"): its layer pictures
     * under the scene's texture budget, its text layers bounded */
    if (epoxy_is_desktop_gl ()) {
        gint64 before = gl_tex_bytes ();
        GString *js = g_string_new ("{\"object\":{\"width\":640,\"height\":360,\"layers\":[");
        for (int i = 0; i < 6; i++)
            g_string_append_printf (js, "%s{\"file\":\"big%d.png\",\"scale\":0.1}", i ? "," : "", i);
        g_string_append (js, ",{\"text\":{\"text\":\"WWWW\",\"size\":6000,\"font\":\"Sans\"}}");
        g_string_append (js, ",{\"text\":{\"text\":\"");
        for (int i = 0; i < 200000; i++)
            g_string_append (js, "word ");
        g_string_append (js, "\",\"size\":20}}]}}");
        mkfile ("gl/old/scene.json", js->str, (gssize) js->len);
        g_string_free (js, TRUE);
        mkfile ("gl/old/project.json", "{\"type\":\"scene\",\"file\":\"scene.json\"}", -1);
        for (int i = 0; i < 6; i++) {
            char *rel = g_strdup_printf ("gl/old/big%d.png", i);
            save_png (rel, 1024, 1024);
            g_free (rel);
        }
        g_setenv ("WALPE_TEX_BUDGET_MB", "8", TRUE);
        char *wp = P ("gl/old");
        GError *err = NULL;
        double c0 = cpu_now ();
        WalpeContent *c = walpe_content_new (wp, NULL, &err);
        CHECK (c != NULL);
        if (c) {
            CHECK (c->gl_init (c, &err));
            double cpu = cpu_now () - c0;
            gint64 used = gl_tex_bytes () - before;
            /* six pictures at file size are 32 MiB, the 6000 px text another
             * 380 MiB; a megabyte of text took Pango seconds and gigabytes */
            if (used > 24 * 1024 * 1024) {
                g_printerr ("FAIL old-format scene holds %.1f MiB of textures under a budget of 8\n",
                            used / 1048576.0);
                failures++;
            }
            if (cpu > 20.0) {
                g_printerr ("FAIL old-format scene: %.1f s of CPU to load\n", cpu);
                failures++;
            }
            glViewport (0, 0, 64, 36);
            c->gl_render (c, 64, 36, 0.0f, 0.0f);
            if (c->gl_free)
                c->gl_free (c);
            c->free (c);
        }
        g_clear_error (&err);
        g_unsetenv ("WALPE_TEX_BUDGET_MB");
        g_free (wp);
    }
    /* 4. a particle sprite sheet whose frame rectangle lies far outside the
     * picture, with numbers whose sum wraps around: the sprite analysis
     * read gigabytes past the pixels (the daemon died on load). The scene
     * loads and draws; the sheet is simply not measured. */
    {
        GByteArray *a = g_byte_array_new ();
        g_byte_array_append (a, (const guint8 *) "TEXV0005\0TEXI0001\0", 18);
        const guint32 hdr[7] = { 0, 4, 16, 16, 16, 16, 0 };    /* RGBA8888, sprite sheet */
        for (int i = 0; i < 7; i++)
            put32 (a, hdr[i]);
        g_byte_array_append (a, (const guint8 *) "TEXB0003\0", 9);
        put32 (a, 1); put32 (a, 0); put32 (a, 1);              /* images, format, mips */
        put32 (a, 16); put32 (a, 16); put32 (a, 0); put32 (a, 16 * 16 * 4); put32 (a, 16 * 16 * 4);
        for (int i = 0; i < 16 * 16; i++)
            put32 (a, 0x80ffffff);
        g_byte_array_append (a, (const guint8 *) "TEXS0003\0", 9);
        put32 (a, 2); put32 (a, 8); put32 (a, 8);
        const float rect[7] = { 0.1f, 2147483520.0f, 0.0f, 2147483520.0f, 0.0f, 0.0f, 4.0f };
        for (int fr = 0; fr < 2; fr++) {
            put32 (a, 0);
            for (int i = 0; i < 7; i++) {
                guint32 u;
                memcpy (&u, &rect[i], 4);
                put32 (a, u);
            }
        }
        mkfile ("gl/sheet/materials/sheet.tex", a->data, a->len);
        g_byte_array_unref (a);
        mkfile ("gl/sheet/materials/part.json",
                "{\"passes\":[{\"blending\":\"additive\",\"shader\":\"genericparticle\","
                "\"textures\":[\"sheet\"]}]}", -1);
        mkfile ("gl/sheet/particles/p.json",
                "{\"emitter\":[{\"name\":\"sphererandom\",\"rate\":20,\"distancemax\":8}],"
                "\"initializer\":[{\"name\":\"lifetimerandom\",\"min\":1,\"max\":2}],"
                "\"renderer\":[{\"name\":\"sprite\"}],\"material\":\"materials/part.json\","
                "\"maxcount\":8}", -1);
        mkfile ("gl/sheet/scene.json",
                "{\"general\":{\"orthogonalprojection\":{\"width\":64,\"height\":36}},"
                "\"objects\":[{\"id\":1,\"name\":\"p\",\"particle\":\"particles/p.json\","
                "\"origin\":\"32 18 0\"}]}", -1);
        mkfile ("gl/sheet/project.json", "{\"type\":\"scene\",\"file\":\"scene.json\"}", -1);
        char *wp = P ("gl/sheet");
        GError *err = NULL;
        WalpeContent *c = walpe_content_new (wp, NULL, &err);
        CHECK (c != NULL);
        if (c) {
            CHECK (c->gl_init (c, &err));
            for (int i = 0; i < 2; i++) {
                glViewport (0, 0, 64, 36);
                c->gl_render (c, 64, 36, 0.0f, 0.0f);
            }
            if (c->gl_free)
                c->gl_free (c);
            c->free (c);
        }
        g_clear_error (&err);
        g_free (wp);
    }
    /* 5. a "scenetexture" option whose default is a link out of the
     * wallpaper: the solid layer keeps its colour, the file is not opened */
    {
        save_png ("gl/outside/private.png", 16, 16);
        mkfile ("gl/utex/models/solid.json", "{\"solidlayer\":true}", -1);
        mkfile ("gl/utex/scene.json",
                "{\"general\":{\"orthogonalprojection\":{\"width\":64,\"height\":36}},"
                "\"objects\":[{\"id\":1,\"name\":\"bg\",\"image\":\"models/solid.json\","
                "\"size\":\"64 36\",\"origin\":\"32 18 0\","
                "\"instance\":{\"usertextures\":[\"pic\"]}}]}", -1);
        mkfile ("gl/utex/project.json",
                "{\"type\":\"scene\",\"file\":\"scene.json\",\"general\":{\"properties\":"
                "{\"pic\":{\"type\":\"scenetexture\",\"text\":\"p\",\"value\":\"files/pic.png\"}}}}",
                -1);
        char *target = P ("gl/outside/private.png"), *link = P ("gl/utex/files/pic.png");
        char *ldir = g_path_get_dirname (link);
        g_mkdir_with_parents (ldir, 0755);
        g_free (ldir);
        if (symlink (target, link) == 0) {
            char *wp = P ("gl/utex");
            GError *err = NULL;
            g_string_truncate (logged, 0);
            WalpeContent *c = walpe_content_new (wp, NULL, &err);
            CHECK (c != NULL);
            CHECK (strstr (logged->str, "user texture pic") != NULL);          /* it was looked for */
            CHECK (strstr (logged->str, "user texture pic = /") == NULL);      /* and not taken */
            if (c) {
                CHECK (c->gl_init (c, &err));
                if (c->gl_free)
                    c->gl_free (c);
                c->free (c);
            }
            g_clear_error (&err);
            g_free (wp);
        }
        g_free (target);
        g_free (link);
    }
    /* 6. numbers no int holds, in every place a scene, text or particle
     * file is asked for a whole number or a size: 1e30, 4294967296, 1e999
     * (infinity to json-glib), "nan". The C casts were undefined there (x86
     * happens to give INT_MIN, which the checks after them catch). The
     * scene loads and draws. On an ordinary build this part passes with or
     * without the fix; built with -Dc_args=-fsanitize=float-cast-overflow
     * and run with UBSAN_OPTIONS=halt_on_error=1 it stops at the first cast
     * without it. */
    {
        mkfile ("gl/num/models/solid.json", "{\"solidlayer\":true}", -1);
        mkfile ("gl/num/materials/part.json",
                "{\"passes\":[{\"blending\":\"additive\",\"shader\":\"genericparticle\","
                "\"textures\":[\"sheet\"]}]}", -1);
        mkfile ("gl/num/particles/p.json",
                "{\"emitter\":[{\"name\":\"sphererandom\",\"rate\":20,\"distancemax\":8,"
                "\"instantaneous\":1e10}],"
                "\"initializer\":[{\"name\":\"lifetimerandom\",\"min\":1,\"max\":2}],"
                "\"operator\":[{\"name\":\"controlpointattract\",\"controlpoint\":3.4e38}],"
                "\"controlpoint\":[{\"id\":\"nan\",\"flags\":1},{\"id\":1,\"flags\":9007199254740992}],"
                "\"renderer\":[{\"name\":\"rope\",\"segments\":1e30}],"
                "\"material\":\"materials/part.json\",\"maxcount\":1e999,\"flags\":1e30}", -1);
        mkfile ("gl/num/scene.json",
                "{\"general\":{\"orthogonalprojection\":{\"width\":64,\"height\":36}},"
                "\"objects\":["
                "{\"id\":1e30,\"parent\":-1e30,\"name\":\"a\",\"image\":\"models/solid.json\","
                "\"size\":\"nan nan\",\"origin\":\"32 18 0\",\"colorBlendMode\":4294967296},"
                "{\"id\":2,\"name\":\"b\",\"image\":\"models/solid.json\",\"size\":\"1e30 1e30\","
                "\"origin\":\"32 18 0\",\"scale\":\"inf nan 1\",\"colorBlendMode\":2},"
                "{\"id\":3,\"name\":\"t\",\"text\":\"Hi\",\"font\":\"systemfont_arial\","
                "\"pointsize\":12,\"size\":\"60 20\",\"origin\":\"32 18 0\",\"limitwidth\":true,"
                "\"maxwidth\":1e30,\"limitrows\":true,\"maxrows\":1e30},"
                "{\"id\":4,\"name\":\"p\",\"particle\":\"particles/p.json\",\"origin\":\"32 18 0\"}"
                "]}", -1);
        mkfile ("gl/num/project.json", "{\"type\":\"scene\",\"file\":\"scene.json\"}", -1);
        char *wp = P ("gl/num");
        GError *err = NULL;
        WalpeContent *c = walpe_content_new (wp, NULL, &err);
        CHECK (c != NULL);
        if (c) {
            CHECK (c->gl_init (c, &err));
            for (int i = 0; i < 2; i++) {
                glViewport (0, 0, 64, 36);
                c->gl_render (c, 64, 36, 0.0f, 0.0f);
            }
            if (c->gl_free)
                c->gl_free (c);
            c->free (c);
        }
        g_clear_error (&err);
        g_free (wp);
        /* the conversion itself */
        CHECK (walpe_d2i (-7.9) == -7 && walpe_d2i (2147483647.5) == G_MAXINT &&
               walpe_d2i (-2147483648.9) == G_MININT);
        CHECK (walpe_d2i (2147483648.0) == G_MININT && walpe_d2i (1e30) == G_MININT &&
               walpe_d2i (-1e30) == G_MININT && walpe_d2i (NAN) == G_MININT &&
               walpe_d2i (INFINITY) == G_MININT);
    }
    while (glGetError () != GL_NO_ERROR)
        ;
}

static gboolean gl_run (EGLDisplay dpy)
{
    EGLint maj, min, nc = 0;
    EGLConfig cfg;
    const EGLint attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                            EGL_NONE };
    if (dpy == EGL_NO_DISPLAY || !eglInitialize (dpy, &maj, &min) || !eglBindAPI (EGL_OPENGL_API) ||
        !eglChooseConfig (dpy, attr, &cfg, 1, &nc) || nc < 1)
        return FALSE;
    const EGLint ca[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
                          EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                          EGL_NONE };
    const EGLint pa[] = { EGL_WIDTH, 64, EGL_HEIGHT, 36, EGL_NONE };
    EGLContext ctx = eglCreateContext (dpy, cfg, EGL_NO_CONTEXT, ca);
    EGLSurface surf = eglCreatePbufferSurface (dpy, cfg, pa);
    if (ctx == EGL_NO_CONTEXT || surf == EGL_NO_SURFACE || !eglMakeCurrent (dpy, surf, surf, ctx)) {
        if (surf != EGL_NO_SURFACE)
            eglDestroySurface (dpy, surf);
        if (ctx != EGL_NO_CONTEXT)
            eglDestroyContext (dpy, ctx);
        eglTerminate (dpy);
        return FALSE;
    }
    gl_part ();
    eglMakeCurrent (dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface (dpy, surf);
    eglDestroyContext (dpy, ctx);
    eglTerminate (dpy);
    return TRUE;
}

static void test_gl (void)
{
    gboolean ran = FALSE;
    if (epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_MESA_platform_surfaceless"))
        ran = gl_run (eglGetPlatformDisplayEXT (EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY,
                                                NULL));
    if (!ran && epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_EXT_device_enumeration") &&
        epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_EXT_platform_device")) {
        EGLDeviceEXT dev[8];
        EGLint nd = 0;
        if (eglQueryDevicesEXT (8, dev, &nd))
            for (EGLint i = 0; i < nd && !ran; i++)
                ran = gl_run (eglGetPlatformDisplayEXT (EGL_PLATFORM_DEVICE_EXT, dev[i], NULL));
    }
    if (!ran)
        g_printerr ("parsers-test: no surfaceless / device GL 3.3 context, gl part skipped\n");
}

int main (int argc, char **argv)
{
    char *tmp = g_dir_make_tmp ("walpe-parsers-test-XXXXXX", NULL);
    if (!tmp)
        g_error ("no temp dir");
    T = realpath (tmp, NULL);
    g_free (tmp);
    char *home = P ("home"), *cfg = P ("xdg-config"), *cache = P ("xdg-cache"),
         *data = P ("xdg-data");
    g_setenv ("HOME", home, TRUE);
    g_setenv ("XDG_CONFIG_HOME", cfg, TRUE);
    g_setenv ("XDG_CACHE_HOME", cache, TRUE);
    g_setenv ("XDG_DATA_HOME", data, TRUE);
    g_setenv ("WALPE_NO_AUDIO", "1", TRUE);
    g_setenv ("WALPE_NO_MEDIA", "1", TRUE);
    g_mkdir_with_parents (home, 0755);
    logged = g_string_new (NULL);
    g_log_set_writer_func (log_writer, NULL, NULL);

    const char *only = argc > 1 ? argv[1] : NULL;
    if (!only || !strcmp (only, "json"))
        test_json ();
    if (!only || !strcmp (only, "links"))
        test_links ();
    if (!only || !strcmp (only, "tex"))
        test_tex ();
    if (!only || !strcmp (only, "model"))
        test_model ();
    if (!only || !strcmp (only, "mp4"))
        test_mp4 ();
    if (!only || !strcmp (only, "pkg"))
        test_pkg ();
    if (!only || !strcmp (only, "gl"))
        test_gl ();

    g_printerr ("%s (%d failure%s, %d warnings logged)\n", failures ? "FAILED" : "OK", failures,
                failures == 1 ? "" : "s", warnings);
    char *rm[] = { "rm", "-rf", T, NULL };
    g_spawn_sync (NULL, rm, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
    g_free (home);
    g_free (cfg);
    g_free (cache);
    g_free (data);
    free (T);
    return failures ? 1 : 0;
}
