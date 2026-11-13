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
#include "common/pichead.h"
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
#if defined (__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
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

    /* Half a surrogate pair with the wrong other half: json-glib 1.8 says
     * "malformed surrogate pair" and keeps the string it was reading - all
     * of it up to that place, at every parse. Such a text is refused before
     * json-glib reads it (it was refused before, too); what json-glib takes
     * is taken as before. */
    {
        static const char *const bad[] = {
            "\\ud800\\u0041",      /* a first half, then a letter */
            "\\uD83D\\ud83d",      /* two first halves */
            "\\udc00\\udc00",      /* two second halves */
            "\\udbff\\ue000",
            "\\ud800\\u12",        /* a second escape that is cut short */
            "\\ud8Z\\u0041",       /* two digits read as d800, and take the character behind them */
            "\\ud83d\\ude00\\ud800\\u0041",   /* a good pair first */
        };
        static const char *const good[] = {
            "\\ud83d\\ude00",                  /* a pair */
            "\\\\ud800\\\\u0041",              /* the two characters backslash and u, twice: no escape of a half */
            "\\u00e9\\u0041 \\n \\u1234",
            "\\\\\\ud83d\\ude00",              /* a backslash, then a pair */
        };
        GString *body = g_string_new (NULL);
        for (int i = 0; i < 1024 * 1024; i++)
            g_string_append_c (body, 'x');
        gsize heap0 = heap_in_use ();
        for (guint i = 0; i < G_N_ELEMENTS (bad); i++) {
            for (int in_name = 0; in_name < 2; in_name++) {
                GString *t = g_string_new (in_name ? "{\"" : "{\"t\":\"");
                g_string_append (t, body->str);
                g_string_append (t, bad[i]);
                g_string_append (t, in_name ? "\":1}" : "\"}");
                for (int k = 0; k < 6; k++) {
                    n = wpj_parse_data (t->str, (gssize) t->len);
                    CHECK (n == NULL);
                    g_clear_pointer (&n, json_node_unref);
                }
                g_string_free (t, TRUE);
            }
        }
        gsize heap1 = heap_in_use ();
        if (heap1 > heap0 + 8 * 1024 * 1024)
            g_printerr ("   parses of texts with half a surrogate pair left %.1f MiB allocated\n",
                        (double) (heap1 - heap0) / 1048576.0);
        CHECK (heap1 <= heap0 + 8 * 1024 * 1024);
        for (guint i = 0; i < G_N_ELEMENTS (good); i++) {
            char *t = g_strdup_printf ("{\"t\":\"%s\",\"%s\":[\"%s\"]}", good[i], good[i], good[i]);
            n = wpj_parse_data (t, -1);
            CHECK (n != NULL && jsonglib_takes (t));
            g_clear_pointer (&n, json_node_unref);
            g_free (t);
        }
        /* a half with something else behind it is json-glib's business, as before */
        static const char *const its[] = { "{\"t\":\"a\\ud800b\"}", "{\"t\":\"\\ud800\"}", "{\"t\":\"\\udfff\\n\"}",
                                           "{\"t\":\"\\ud800\\\\\"}", "{\"t\":\"\\u12\"}", "{\"t\":\"x\\",
                                           /* (d800, the backslash taken with it, then "\udc00": a pair to json-glib) */
                                           "{\"t\":\"\\ud8\\\\udc00\"}", "{\"t\":\"\\ud8\\\\udc00\\ud8", "\\ud800\\u" };
        for (guint i = 0; i < G_N_ELEMENTS (its); i++) {
            n = wpj_parse_data (its[i], -1);
            CHECK ((n != NULL) == jsonglib_takes (its[i]));
            g_clear_pointer (&n, json_node_unref);
        }
        /* the file reader */
        GString *t = g_string_new ("{\"title\":\"");
        g_string_append (t, body->str);
        g_string_append (t, "\\ud800\\u0041\"}");
        mkfile ("json/halfpair.json", t->str, (gssize) t->len);
        f = P ("json/halfpair.json");
        heap0 = heap_in_use ();
        for (int k = 0; k < 20; k++)
            CHECK (wpj_parse_file (f) == NULL);
        CHECK (heap_in_use () <= heap0 + 8 * 1024 * 1024);
        g_unlink (f);
        g_free (f);
        g_string_free (t, TRUE);
        g_string_free (body, TRUE);
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
        /* which value is the first does not depend on the json-glib
         * underneath: 1.8 keeps the first value as its root while it reads
         * a second, 1.10 an array of all of them (and gave "no object"
         * for every text here that has an object first) */
        static const struct { const char *text, *member; } firsts[] = {
            { "{\"a\":1} {\"b\":2}", "a" }, { "{\"a\":1}[2]", "a" }, { "{\"a\":1}{\"a2\":2}{\"a3\":3}", "a" },
            { "{\"a\":{\"x\":[1,{\"y\":2}]}} [[3]] {\"c\":3}", "a" },
            { "{\"a\":1} {\"b\":", "a" },           /* the second one cut short */
            { "{\"a\":1} [}", "a" },
            { "\n\t{\"a\":[]}\n{\"b\":{}}\n", "a" },
            /* an array, or a plain value, first: no object */
            { "[1] {\"a\":1}", NULL }, { "7 {\"a\":1}", NULL }, { "\"s\" {\"a\":1}", NULL },
            { "true [1] {\"a\":1}", NULL },
            /* anything else behind the first value: refused, as json-glib refuses it */
            { "{\"a\":1} 7", NULL }, { "{\"a\":1} x", NULL }, { "{\"a\":1}}", NULL }, { "{\"a\":1}]", NULL },
            { "{\"a\":1} \"s\"", NULL }, { "{\"a\":1},{\"b\":2}", NULL }, { "{\"a\":1} 7 {\"b\":2}", NULL },
        };
        for (guint i = 0; i < G_N_ELEMENTS (firsts); i++) {
            n = wpj_parse_data (firsts[i].text, -1);
            if ((n != NULL) != (firsts[i].member != NULL))
                g_printerr ("parsers-test: %s: %s\n", firsts[i].text, n ? "an object" : "nothing");
            CHECK ((n != NULL) == (firsts[i].member != NULL));
            CHECK (!n || (json_object_get_size (json_node_get_object (n)) == 1 &&
                          json_object_has_member (json_node_get_object (n), firsts[i].member)));
            g_clear_pointer (&n, json_node_unref);
        }
        /* ... and for the reader of any first value (a Workshop reply) */
        static const struct { const char *text; int type; } any[] = {
            { "[1] [2] {\"a\":1}", JSON_NODE_ARRAY }, { "7 [1]", JSON_NODE_VALUE }, { "\"s\" {\"a\":1}", JSON_NODE_VALUE },
            { "{\"a\":1} [2]", JSON_NODE_OBJECT }, { "[[1,2],{\"k\":[3]}]{\"a\":1}", JSON_NODE_ARRAY },
        };
        for (guint i = 0; i < G_N_ELEMENTS (any); i++) {
            char *text = g_strdup (any[i].text);
            n = wpj_parse_first (text, strlen (text), NULL);
            CHECK (n && (int) json_node_get_node_type (n) == any[i].type);
            if (n && JSON_NODE_HOLDS_ARRAY (n))
                CHECK (json_array_get_length (json_node_get_array (n)) == (i == 0 ? 1 : 2));
            if (n && JSON_NODE_HOLDS_VALUE (n) && i == 1)
                CHECK (json_node_get_int (n) == 7);
            g_clear_pointer (&n, json_node_unref);
            g_free (text);
        }
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

/* -------------------------------------------------------------------- log */

/* strings of a wallpaper as arguments of log lines (util.h walpe_log_str):
 * the helper itself, then what a scene file's names put into the log */
static void test_log (void)
{
    const char *plain = "materials/Rainy Day \xe2\x98\x94.json";
    CHECK (walpe_log_str ("plain name") != NULL && !strcmp (walpe_log_str ("plain name"), "plain name"));
    CHECK (!strcmp (walpe_log_str (plain), plain));         /* text beyond ASCII stays */
    CHECK (!strcmp (walpe_log_str (NULL), "?"));
    CHECK (!strcmp (walpe_log_str (""), ""));
    CHECK (!strcmp (walpe_log_str ("a\nb\rc\td\x1b[31me\x7f"), "a\\x0ab\\x0dc\\x09d\\x1b[31me\\x7f"));
    CHECK (!strcmp (walpe_log_str ("c1 \xc2\x85 \xc2\x9b"), "c1 \\x85 \\x9b"));   /* NEL, CSI */
    CHECK (!strcmp (walpe_log_str ("bad \xff\xfe utf8 \xc3"), "bad \\xff\\xfe utf8 \\xc3"));
    /* cut between two characters, marked */
    CHECK (!strcmp (walpe_log_strn ("abcdef", 3), "abc [...]"));
    CHECK (!strcmp (walpe_log_strn ("ab\xe2\x98\x94z", 4), "ab [...]"));
    CHECK (!strcmp (walpe_log_strn ("abc", 3), "abc"));
    {
        char *big = g_strnfill (100000, 'W');
        const char *cut = walpe_log_str (big);
        CHECK (strlen (cut) == 2048 + strlen (" [...]"));
        g_free (big);
    }
    /* eight results of one thread are valid together (one log line's arguments) */
    {
        const char *r[8];
        char in[8][8];
        gboolean all = TRUE;
        for (int i = 0; i < 8; i++) {
            g_snprintf (in[i], sizeof in[i], "n%d\n", i);
            r[i] = walpe_log_str (in[i]);
        }
        for (int i = 0; i < 8; i++) {
            char want[16];
            g_snprintf (want, sizeof want, "n%d\\x0a", i);
            all = all && !strcmp (r[i], want);
        }
        CHECK (all);
    }
    /* a scene whose model and layer names carry a line break and an escape
     * sequence: the log has them as text. Before, the part behind the line
     * break stood in the log as a line of the scene's own wording. */
    mkfile ("log/scene/project.json", "{\"file\":\"scene.json\",\"type\":\"scene\"}", -1);
    mkfile ("log/scene/scene.json",
            "{\"general\":{\"orthogonalprojection\":{\"width\":64,\"height\":36}},\"objects\":["
            "{\"id\":1,\"name\":\"n\\nFORGED-NAME\",\"image\":\"models/x\\nFORGED-MODEL \\u001b[2J.json\","
            "\"origin\":\"1 1 0\",\"size\":\"2 2\"}]}", -1);
    char *wp = P ("log/scene");
    GError *err = NULL;
    g_string_truncate (logged, 0);
    WalpeContent *c = walpe_content_new (wp, NULL, &err);
    CHECK (c != NULL);
    CHECK (strstr (logged->str, "FORGED-MODEL") != NULL);                  /* the line was written */
    CHECK (strstr (logged->str, "\nFORGED") == NULL && strchr (logged->str, 0x1b) == NULL);
    CHECK (strstr (logged->str, "models/x\\x0aFORGED-MODEL \\x1b[2J.json") != NULL);
    if (c)
        c->free (c);
    g_clear_error (&err);
    g_free (wp);
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

    /* An arithmetic-coded JPEG is not given to the decoder. The 106 bytes
     * below (found by tests/fuzz, target jpegstore; tests/fuzz/regress has
     * the same bytes as hex dumps) are a progressive arithmetic-coded frame
     * of 24 x 16 pixels and the header of one scan, then the file ends:
     * libjpeg-turbo 2.1.5's arithmetic decoder never returns from them, so
     * before the check this test did not end. What the check must not do is
     * take a frame marker inside another segment for a frame, or lose the
     * frame behind something libjpeg reads across; where it cannot follow
     * a header it refuses. */
    {
        static const guint8 cut[106] = {
            0xFF, 0xD8, 0xFF, 0xDB, 0x00, 0x43, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x32,
            0x32, 0x32, 0xFF, 0xCA, 0x32, 0x32, 0x32, 0xFF, 0xCA, 0x00, 0x11, 0x08, 0x00, 0x10, 0x00, 0x18,
            0x03, 0x01, 0x22, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x2E, 0x00, 0x00, 0x00, 0x00, 0x03, 0x11,
            0xFF, 0xDA, 0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x00, 0x02 };
        static const char *const jpeg_only[] = { "jpeg", NULL };
        CHECK (!walpe_jpeg_coding_ok (cut, sizeof cut));
        /* (its quantisation table holds FF CA: not there that the answer comes from) */
        CHECK (walpe_jpeg_coding_ok (cut, 71));
        for (gsize n = 0; n < 73; n++)          /* cut before the frame's marker: not this check's */
            CHECK (walpe_jpeg_coding_ok (cut, n));
        for (gsize n = 73; n <= sizeof cut; n++)
            CHECK (!walpe_jpeg_coding_ok (cut, n));
        GError *e = NULL;
        CHECK (walpe_picture_decode (cut, sizeof cut, jpeg_only, NULL, NULL, &e) == NULL);
        CHECK (e && strstr (e->message, "arithmetic"));
        g_clear_error (&e);
        /* as a file under three names: the bytes decide */
        static const char *const names[] = { "tex/arith.jpg", "tex/arith.png", "tex/arith" };
        for (guint i = 0; i < G_N_ELEMENTS (names); i++) {
            mkfile (names[i], cut, sizeof cut);
            f = P (names[i]);
            CHECK (!walpe_jpeg_file_coding_ok (f));
            CHECK (walpe_picture_decode_file (f, jpeg_only, NULL, NULL, &e) == NULL);
            CHECK (e && strstr (e->message, "arithmetic"));
            g_clear_error (&e);
            t = walpe_tex_load (f);             /* a picture file as a texture */
            CHECK (t == NULL);
            walpe_tex_free (t);
            CHECK (!walpe_picture_size_ok (f, &e));     /* a picture or GIF wallpaper */
            CHECK (e && strstr (e->message, "arithmetic"));
            g_clear_error (&e);
            g_free (f);
        }
        {   /* ... and inside a texture container, as a wallpaper's textures have their pictures */
            GByteArray *c = tex_container (cut, sizeof cut);
            mkfile ("tex/arith-inside.tex", c->data, c->len);
            f = P ("tex/arith-inside.tex");
            t = walpe_tex_load (f);
            CHECK (t == NULL);
            walpe_tex_free (t);
            g_free (f);
            g_byte_array_unref (c);
        }
        CHECK (walpe_jpeg_file_coding_ok ("/nonexistent/picture.jpg"));
        CHECK (walpe_jpeg_coding_ok (NULL, 0));
        CHECK (walpe_jpeg_coding_ok ((const guint8 *) "\x89PNG\r\n\x1a\n....", 12));
        CHECK (walpe_jpeg_coding_ok ((const guint8 *) "\xff", 1));

        /* headers put together from segments: SOI, then `mid`, then a frame of
         * type `sof` (one component), then a scan header */
        static const guint8 soi[] = { 0xFF, 0xD8 };
        static const guint8 sos[] = { 0xFF, 0xDA, 0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x3F, 0x00 };
        static const guint8 com[] = { 0xFF, 0xFE, 0x00, 0x06, 0xFF, 0xCA, 0xFF, 0xC9 };  /* a comment: FF CA FF C9 */
        static const guint8 exif[] = { 0xFF, 0xE1, 0x00, 0x15, 'E', 'x', 'i', 'f', 0, 0,  /* with a thumbnail that */
                                       0xFF, 0xD8, 0xFF, 0xCA, 0x00, 0x0B, 0x08, 0, 1, 0, 1, 1, 0xFF };  /* is one */
        static const guint8 icc[] = { 0xFF, 0xE2, 0x00, 0x12, 'I', 'C', 'C', '_', 'P', 'R', 'O', 'F', 'I', 'L', 'E', 0,
                                      1, 1, 0xFF, 0xC9 };
        static const guint8 adobe[] = { 0xFF, 0xEE, 0x00, 0x0E, 'A', 'd', 'o', 'b', 'e', 0, 100, 0, 0, 0, 0, 2 };
        static const guint8 fill[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xD0, 0xFF, 0x01, 0x12, 0x34, 0xFF, 0x00 };
        static const guint8 dri[] = { 0xFF, 0xDD, 0x00, 0x04, 0xFF, 0xCA };     /* a restart interval of FFCA */
        static const guint8 dnl[] = { 0xFF, 0xDC, 0x00, 0x04, 0xFF, 0xC9 };
        static const guint8 com0[] = { 0xFF, 0xFE, 0x00, 0x00 };        /* a length libjpeg reads across */
        static const guint8 app1[] = { 0xFF, 0xE5, 0x00, 0x01 };
        static const guint8 dqt0[] = { 0xFF, 0xDB, 0x00, 0x00 };        /* one it gives up on */
        static const guint8 dht1[] = { 0xFF, 0xC4, 0x00, 0x01 };
        static const guint8 jpg[] = { 0xFF, 0xC8, 0x00, 0x02 };         /* a marker it does not take */
        static const guint8 eoi[] = { 0xFF, 0xD9 };
        static const guint8 frame0[] = { 0xFF, 0xC0, 0x00, 0x0B, 0x08, 0x00, 0x10, 0x00, 0x18, 0x01, 0x01, 0x11, 0x00 };
        static const guint8 frame10[] = { 0xFF, 0xCA, 0x00, 0x0B, 0x08, 0x00, 0x10, 0x00, 0x18, 0x01, 0x01, 0x11, 0x00 };
        static const guint8 stray[] = { 0x00, 0x00, 0x00, 0x00, 0x03, 0x11 };
        static const guint8 exif0[] = { 0xFF, 0xE1, 0x00, 0x1F, 'E', 'x', 'i', 'f', 0, 0,   /* thumbnail: SOI, SOF0, SOS, EOI */
                                        0xFF, 0xD8, 0xFF, 0xC0, 0x00, 0x0B, 0x08, 0, 1, 0, 1, 1, 1, 0x11, 0,
                                        0xFF, 0xDA, 0x00, 0x02, 0xFF, 0xD9, 0x00, 0x00 };
#define SEG(x) { x, sizeof x }
        static const struct { const guint8 *d; gsize n; } all[] = { SEG (com), SEG (exif), SEG (icc), SEG (adobe),
                                                                    SEG (fill), SEG (dri), SEG (dnl) };
        static const struct { struct { const guint8 *d; gsize n; } mid[3]; guint8 sof; gboolean ok; } hdr[] = {
            /* a frame that follows what libjpeg reads across is the frame */
            { { SEG (com0) }, 0xCA, FALSE }, { { SEG (app1) }, 0xC9, FALSE }, { { SEG (com0), SEG (app1) }, 0xCB, FALSE },
            /* ... also behind what libjpeg gives up on (a table with no length): the walk reads on */
            { { SEG (dqt0) }, 0xCA, FALSE }, { { SEG (dht1) }, 0xCA, FALSE }, { { SEG (dqt0) }, 0xC0, TRUE },
            /* ... behind bytes that are no marker, as the six in the file above are */
            { { SEG (stray) }, 0xCA, FALSE }, { { SEG (stray) }, 0xC2, TRUE },
            /* ... and behind an Exif thumbnail that is a Huffman-coded JPEG with a frame of its own */
            { { SEG (exif0) }, 0xCA, FALSE }, { { SEG (exif0), SEG (com) }, 0xC0, TRUE },
            /* a marker the walk does not know is not followed, whatever comes then */
            { { SEG (jpg) }, 0xCA, FALSE }, { { SEG (jpg) }, 0xC0, FALSE }, { { SEG (soi) }, 0xC0, FALSE },
            /* a scan or the end of the picture before any frame: there is nothing to decode */
            { { SEG (eoi) }, 0xCA, TRUE }, { { SEG (sos) }, 0xCA, TRUE },
            /* the first frame says how the file is coded */
            { { SEG (frame0) }, 0xCA, TRUE }, { { SEG (frame10) }, 0xC0, FALSE },
        };
#undef SEG
        static const guint8 sofs[] = { 0xC0, 0xC1, 0xC2, 0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF };
        for (guint i = 0; i < G_N_ELEMENTS (sofs) + G_N_ELEMENTS (hdr); i++) {
            gboolean plain = i < G_N_ELEMENTS (sofs);
            guint8 sof = plain ? sofs[i] : hdr[i - G_N_ELEMENTS (sofs)].sof;
            gboolean want = plain ? sof <= 0xC2 : hdr[i - G_N_ELEMENTS (sofs)].ok;
            GByteArray *a = g_byte_array_new ();
            g_byte_array_append (a, soi, sizeof soi);
            if (plain)      /* every kind of segment that may hold the two bytes of a frame marker */
                for (guint k = 0; k < G_N_ELEMENTS (all); k++)
                    g_byte_array_append (a, all[k].d, (guint) all[k].n);
            else
                for (guint k = 0; k < 3 && hdr[i - G_N_ELEMENTS (sofs)].mid[k].d; k++)
                    g_byte_array_append (a, hdr[i - G_N_ELEMENTS (sofs)].mid[k].d,
                                         (guint) hdr[i - G_N_ELEMENTS (sofs)].mid[k].n);
            const guint8 frame[] = { 0xFF, sof, 0x00, 0x0B, 0x08, 0x00, 0x10, 0x00, 0x18, 0x01, 0x01, 0x11, 0x00 };
            g_byte_array_append (a, frame, sizeof frame);
            g_byte_array_append (a, sos, sizeof sos);
            CHECK (walpe_jpeg_coding_ok (a->data, a->len) == want);
            /* bytes before the start of the picture: no JPEG for libjpeg, not this check's */
            g_byte_array_prepend (a, (const guint8 *) "x", 1);
            CHECK (walpe_jpeg_coding_ok (a->data, a->len));
            g_byte_array_unref (a);
        }

        /* a JPEG as every encoder writes it still decodes: this gdk-pixbuf's own, with a comment that
         * holds an arithmetic frame's marker put in front of its first segment */
        GdkPixbuf *src = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, 24, 16);
        gdk_pixbuf_fill (src, 0x336699ff);
        gchar *jd = NULL;
        gsize jn = 0;
        if (gdk_pixbuf_save_to_buffer (src, &jd, &jn, "jpeg", NULL, NULL)) {
            GByteArray *a = g_byte_array_new ();
            g_byte_array_append (a, (const guint8 *) jd, 2);
            g_byte_array_append (a, com, sizeof com);
            g_byte_array_append (a, (const guint8 *) jd + 2, (guint) (jn - 2));
            CHECK (walpe_jpeg_coding_ok (a->data, a->len));
            GdkPixbuf *pb = walpe_picture_decode (a->data, a->len, jpeg_only, NULL, NULL, NULL);
            CHECK (pb && gdk_pixbuf_get_width (pb) == 24 && gdk_pixbuf_get_height (pb) == 16);
            g_clear_object (&pb);
            mkfile ("tex/plain.jpg", a->data, a->len);
            f = P ("tex/plain.jpg");
            CHECK (walpe_jpeg_file_coding_ok (f));
            t = walpe_tex_load (f);
            CHECK (t && t->width == 24 && t->height == 16);
            walpe_tex_free (t);
            CHECK (walpe_picture_size_ok (f, NULL));
            g_free (f);
            GByteArray *c = tex_container (a->data, a->len);
            mkfile ("tex/plain-inside.tex", c->data, c->len);
            f = P ("tex/plain-inside.tex");
            t = walpe_tex_load (f);
            CHECK (t && t->width == 24 && t->height == 16);
            walpe_tex_free (t);
            g_free (f);
            g_byte_array_unref (c);
            g_byte_array_unref (a);
            g_free (jd);
        }
        g_object_unref (src);
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
            /* gdk-pixbuf's animation interface, as engine/gif.c calls it:
             * deprecated in 2.44 without a successor in the library */
            G_GNUC_BEGIN_IGNORE_DEPRECATIONS
            GdkPixbufAnimation *an = gdk_pixbuf_animation_new_from_file (f, NULL);
            gboolean still = an && gdk_pixbuf_animation_is_static_image (an);
            G_GNUC_END_IGNORE_DEPRECATIONS
            CHECK (an != NULL);
            if (an)
                CHECK (!still == (k == 0));
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

/* the peak of this process's resident memory in KiB (VmHWM), 0 = not
 * known; _restart: the peak begins again at what is resident now */
static gint64 rss_peak_kb (void)
{
    char *st = NULL;
    gint64 kb = 0;
    if (g_file_get_contents ("/proc/self/status", &st, NULL, NULL)) {
        const char *p = strstr (st, "VmHWM:");
        if (p)
            kb = g_ascii_strtoll (p + 6, NULL, 10);
    }
    g_free (st);
    return kb;
}

static gint64 rss_peak_restart (void)
{
    FILE *f = fopen ("/proc/self/clear_refs", "w");
    if (!f)
        return 0;
    gboolean ok = fputs ("5", f) >= 0;
    ok = fclose (f) == 0 && ok;
    return ok ? rss_peak_kb () : 0;
}

/* bytes of every texture the context holds */
static gint64 gl_tex_bytes (void)
{
    int count = 0;
    gint64 bytes = 0, comp = 0;
    walpe_gl_tex_totals (&count, &bytes, &comp);
    return bytes;
}

/* is a gdk-pixbuf loader module of that name in this process? */
static gboolean loader_mapped (const char *name)
{
    char *maps = NULL, *lib = g_strdup_printf ("libpixbufloader-%s.", name);
    gboolean in = g_file_get_contents ("/proc/self/maps", &maps, NULL, NULL) && strstr (maps, lib);
    g_free (maps);
    g_free (lib);
    return in;
}

/* The decoder processes of this process: where gdk-pixbuf decodes through
 * glycin (2.44) a decoder is a child process ("glycin-svg", "glycin-heif",
 * "glycin-image-rs" ...; inside a sandbox its grandchild), kept for half a
 * minute after its last picture. The number of them whose name starts with
 * `name`, and through *peak_kb (may be NULL) the largest resident memory
 * any of them has had. 0 where gdk-pixbuf decodes in this process. */
static int decoder_children (const char *name, gint64 *peak_kb)
{
    GDir *proc = g_dir_open ("/proc", 0, NULL);
    const char *e;
    int n = 0;
    if (peak_kb)
        *peak_kb = 0;
    while (proc && (e = g_dir_read_name (proc))) {
        const int pid = atoi (e);
        if (pid <= 1 || pid == getpid ())
            continue;
        int up = pid, hops = 0;
        char *st = NULL, comm[64] = "";
        gint64 hwm = 0;
        while (up > 1 && up != getpid () && hops++ < 4) {
            char *f = g_strdup_printf ("/proc/%d/status", up), *t = NULL;
            int next = 0;
            if (g_file_get_contents (f, &t, NULL, NULL)) {
                const char *q = strstr (t, "\nPPid:");
                next = q ? atoi (q + 6) : 0;
                if (up == pid)
                    st = g_steal_pointer (&t);
            }
            g_free (t);
            g_free (f);
            up = next;
        }
        if (up == getpid () && st) {
            const char *q = strstr (st, "\nVmHWM:");
            if (q)
                hwm = g_ascii_strtoll (q + 7, NULL, 10);
            sscanf (st, "Name:\t%63s", comm);
            if (g_str_has_prefix (comm, name)) {
                n++;
                if (peak_kb)
                    *peak_kb = MAX (*peak_kb, hwm);
            }
        }
        g_free (st);
    }
    if (proc)
        g_dir_close (proc);
    return n;
}

/* a HEIC picture of 64 x 48 pixels (523 bytes, as heif-enc 1.23 writes it) */
static const guint8 heic_64x48[] = {
            0x00,0x00,0x00,0x1c,0x66,0x74,0x79,0x70,0x68,0x65,0x69,0x63,0x00,0x00,0x00,0x00,0x6d,0x69,0x66,0x31,
            0x68,0x65,0x69,0x63,0x6d,0x69,0x61,0x66,0x00,0x00,0x01,0x7d,0x6d,0x65,0x74,0x61,0x00,0x00,0x00,0x00,
            0x00,0x00,0x00,0x21,0x68,0x64,0x6c,0x72,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x70,0x69,0x63,0x74,
            0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x22,0x69,0x6c,0x6f,
            0x63,0x00,0x00,0x00,0x00,0x44,0x40,0x00,0x01,0x00,0x01,0x00,0x00,0x00,0x00,0x01,0xa1,0x00,0x01,0x00,
            0x00,0x00,0x00,0x00,0x00,0x00,0x6a,0x00,0x00,0x00,0x23,0x69,0x69,0x6e,0x66,0x00,0x00,0x00,0x00,0x00,
            0x01,0x00,0x00,0x00,0x15,0x69,0x6e,0x66,0x65,0x02,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x68,0x76,0x63,
            0x31,0x00,0x00,0x00,0x00,0x0e,0x70,0x69,0x74,0x6d,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0xfd,
            0x69,0x70,0x72,0x70,0x00,0x00,0x00,0xdd,0x69,0x70,0x63,0x6f,0x00,0x00,0x00,0x76,0x68,0x76,0x63,0x43,
            0x01,0x03,0x70,0x00,0x00,0x00,0x90,0x00,0x00,0x00,0x00,0x00,0x1e,0xf0,0x00,0xfc,0xfd,0xf8,0xf8,0x00,
            0x00,0x0f,0x03,0x60,0x00,0x01,0x00,0x18,0x40,0x01,0x0c,0x01,0xff,0xff,0x03,0x70,0x00,0x00,0x03,0x00,
            0x90,0x00,0x00,0x03,0x00,0x00,0x03,0x00,0x1e,0xba,0x02,0x40,0x61,0x00,0x01,0x00,0x2a,0x42,0x01,0x01,
            0x03,0x70,0x00,0x00,0x03,0x00,0x90,0x00,0x00,0x03,0x00,0x00,0x03,0x00,0x1e,0xa0,0x20,0x81,0x05,0x96,
            0xea,0xe4,0xa6,0xb9,0xb8,0x08,0x68,0x30,0x20,0x00,0x00,0x03,0x03,0x20,0x00,0x00,0x03,0x00,0x21,0x62,
            0x00,0x01,0x00,0x06,0x44,0x01,0xc1,0x73,0xc0,0x89,0x00,0x00,0x00,0x13,0x63,0x6f,0x6c,0x72,0x6e,0x63,
            0x6c,0x78,0x00,0x01,0x00,0x0d,0x00,0x06,0x80,0x00,0x00,0x00,0x14,0x69,0x73,0x70,0x65,0x00,0x00,0x00,
            0x00,0x00,0x00,0x00,0x40,0x00,0x00,0x00,0x40,0x00,0x00,0x00,0x28,0x63,0x6c,0x61,0x70,0x00,0x00,0x00,
            0x40,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x30,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
            0x02,0xff,0xff,0xff,0xf0,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x10,0x70,0x69,0x78,0x69,0x00,0x00,0x00,
            0x00,0x03,0x08,0x08,0x08,0x00,0x00,0x00,0x18,0x69,0x70,0x6d,0x61,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
            0x01,0x00,0x01,0x05,0x81,0x02,0x03,0x05,0x84,0x00,0x00,0x00,0x72,0x6d,0x64,0x61,0x74,0x00,0x00,0x00,
            0x66,0x28,0x01,0xaf,0x13,0x80,0xf5,0x45,0x3e,0x14,0x9d,0x9b,0x8e,0xc9,0xf6,0x37,0x34,0x56,0x84,0xec,
            0x52,0xae,0x90,0x1a,0xe4,0x3b,0x51,0xaa,0x1d,0x9b,0x7b,0xf2,0xfc,0xf0,0xea,0x10,0x65,0x1a,0xa2,0xbf,
            0xc9,0x3a,0xec,0x7d,0x6b,0xc8,0xdf,0x92,0xda,0x97,0x1e,0xc4,0xa9,0x9c,0xa2,0xbd,0xc7,0x1f,0x2b,0x7b,
            0xd3,0xda,0x04,0x39,0xf0,0x67,0x61,0x17,0x42,0x45,0xa1,0x8a,0xca,0xa5,0x8d,0x71,0xa5,0xd8,0x60,0x21,
            0xff,0xa7,0x55,0xb2,0x11,0xb2,0x7a,0xbe,0x42,0xef,0x75,0x9f,0xf0,0x6f,0xc7,0x94,0x56,0x38,0x4c,0xca,
            0xd7,0xce,0xee,
};

/* a 4 x 4 TGA picture: colour map type cmap (1: with a colour map of two
 * entries, used or not), picture type 1 / 2 / 3 / 9 / 10 / 11, the colour
 * map's first index `first` */
static void save_tga (const char *rel, int cmap, int type, int first)
{
    guint8 b[18 + 6 + 4 * (1 + 4 * 3)], *o = b;
    const int grey = type == 3 || type == 11, mapped = type == 1 || type == 9, rle = type >= 9;
    const int bytes = grey || mapped ? 1 : 3;
    memset (b, 0, sizeof b);
    b[1] = (guint8) cmap;
    b[2] = (guint8) type;
    b[3] = (guint8) (first & 0xff);
    b[4] = (guint8) (first >> 8);
    b[5] = cmap ? 2 : 0;
    b[7] = cmap ? 24 : 0;
    b[12] = b[14] = 4;
    b[16] = (guint8) (bytes * 8);
    b[17] = 0x20;
    o += 18;
    if (cmap) {
        static const guint8 map[6] = { 10, 20, 30, 200, 210, 220 };
        memcpy (o, map, 6);
        o += 6;
    }
    for (int y = 0; y < 4; y++) {
        if (rle)
            *o++ = 3;       /* a raw packet of four pixels */
        for (int x = 0; x < 4; x++)
            for (int c = 0; c < bytes; c++)
                *o++ = mapped ? (guint8) ((x + y) & 1) : (guint8) (40 * x + 20 * y + 30 * c);
    }
    mkfile (rel, b, o - b);
}

/* a 4 x 4 RGB picture as a TIFF file of the 64-bit kind ("BigTIFF") */
static void save_bigtiff (const char *rel)
{
    static const struct { guint16 tag, type; guint64 count, value; } e[] = {
        { 256, 3, 1, 4 }, { 257, 3, 1, 4 }, { 258, 3, 3, 8 | 8 << 16 | (guint64) 8 << 32 }, { 259, 3, 1, 1 },
        { 262, 3, 1, 2 }, { 273, 16, 1, 232 }, { 277, 3, 1, 3 }, { 278, 3, 1, 4 }, { 279, 16, 1, 48 },
        { 284, 3, 1, 1 },
    };
    guint8 b[232 + 48], *o = b;
    memset (b, 0, sizeof b);
    memcpy (o, "II+\0\10\0\0\0\20\0\0\0\0\0\0\0", 16);     /* 8-byte offsets, the directory at 16 */
    o += 16;
    *o = G_N_ELEMENTS (e);
    o += 8;
    for (guint i = 0; i < G_N_ELEMENTS (e); i++, o += 20) {
        o[0] = (guint8) (e[i].tag & 0xff);
        o[1] = (guint8) (e[i].tag >> 8);
        o[2] = (guint8) e[i].type;
        o[4] = (guint8) e[i].count;
        for (int k = 0; k < 8; k++)
            o[12 + k] = (guint8) (e[i].value >> (8 * k));
    }
    for (int i = 0; i < 48; i++)
        b[232 + i] = (guint8) (5 * i);
    mkfile (rel, b, sizeof b);
}

static void gl_part (void)
{
    walpe_glutil_init ();
    /* 0. (first: nothing below may have opened these loaders yet) a layer
     * picture that is a file of another kind does not reach that kind's
     * decoder. The format used to be asked from gdk-pixbuf, which hands
     * the file to whichever loader matches its bytes before it can say
     * what it is: a PNM, a Windows icon, an XPM or an SVG under a
     * picture's name was read by those loaders and refused afterwards. */
    {
        static const guint8 ico[] = { 0, 0, 1, 0, 1, 0, 16, 16, 0, 0, 1, 0, 32, 0, 0x28, 0, 0, 0, 22, 0, 0, 0,
                                      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        /* a Windows cursor: its first bytes are also those of a TGA
         * picture, and gdk-pixbuf took it for the cursor */
        static const guint8 cur[] = { 0, 0, 2, 0, 1, 0, 16, 16, 0, 0, 1, 0, 1, 0, 0x28, 0, 0, 0, 22, 0, 0, 0,
                                      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        static const struct { const char *file, *loader; const void *data; gssize len; } other[] = {
            { "gl/other/pnm.png", "pnm", "P6\n4 4\n255\n" "000000000000000000000000000000000000000000000000", -1 },
            { "gl/other/ico.png", "ico", ico, sizeof ico },
            { "gl/other/pnm.tga", "pnm", "P6\n4 4\n255\n" "000000000000000000000000000000000000000000000000", -1 },
            { "gl/other/cur.tga", "ico", cur, sizeof cur },
            /* ... and where gdk-pixbuf decodes through glycin, which goes by
             * the bytes whatever loader it was asked through: an SVG
             * document, a HEIC picture, a QOI picture or a PNM under names
             * whose loaders took them there (every name but ".png", which
             * was held to a PNG's chunks) */
            { "gl/other/svg.jpg", "svg", "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"8\" height=\"8\">"
                                         "<rect width=\"8\" height=\"8\" fill=\"red\"/></svg>", -1 },
            { "gl/other/svg.tga", "svg", "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"8\" height=\"8\">"
                                         "<rect width=\"8\" height=\"8\" fill=\"red\"/></svg>", -1 },
            { "gl/other/svg-large.gif", "svg", "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"16000\" "
                                               "height=\"16000\"><rect width=\"8\" height=\"8\" fill=\"red\"/></svg>", -1 },
            { "gl/other/heic.jpg", "heif", heic_64x48, sizeof heic_64x48 },
            { "gl/other/heic.avif", "heif", heic_64x48, sizeof heic_64x48 },
            { "gl/other/qoi.bmp", "qoi", "qoif\0\0\0\4\0\0\0\4\3\0\xfe\x10\x20\x30\xfd\xcd\0\0\0\0\0\0\0\1", 28 },
            { "gl/other/pnm.jxl", "pnm", "P6\n4 4\n255\n" "000000000000000000000000000000000000000000000000", -1 },
        };
        for (guint i = 0; i < G_N_ELEMENTS (other); i++) {
            mkfile (other[i].file, other[i].data, other[i].len);
            char *of = P (other[i].file);
            gboolean before = loader_mapped (other[i].loader);
            WalpeGlTex ot = walpe_gl_tex_from_file (of);
            CHECK (ot.tex == 0);
            if (ot.tex)
                glDeleteTextures (1, &ot.tex);
            if (!before) {
                if (loader_mapped (other[i].loader))
                    g_printerr ("parsers-test: %s was read by gdk-pixbuf's %s loader\n", other[i].file,
                                other[i].loader);
                CHECK (!loader_mapped (other[i].loader));
            }
            g_free (of);
        }
        /* no decoder process was started for an SVG document or a HEIC
         * picture (glycin's; there are none where gdk-pixbuf decodes in
         * this process) */
        CHECK (decoder_children ("glycin-svg", NULL) == 0);
        CHECK (decoder_children ("glycin-heif", NULL) == 0);
        /* The bytes decide, and the name where they say nothing: a TGA
         * picture (2 x 2, 32 bits) under a PNG's name is a TGA picture;
         * one whose second byte is damaged is still handed to the TGA
         * decoder under its own name, and to none under another. */
        {
            guint8 tga[18 + 16] = { 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 2, 0, 32, 0x28 };
            memset (tga + 18, 0xc0, 16);
            mkfile ("gl/other/real-tga.png", tga, sizeof tga);
            tga[1] = 0xff;
            mkfile ("gl/other/odd.tga", tga, sizeof tga);
            mkfile ("gl/other/odd.dat", tga, sizeof tga);
            static const struct { const char *file; gboolean loads; } named[] = {
                { "gl/other/real-tga.png", TRUE }, { "gl/other/odd.tga", TRUE }, { "gl/other/odd.dat", FALSE } };
            for (guint i = 0; i < G_N_ELEMENTS (named); i++) {
                char *nf = P (named[i].file);
                WalpeGlTex nt = walpe_gl_tex_from_file (nf);
                if ((nt.tex != 0) != named[i].loads)
                    g_printerr ("parsers-test: %s %s\n", named[i].file, nt.tex ? "loaded" : "refused");
                CHECK ((nt.tex != 0) == named[i].loads && (!nt.tex || (nt.width == 2 && nt.height == 2)));
                if (nt.tex)
                    glDeleteTextures (1, &nt.tex);
                g_free (nf);
            }
        }
        /* a PNG that ends before its last chunk (here: behind its header,
         * a file of under 64 bytes) is refused, not shown as an empty
         * picture: the decoder fed piece by piece does not say that the
         * file ended early */
        {
            save_png ("gl/other/whole.png", 4, 4);
            char *wf = P ("gl/other/whole.png"), *data = NULL;
            gsize wl = 0;
            if (g_file_get_contents (wf, &data, &wl, NULL) && wl > 48) {
                /* the header chunk, then the picture data's chunk begun */
                guint8 cut[33 + 8 + 6];
                memcpy (cut, data, 33);
                memcpy (cut + 33, "\0\0\0\x10IDAT\x78\x01\x63\x60\x60\x60", 14);
                mkfile ("gl/other/cut.png", cut, sizeof cut);
                char *cf = P ("gl/other/cut.png");
                WalpeGlTex ct = walpe_gl_tex_from_file (cf);
                CHECK (ct.tex == 0);
                if (ct.tex)
                    glDeleteTextures (1, &ct.tex);
                g_free (cf);
            }
            g_free (data);
            g_free (wf);
        }
        /* (where gdk-pixbuf has these loaders built in, or not at all, the
         * check above cannot fail) */
        char *of = P (other[0].file);
        GdkPixbuf *direct = gdk_pixbuf_new_from_file (of, NULL);
        if (!loader_mapped ("pnm"))
            g_printerr ("parsers-test: gdk-pixbuf's pnm loader is no module here (check cannot fail)\n");
        g_clear_object (&direct);
        g_free (of);
    }
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

    /* 1b. ... and all of those. The format is settled from the file's
     * first bytes, and a TGA header or a TIFF file can say more than the
     * common case: a true-colour or grey TGA picture that carries a colour
     * map, one without a colour map whose colour map fields are not zero
     * (its first bytes are a Windows cursor's), a TIFF of the 64-bit kind.
     * All were layer pictures while gdk-pixbuf chose the loader - it knew
     * them by their names - and stay ones. */
    {
        static const struct { const char *file; int cmap, type, first; } tga[] = {
            { "gl/tga/plain.tga", 0, 2, 0 }, { "gl/tga/mapped.tga", 1, 1, 0 },
            { "gl/tga/true-with-map.tga", 1, 2, 0 }, { "gl/tga/true-rle-with-map.tga", 1, 10, 0 },
            { "gl/tga/grey-with-map.tga", 1, 3, 0 }, { "gl/tga/grey-rle-with-map.TGA", 1, 11, 0 },
            { "gl/tga/map-origin.tga", 0, 2, 256 },
        };
        for (guint i = 0; i < G_N_ELEMENTS (tga); i++) {
            save_tga (tga[i].file, tga[i].cmap, tga[i].type, tga[i].first);
            f = P (tga[i].file);
            t = walpe_gl_tex_from_file (f);
            if (!(t.tex && t.width == 4 && t.height == 4))
                g_printerr ("parsers-test: %s is no layer picture any more\n", tga[i].file);
            CHECK (t.tex && t.width == 4 && t.height == 4);
            if (t.tex)
                glDeleteTextures (1, &t.tex);
            g_free (f);
        }
        /* the same bytes under a name that is no TGA's: not a TGA by them alone */
        save_tga ("gl/tga/true-with-map.png", 1, 2, 0);
        f = P ("gl/tga/true-with-map.png");
        t = walpe_gl_tex_from_file (f);
        CHECK (t.tex == 0);
        if (t.tex)
            glDeleteTextures (1, &t.tex);
        g_free (f);

        save_bigtiff ("gl/big.tif");
        f = P ("gl/big.tif");
        char *raw = NULL;
        gsize raw_len = 0;
        GdkPixbufLoader *ld = gdk_pixbuf_loader_new_with_type ("tiff", NULL);
        gboolean reads = ld && g_file_get_contents (f, &raw, &raw_len, NULL) &&
                         gdk_pixbuf_loader_write (ld, (const guchar *) raw, raw_len, NULL);
        if (ld)
            reads = gdk_pixbuf_loader_close (ld, NULL) && reads && gdk_pixbuf_loader_get_pixbuf (ld) != NULL;
        g_clear_object (&ld);
        g_free (raw);
        if (!reads) {
            g_printerr ("parsers-test: this gdk-pixbuf's TIFF loader does not read a 64-bit TIFF file "
                        "(check cannot fail here)\n");
        } else {
            t = walpe_gl_tex_from_file (f);
            if (!(t.tex && t.width == 4 && t.height == 4))
                g_printerr ("parsers-test: a TIFF file of the 64-bit kind is no layer picture any more\n");
            CHECK (t.tex && t.width == 4 && t.height == 4);
            if (t.tex)
                glDeleteTextures (1, &t.tex);
        }
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
    /* 5b. an animated PNG as a picture wallpaper: its first frame is shown.
     * The bytes are a PNG's by their signature and "image/apng" to the
     * system's file type table; a gdk-pixbuf with its own PNG decoder
     * (2.42) lists no loader for that type and, left to choose, knows no
     * format for the file. Such a file was the texture loader's (the "png"
     * loader by name) and has to stay a picture; asked through
     * gdk_pixbuf_new_from_file alone it was "failed to load image". */
    {
        static const guint8 apng[] = {
            0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
            0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x06, 0x08, 0x02, 0x00, 0x00, 0x00, 0x71, 0x67, 0x48,
            0xac, 0x00, 0x00, 0x00, 0x08, 0x61, 0x63, 0x54, 0x4c, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
            0x00, 0xf3, 0x8d, 0x93, 0x70, 0x00, 0x00, 0x00, 0x1a, 0x66, 0x63, 0x54, 0x4c, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x01, 0x00, 0x0a, 0x00, 0x00, 0x1f, 0x3d, 0xa2, 0x17, 0x00, 0x00, 0x00, 0x14, 0x49,
            0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x3c, 0xa1, 0xa1, 0xc1, 0x80, 0x0d, 0x30, 0x61, 0x15, 0xa5,
            0x93, 0x04, 0x00, 0xa6, 0x9a, 0x01, 0x24, 0x28, 0x96, 0xe0, 0x48, 0x00, 0x00, 0x00, 0x1a, 0x66,
            0x63, 0x54, 0x4c, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x06, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x0a, 0x00, 0x00, 0x84, 0x4e, 0x48,
            0xc3, 0x00, 0x00, 0x00, 0x18, 0x66, 0x64, 0x41, 0x54, 0x00, 0x00, 0x00, 0x02, 0x78, 0x9c, 0x63,
            0xd4, 0xd0, 0x38, 0xc1, 0x80, 0x0d, 0x30, 0x61, 0x15, 0xa5, 0x93, 0x04, 0x00, 0xa5, 0x5a, 0x01,
            0x24, 0x54, 0xd6, 0xa0, 0x9b, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60,
            0x82,
        };
        mkfile ("gl/two-frames.png", apng, sizeof apng);
        f = P ("gl/two-frames.png");
        GError *err = NULL;
        WalpeContent *c = walpe_image_new (f, &err);
        CHECK (c != NULL);
        if (c) {
            glViewport (0, 0, 64, 36);
            gboolean up = c->gl_init (c, &err);
            if (!up)
                g_printerr ("parsers-test: an animated PNG as a picture wallpaper: %s\n", err ? err->message : "?");
            CHECK (up);
            if (up) {
                /* twice: nothing is decoded again for the size it is shown at */
                CHECK (c->gl_render (c, 64, 36, 0.0f, 0.0f));
                CHECK (c->gl_render (c, 64, 36, 0.0f, 0.0f));
            }
            c->gl_free (c);
            c->free (c);
        }
        g_clear_error (&err);
        g_free (f);
    }
    /* 6. a JPEG layer picture above the size one texture gets is decoded
     * at a smaller size - but a progressive one is held whole by its
     * decoder first, at the size its header declares. 136 bytes that
     * declare 20000 x 16000 pixels (one component, the first scan cut
     * short): a store of 640 MiB was filled before the decoder found out
     * that there is no picture. Now the file is refused before it is
     * opened for decoding; the peak of resident memory says which. */
    {
        static const guint8 jpg[] = {
            0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 0x4a, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01,
            0x00, 0x01, 0x00, 0x00, 0xff, 0xdb, 0x00, 0x43, 0x00, 0x10, 0x0b, 0x0c, 0x0e, 0x0c, 0x0a, 0x10,
            0x0e, 0x0d, 0x0e, 0x12, 0x11, 0x10, 0x13, 0x18, 0x28, 0x1a, 0x18, 0x16, 0x16, 0x18, 0x31, 0x23,
            0x25, 0x1d, 0x28, 0x3a, 0x33, 0x3d, 0x3c, 0x39, 0x33, 0x38, 0x37, 0x40, 0x48, 0x5c, 0x4e, 0x40,
            0x44, 0x57, 0x45, 0x37, 0x38, 0x50, 0x6d, 0x51, 0x57, 0x5f, 0x62, 0x67, 0x68, 0x67, 0x3e, 0x4d,
            0x71, 0x79, 0x70, 0x64, 0x78, 0x5c, 0x65, 0x67, 0x63, 0xff, 0xc2, 0x00, 0x0b, 0x08,
            0x3e, 0x80,     /* height 16000 */
            0x4e, 0x20,     /* width 20000 */
            0x01, 0x01, 0x11, 0x00, 0xff, 0xc4, 0x00, 0x14, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xda, 0x00, 0x08,
            0x01, 0x01, 0x00, 0x00, 0x00, 0x01, 0x0f, 0xff,
        };
        mkfile ("gl/held-whole.jpg", jpg, sizeof jpg);
        f = P ("gl/held-whole.jpg");
        int jw = 0, jh = 0;
        if (gdk_pixbuf_get_file_info (f, &jw, &jh) && jw == 20000 && jh == 16000) {
            CHECK (walpe_jpeg_store_bytes (jpg, sizeof jpg) == (guint64) 2500 * 2000 * 128);
            gint64 hwm0 = rss_peak_restart ();
            t = walpe_gl_tex_from_file (f);
            gint64 hwm1 = rss_peak_kb ();
            CHECK (t.tex == 0);
            if (hwm0 > 0 && hwm1 > 0) {
                if (hwm1 - hwm0 >= 256 * 1024)
                    g_printerr ("parsers-test: a refused JPEG layer picture took %.0f MiB\n",
                                (hwm1 - hwm0) / 1024.0);
                CHECK (hwm1 - hwm0 < 256 * 1024);
            } else {
                g_printerr ("parsers-test: no peak of resident memory here (check cannot fail)\n");
            }
            if (t.tex)
                glDeleteTextures (1, &t.tex);
        } else {
            g_printerr ("parsers-test: this gdk-pixbuf does not read the JPEG header (check cannot fail here)\n");
        }
        g_free (f);
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

/* ---- common/pichead.c: a picture's format and size from its own header,
 * read before gdk-pixbuf is asked anything */
static void ph_be32 (GByteArray *a, guint32 v)
{
    guint8 b[4] = { (guint8) (v >> 24), (guint8) (v >> 16), (guint8) (v >> 8), (guint8) v };
    g_byte_array_append (a, b, 4);
}

static void ph_box (GByteArray *a, const char *type, const guint8 *body, gsize n)
{
    ph_be32 (a, (guint32) (n + 8));
    g_byte_array_append (a, (const guint8 *) type, 4);
    if (n)
        g_byte_array_append (a, body, (guint) n);
}

/* bits from the lowest of each byte on, as a JPEG XL codestream has them */
static void ph_bits (GByteArray *a, guint *pos, guint64 v, guint n)
{
    for (guint i = 0; i < n; i++, (*pos)++) {
        if (*pos / 8 >= a->len) {
            guint8 z = 0;
            g_byte_array_append (a, &z, 1);
        }
        a->data[*pos / 8] |= (guint8) (((v >> i) & 1) << (*pos % 8));
    }
}

static gboolean ph_size (const char *kind, const guint8 *d, gsize n, int ew, int eh)
{
    int w = -1, h = -1, fw = -1, fh = -1;
    const gboolean ok = walpe_pichead_size (kind, d, n, &w, &h);
    /* the reader of a file says the same as the reader of a buffer */
    mkfile ("pichead/probe.bin", d, (gssize) n);
    char *f = P ("pichead/probe.bin");
    const gboolean fok = walpe_pichead_file_size (kind, f, &fw, &fh);
    g_free (f);
    if (ok != fok || (ok && (w != fw || h != fh))) {
        g_printerr ("parsers-test: %s header: buffer %d (%d x %d), file %d (%d x %d)\n", kind, ok, w, h, fok, fw, fh);
        return FALSE;
    }
    if (ew == 0)
        return !ok;
    if (!ok || w != ew || h != eh)
        g_printerr ("parsers-test: %s header: %d, %d x %d, expected %d x %d\n", kind, ok, w, h, ew, eh);
    return ok && w == ew && h == eh;
}

static void test_pichead (void)
{
    /* 1. signatures. A file with "ftyp" at offset 4 is an ISO media file
     * whatever its first four bytes are: glycin goes by that box before it
     * looks for a JPEG's, a BMP's or a TIFF's start, and decodes with
     * libheif */
    static const struct { const char *bytes; gsize n; const char *kind; } sig[] = {
        { "\x89PNG\r\n\x1a\n", 8, "png" }, { "\xff\xd8\xff\xe0", 4, "jpeg" }, { "GIF89a", 6, "gif" },
        { "GIF87a", 6, "gif" }, { "GIF8xa", 6, NULL }, { "RIFF\0\0\0\0WEBP", 12, "webp" },
        { "II*\0", 4, "tiff" }, { "MM\0*", 4, "tiff" }, { "II+\0", 4, NULL }, { "\xff\x0a", 2, "jxl" },
        { "\0\0\0\x0cJXL \r\n\x87\n", 12, "jxl" }, { "BM", 2, "bmp" },
        { "\0\0\0\x1c" "ftypavif\0\0\0\0", 16, "avif" },
        { "\0\0\0\x1c" "ftypmif1\0\0\0\0miafavif", 24, "avif" },
        { "\0\0\0\x18" "ftypheic\0\0\0\0mif1heic", 24, NULL },
        { "BM\0\0ftypheic\0\0\0\0", 16, NULL }, { "\xff\xd8\xff\xe0" "ftypheic\0\0\0\0", 16, NULL },
        { "II*\0ftypheic\0\0\0\0", 16, NULL }, { "\0\0\2\0ftypheic\0\0\0\0", 16, NULL },
        /* a box length that is a TGA header's map flag and picture type (bytes 1 and 2) to
         * glycin, which ranks that above the brand: 0x0A00 with a brand list that names avif
         * (a run-length picture there), 0x0200 with avif as the major brand (true colour) */
        { "\0\0\x0a\0" "ftypmif1\xe0\x2e\xe0\x2e\x20\x28\0\0avif", 24, NULL },
        { "\0\0\2\0" "ftypavif\x64\0\x64\0\x20\x28\0\0", 20, NULL },
        { "\0\1\1\0" "ftypmif1\0\0\0\0avif", 20, NULL }, { "\x20\0\0\x20" "ftypavif\0\0\0\0", 16, NULL },
        { "\0\0\0\xff" "ftypavif\0\0\0\0", 16, "avif" },
        { "<svg xmlns", 10, NULL }, { "P6\n4 4\n", 7, NULL }, { "qoif", 4, NULL }, { "DDS ", 4, NULL },
        { "\0\0\1\0", 4, NULL }, { "/* XPM */", 9, NULL }, { "", 0, NULL },
    };
    for (guint i = 0; i < G_N_ELEMENTS (sig); i++) {
        guint8 *b = g_memdup2 (sig[i].n ? sig[i].bytes : "x", sig[i].n ? sig[i].n : 1);
        if (g_strcmp0 (walpe_pichead_kind (b, sig[i].n), sig[i].kind))
            g_printerr ("parsers-test: signature %u is %s\n", i, walpe_pichead_kind (b, sig[i].n));
        CHECK (!g_strcmp0 (walpe_pichead_kind (b, sig[i].n), sig[i].kind));
        g_free (b);
    }
    CHECK (walpe_pichead_kind (NULL, 0) == NULL);
    CHECK (walpe_pichead_is_bigtiff ((const guint8 *) "MM\0+", 4) && !walpe_pichead_is_bigtiff ((const guint8 *) "II*\0", 4));
    CHECK (walpe_pichead_exact ("png") && walpe_pichead_exact ("tga") && !walpe_pichead_exact ("tiff") &&
           !walpe_pichead_exact ("avif") && !walpe_pichead_exact (NULL));

    /* 2. sizes, each format's header written by hand */
    GByteArray *a = g_byte_array_new ();
    {   /* PNG: the header chunk first; not when another chunk is, not cut short */
        g_byte_array_append (a, (const guint8 *) "\x89PNG\r\n\x1a\n\0\0\0\x0dIHDR", 16);
        ph_be32 (a, 30000);
        ph_be32 (a, 20000);
        g_byte_array_append (a, (const guint8 *) "\x08\x06\0\0\0", 5);
        CHECK (ph_size ("png", a->data, a->len, 30000, 20000));
        CHECK (ph_size ("png", a->data, 23, 0, 0));
        a->data[12] = 'i';
        CHECK (ph_size ("png", a->data, a->len, 0, 0));
        a->data[12] = 'I';
        memset (a->data + 16, 0, 4);
        CHECK (ph_size ("png", a->data, a->len, 0, 0));        /* no pixels wide */
    }
    {
        static const guint8 gif[] = { 'G', 'I', 'F', '8', '9', 'a', 0x40, 0x1f, 0x70, 0x17, 0, 0, 0 };
        CHECK (ph_size ("gif", gif, sizeof gif, 8000, 6000));
        CHECK (ph_size ("gif", gif, 9, 0, 0));
    }
    {   /* BMP: signed 32-bit fields (rows from the top: a negative height), 16-bit ones in the oldest header */
        guint8 bmp[54] = { 'B', 'M', 0, 0, 0, 0, 0, 0, 0, 0, 54, 0, 0, 0, 40, 0, 0, 0,
                           0x2c, 1, 0, 0, 0x38, 0xff, 0xff, 0xff, 1, 0, 24, 0 };
        CHECK (ph_size ("bmp", bmp, sizeof bmp, 300, 200));
        bmp[21] = 0x80;     /* a width below zero */
        CHECK (ph_size ("bmp", bmp, sizeof bmp, 0, 0));
        static const guint8 old[26] = { 'B', 'M', 0, 0, 0, 0, 0, 0, 0, 0, 26, 0, 0, 0, 12, 0, 0, 0,
                                        0x10, 0x27, 0x20, 0x4e, 1, 0, 24, 0 };
        CHECK (ph_size ("bmp", old, sizeof old, 10000, 20000));
    }
    {   /* WebP: the three first chunks */
        static const guint8 x[] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P', 'V', 'P', '8', 'X', 10, 0, 0, 0,
                                    0, 0, 0, 0, 0xe7, 0x03, 0, 0xcf, 0x07, 0 };
        CHECK (ph_size ("webp", x, sizeof x, 1000, 2000));
        static const guint8 l[] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P', 'V', 'P', '8', 'L', 10, 0, 0, 0,
                                    0x2f, 0x3f, 0xc0, 0x2f, 0x00 };     /* 64 x 192 */
        CHECK (ph_size ("webp", l, sizeof l, 64, 192));
        guint8 v[] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P', 'V', 'P', '8', ' ', 10, 0, 0, 0,
                       0x10, 0, 0, 0x9d, 0x01, 0x2a, 0x80, 0x02, 0xe0, 0x01 };
        CHECK (ph_size ("webp", v, sizeof v, 640, 480));
        v[23] = 0;          /* not a key frame's start code */
        CHECK (ph_size ("webp", v, sizeof v, 0, 0));
        static const guint8 other[] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P', 'A', 'L', 'P', 'H', 0, 0, 0, 0,
                                        0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        CHECK (ph_size ("webp", other, sizeof other, 0, 0));
    }
    {   /* JPEG: the first frame header on libjpeg's walk - not one inside a comment */
        static const guint8 jpg[] = { 0xff, 0xd8, 0xff, 0xfe, 0, 11, 0xff, 0xc0, 0, 11, 8, 0, 1, 0, 1,
                                      0xff, 0xe0, 0, 4, 'J', 'F',
                                      0xff, 0xc2, 0, 11, 8, 0x04, 0xd2, 0x10, 0xe1, 1, 1, 0x11, 0,
                                      0xff, 0xda, 0, 8, 1, 1, 0, 0, 63, 0 };
        CHECK (ph_size ("jpeg", jpg, sizeof jpg, 4321, 1234));
        CHECK (ph_size ("jpeg", jpg, 26, 0, 0));        /* ends inside the frame header */
        static const guint8 noframe[] = { 0xff, 0xd8, 0xff, 0xda, 0, 8, 1, 1, 0, 0, 63, 0, 0xff, 0xd9 };
        CHECK (ph_size ("jpeg", noframe, sizeof noframe, 0, 0));
        /* Where libjpeg and zune-jpeg (the decoder behind glycin) walk differently, there is
         * no size. A restart marker in front of the scan has no segment for the one and a
         * length for the other, which skips with it a frame header of 16 x 16 and decodes
         * the next, 9000 x 9000 */
        #define SOF_16 0, 11, 8, 0, 16, 0, 16, 1, 1, 0x11, 0
        #define SOF_9000 0, 11, 8, 0x23, 0x28, 0x23, 0x28, 1, 1, 0x11, 0
        #define SCAN 0xff, 0xda, 0, 8, 1, 1, 0, 0, 63, 0
        static const guint8 behind_rst[] = { 0xff, 0xd8, 0xff, 0xd0, 0, 15, 0xff, 0xc0, SOF_16, 0xff, 0xc0, SOF_9000, SCAN };
        CHECK (ph_size ("jpeg", behind_rst, sizeof behind_rst, 0, 0));
        static const guint8 behind_tem[] = { 0xff, 0xd8, 0xff, 0x01, 0, 15, 0xff, 0xc0, SOF_16, 0xff, 0xc0, SOF_9000, SCAN };
        CHECK (ph_size ("jpeg", behind_tem, sizeof behind_tem, 0, 0));
        /* a frame header of a kind zune-jpeg skips (lossless) and then one it decodes; two of one kind */
        static const guint8 two_kinds[] = { 0xff, 0xd8, 0xff, 0xc3, SOF_16, 0xff, 0xc0, SOF_9000, SCAN };
        CHECK (ph_size ("jpeg", two_kinds, sizeof two_kinds, 0, 0));
        static const guint8 two[] = { 0xff, 0xd8, 0xff, 0xc0, SOF_16, 0xff, 0xc4, 0, 2, 0xff, 0xc0, SOF_9000, SCAN };
        CHECK (ph_size ("jpeg", two, sizeof two, 0, 0));
        /* one frame header of any kind and the scan behind it: the size, as before */
        static const guint8 lossless[] = { 0xff, 0xd8, 0xff, 0xc3, SOF_9000, 0xff, 0xdd, 0, 4, 0, 0, SCAN };
        CHECK (ph_size ("jpeg", lossless, sizeof lossless, 9000, 9000));
        CHECK (ph_size ("jpeg", lossless, sizeof lossless - 10, 0, 0));     /* (no scan: no picture) */
        static const guint8 ended[] = { 0xff, 0xd8, 0xff, 0xc0, SOF_9000, 0xff, 0xd9 };
        CHECK (ph_size ("jpeg", ended, sizeof ended, 0, 0));
        #undef SOF_16
        #undef SOF_9000
        #undef SCAN
    }
    {   /* TIFF: both byte orders; of a tag that is there twice the larger value; not the 64-bit kind */
        static const guint8 le[] = { 'I', 'I', 42, 0, 8, 0, 0, 0, 3, 0,
                                     0, 1, 3, 0, 1, 0, 0, 0, 0x20, 0x03, 0, 0,
                                     1, 1, 4, 0, 1, 0, 0, 0, 0x58, 0x02, 0, 0,
                                     0, 1, 4, 0, 1, 0, 0, 0, 0x40, 0x9c, 0, 0,
                                     0, 0, 0, 0 };
        CHECK (ph_size ("tiff", le, sizeof le, 40000, 600));
        static const guint8 be[] = { 'M', 'M', 0, 42, 0, 0, 0, 8, 0, 2,
                                     1, 0, 0, 3, 0, 0, 0, 1, 0x03, 0x20, 0, 0,
                                     1, 1, 0, 4, 0, 0, 0, 1, 0, 0, 0x02, 0x58,
                                     0, 0, 0, 0 };
        CHECK (ph_size ("tiff", be, sizeof be, 800, 600));
        CHECK (ph_size ("tiff", be, 30, 0, 0));
        static const guint8 big[] = { 'I', 'I', 43, 0, 8, 0, 0, 0, 16, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        CHECK (ph_size ("tiff", big, sizeof big, 0, 0));        /* (a directory without the two tags) */
        /* the number types a decoder takes for the two tags, not only SHORT and LONG: the
         * tiff crate behind glycin reads a width of type IFD (13) or LONG8 (16, the value
         * where the entry points), libtiff the signed kinds and BYTE */
        static const guint8 types[] = { 'I', 'I', 42, 0, 8, 0, 0, 0, 2, 0,
                                        0, 1, 13, 0, 1, 0, 0, 0, 0x28, 0x23, 0, 0,
                                        1, 1, 16, 0, 1, 0, 0, 0, 38, 0, 0, 0,
                                        0, 0, 0, 0,
                                        0x70, 0x11, 0x01, 0, 0, 0, 0, 0 };
        CHECK (ph_size ("tiff", types, sizeof types, 9000, 70000));
        CHECK (ph_size ("tiff", types, sizeof types - 1, 0, 0));       /* the far value is cut short */
        static const guint8 small[] = { 'M', 'M', 0, 42, 0, 0, 0, 8, 0, 2,
                                        1, 0, 0, 1, 0, 0, 0, 1, 200, 0, 0, 0,
                                        1, 1, 0, 9, 0, 0, 0, 1, 0, 0, 0x02, 0x58,
                                        0, 0, 0, 0 };
        CHECK (ph_size ("tiff", small, sizeof small, 200, 600));
        static const guint8 below[] = { 'M', 'M', 0, 42, 0, 0, 0, 8, 0, 2,
                                        1, 0, 0, 3, 0, 0, 0, 1, 0, 200, 0, 0,
                                        1, 1, 0, 9, 0, 0, 0, 1, 0xff, 0xff, 0xff, 0xfe,
                                        0, 0, 0, 0 };
        CHECK (ph_size ("tiff", below, sizeof below, 0, 0));          /* a height below zero */
        static const guint8 twice[] = { 'I', 'I', 42, 0, 8, 0, 0, 0, 2, 0,
                                        0, 1, 4, 0, 2, 0, 0, 0, 0x28, 0x23, 0, 0,
                                        1, 1, 4, 0, 1, 0, 0, 0, 0x28, 0x23, 0, 0,
                                        0, 0, 0, 0 };
        CHECK (ph_size ("tiff", twice, sizeof twice, 0, 0));          /* two values where one is asked for */
        /* the 64-bit kind: its directory, 20 bytes an entry, the values in the entries */
        static const guint8 big2[] = { 'M', 'M', 0, 43, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 16,
                                       0, 0, 0, 0, 0, 0, 0, 2,
                                       1, 0, 0, 16, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0xe2, 0x40,
                                       1, 1, 0, 3, 0, 0, 0, 0, 0, 0, 0, 1, 0, 77, 0, 0, 0, 0, 0, 0,
                                       0, 0, 0, 0, 0, 0, 0, 0 };
        CHECK (ph_size ("tiff", big2, sizeof big2, 123456, 77));
        CHECK (ph_size ("tiff", big2, sizeof big2 - 9, 0, 0));
        guint8 many[sizeof big2];
        memcpy (many, big2, sizeof many);
        many[21] = 0x10;        /* 2^20 + 2 entries: no directory a reader follows */
        CHECK (ph_size ("tiff", many, sizeof many, 0, 0));
    }
    {   /* AVIF: the largest of the "ispe" properties (meta > iprp > ipco) */
        GByteArray *ipco = g_byte_array_new (), *iprp = g_byte_array_new (), *meta = g_byte_array_new ();
        static const guint8 ispe1[12] = { 0, 0, 0, 0, 0, 0, 0, 64, 0, 0, 0, 48 },
                            ispe2[12] = { 0, 0, 0, 0, 0, 0, 0x0f, 0xa0, 0, 0, 0x0b, 0xb8 },
                            pixi[5] = { 0, 0, 0, 0, 3 };
        ph_box (ipco, "ispe", ispe1, 12);
        ph_box (ipco, "pixi", pixi, 5);
        ph_box (ipco, "ispe", ispe2, 12);
        ph_box (iprp, "ipco", ipco->data, ipco->len);
        g_byte_array_append (meta, (const guint8 *) "\0\0\0\0", 4);
        ph_box (meta, "hdlr", (const guint8 *) "\0\0\0\0\0\0\0\0pict", 12);
        ph_box (meta, "iprp", iprp->data, iprp->len);
        g_byte_array_set_size (a, 0);
        ph_box (a, "ftyp", (const guint8 *) "avif\0\0\0\0avifmif1", 16);
        ph_box (a, "free", NULL, 0);
        ph_box (a, "meta", meta->data, meta->len);
        CHECK (ph_size ("avif", a->data, a->len, 4000, 3000));
        CHECK (ph_size ("avif", a->data, a->len - 14, 0, 0));       /* the box says more than the file has */
        g_byte_array_set_size (a, 0);
        ph_box (a, "ftyp", (const guint8 *) "avif\0\0\0\0avifmif1", 16);
        ph_box (a, "mdat", (const guint8 *) "\0\0\0\0", 4);
        CHECK (ph_size ("avif", a->data, a->len, 0, 0));
        {
            /* A file without a size is not handed to a decoder: libheif decodes an AVIF
             * without "ispe" all the same (the error is the header's, not "no loader") */
            static const char *const avif_only[] = { "avif", NULL };
            GError *e = NULL;
            CHECK (walpe_picture_decode (a->data, a->len, avif_only, NULL, NULL, &e) == NULL);
            CHECK (g_error_matches (e, G_IO_ERROR, G_IO_ERROR_INVALID_DATA));
            g_clear_error (&e);
            /* An image sequence - a "moov" box, in front of the "meta" box or behind it -
             * has no size: it is played from its track, at the size of its AV1 data, whatever
             * "ispe" says (64 x 64 here stood for frames of 9000 x 9000 on Arch) */
            static const guint8 seq[12] = { 0, 0, 0, 0, 0, 0, 0, 64, 0, 0, 0, 64 };
            GByteArray *co = g_byte_array_new (), *rp = g_byte_array_new (), *me = g_byte_array_new ();
            ph_box (co, "ispe", seq, 12);
            ph_box (rp, "ipco", co->data, co->len);
            g_byte_array_append (me, (const guint8 *) "\0\0\0\0", 4);
            ph_box (me, "iprp", rp->data, rp->len);
            g_byte_array_set_size (a, 0);
            ph_box (a, "ftyp", (const guint8 *) "avis\0\0\0\0avifavis", 16);
            ph_box (a, "meta", me->data, me->len);
            const guint still = a->len;
            CHECK (ph_size ("avif", a->data, a->len, 64, 64));
            ph_box (a, "moov", (const guint8 *) "\0\0\0\0", 4);
            ph_box (a, "mdat", (const guint8 *) "\0\0\0\0", 4);
            CHECK (ph_size ("avif", a->data, a->len, 0, 0));
            CHECK (walpe_picture_decode (a->data, a->len, avif_only, NULL, NULL, &e) == NULL);
            CHECK (g_error_matches (e, G_IO_ERROR, G_IO_ERROR_INVALID_DATA));
            g_clear_error (&e);
            g_byte_array_set_size (a, 0);
            ph_box (a, "ftyp", (const guint8 *) "avis\0\0\0\0avifavis", 16);
            ph_box (a, "moov", (const guint8 *) "\0\0\0\0", 4);
            ph_box (a, "meta", me->data, me->len);
            CHECK (ph_size ("avif", a->data, a->len, 0, 0));
            /* the file is walked to its end: up to seven bytes there are no box to any
             * reader; eight that are none (a length past the end) could hide one */
            g_byte_array_set_size (a, 0);
            ph_box (a, "ftyp", (const guint8 *) "avis\0\0\0\0avifavis", 16);
            ph_box (a, "meta", me->data, me->len);
            g_byte_array_append (a, (const guint8 *) "\xff\xff\xff\xff\xff\xff\xff", 7);
            CHECK (a->len == still + 7 && ph_size ("avif", a->data, a->len, 64, 64));
            g_byte_array_append (a, (const guint8 *) "\xff", 1);
            CHECK (ph_size ("avif", a->data, a->len, 0, 0));
            /* and more boxes than are walked: no size */
            g_byte_array_set_size (a, 0);
            ph_box (a, "ftyp", (const guint8 *) "avis\0\0\0\0avifavis", 16);
            ph_box (a, "meta", me->data, me->len);
            for (guint k = 0; k < 4096; k++)
                ph_box (a, "free", NULL, 0);
            CHECK (ph_size ("avif", a->data, a->len, 0, 0));
            g_byte_array_unref (co);
            g_byte_array_unref (rp);
            g_byte_array_unref (me);
        }
        g_byte_array_unref (ipco);
        g_byte_array_unref (iprp);
        g_byte_array_unref (meta);
    }
    {   /* JPEG XL: the size header, short form, long form and by a ratio; in a container's first part */
        static const guint8 small[] = { 0xff, 0x0a, 0x0b, 0x0e };
        CHECK (ph_size ("jxl", small, sizeof small, 64, 48));
        static const guint8 ratio[] = { 0xff, 0x0a, 0xcb, 0x06 };         /* what cjxl writes for 64 x 48: 4:3 */
        CHECK (ph_size ("jxl", ratio, sizeof ratio, 64, 48));
        guint pos = 16;
        g_byte_array_set_size (a, 0);
        g_byte_array_append (a, (const guint8 *) "\xff\x0a", 2);
        ph_bits (a, &pos, 0, 1);
        ph_bits (a, &pos, 1, 2);
        ph_bits (a, &pos, 2999, 13);
        ph_bits (a, &pos, 0, 3);
        ph_bits (a, &pos, 3, 2);
        ph_bits (a, &pos, 99999, 30);
        CHECK (ph_size ("jxl", a->data, a->len, 100000, 3000));
        CHECK (ph_size ("jxl", a->data, 5, 0, 0));
        GByteArray *part = g_byte_array_new ();
        g_byte_array_append (part, (const guint8 *) "\0\0\0\0", 4);
        g_byte_array_append (part, a->data, a->len);
        g_byte_array_set_size (a, 0);
        g_byte_array_append (a, (const guint8 *) "\0\0\0\x0cJXL \r\n\x87\n", 12);
        ph_box (a, "ftyp", (const guint8 *) "jxl \0\0\0\0jxl ", 12);
        ph_box (a, "jxlp", part->data, part->len);
        CHECK (ph_size ("jxl", a->data, a->len, 100000, 3000));
        /* the codestream cut into parts anywhere - a first part of the two signature bytes,
         * an empty one, the rest: the size is the codestream's, not the next box's header
         * read as one (1 x 1 to 1 x 512 it was, for a picture of any size) */
        {
            GByteArray *c = g_byte_array_new ();
            g_byte_array_append (c, (const guint8 *) "\0\0\0\x0cJXL \r\n\x87\n", 12);
            ph_box (c, "ftyp", (const guint8 *) "jxl \0\0\0\0jxl ", 12);
            ph_box (c, "jxlp", (const guint8 *) "\0\0\0\0\xff\x0a", 6);
            ph_box (c, "jxlp", (const guint8 *) "\0\0\0\1", 4);
            ph_box (c, "free", (const guint8 *) "\xff\x0a\x0b\x0e", 4);
            GByteArray *rest = g_byte_array_new ();
            g_byte_array_append (rest, (const guint8 *) "\x80\0\0\2", 4);
            g_byte_array_append (rest, part->data + 6, part->len - 6);
            ph_box (c, "jxlp", rest->data, rest->len);
            CHECK (ph_size ("jxl", c->data, c->len, 100000, 3000));
            /* cut inside the size: no answer */
            CHECK (ph_size ("jxl", c->data, c->len - 6, 0, 0));
            g_byte_array_unref (rest);
            g_byte_array_unref (c);
        }
        g_byte_array_unref (part);
    }
    CHECK (!walpe_pichead_size ("svg", (const guint8 *) "<svg", 4, &(int) { 0 }, &(int) { 0 }));
    CHECK (!walpe_pichead_size (NULL, (const guint8 *) "<svg", 4, &(int) { 0 }, &(int) { 0 }));
    CHECK (!walpe_pichead_file_size ("png", "/nonexistent/x.png", &(int) { 0 }, &(int) { 0 }));

    /* 3. against this gdk-pixbuf: the size of what it writes */
    {
        static const char *const kinds[] = { "png", "jpeg", "bmp", "tiff" };
        GRand *r = g_rand_new_with_seed (7);
        for (guint i = 0; i < 24; i++) {
            const char *kind = kinds[i % G_N_ELEMENTS (kinds)];
            const int pw = g_rand_int_range (r, 1, 700), ph = g_rand_int_range (r, 1, 700);
            GdkPixbuf *pb = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, pw, ph);
            char *buf = NULL;
            gsize n = 0;
            gdk_pixbuf_fill (pb, 0x406080ff);
            if (gdk_pixbuf_save_to_buffer (pb, &buf, &n, kind, NULL, NULL)) {
                CHECK (!g_strcmp0 (walpe_pichead_kind ((const guint8 *) buf, n), kind));
                CHECK (ph_size (kind, (const guint8 *) buf, n, pw, ph));
            }
            g_free (buf);
            g_object_unref (pb);
        }
        g_rand_free (r);
    }

    /* 4. damaged headers: the two readers agree, and nothing is read past the bytes given */
    {
        static const char *const kinds[] = { "png", "jpeg", "gif", "bmp", "webp", "tga", "tiff", "avif", "jxl" };
        GRand *r = g_rand_new_with_seed (11);
        for (guint i = 0; i < 4000; i++) {
            const gsize n = (gsize) g_rand_int_range (r, 0, 96);
            guint8 *b = g_malloc (n + 1);
            for (gsize k = 0; k < n; k++)
                b[k] = (guint8) (g_rand_int_range (r, 0, 4) ? a->data[k % a->len] : g_rand_int_range (r, 0, 256));
            if (n >= 12 && (i & 1))
                memcpy (b + 4, i & 2 ? "ftyp" : "meta", 4);
            int w = 0, h = 0;
            for (guint k = 0; k < G_N_ELEMENTS (kinds); k++)
                if (walpe_pichead_size (kinds[k], b, n, &w, &h))
                    CHECK (w >= 1 && h >= 1);
            walpe_pichead_kind (b, n);
            walpe_tga_head (b, n, NULL);
            g_free (b);
        }
        g_rand_free (r);
    }
    g_byte_array_unref (a);

    /* 5. TGA headers: what gdk-pixbuf's decoder takes, and the header a decoder is given */
    {
        guint8 t[18] = { 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 3, 0, 24, 0x20 };
        WalpeTga tga;
        CHECK (walpe_tga_head (t, 18, &tga) && tga.w == 4 && tga.h == 3 && !tga.changed && tga.skip_len == 0);
        CHECK (!walpe_tga_head (t, 17, &tga));
        /* true colour with a colour map of two 24-bit entries behind an id of 5 bytes */
        t[0] = 5; t[1] = 1; t[3] = 1; t[5] = 2; t[7] = 24;
        CHECK (walpe_tga_head (t, 18, &tga) && tga.changed && tga.skip_off == 23 && tga.skip_len == 6);
        CHECK (tga.head[0] == 5 && tga.head[1] == 0 && tga.head[2] == 2 && !tga.head[3] && !tga.head[5] &&
               !tga.head[7] && tga.head[12] == 4 && tga.head[16] == 24);
        t[7] = 8;       /* entries of 8 bits: no colour map gdk-pixbuf reads */
        CHECK (!walpe_tga_head (t, 18, &tga));
        t[5] = 0;       /* ... but no entries of them */
        CHECK (walpe_tga_head (t, 18, &tga) && tga.skip_len == 0 && tga.changed);
        /* the map flag is a flag: 0xff with a colour-mapped picture is 1 for the decoder */
        guint8 m[18] = { 0, 0xff, 1, 0, 0, 2, 0, 24, 0, 0, 0, 0, 4, 0, 3, 0, 8, 0 };
        CHECK (walpe_tga_head (m, 18, &tga) && tga.changed && tga.head[1] == 1 && tga.head[5] == 2 && tga.skip_len == 0);
        m[1] = 0;       /* colour-mapped without a map */
        CHECK (!walpe_tga_head (m, 18, &tga));
        static const guint8 bad[][18] = {
            { 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 3, 0, 0, 0 },        /* no bits a pixel (a cursor's header) */
            { 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 3, 0, 24, 0 },       /* no picture type */
            { 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 24, 0 },       /* no width */
            { 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 3, 0, 24, 0x40 },    /* interleaved */
            { 0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 3, 0, 24, 0 },       /* grey of 24 bits */
        };
        for (guint i = 0; i < G_N_ELEMENTS (bad); i++)
            CHECK (!walpe_tga_head (bad[i], 18, NULL));
        /* true colour of 15 bits: a TGA header (the decoder behind glycin shows the
         * picture, and showed it as a layer before the header was looked at here;
         * gdk-pixbuf's own decoder refuses it, as it did) */
        static const guint8 t15[18 + 24] = { 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 3, 0, 15, 0x20 };
        CHECK (walpe_tga_head (t15, 18, &tga) && tga.w == 4 && tga.h == 3 && !tga.changed);
        mkfile ("pichead/fifteen.tga", t15, sizeof t15);
        char *f15 = P ("pichead/fifteen.tga");
        CHECK (!g_strcmp0 (walpe_pichead_file_kind (f15, NULL), "tga"));
        g_free (f15);
    }

    /* 6. a file's format: the bytes; the name only for which TGA header is one, and for a 64-bit TIFF */
    {
        static const guint8 tga[34] = { 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 2, 0, 32, 0x28 };
        static const guint8 tga_map[18 + 6 + 12] = { 0, 1, 2, 0, 0, 2, 0, 24, 0, 0, 0, 0, 2, 0, 2, 0, 24, 0x20 };
        static const guint8 tga_iso[34] = { 0, 0, 2, 0, 'f', 't', 'y', 'p', 'h', 'e', 'i', 'c', 2, 0, 2, 0, 32, 0x28 };
        static const guint8 big[24] = { 'I', 'I', 43, 0, 8, 0, 0, 0, 16 };
        static const char svg[] = "<?xml version=\"1.0\"?>\n<svg xmlns=\"http://www.w3.org/2000/svg\"/>";
        static const struct { const char *file; const void *d; gssize n; const char *kind, *other; } f[] = {
            { "pichead/a.png", tga, sizeof tga, "tga", NULL },
            { "pichead/map.tga", tga_map, sizeof tga_map, "tga", NULL },
            { "pichead/map.TGA", tga_map, sizeof tga_map, "tga", NULL },
            { "pichead/map.png", tga_map, sizeof tga_map, NULL, NULL },
            { "pichead/iso.tga", tga_iso, sizeof tga_iso, NULL, "heif" },
            { "pichead/big.tif", big, sizeof big, "tiff", NULL },
            { "pichead/big.tiff", big, sizeof big, "tiff", NULL },
            { "pichead/big.png", big, sizeof big, NULL, NULL },
            { "pichead/doc.png", svg, -1, NULL, "svg" },
            { "pichead/doc.jpg", svg, -1, NULL, "svg" },
            { "pichead/doc.tga", svg, -1, NULL, "svg" },
            { "pichead/p.tga", "P6\n4 4\n255\n000000000000000000000000000000000000000000000000", -1, NULL, "pnm" },
            { "pichead/q.bmp", "qoif\0\0\0\4\0\0\0\4\3\0", 14, NULL, "qoi" },
            { "pichead/i.png", "\0\0\1\0\1\0\20\20\0\0\1\0\40\0", 14, NULL, "ico" },
            { "pichead/x.gif", "/* XPM */\nstatic char *a[] = {", -1, NULL, "xpm" },
            { "pichead/empty.png", "", 0, NULL, NULL },
        };
        for (guint i = 0; i < G_N_ELEMENTS (f); i++) {
            WalpeTga t;
            mkfile (f[i].file, f[i].d, f[i].n);
            char *p = P (f[i].file);
            const char *kind = walpe_pichead_file_kind (p, &t);
            if (g_strcmp0 (kind, f[i].kind) || (!kind && g_strcmp0 (walpe_pichead_file_other (p), f[i].other)))
                g_printerr ("parsers-test: %s is %s / %s\n", f[i].file, kind, walpe_pichead_file_other (p));
            CHECK (!g_strcmp0 (kind, f[i].kind));
            CHECK (kind || !g_strcmp0 (walpe_pichead_file_other (p), f[i].other));
            g_free (p);
        }
        char *dir = P ("pichead");
        CHECK (walpe_pichead_file_kind (dir, NULL) == NULL && walpe_pichead_file_other (dir) == NULL);
        g_free (dir);
    }
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
    if (!only || !strcmp (only, "log"))
        test_log ();
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
    if (!only || !strcmp (only, "pichead"))
        test_pichead ();
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
