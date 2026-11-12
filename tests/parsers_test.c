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
 *              number of decode attempts; both also where only the size of
 *              a container's picture is asked for
 *   pkg      - directory chains and entry counts are bounded, the cache's
 *              bookkeeping names cannot be written by a package (as a file
 *              or as a directory, however the path spells them), a refused
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
 *              fills its last page is not read past the file, a sample size
 *              box repeated thousands of times costs no time; an ordinary
 *              file opens
 *   font     - the font file of a text layer is handed to fontconfig /
 *              FreeType only when it is a TrueType / OpenType font or a
 *              collection with a bounded number of faces and named
 *              instances: not thousands of them, not a bitmap, Type 1 or
 *              WOFF font; ordinary fonts (written here) are used as before
 *   gif      - a GIF whose image does not lie inside the picture its header
 *              declares, or has no pixels, is not given to the decoder: not
 *              as a GIF wallpaper, a still picture or a texture (the last
 *              two with the gl group); one pixel past an edge, far outside,
 *              the largest numbers the file holds. GIFs with the image
 *              inside the picture or touching its edges give the pixels the
 *              decoder gives for them
 * Run: meson test -C build parsers -v   (parsers-test json|links|tex|model|mp4|pkg|font|gif|gl = one group) */
#include "engine/content.h"
#include "engine/gif.h"
#include "engine/glutil.h"
#include "engine/image.h"
#include "engine/mp4demux.h"
#include "engine/pkg.h"
#include "engine/puppet.h"
#include "engine/scenetext.h"
#include "common/gifcheck.h"
#include "common/tex.h"
#include "common/util.h"
#include <epoxy/gl.h>
#include <epoxy/egl.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gstdio.h>
#include <malloc.h>
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

/* bytes the C library has handed out (0 where it cannot say: under
 * AddressSanitizer, whose leak check covers the same ground) */
static gsize heap_in_use (void)
{
#if defined (__GLIBC__) && defined (__GLIBC_PREREQ) && __GLIBC_PREREQ (2, 33)
    struct mallinfo2 mi = mallinfo2 ();
    return mi.uordblks + mi.hblkhd;
#else
    return 0;
#endif
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

    /* one value per text: an array or object behind the first value is not
     * left in memory (json-glib 1.8 parses it and never frees it: thirty
     * times its text, at every parse). What comes back is the first value,
     * as before; a text that starts with a plain value is still no object */
    {
        GString *big = g_string_new ("{");
        for (int i = 0; i < 4000; i++)
            g_string_append_printf (big, "%s\"k%d\":{\"a\":[1,2,3],\"b\":\"text %d\"}", i ? "," : "", i, i);
        g_string_append_c (big, '}');
        GString *after = g_string_new ("{\"first\":1} "), *num = g_string_new ("7 ");
        GString *before = g_string_new (big->str);
        g_string_append (after, big->str);
        g_string_append (num, big->str);
        g_string_append (before, " [{\"second\":2}]");
        gsize heap0 = heap_in_use ();
        for (int k = 0; k < 40; k++) {
            n = wpj_parse_data (after->str, (gssize) after->len);
            CHECK (n && json_object_get_size (json_node_get_object (n)) == 1 &&
                   json_object_has_member (json_node_get_object (n), "first"));
            g_clear_pointer (&n, json_node_unref);
            CHECK (wpj_parse_data (num->str, (gssize) num->len) == NULL);
        }
        gsize heap1 = heap_in_use ();
        if (heap1 > heap0 + 8 * 1024 * 1024)
            g_printerr ("   80 parses of a text with a second value left %.1f MiB allocated\n",
                        (double) (heap1 - heap0) / 1048576.0);
        CHECK (heap1 <= heap0 + 8 * 1024 * 1024);
        n = wpj_parse_data (before->str, (gssize) before->len);
        CHECK (n && json_object_get_size (json_node_get_object (n)) == 4000);
        g_clear_pointer (&n, json_node_unref);
        /* the file reader, and a comment in front (not the plain reading) */
        mkfile ("json/two.json", after->str, (gssize) after->len);
        f = P ("json/two.json");
        n = wpj_parse_file (f);
        CHECK (n && json_object_has_member (json_node_get_object (n), "first"));
        g_clear_pointer (&n, json_node_unref);
        g_free (f);
        g_string_prepend (after, "/* by hand */ ");
        n = wpj_parse_data (after->str, (gssize) after->len);
        CHECK ((n != NULL) == jsonglib_takes ("/* by hand */ {\"first\":1}"));
        CHECK (!n || json_object_has_member (json_node_get_object (n), "first"));
        g_clear_pointer (&n, json_node_unref);
        /* one value with a comment or a 'string' behind it is one value */
        static const char *const one[] = { "{\"a\":[1]} /* {\"b\":[2]} */", "{\"a\":[1],\"c\":'{'} " };
        for (guint i = 0; i < G_N_ELEMENTS (one); i++) {
            n = wpj_parse_data (one[i], -1);
            CHECK ((n != NULL) == jsonglib_takes (one[i]));
            CHECK (!n || json_object_has_member (json_node_get_object (n), "a"));
            g_clear_pointer (&n, json_node_unref);
        }
        g_string_free (big, TRUE);
        g_string_free (after, TRUE);
        g_string_free (num, TRUE);
        g_string_free (before, TRUE);
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

/* a texture container (TEXB0002, RGBA8888, 16 x 16, one image with one
 * level, not compressed) around `n` bytes as that level */
static GByteArray *tex_container (const void *level0, gsize n)
{
    GByteArray *a = g_byte_array_new ();
    g_byte_array_append (a, (const guint8 *) "TEXV0005\0TEXI0001\0", 18);
    put32 (a, 0); put32 (a, 0);
    put32 (a, 16); put32 (a, 16); put32 (a, 16); put32 (a, 16);
    put32 (a, 0);
    g_byte_array_append (a, (const guint8 *) "TEXB0002\0", 9);
    put32 (a, 1);
    put32 (a, 1);
    put32 (a, 16); put32 (a, 16); put32 (a, 0); put32 (a, (guint32) n); put32 (a, (guint32) n);
    g_byte_array_append (a, level0, (guint) n);
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

    /* A "BM" file whose first bytes make gdk-pixbuf's BMP decoder reserve
     * gigabytes before it checks anything (found by tests/fuzz, target tex):
     * an info header of 3 GiB, pixel data at 3 GiB (no palette), a palette
     * of 4 thousand million entries. Not handed to the decoder; a real BMP
     * (the loop above) still is. The same files lie in
     * tests/fuzz/regress/tex, where the address sanitizer watches the
     * allocation itself. */
    {
        static const struct { guint32 off, hs, used; guint16 bpp; } bad[] = {
            { 54, 0xBFFFFFF2u, 0, 32 }, { 0xC0000000u, 40, 0, 32 }, { 0, 40, 0xFFFFFFFFu, 1 },
            { 54, 41, 0, 24 }, { 54, 40, 257, 8 },
        };
        for (guint i = 0; i <= G_N_ELEMENTS (bad); i++) {
            /* the last round: a header with nothing wrong (2 x 2 pixels, 32 bit) */
            gboolean good = i == G_N_ELEMENTS (bad);
            GByteArray *a = g_byte_array_new ();
            g_byte_array_append (a, (const guint8 *) "BM", 2);
            put32 (a, 70); put32 (a, 0); put32 (a, good ? 54 : bad[i].off);
            put32 (a, good ? 40 : bad[i].hs); put32 (a, 2); put32 (a, 2);
            put32 (a, 1u | ((guint32) (good ? 32 : bad[i].bpp) << 16));
            put32 (a, 0); put32 (a, 16); put32 (a, 2835); put32 (a, 2835);
            put32 (a, good ? 0 : bad[i].used); put32 (a, 0);
            for (int k = 0; k < 4; k++)
                put32 (a, 0xff336699u);
            CHECK (walpe_bmp_header_ok (a->data, a->len, a->len) == good);
            CHECK (walpe_bmp_header_ok (a->data, 17, a->len) == FALSE);
            char *rel = g_strdup_printf ("tex/bmp%u.tex", i);
            mkfile (rel, a->data, a->len);
            f = P (rel);
            CHECK (walpe_bmp_file_header_ok (f) == good);
            t = walpe_tex_load (f);
            if (!good)
                CHECK (t == NULL);
            walpe_tex_free (t);
            g_free (f);
            g_free (rel);
            g_byte_array_unref (a);
        }
        CHECK (walpe_bmp_header_ok ((const guint8 *) "\x89PNG....", 10, 10));   /* not a BMP: not this check's */
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

    /* The size of a container's picture, asked before the texture is
     * loaded (every layer texture whose level 0 starts like a PNG / JPEG):
     * the loader's rules hold there too. */
    {
        int w = 0, h = 0;
        /* a PNG and a JPEG as level 0: their own size, and the loader agrees */
        GdkPixbuf *pb = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, 8, 6);
        gdk_pixbuf_fill (pb, 0x336699ff);
        static const char *const kinds[] = { "png", "jpeg" };
        for (guint i = 0; i < G_N_ELEMENTS (kinds); i++) {
            gchar *pic = NULL;
            gsize pl = 0;
            if (!gdk_pixbuf_save_to_buffer (pb, &pic, &pl, kinds[i], NULL, NULL))
                continue;
            GByteArray *c = tex_container (pic, pl);
            mkfile ("tex/picture-in.tex", c->data, c->len);
            g_byte_array_unref (c);
            g_free (pic);
            f = P ("tex/picture-in.tex");
            w = h = 0;
            CHECK (walpe_tex_picture_size (f, &w, &h) && w == 8 && h == 6);
            t = walpe_tex_load (f);
            CHECK (t && t->width == 8 && t->height == 6);
            walpe_tex_free (t);
            g_free (f);
        }
        g_object_unref (pb);

        /* level 0 starts with a PNG signature and is no PNG; an animated
         * cursor follows. The size was asked of whatever decoder gdk-pixbuf
         * chose for the bytes behind "RIFF": the cursor decoders ran and the
         * answer was the cursor's 2 x 2 (the loader reads such a level as
         * pixels: 16 x 16). */
        GByteArray *lv = g_byte_array_new ();
        g_byte_array_append (lv, (const guint8 *) "\x89PNG\r\n\x1a\n", 8);
        for (int i = 0; i < 24; i++)
            put32 (lv, 0x11111111);
        ani = ani_bytes ();
        g_byte_array_append (lv, ani->data, ani->len);
        g_byte_array_unref (ani);
        for (int i = 0; i < 300; i++)
            put32 (lv, 0);
        GByteArray *c = tex_container (lv->data, lv->len);
        g_byte_array_unref (lv);
        mkfile ("tex/cursor-in.tex", c->data, c->len);
        g_byte_array_unref (c);
        f = P ("tex/cursor-in.tex");
        w = h = 0;
        CHECK (!walpe_tex_picture_size (f, &w, &h));
        if (w || h)
            g_printerr ("  the size of a %d x %d cursor was taken for the texture's\n", w, h);
        t = walpe_tex_load (f);
        CHECK (t && t->width == 16 && t->height == 16);
        walpe_tex_free (t);
        g_free (f);

        /* level 0 = the nested JPEG starts from above in front of 8 MiB of
         * zeros: every start was an attempt over the rest of the file here
         * as well (10 s of CPU before the load began) */
        const int N = 600, end = 6 * N + 2;
        lv = g_byte_array_new ();
        for (int i = 0; i < N; i++) {
            int seg = end - (6 * i + 4);
            const guint8 hd[6] = { 0xff, 0xd8, 0xff, 0xfe, (guint8) (seg >> 8), (guint8) seg };
            g_byte_array_append (lv, hd, 6);
        }
        g_byte_array_set_size (lv, lv->len + 8 * 1024 * 1024);
        memset (lv->data + 6 * N, 0, 8 * 1024 * 1024);
        c = tex_container (lv->data, lv->len);
        g_byte_array_unref (lv);
        mkfile ("tex/nest-in.tex", c->data, c->len);
        g_byte_array_unref (c);
        f = P ("tex/nest-in.tex");
        double c0 = cpu_now ();
        CHECK (!walpe_tex_picture_size (f, &w, &h));
        double cpu = cpu_now () - c0;
        if (cpu > 3.0) {
            g_printerr ("FAIL the picture size of a container of nested JPEG starts: %.1f s of CPU\n",
                        cpu);
            failures++;
        }
        g_unlink (f);
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

/* mp4_moov: this many sample size boxes with one fixed size for "as many
 * samples as there can be" are written in front of the real one */
static guint mp4_stsz_repeats;

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
    for (guint i = 0; i < mp4_stsz_repeats; i++) {
        GByteArray *rep = g_byte_array_new ();
        be32 (rep, 0);
        be32 (rep, 1);
        be32 (rep, 0xffffffffu);
        mp4_box (stbl, "stsz", rep);
    }
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

    /* 5. the sample size box 6000 times with one fixed size (20 bytes each,
     * the last one of a sample table counts), then the real one, in a file of
     * 4 MiB: each of them was written out as a table of 4 million entries,
     * 2.5 ms a box - 15 s for this file, nine minutes for 4 MiB of boxes */
    mp4_stsz_repeats = 6000;
    char *rep = mp4_file ("mp4/repeat.mp4", mp4_moov (0, 3, 600, FALSE), 4 * 1024 * 1024);
    mp4_stsz_repeats = 0;
    double c0 = cpu_now ();
    d = mp4_demux_open (rep, &err);
    double cpu = cpu_now () - c0;
    CHECK (d && d->nsamples == 3 && !err);
    mp4_demux_free (d);
    g_clear_error (&err);
    if (cpu > 3.0) {
        g_printerr ("FAIL a sample table with 6000 size boxes: %.1f s of CPU to open\n", cpu);
        failures++;
    }
    /* one fixed size for all samples is still read, each at its place */
    char *fx = mp4_file ("mp4/fixed.mp4", mp4_moov (8, 5, 600, FALSE), 4096);
    d = mp4_demux_open (fx, &err);
    CHECK (d && d->nsamples == 5 && !err);
    if (d && d->nsamples == 5) {
        CHECK (d->samples[0].size == 8 && d->samples[0].off == 0);
        CHECK (d->samples[4].size == 8 && d->samples[4].off == 32);
        const Mp4Sample *s = mp4_demux_next (d);
        CHECK (s && s->size == 8 && s->data && !memcmp (s->data + 4, "ftyp", 4));
    }
    mp4_demux_free (d);
    g_clear_error (&err);

    g_free (ok);
    g_free (many);
    g_free (bomb);
    g_free (ts);
    g_free (tk);
    g_free (rep);
    g_free (fx);
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

    /* ... however the name is spelled: "./" in front of it, or the name as
     * a directory with an entry below it */
    ent (ents, g_strdup ("scene.json"), 0, 2);
    ent (ents, g_strdup ("./.walpe-pkg-stamp"), 0, 8);
    ent (ents, g_strdup (".//.walpe-pkg-src"), 0, 8);
    ent (ents, g_strdup ("././.walpe-pkg-stamp"), 0, 8);
    ent (ents, g_strdup (".walpe-pkg-stamp/x"), 0, 8);
    ent (ents, g_strdup ("./.walpe-pkg-src/y/z"), 0, 8);
    ent (ents, g_strdup (".WALPE-PKG-STAMP"), 0, 8);
    ent (ents, g_strdup (".walpe-pkg-stamp.txt"), 0, 8);   /* another name: the package's */
    ent (ents, g_strdup ("./sub/.walpe-pkg-src"), 0, 8);
    write_pkg ("pkg/stamp2.pkg", ents, 16);
    pkg = P ("pkg/stamp2.pkg");
    out = P ("pkg/stamp2-out");
    CHECK (walpe_pkg_extract (pkg, out, &err) && !err);
    g_clear_error (&err);
    s1 = g_build_filename (out, ".walpe-pkg-stamp", NULL);
    s2 = g_build_filename (out, ".walpe-pkg-src", NULL);
    s3 = g_build_filename (out, ".WALPE-PKG-STAMP", NULL);
    CHECK (!g_file_test (s1, G_FILE_TEST_EXISTS) && !g_file_test (s2, G_FILE_TEST_EXISTS) &&
           !g_file_test (s3, G_FILE_TEST_EXISTS));
    g_free (s1); g_free (s2); g_free (s3);
    s1 = g_build_filename (out, ".walpe-pkg-stamp.txt", NULL);
    s2 = g_build_filename (out, "sub", ".walpe-pkg-src", NULL);
    s3 = g_build_filename (out, "scene.json", NULL);
    CHECK (g_file_test (s1, G_FILE_TEST_IS_REGULAR) && g_file_test (s2, G_FILE_TEST_IS_REGULAR) &&
           g_file_test (s3, G_FILE_TEST_IS_REGULAR));
    g_free (s1); g_free (s2); g_free (s3);
    g_free (pkg);
    g_free (out);

    /* through the cache: a package that makes the stamp's name a directory
     * kept the cache from writing its stamp, and was extracted again at
     * every load. A file put into the copy after the first load is still
     * there after the second: the copy was reused. */
    ent (ents, g_strdup ("scene.json"), 0, 2);
    ent (ents, g_strdup (".walpe-pkg-stamp/x"), 0, 8);
    ent (ents, g_strdup (".walpe-pkg-src/y"), 0, 8);
    write_pkg ("cache/item0/scene.pkg", ents, 16);
    mkfile ("cache/item0/project.json", "{\"type\":\"scene\",\"file\":\"scene.json\"}", -1);
    pkg = P ("cache/item0/scene.pkg");
    char *dir0 = P ("cache/item0");
    char *cache0 = walpe_pkg_extract_cached (pkg, dir0, &err);
    CHECK (cache0 && !err);
    g_clear_error (&err);
    if (cache0) {
        s1 = g_build_filename (cache0, ".walpe-pkg-stamp", NULL);
        s2 = g_build_filename (cache0, ".walpe-pkg-src", NULL);
        s3 = g_build_filename (cache0, "marker", NULL);
        CHECK (g_file_test (s1, G_FILE_TEST_IS_REGULAR) && g_file_test (s2, G_FILE_TEST_IS_REGULAR));
        walpe_pkg_cache_release (cache0);
        CHECK (g_file_set_contents (s3, "x", 1, NULL));
        char *again = walpe_pkg_extract_cached (pkg, dir0, &err);
        CHECK (again && !err && !strcmp (again, cache0));
        g_clear_error (&err);
        CHECK (g_file_test (s3, G_FILE_TEST_IS_REGULAR));
        walpe_pkg_cache_release (again);
        g_free (again);
        /* (the later cases expect an empty cache) */
        g_unlink (pkg);
        ent (ents, g_strdup ("clash"), 0, 4);
        ent (ents, g_strdup ("clash/inside"), 0, 4);
        write_pkg ("cache/item0/scene.pkg", ents, 16);
        again = walpe_pkg_extract_cached (pkg, dir0, &err);
        CHECK (again == NULL && err);
        g_clear_error (&err);
        g_free (again);
        g_free (s1); g_free (s2); g_free (s3);
    }
    g_free (cache0);
    g_free (pkg);
    g_free (dir0);

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

    /* A FIFO named like a package: opening it never returned (and the cache
     * lock was held meanwhile). In a child with an alarm: the extractor
     * refuses it at once, and a wallpaper folder whose only "package" is a
     * FIFO has no content. */
    mkfile ("cache/item3/project.json", "{\"type\":\"scene\",\"file\":\"scene.json\"}", -1);
    char *fifo = P ("cache/item3/scene.pkg"), *lone = P ("pkg/fifo.pkg"), *item = P ("cache/item3");
    if (mkfifo (fifo, 0600) == 0 && mkfifo (lone, 0600) == 0) {
        fflush (NULL);
        pid_t pid = fork ();
        if (pid == 0) {
            alarm (20);
            GError *e = NULL;
            char *o = P ("pkg/fifo-out");
            if (walpe_pkg_extract (lone, o, &e) || !e)
                _exit (3);
            _exit (walpe_content_new (item, NULL, NULL) ? 4 : 0);
        }
        int st = 0;
        CHECK (pid > 0 && waitpid (pid, &st, 0) == pid);
        CHECK (WIFEXITED (st) && WEXITSTATUS (st) == 0);
        if (WIFSIGNALED (st))
            g_printerr ("  a FIFO named like a package was opened and never returned\n");
    }
    g_unlink (fifo);
    g_unlink (lone);
    g_free (fifo);
    g_free (lone);
    g_free (item);
    /* a directory "0.pkg" next to the real package is not taken for it */
    ent (ents, g_strdup ("scene.json"), 0, 2);
    write_pkg ("cache/item4/scene.pkg", ents, 16);
    mkfile ("cache/item4/0.pkg/x", "x", 1);
    mkfile ("cache/item4/project.json", "{\"type\":\"scene\",\"file\":\"scene.json\"}", -1);
    item = P ("cache/item4");
    {
        char *root = g_build_filename (g_get_user_cache_dir (), "walpe", "pkg", NULL);
        guint before = tree_objects (root, 0);
        WalpeContent *c = walpe_content_new (item, NULL, NULL);    /* (its scene.json is no scene) */
        CHECK (c == NULL);
        CHECK (tree_objects (root, 0) > before);                   /* ... but it was extracted */
        g_free (root);
    }
    g_free (item);
    g_array_free (ents, TRUE);
}

/* ------------------------------------------------------------------- font */

static void f16 (GByteArray *a, guint v)
{
    guint8 b[2] = { (guint8) (v >> 8), (guint8) v };
    g_byte_array_append (a, b, 2);
}

static void f32 (GByteArray *a, guint32 v)
{
    guint8 b[4] = { (guint8) (v >> 24), (guint8) (v >> 16), (guint8) (v >> 8), (guint8) v };
    g_byte_array_append (a, b, 4);
}

static void font_name (GByteArray *recs, GByteArray *strs, guint id, const char *s)
{
    f16 (recs, 3); f16 (recs, 1); f16 (recs, 0x409); f16 (recs, id);
    f16 (recs, (guint) strlen (s) * 2);
    f16 (recs, strs->len);
    for (const char *c = s; *c; c++)
        f16 (strs, (guint8) *c);
}

/* A TrueType font written here: 95 glyphs for U+0020..U+007E, each a
 * rectangle, and .notdef. instances > 0: with a variations table of one
 * axis and that many named instances. The table directory is meant to lie
 * at `base` of the file (table offsets count from the file's start). */
static GByteArray *font_tables (const char *family, guint instances, gsize base)
{
    enum { NG = 96 };
    struct { const char *tag; GByteArray *d; } t[11];
    int nt = 0;
    GByteArray *d;

    d = g_byte_array_new ();    /* cmap: format 4, U+0020..U+007E -> glyphs 1..95 */
    f16 (d, 0); f16 (d, 1);
    f16 (d, 3); f16 (d, 1); f32 (d, 12);
    f16 (d, 4); f16 (d, 32); f16 (d, 0);
    f16 (d, 4); f16 (d, 4); f16 (d, 1); f16 (d, 0);
    f16 (d, 0x7e); f16 (d, 0xffff);
    f16 (d, 0);
    f16 (d, 0x20); f16 (d, 0xffff);
    f16 (d, (guint) (1 - 0x20) & 0xffff); f16 (d, 1);
    f16 (d, 0); f16 (d, 0);
    t[nt].tag = "cmap"; t[nt++].d = d;

    if (instances) {            /* fvar: weight 100..900 */
        d = g_byte_array_new ();
        f16 (d, 1); f16 (d, 0);
        f16 (d, 16); f16 (d, 2);
        f16 (d, 1); f16 (d, 20);
        f16 (d, instances); f16 (d, 8);
        g_byte_array_append (d, (const guint8 *) "wght", 4);
        f32 (d, 100u << 16); f32 (d, 400u << 16); f32 (d, 900u << 16);
        f16 (d, 0); f16 (d, 256);
        for (guint i = 0; i < instances; i++) {
            f16 (d, 2); f16 (d, 0);
            f32 (d, (100u + i % 800u) << 16);
        }
        t[nt].tag = "fvar"; t[nt++].d = d;
    }

    GByteArray *glyf = g_byte_array_new (), *loca = g_byte_array_new ();
    f16 (loca, 0);
    for (int g = 1; g < NG; g++) {
        f16 (loca, glyf->len / 2);
        f16 (glyf, 1);
        f16 (glyf, 100); f16 (glyf, 0); f16 (glyf, 600); f16 (glyf, 700);
        f16 (glyf, 3);
        f16 (glyf, 0);
        const guint8 flags[4] = { 1, 1, 1, 1 };
        g_byte_array_append (glyf, flags, 4);
        f16 (glyf, 100); f16 (glyf, 500); f16 (glyf, 0); f16 (glyf, (guint) -500 & 0xffff);
        f16 (glyf, 0); f16 (glyf, 0); f16 (glyf, 700); f16 (glyf, 0);
    }
    f16 (loca, glyf->len / 2);
    t[nt].tag = "glyf"; t[nt++].d = glyf;

    d = g_byte_array_new ();    /* head */
    f32 (d, 0x00010000); f32 (d, 0x00010000); f32 (d, 0); f32 (d, 0x5F0F3CF5);
    f16 (d, 0x000b); f16 (d, 1000);
    f32 (d, 0); f32 (d, 0); f32 (d, 0); f32 (d, 0);
    f16 (d, 0); f16 (d, 0); f16 (d, 700); f16 (d, 700);
    f16 (d, 0); f16 (d, 8); f16 (d, 2); f16 (d, 0); f16 (d, 0);
    t[nt].tag = "head"; t[nt++].d = d;

    d = g_byte_array_new ();    /* hhea */
    f32 (d, 0x00010000);
    f16 (d, 800); f16 (d, (guint) -200 & 0xffff); f16 (d, 0);
    f16 (d, 700); f16 (d, 0); f16 (d, 0); f16 (d, 700);
    f16 (d, 1); f16 (d, 0); f16 (d, 0);
    f16 (d, 0); f16 (d, 0); f16 (d, 0); f16 (d, 0);
    f16 (d, 0); f16 (d, NG);
    t[nt].tag = "hhea"; t[nt++].d = d;

    d = g_byte_array_new ();    /* hmtx */
    for (int g = 0; g < NG; g++) {
        f16 (d, 700);
        f16 (d, g ? 100 : 0);
    }
    t[nt].tag = "hmtx"; t[nt++].d = d;
    t[nt].tag = "loca"; t[nt++].d = loca;

    d = g_byte_array_new ();    /* maxp */
    f32 (d, 0x00010000); f16 (d, NG);
    f16 (d, 4); f16 (d, 1); f16 (d, 0); f16 (d, 0);
    f16 (d, 1); f16 (d, 0); f16 (d, 0); f16 (d, 0); f16 (d, 0);
    f16 (d, 0); f16 (d, 0); f16 (d, 0); f16 (d, 0);
    t[nt].tag = "maxp"; t[nt++].d = d;

    d = g_byte_array_new ();    /* name */
    {
        GByteArray *recs = g_byte_array_new (), *strs = g_byte_array_new ();
        font_name (recs, strs, 1, family);
        font_name (recs, strs, 2, "Regular");
        font_name (recs, strs, 4, family);
        font_name (recs, strs, 6, family);
        f16 (d, 0); f16 (d, 4); f16 (d, 6 + recs->len);
        g_byte_array_append (d, recs->data, recs->len);
        g_byte_array_append (d, strs->data, strs->len);
        g_byte_array_unref (recs);
        g_byte_array_unref (strs);
    }
    t[nt].tag = "name"; t[nt++].d = d;

    d = g_byte_array_new ();    /* post */
    f32 (d, 0x00030000); f32 (d, 0);
    f16 (d, (guint) -100 & 0xffff); f16 (d, 50);
    f32 (d, 0); f32 (d, 0); f32 (d, 0); f32 (d, 0); f32 (d, 0);
    t[nt].tag = "post"; t[nt++].d = d;

    GByteArray *out = g_byte_array_new ();
    f32 (out, 0x00010000);
    f16 (out, (guint) nt); f16 (out, 128); f16 (out, 3); f16 (out, (guint) nt * 16 - 128);
    gsize off = 12 + (gsize) nt * 16;
    for (int i = 0; i < nt; i++) {
        g_byte_array_append (out, (const guint8 *) t[i].tag, 4);
        f32 (out, 0);
        f32 (out, (guint32) (base + off));
        f32 (out, t[i].d->len);
        off += (t[i].d->len + 3u) & ~3u;
    }
    for (int i = 0; i < nt; i++) {
        static const guint8 pad[4] = { 0, 0, 0, 0 };
        g_byte_array_append (out, t[i].d->data, t[i].d->len);
        g_byte_array_append (out, pad, (4 - t[i].d->len % 4) % 4);
        g_byte_array_unref (t[i].d);
    }
    return out;
}

/* writes rel: the font itself (faces == 0), or a collection whose header
 * names the one font `faces` times; a collection is padded to the 32
 * bytes a face FreeType wants to see behind each entry. Returns the path. */
static char *font_file (const char *rel, const char *family, guint instances, guint faces)
{
    GByteArray *out = g_byte_array_new ();
    gsize base = faces ? 12 + (gsize) faces * 4 : 0;
    if (faces) {
        g_byte_array_append (out, (const guint8 *) "ttcf", 4);
        f32 (out, 0x00010000);
        f32 (out, faces);
        for (guint i = 0; i < faces; i++)
            f32 (out, (guint32) base);
    }
    GByteArray *f = font_tables (family, instances, base);
    g_byte_array_append (out, f->data, f->len);
    g_byte_array_unref (f);
    if (faces && out->len < faces * 32 + 64) {
        guint o = out->len;
        g_byte_array_set_size (out, faces * 32 + 64);
        memset (out->data + o, 0, out->len - o);
    }
    mkfile (rel, out->data, out->len);
    g_byte_array_unref (out);
    return P (rel);
}

/* is the layer drawn with the file's own font? (a refused or unreadable
 * one is named in a message and a system font takes its place) */
static gboolean font_taken (const char *path)
{
    WalpeTextStyle st = { 0 };
    float quad[4] = { 0 };
    st.font_path = (char *) path;
    st.pointsize = 12;
    st.halign = st.valign = 1;
    g_string_truncate (logged, 0);
    cairo_surface_t *sf = walpe_text_render (&st, "Text 12:34", quad);
    CHECK (sf != NULL);     /* text is drawn either way */
    if (sf)
        cairo_surface_destroy (sf);
    return strstr (logged->str, "not usable") == NULL;
}

static void test_font (void)
{
    walpe_text_hold ();
    /* a font, a collection of two, a variable font with three named
     * instances: taken as before */
    char *f = font_file ("font/plain.ttf", "Walpe Test Plain", 0, 0);
    gboolean usable = font_taken (f);
    g_free (f);
    if (!usable) {
        g_printerr ("parsers-test: fontconfig here does not take the font this test writes; "
                    "font checks skipped\n");
        walpe_text_release ();
        return;
    }
    f = font_file ("font/two.ttc", "Walpe Test Two", 0, 2);
    CHECK (font_taken (f));
    g_free (f);
    f = font_file ("font/variable.ttf", "Walpe Test Var", 3, 0);
    CHECK (font_taken (f));
    g_free (f);

    /* A collection header that names the font 3000 times (96 KiB of file):
     * fontconfig read every one and kept a pattern for it - 32000 of them
     * were 2.9 s and 86 MiB. Named instances the same: 1000 in each of 4
     * faces. Such a file is not handed over. */
    double c0 = cpu_now ();
    f = font_file ("font/many-faces.ttc", "Walpe Test Many", 0, 3000);
    CHECK (!font_taken (f));
    g_free (f);
    f = font_file ("font/many-instances.ttc", "Walpe Test Inst", 1000, 4);
    CHECK (!font_taken (f));
    g_free (f);
    f = font_file ("font/instances.ttf", "Walpe Test Inst One", 5000, 0);
    CHECK (!font_taken (f));
    g_free (f);
    double cpu = cpu_now () - c0;
    if (cpu > 2.0) {
        g_printerr ("FAIL font files with thousands of faces: %.1f s of CPU\n", cpu);
        failures++;
    }

    /* other kinds of font files FreeType has a parser for: a bitmap font in
     * the BDF format (text, written here), and what only starts like a
     * PCF, Type 1 or WOFF file */
    static const char bdf[] =
        "STARTFONT 2.1\n"
        "FONT -walpe-test-medium-r-normal--8-80-75-75-c-80-iso10646-1\n"
        "SIZE 8 75 75\n"
        "FONTBOUNDINGBOX 8 8 0 0\n"
        "STARTPROPERTIES 2\n"
        "FONT_ASCENT 8\n"
        "FONT_DESCENT 0\n"
        "ENDPROPERTIES\n"
        "CHARS 1\n"
        "STARTCHAR T\n"
        "ENCODING 84\n"
        "SWIDTH 1000 0\n"
        "DWIDTH 8 0\n"
        "BBX 8 8 0 0\n"
        "BITMAP\nFF\n18\n18\n18\n18\n18\n18\n18\n"
        "ENDCHAR\n"
        "ENDFONT\n";
    mkfile ("font/bitmap.bdf.ttf", bdf, -1);
    f = P ("font/bitmap.bdf.ttf");
    CHECK (!font_taken (f));
    g_free (f);
    static const struct { const char *name, *start; gsize n; } other[] = {
        { "font/x.pcf.ttf", "\1fcp\0\0\0\0", 8 },
        { "font/x.pfa.ttf", "%!PS-AdobeFont-1.0: X 1.0\n", 26 },
        { "font/x.woff.ttf", "wOFF\0\1\0\0", 8 },
        { "font/x.empty.ttf", "", 0 },
    };
    for (guint i = 0; i < G_N_ELEMENTS (other); i++) {
        GByteArray *a = g_byte_array_new ();
        g_byte_array_append (a, (const guint8 *) other[i].start, (guint) other[i].n);
        g_byte_array_set_size (a, 512);
        memset (a->data + other[i].n, 0, 512 - other[i].n);
        mkfile (other[i].name, a->data, other[i].n ? a->len : 0);
        g_byte_array_unref (a);
        f = P (other[i].name);
        CHECK (!font_taken (f));
        g_free (f);
    }
    /* a font cut off inside its table directory, a collection whose offsets
     * leave the file: refused or left to the font library, nothing read
     * outside the file */
    f = font_file ("font/cut.ttf", "Walpe Test Cut", 0, 0);
    CHECK (truncate (f, 40) == 0);
    CHECK (!font_taken (f));
    g_free (f);
    f = font_file ("font/cut.ttc", "Walpe Test Cut Two", 0, 4);
    CHECK (truncate (f, 20) == 0);
    CHECK (!font_taken (f));
    g_free (f);
    walpe_text_release ();
}

/* -------------------------------------------------------------------- gif */

/* One image of a GIF: w x h pixels at x, y, every pixel colour 1. 3-bit
 * codes with a clear code before every second pixel (the code table never
 * grows), in data blocks of at most 255 bytes. Codes for 4096 pixels at
 * most: the files with larger images are the ones that must be refused
 * before a pixel is read. local: with a colour table of its own. */
static void gif_image (GByteArray *a, guint x, guint y, guint w, guint h, gboolean local)
{
    const guint8 intro = 0x2c, flags = local ? 0x80 : 0, code_size = 2, end = 0;
    static const guint8 table[] = { 0x10, 0x20, 0x30, 0xe0, 0x80, 0x40 };
    g_byte_array_append (a, &intro, 1);
    put16 (a, (guint16) x);
    put16 (a, (guint16) y);
    put16 (a, (guint16) w);
    put16 (a, (guint16) h);
    g_byte_array_append (a, &flags, 1);
    if (local)
        g_byte_array_append (a, table, sizeof table);
    g_byte_array_append (a, &code_size, 1);
    GByteArray *bits = g_byte_array_new ();
    guint acc = 0, nbits = 0, px = MIN (w * h, 4096u);
    for (guint i = 0; i <= px; i++) {
        /* clear (4) and a pixel, a pixel, ... and end of data (5) */
        guint codes[2] = { 4, 1 }, n = 2;
        if (i == px || (i & 1)) {
            codes[0] = i == px ? 5 : 1;
            n = 1;
        }
        for (guint k = 0; k < n; k++) {
            acc |= codes[k] << nbits;
            nbits += 3;
            while (nbits >= 8) {
                guint8 b = (guint8) acc;
                g_byte_array_append (bits, &b, 1);
                acc >>= 8;
                nbits -= 8;
            }
        }
    }
    if (nbits) {
        guint8 b = (guint8) acc;
        g_byte_array_append (bits, &b, 1);
    }
    for (guint off = 0; off < bits->len; off += 255) {
        guint8 n = (guint8) MIN (255u, bits->len - off);
        g_byte_array_append (a, &n, 1);
        g_byte_array_append (a, bits->data + off, n);
    }
    g_byte_array_append (a, &end, 1);
    g_byte_array_unref (bits);
}

#define GIF_COMMENT "written by the parser tests of this tree"

/* The start of a GIF: a picture of sw x sh pixels with two colours
 * (0x102030, 0xe08040) and, with comment, a comment extension - 63 bytes
 * then, so that a file with one image is above the 64 bytes the texture
 * loader asks of a file. */
static GByteArray *gif_start (guint sw, guint sh, gboolean comment)
{
    GByteArray *a = g_byte_array_new ();
    static const guint8 rest[] = { 0x80, 0, 0, 0x10, 0x20, 0x30, 0xe0, 0x80, 0x40 };
    g_byte_array_append (a, (const guint8 *) "GIF89a", 6);
    put16 (a, (guint16) sw);
    put16 (a, (guint16) sh);
    g_byte_array_append (a, rest, sizeof rest);
    if (comment) {
        const guint8 ext[] = { 0x21, 0xfe, (guint8) strlen (GIF_COMMENT) }, end = 0;
        g_byte_array_append (a, ext, sizeof ext);
        g_byte_array_append (a, (const guint8 *) GIF_COMMENT, (guint) strlen (GIF_COMMENT));
        g_byte_array_append (a, &end, 1);
    }
    return a;
}

static void gif_end (GByteArray *a)
{
    const guint8 trailer = 0x3b;
    g_byte_array_append (a, &trailer, 1);
}

/* one image of w x h pixels at x, y on a picture of sw x sh: is the file
 * taken? */
static const struct {
    const char *name;
    guint sw, sh, x, y, w, h;
    gboolean ok;
} gif_case[] = {
    /* inside the 8 x 6 picture, and touching each of its edges */
    { "inside",        8, 6, 2, 1, 3, 2, TRUE },
    { "whole",         8, 6, 0, 0, 8, 6, TRUE },
    { "left-top",      8, 6, 0, 0, 3, 2, TRUE },
    { "right",         8, 6, 5, 1, 3, 2, TRUE },
    { "bottom",        8, 6, 2, 4, 3, 2, TRUE },
    { "corner",        8, 6, 7, 5, 1, 1, TRUE },
    /* one pixel past the right and the bottom edge: by place and by size */
    { "past-right",    8, 6, 6, 1, 3, 2, FALSE },
    { "past-bottom",   8, 6, 2, 5, 3, 2, FALSE },
    { "too-wide",      8, 6, 0, 0, 9, 6, FALSE },
    { "too-high",      8, 6, 0, 0, 8, 7, FALSE },
    { "at-right",      8, 6, 8, 0, 1, 1, FALSE },   /* starts where the picture ends */
    { "at-bottom",     8, 6, 0, 6, 1, 1, FALSE },
    /* far outside */
    { "far-right",     8, 6, 1000, 1, 3, 2, FALSE },
    { "far-below",     8, 6, 2, 1000, 3, 2, FALSE },
    { "far-both",      8, 6, 40000, 50000, 3, 2, FALSE },
    /* no pixels: the image, the picture */
    { "no-width",      8, 6, 2, 1, 0, 2, FALSE },
    { "no-height",     8, 6, 2, 1, 3, 0, FALSE },
    { "no-size",       8, 6, 0, 0, 0, 0, FALSE },
    { "no-picture-w",  0, 6, 0, 0, 1, 1, FALSE },
    { "no-picture-h",  8, 0, 0, 0, 1, 1, FALSE },
    { "no-picture",    0, 0, 0, 0, 0, 0, FALSE },
    /* the largest numbers 16 bits hold */
    { "max-size",      8, 6, 0, 0, 65535, 65535, FALSE },
    { "max-width",     8, 6, 0, 0, 65535, 6, FALSE },
    { "max-height",    8, 6, 0, 0, 8, 65535, FALSE },
    { "max-place",     8, 6, 65535, 65535, 1, 1, FALSE },
    { "max-all",       8, 6, 65535, 65535, 65535, 65535, FALSE },
};

static GByteArray *gif_case_bytes (guint i, gboolean comment)
{
    GByteArray *a = gif_start (gif_case[i].sw, gif_case[i].sh, comment);
    gif_image (a, gif_case[i].x, gif_case[i].y, gif_case[i].w, gif_case[i].h, FALSE);
    gif_end (a);
    return a;
}

/* the case's file under dir/, g_free () */
static char *gif_case_file (const char *dir, guint i, gboolean comment)
{
    char *rel = g_strdup_printf ("%s/%s%s.gif", dir, gif_case[i].name, comment ? "" : "-short");
    GByteArray *a = gif_case_bytes (i, comment);
    mkfile (rel, a->data, a->len);
    g_byte_array_unref (a);
    char *p = P (rel);
    g_free (rel);
    return p;
}

/* What gdk-pixbuf alone makes of a picture file (a GIF's first frame on
 * its picture), with no check of this tree in between: RGBA, tight rows,
 * g_free (). NULL: not decoded. */
static guint8 *pixels_direct (const char *path, int *w, int *h)
{
    GdkPixbuf *pb = gdk_pixbuf_new_from_file (path, NULL);
    if (!pb)
        return NULL;
    int nch = gdk_pixbuf_get_n_channels (pb), rs = gdk_pixbuf_get_rowstride (pb);
    const guint8 *px = gdk_pixbuf_read_pixels (pb);
    *w = gdk_pixbuf_get_width (pb);
    *h = gdk_pixbuf_get_height (pb);
    guint8 *out = g_malloc ((gsize) *w * *h * 4);
    for (int y = 0; y < *h; y++)
        for (int x = 0; x < *w; x++) {
            const guint8 *s = px + (gsize) y * rs + (gsize) x * nch;
            guint8 *d = out + ((gsize) y * *w + x) * 4;
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
            d[3] = nch == 4 ? s[3] : 0xff;
        }
    g_object_unref (pb);
    return out;
}

/* the pixels of the case's image in an 8 x 6 RGBA picture are the second
 * colour, opaque */
static gboolean gif_case_drawn (guint i, const guint8 *rgba)
{
    for (guint y = gif_case[i].y; y < gif_case[i].y + gif_case[i].h; y++)
        for (guint x = gif_case[i].x; x < gif_case[i].x + gif_case[i].w; x++)
            if (memcmp (rgba + (y * 8 + x) * 4, "\xe0\x80\x40\xff", 4))
                return FALSE;
    return TRUE;
}

static gboolean gif_data_ok (const GByteArray *a)
{
    return walpe_gif_layout_ok (a->data, a->len, NULL);
}

static void test_gif (void)
{
    /* 1. the rule, on the bytes and on the file, and on every start of the
     * file in a block of exactly that size (a GIF that ends early is
     * decoded as far as it goes: checked as far as it goes, never read
     * past its end). A refused file is refused from the byte on that
     * completes the header (no picture) or the image's rectangle. */
    for (guint i = 0; i < G_N_ELEMENTS (gif_case); i++) {
        GByteArray *a = gif_case_bytes (i, TRUE);
        GError *err = NULL;
        gboolean ok = walpe_gif_layout_ok (a->data, a->len, &err);
        if (ok != gif_case[i].ok)
            g_printerr ("parsers-test: gif case %s\n", gif_case[i].name);
        CHECK (ok == gif_case[i].ok);
        CHECK (ok ? err == NULL : err && g_str_has_prefix (err->message, "the GIF is damaged"));
        g_clear_error (&err);
        gsize known = !gif_case[i].sw || !gif_case[i].sh ? 13 : 63 + 10;
        for (gsize len = 0; len < a->len; len++) {
            guint8 *cut = g_malloc (len ? len : 1);
            memcpy (cut, a->data, len);
            CHECK (walpe_gif_layout_ok (cut, len, NULL) == (gif_case[i].ok || len < known));
            g_free (cut);
        }
        g_byte_array_unref (a);
        char *f = gif_case_file ("gif", i, TRUE);
        CHECK (walpe_gif_file_layout_ok (f, &err) == gif_case[i].ok);
        CHECK (gif_case[i].ok ? err == NULL
                              : err && g_str_has_prefix (err->message, "the GIF is damaged"));
        g_clear_error (&err);
        g_free (f);
    }

    /* 2. every image is checked, and exactly what a decoder reads as one */
    {
        /* the second image; an image with a colour table of its own in front */
        GByteArray *a = gif_start (8, 6, TRUE);
        gif_image (a, 0, 0, 8, 6, FALSE);
        gif_image (a, 2, 1, 3, 2, TRUE);
        gif_end (a);
        CHECK (gif_data_ok (a));
        g_byte_array_unref (a);
        a = gif_start (8, 6, TRUE);
        gif_image (a, 0, 0, 8, 6, TRUE);
        gif_image (a, 6, 1, 3, 2, FALSE);
        gif_end (a);
        CHECK (!gif_data_ok (a));
        g_byte_array_unref (a);
        /* bytes between blocks are passed over, as the decoder does */
        a = gif_start (8, 6, TRUE);
        g_byte_array_append (a, (const guint8 *) "\0\0junk\0", 7);
        gif_image (a, 6, 1, 3, 2, FALSE);
        gif_end (a);
        CHECK (!gif_data_ok (a));
        g_byte_array_unref (a);
        a = gif_start (8, 6, TRUE);
        g_byte_array_append (a, (const guint8 *) "\0\0junk\0", 7);
        gif_image (a, 5, 1, 3, 2, FALSE);
        gif_end (a);
        CHECK (gif_data_ok (a));
        g_byte_array_unref (a);
        /* nothing behind the trailer is an image, nor are the bytes of a
         * comment or of an image's data that look like one */
        a = gif_start (8, 6, TRUE);
        gif_image (a, 2, 1, 3, 2, FALSE);
        gif_end (a);
        gif_image (a, 6, 1, 3, 2, FALSE);
        CHECK (gif_data_ok (a));
        g_byte_array_unref (a);
        a = gif_start (8, 6, FALSE);
        g_byte_array_append (a, (const guint8 *) "\x21\xfe\x0b", 3);
        gif_image (a, 6, 1, 0, 0, FALSE);    /* 14 bytes: a block of 11, a block of 1, the end */
        gif_image (a, 2, 1, 3, 2, FALSE);
        gif_end (a);
        CHECK (gif_data_ok (a));
        g_byte_array_unref (a);
        /* a graphic control and a loop extension as every animation has them */
        a = gif_start (8, 6, TRUE);
        g_byte_array_append (a, (const guint8 *) "\x21\xff\x0bNETSCAPE2.0\x03\x01\0\0\0", 19);
        g_byte_array_append (a, (const guint8 *) "\x21\xf9\x04\x04\x0a\0\0\0", 8);
        gif_image (a, 0, 0, 8, 6, FALSE);
        g_byte_array_append (a, (const guint8 *) "\x21\xf9\x04\x04\x0a\0\0\0", 8);
        guint second = a->len;
        gif_image (a, 2, 1, 3, 2, FALSE);
        gif_end (a);
        CHECK (gif_data_ok (a));
        a->data[second + 1] = 6;            /* the second image's left: 6 + 3 > 8 */
        CHECK (!gif_data_ok (a));
        g_byte_array_unref (a);
        /* extensions that decoders do not read alike (so that an image
         * could be one for the decoder and data for this check): label 0;
         * a graphic control or application extension with an empty first
         * block. A comment may be empty. */
        static const struct { const char *ext; guint len; gboolean ok; } odd[] = {
            { "\x21\x00\x01\x00\x00", 5, FALSE },
            { "\x21\x00\x00", 3, FALSE },
            { "\x21\xf9\x00", 3, FALSE },
            { "\x21\xff\x00", 3, FALSE },
            { "\x21\xfe\x00", 3, TRUE },
            { "\x21\x01\x00", 3, TRUE },
        };
        for (guint i = 0; i < G_N_ELEMENTS (odd); i++) {
            a = gif_start (8, 6, TRUE);
            g_byte_array_append (a, (const guint8 *) odd[i].ext, odd[i].len);
            gif_image (a, 2, 1, 3, 2, FALSE);
            gif_end (a);
            CHECK (gif_data_ok (a) == odd[i].ok);
            g_byte_array_unref (a);
        }
        /* GIF87a; a version no decoder has; what is not a GIF at all is
         * not this check's to refuse */
        a = gif_case_bytes (0, TRUE);
        a->data[4] = '7';
        CHECK (gif_data_ok (a));
        a->data[4] = '8';
        CHECK (!gif_data_ok (a));
        g_byte_array_unref (a);
        CHECK (walpe_gif_layout_ok ((const guint8 *) "GIF8", 4, NULL));
        CHECK (walpe_gif_layout_ok ((const guint8 *) "\x89PNG\r\n\x1a\n\0\0\0\rIHDR", 16, NULL));
        CHECK (walpe_gif_layout_ok ((const guint8 *) "BM,\0\0\0\0\0\0\0\xff\xff\xff\xff", 14, NULL));
        CHECK (walpe_gif_layout_ok (NULL, 0, NULL));
    }

    /* 3. the file is read in pieces: 51 KiB of comment (every byte the
     * start of an image, were it not data) in front of the image */
    for (int bad = 0; bad < 2; bad++) {
        GByteArray *a = gif_start (300, 200, TRUE);
        guint8 block[256];
        memset (block, 0x2c, sizeof block);
        block[0] = 255;
        g_byte_array_append (a, (const guint8 *) "\x21\xfe", 2);
        for (int i = 0; i < 200; i++)
            g_byte_array_append (a, block, sizeof block);
        g_byte_array_append (a, (const guint8 *) "", 1);
        gif_image (a, 0, 0, 300, 10, FALSE);
        gif_image (a, bad ? 291 : 290, 190, 10, 10, FALSE);
        gif_end (a);
        mkfile (bad ? "gif/long-bad.gif" : "gif/long.gif", a->data, a->len);
        char *f = P (bad ? "gif/long-bad.gif" : "gif/long.gif");
        GError *err = NULL;
        CHECK (gif_data_ok (a) == !bad);
        CHECK (walpe_gif_file_layout_ok (f, &err) == !bad);
        CHECK (bad ? err && strstr (err->message, "image 2 is 10 x 10 pixels at 291, 190") &&
                         strstr (err->message, "300 x 200")
                   : err == NULL);
        g_clear_error (&err);
        g_free (f);
        g_byte_array_unref (a);
    }
    /* what cannot be read cannot be checked and is not handed on: no file,
     * a directory, a FIFO (not waited for) */
    {
        GError *err = NULL;
        char *f = P ("gif/none.gif");
        CHECK (!walpe_gif_file_layout_ok (f, &err) && err);
        g_clear_error (&err);
        g_free (f);
        f = P ("gif");
        CHECK (!walpe_gif_file_layout_ok (f, &err) && err);
        g_clear_error (&err);
        g_free (f);
        f = P ("gif/pipe.gif");
        if (mkfifo (f, 0600) == 0) {
            CHECK (!walpe_gif_file_layout_ok (f, &err) && err);
            g_clear_error (&err);
        }
        g_free (f);
        CHECK (!walpe_gif_file_layout_ok (NULL, NULL));
    }

    /* 4. the loaders that need no GL context. A GIF as a texture file: the
     * files that are taken give the pixels gdk-pixbuf gives for them, the
     * others no texture. A GIF wallpaper: refused when it is opened, as a
     * damaged file, for every monitor that tries. */
    char *probe = gif_case_file ("gif", 0, TRUE);
    int w = 0, h = 0;
    gboolean have_gif = gdk_pixbuf_get_file_info (probe, &w, &h) != NULL;
    g_free (probe);
    if (!have_gif)
        g_printerr ("parsers-test: this gdk-pixbuf reads no GIF; the loader checks are skipped\n");
    for (guint i = 0; have_gif && i < G_N_ELEMENTS (gif_case); i++) {
        char *f = gif_case_file ("gif", i, TRUE);
        WalpeTex *t = walpe_tex_load (f);
        if (gif_case[i].ok) {
            guint8 *ref = pixels_direct (f, &w, &h);
            CHECK (ref && w == 8 && h == 6 && gif_case_drawn (i, ref));
            CHECK (t && t->width == 8 && t->height == 6 && t->len == 8 * 6 * 4);
            gboolean same = ref && t && t->len == 8 * 6 * 4 && w == 8 && h == 6;
            for (guint p = 0; same && p < 8 * 6; p++)   /* the loader stores BGRA */
                same = t->data[p * 4] == ref[p * 4 + 2] && t->data[p * 4 + 1] == ref[p * 4 + 1] &&
                       t->data[p * 4 + 2] == ref[p * 4] && t->data[p * 4 + 3] == ref[p * 4 + 3];
            if (!same)
                g_printerr ("parsers-test: gif case %s as a texture file\n", gif_case[i].name);
            CHECK (same);       /* the texture's pixels are the decoder's */
            g_free (ref);
        } else {
            CHECK (t == NULL);
        }
        walpe_tex_free (t);
        GError *err = NULL;
        WalpeContent *c = gif_case[i].ok ? NULL : walpe_gif_new (f, &err);
        CHECK (gif_case[i].ok || c);
        if (c) {
            for (int monitor = 0; monitor < 2; monitor++) {
                CHECK (!c->gl_init (c, &err));
                CHECK (err && g_str_has_prefix (err->message, "the GIF is damaged"));
                g_clear_error (&err);
            }
            c->gl_free (c);
            c->free (c);
        }
        g_clear_error (&err);
        g_free (f);
    }
    if (have_gif) {
        /* The check and the decoder agree on what is no image: one behind
         * the trailer, one that is the data of a comment (every image here
         * lies inside the picture). A file with two images is an animation
         * for the decoder; these are stills. */
        for (int k = 0; k < 3; k++) {
            GByteArray *a = gif_start (8, 6, TRUE);
            gif_image (a, 0, 0, 8, 6, FALSE);
            if (k == 1)
                gif_end (a);
            if (k == 2)
                g_byte_array_append (a, (const guint8 *) "\x21\xfe\x0b", 3);
            gif_image (a, 2, 1, 3, 2, FALSE);
            gif_end (a);
            CHECK (gif_data_ok (a));
            mkfile ("gif/two.gif", a->data, a->len);
            g_byte_array_unref (a);
            char *f = P ("gif/two.gif");
            GdkPixbufAnimation *an = gdk_pixbuf_animation_new_from_file (f, NULL);
            CHECK (an != NULL);
            if (an)
                CHECK (!gdk_pixbuf_animation_is_static_image (an) == (k == 0));
            g_clear_object (&an);
            g_free (f);
        }
        /* the largest picture and an image that fills it: inside, and
         * refused by the limit on the picture's size as before */
        GByteArray *a = gif_start (65535, 65535, TRUE);
        gif_image (a, 0, 0, 65535, 65535, FALSE);
        gif_end (a);
        CHECK (gif_data_ok (a));
        mkfile ("gif/max-picture.gif", a->data, a->len);
        g_byte_array_unref (a);
        char *f = P ("gif/max-picture.gif");
        GError *err = NULL;
        WalpeContent *c = walpe_gif_new (f, &err);
        CHECK (c != NULL);
        if (c) {
            CHECK (!c->gl_init (c, &err));
            CHECK (err && strstr (err->message, "the picture is 65535x65535") &&
                   strstr (err->message, "too large"));
            c->gl_free (c);
            c->free (c);
        }
        g_clear_error (&err);
        g_free (f);
    }
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

    /* 5. GIF files (the gif group's) through the loaders that need a
     * context: as a texture (a file of 64 bytes and more goes to the
     * texture loader, a shorter one straight to gdk-pixbuf), as a still
     * picture, as a GIF wallpaper. The ones with their image inside the
     * picture load, and the wallpaper's texture holds the decoder's pixels;
     * the others give nothing, as damaged files. */
    {
        char *probe = gif_case_file ("gl/gif", 0, TRUE);
        int pw = 0, ph = 0;
        gboolean have_gif = gdk_pixbuf_get_file_info (probe, &pw, &ph) != NULL;
        g_free (probe);
        for (guint i = 0; have_gif && i < G_N_ELEMENTS (gif_case); i++) {
            gboolean good = gif_case[i].ok;
            for (int comment = 0; comment < 2; comment++) {
                f = gif_case_file ("gl/gif", i, comment);
                t = walpe_gl_tex_from_file (f);
                if (good ? !t.tex : t.tex != 0)
                    g_printerr ("parsers-test: gif case %s as a texture\n", gif_case[i].name);
                CHECK (good ? t.tex && t.width == 8 && t.height == 6 : t.tex == 0);
                if (t.tex)
                    glDeleteTextures (1, &t.tex);
                g_free (f);
            }
            f = gif_case_file ("gl/gif", i, TRUE);
            GError *err = NULL;
            WalpeContent *c = walpe_image_new (f, &err);
            CHECK (c != NULL);
            if (c) {
                glViewport (0, 0, 64, 36);
                gboolean up = c->gl_init (c, &err);
                CHECK (up == good);
                /* (a picture of no pixels may be no picture at all for gdk-pixbuf) */
                CHECK (up || (err && (g_str_has_prefix (err->message, "the GIF is damaged") ||
                                      !gif_case[i].sw || !gif_case[i].sh)));
                if (up)
                    CHECK (c->gl_render (c, 64, 36, 0.0f, 0.0f));
                c->gl_free (c);
                c->free (c);
            }
            g_clear_error (&err);
            c = good ? walpe_gif_new (f, &err) : NULL;
            CHECK (!good || c);
            if (c && c->gl_init (c, &err)) {
                /* gl_init leaves the content's texture bound */
                GLint tw = 0, th = 0;
                glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
                glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
                CHECK (tw == 8 && th == 6);
                int rw = 0, rh = 0;
                guint8 *ref = pixels_direct (f, &rw, &rh), got[8 * 6 * 4];
                CHECK (ref && rw == 8 && rh == 6 && gif_case_drawn (i, ref));
                gboolean same = ref && rw == 8 && rh == 6 && tw == 8 && th == 6;
                if (same) {
                    memset (got, 0x55, sizeof got);
                    glPixelStorei (GL_PACK_ALIGNMENT, 1);
                    glGetTexImage (GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, got);
                }
                /* premultiplied: what is transparent is 0 */
                for (guint p = 0; same && p < 8 * 6; p++)
                    same = !memcmp (got + p * 4, ref[p * 4 + 3] == 0xff ? ref + p * 4
                                                                       : (const guint8 *) "\0\0\0", 4);
                if (!same)
                    g_printerr ("parsers-test: gif case %s as a GIF wallpaper\n", gif_case[i].name);
                CHECK (same);   /* the wallpaper's texture holds the decoder's pixels */
                g_free (ref);
                glViewport (0, 0, 64, 36);
                CHECK (c->gl_render (c, 64, 36, 0.0f, 0.0f));
                c->gl_free (c);
            } else {
                CHECK (!good);  /* a GIF with its image inside the picture opens */
            }
            if (c)
                c->free (c);
            g_clear_error (&err);
            g_free (f);
        }
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
    if (!only || !strcmp (only, "font"))
        test_font ();
    if (!only || !strcmp (only, "gif"))
        test_gif ();
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
