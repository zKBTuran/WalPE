/* formats_test.c - hostile wallpaper files (version 0.12 audit): what a crafted
 * workshop item must not be able to do through the file parsers.
 *   - presets that name each other (or themselves, from a second library)
 *     as "dependency": type detection and loading end, with an error
 *   - particle files that list themselves (or each other) as children do
 *     not multiply into tens of thousands of systems
 *   - scene.pkg: entries that share bytes cannot write more than a few
 *     times the archive's size; ".." / absolute entry names stay inside
 *   - .tex / pictures: a picture larger than the renderer accepts is not
 *     decoded; sprite tables with absurd floats; raw pixels that start like
 *     a JPEG are still pixels; a normal PNG and an embedded PNG still load
 *   - asset names: "..", absolute paths, directories, FIFOs and device files
 *     are not resolved; /dev/zero is not read
 *
 * Everything happens in a private temp dir that is also HOME and the XDG
 * dirs (set before GLib caches them).
 *
 * Run: meson test -C build formats -v   (formats-test preset|particles|pkg|tex|assets = one group) */
#include "engine/content.h"
#include "engine/pkg.h"
#include "engine/propnotes.h"
#include "engine/weparticles.h"
#include "common/tex.h"
#include "common/util.h"
#include "ui/store.h"
#include <glib/gstdio.h>
#include <lz4.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

static void put32 (GByteArray *a, guint32 v)
{
    guint8 b[4] = { (guint8) v, (guint8) (v >> 8), (guint8) (v >> 16), (guint8) (v >> 24) };
    g_byte_array_append (a, b, 4);
}

static guint64 tree_bytes (const char *dir)
{
    guint64 sum = 0;
    GDir *d = g_dir_open (dir, 0, NULL);
    const char *n;
    while (d && (n = g_dir_read_name (d))) {
        char *p = g_build_filename (dir, n, NULL);
        GStatBuf st;
        if (g_lstat (p, &st) == 0)
            sum += S_ISDIR (st.st_mode) ? tree_bytes (p) : (guint64) st.st_size;
        g_free (p);
    }
    if (d)
        g_dir_close (d);
    return sum;
}

/* ---------------------------------------------------------------- presets */

static void test_preset_cycle (void)
{
    /* two items naming each other */
    mkfile ("lib/111/project.json", "{\"dependency\":\"222\",\"title\":\"a\"}", -1);
    mkfile ("lib/222/project.json", "{\"dependency\":\"111\",\"title\":\"b\"}", -1);
    char *a = P ("lib/111");
    char *media = (char *) 1;
    CHECK (walpe_detect_type (a, &media) == WALPE_TYPE_NONE);
    CHECK (media == NULL);
    GError *err = NULL;
    WalpeContent *c = walpe_content_new (a, NULL, &err);
    CHECK (!c && err && err->message && *err->message);
    g_clear_error (&err);
    JsonNode *defs = walpe_content_property_defs (a, NULL);   /* the GUI's options panel */
    if (defs)
        json_node_unref (defs);
    g_free (a);

    /* one item naming itself, installed in two libraries (Steam's and
     * WalPE's default one): each copy is the other's "base" */
    const char *self = "{\"dependency\":\"333\",\"title\":\"self\"}";
    mkfile ("home/.local/share/walpe/workshop/333/project.json", self, -1);
    mkfile ("home/.local/share/Steam/steamapps/workshop/content/431960/333/project.json", self, -1);
    char *s = P ("home/.local/share/walpe/workshop/333");
    CHECK (walpe_detect_type (s, NULL) == WALPE_TYPE_NONE);
    c = walpe_content_new (s, NULL, &err);
    CHECK (!c && err);
    g_clear_error (&err);
    g_free (s);

    /* a real preset still resolves to its base */
    mkfile ("lib/444/project.json", "{\"dependency\":\"555\",\"preset\":{\"x\":1}}", -1);
    mkfile ("lib/555/project.json", "{\"type\":\"video\",\"file\":\"v.mp4\"}", -1);
    char *p = P ("lib/444"), *want = P ("lib/555/v.mp4");
    media = NULL;
    CHECK (walpe_detect_type (p, &media) == WALPE_TYPE_VIDEO);
    CHECK (!g_strcmp0 (media, want));
    g_free (media);
    g_free (p);
    g_free (want);
}

/* -------------------------------------------------------------- particles */

/* a particle file with `n` children, all named `child` */
static void write_particle (const char *rel, const char *child, int n)
{
    GString *g = g_string_new ("{\"emitter\":[{\"name\":\"boxrandom\",\"rate\":1}],"
                               "\"maxcount\":2,\"children\":[");
    for (int i = 0; i < n; i++)
        g_string_append_printf (g, "%s{\"name\":\"%s\",\"type\":\"static\"}", i ? "," : "", child);
    g_string_append (g, "]}");
    mkfile (rel, g->str, (gssize) g->len);
    g_string_free (g, TRUE);
}

static int particle_systems (const char *rel)
{
    char *file = P (rel), *dir = P ("part");
    JsonNode *root = wpj_parse_file (file);
    WalpeWeParticles *p = root ? walpe_weparticles_new (json_node_get_object (root), dir, NULL, NULL)
                               : NULL;
    int n = walpe_weparticles_systems (p);
    walpe_weparticles_free (p);
    if (root)
        json_node_unref (root);
    g_free (file);
    g_free (dir);
    return n;
}

static void test_particles (void)
{
    /* an ordinary tree: root -> two children -> one child each */
    write_particle ("part/particles/leaf.json", "", 0);
    write_particle ("part/particles/mid.json", "particles/leaf.json", 1);
    write_particle ("part/particles/root.json", "particles/mid.json", 2);
    CHECK (particle_systems ("part/particles/root.json") == 5);

    /* a file that lists itself 40 times: 65641 systems without a check.
     * The root and its 40 copies exist; none of the copies has children. */
    write_particle ("part/particles/self.json", "particles/self.json", 40);
    int n = particle_systems ("part/particles/self.json");
    CHECK (n >= 1 && n <= 41);

    /* two files listing each other 40 times: stopped by the total */
    write_particle ("part/particles/a.json", "particles/b.json", 40);
    write_particle ("part/particles/b.json", "particles/a.json", 40);
    n = particle_systems ("part/particles/a.json");
    CHECK (n >= 1 && n <= 128);

    /* 40 different children with 40 children each: the total again */
    write_particle ("part/particles/wide.json", "particles/mid40.json", 40);
    write_particle ("part/particles/mid40.json", "particles/leaf.json", 40);
    n = particle_systems ("part/particles/wide.json");
    CHECK (n >= 41 && n <= 128);
}

/* -------------------------------------------------------------------- pkg */

typedef struct { const char *name; guint32 off, size; } Ent;

static void write_pkg (const char *rel, const Ent *e, guint n, guint repeat, gsize blob)
{
    GByteArray *a = g_byte_array_new ();
    put32 (a, 8);
    g_byte_array_append (a, (const guint8 *) "PKGV0016", 8);
    put32 (a, n * repeat);
    for (guint r = 0; r < repeat; r++)
        for (guint i = 0; i < n; i++) {
            char *name = repeat > 1 ? g_strdup_printf ("%u/%s", r, e[i].name) : g_strdup (e[i].name);
            put32 (a, (guint32) strlen (name));
            g_byte_array_append (a, (const guint8 *) name, (guint) strlen (name));
            put32 (a, e[i].off);
            put32 (a, e[i].size);
            g_free (name);
        }
    guint8 *data = g_malloc (blob);
    for (gsize i = 0; i < blob; i++)
        data[i] = (guint8) ('a' + i % 26);
    g_byte_array_append (a, data, (guint) blob);
    g_free (data);
    mkfile (rel, a->data, a->len);
    g_byte_array_unref (a);
}

static void test_pkg (void)
{
    /* a normal archive: every entry lands, names that would leave the
     * target dir are skipped */
    const Ent good[] = {
        { "scene.json", 0, 10 }, { "materials/a.json", 10, 20 }, { "empty", 30, 0 },
        { "../escape.txt", 0, 5 }, { "/abs/escape.txt", 0, 5 }, { "a/../../escape2", 0, 5 },
    };
    write_pkg ("pkg/good.pkg", good, G_N_ELEMENTS (good), 1, 64);
    char *pkg = P ("pkg/good.pkg"), *out = P ("pkg/out");
    GError *err = NULL;
    CHECK (walpe_pkg_extract (pkg, out, &err) && !err);
    g_clear_error (&err);
    char *f = g_build_filename (out, "scene.json", NULL);
    char *txt = NULL;
    gsize len = 0;
    CHECK (g_file_get_contents (f, &txt, &len, NULL) && len == 10 && !memcmp (txt, "abcdefghij", 10));
    g_free (txt);
    g_free (f);
    f = g_build_filename (out, "materials", "a.json", NULL);
    CHECK (g_file_test (f, G_FILE_TEST_IS_REGULAR));
    g_free (f);
    f = g_build_filename (out, "empty", NULL);
    CHECK (g_file_test (f, G_FILE_TEST_IS_REGULAR));
    g_free (f);
    char *e1 = P ("pkg/escape.txt"), *e2 = P ("escape2"), *e3 = P ("pkg/escape2");
    CHECK (!g_file_test (e1, G_FILE_TEST_EXISTS) && !g_file_test (e2, G_FILE_TEST_EXISTS) &&
           !g_file_test (e3, G_FILE_TEST_EXISTS) && !g_file_test ("/abs/escape.txt", G_FILE_TEST_EXISTS));
    g_free (e1);
    g_free (e2);
    g_free (e3);
    g_free (pkg);
    g_free (out);

    /* 20000 entries that all are the same 256 KiB: 4.9 GiB if written */
    const Ent same[] = { { "blob.bin", 0, 256 * 1024 } };
    write_pkg ("pkg/bomb.pkg", same, 1, 20000, 256 * 1024);
    pkg = P ("pkg/bomb.pkg");
    out = P ("pkg/bomb-out");
    GStatBuf st;
    CHECK (g_stat (pkg, &st) == 0 && st.st_size < 1024 * 1024);
    CHECK (!walpe_pkg_extract (pkg, out, &err) && err);
    g_clear_error (&err);
    CHECK (tree_bytes (out) <= (guint64) st.st_size * 4 + (16u << 20));
    g_free (pkg);
    g_free (out);

    /* the options panel reads the .json entries of a package when a
     * wallpaper is selected (GUI thread): 8000 entries that all name one
     * 100 KB file (800 MB of JSON) are not all parsed */
    {
        GString *js = g_string_new ("{\"objects\":[");
        for (int i = 0; i < 1200; i++)
            g_string_append_printf (js, "%s{\"id\":%d,\"name\":\"o%d\",\"origin\":\"1 2 3\","
                                    "\"visible\":{\"user\":\"opt\",\"value\":true}}",
                                    i ? "," : "", i, i);
        g_string_append (js, "]}");
        GByteArray *a = g_byte_array_new ();
        put32 (a, 8);
        g_byte_array_append (a, (const guint8 *) "PKGV0016", 8);
        put32 (a, 8001);
        for (int i = 0; i <= 8000; i++) {
            char *name = i ? g_strdup_printf ("m/f%05d.json", i) : g_strdup ("scene.json");
            put32 (a, (guint32) strlen (name));
            g_byte_array_append (a, (const guint8 *) name, (guint) strlen (name));
            put32 (a, 0);
            put32 (a, (guint32) js->len);
            g_free (name);
        }
        g_byte_array_append (a, (const guint8 *) js->str, (guint) js->len);
        g_string_free (js, TRUE);
        mkfile ("notes/wp/scene.pkg", a->data, a->len);
        g_byte_array_unref (a);
        mkfile ("notes/wp/project.json",
                "{\"file\":\"scene.json\",\"type\":\"scene\",\"general\":{\"properties\":"
                "{\"opt\":{\"type\":\"bool\",\"value\":true,\"text\":\"x\"},"
                "\"idle\":{\"type\":\"bool\",\"value\":true,\"text\":\"y\"}}}}", -1);
        char *wp = P ("notes/wp");
        clock_t c0 = clock ();
        JsonObject *notes = walpe_prop_notes (wp);
        double cpu = (double) (clock () - c0) / CLOCKS_PER_SEC;
        /* "opt" is bound in scene.json (no note), "idle" is not (a note) */
        CHECK (notes && !json_object_has_member (notes, "opt") &&
               json_object_has_member (notes, "idle"));
        if (notes)
            json_object_unref (notes);
        if (cpu > 60.0) {
            g_printerr ("FAIL option notes of an aliased package: %.1f s of CPU\n", cpu);
            failures++;
        }
        g_free (wp);
    }
}

/* -------------------------------------------------------------------- tex */

static void save_png (const char *rel, int w, int h)
{
    GdkPixbuf *pb = gdk_pixbuf_new (GDK_COLORSPACE_RGB, TRUE, 8, w, h);
    gdk_pixbuf_fill (pb, 0x102030ff);   /* R=0x10 G=0x20 B=0x30 A=0xff */
    char *buf = NULL;
    gsize len = 0;
    if (!gdk_pixbuf_save_to_buffer (pb, &buf, &len, "png", NULL, NULL))
        g_error ("cannot encode a PNG");
    mkfile (rel, buf, (gssize) len);
    g_free (buf);
    g_object_unref (pb);
}

/* a TEXB0003 container with one uncompressed mip of raw pixels: an image
 * of iw x ih in the top-left corner of a w x h texture */
static void write_raw_tex_img (const char *rel, guint32 type, guint32 w, guint32 h,
                               guint32 iw, guint32 ih, const guint8 *px, guint32 len);

static void write_raw_tex (const char *rel, guint32 type, guint32 w, guint32 h,
                           const guint8 *px, guint32 len)
{
    write_raw_tex_img (rel, type, w, h, w, h, px, len);
}

static void write_raw_tex_img (const char *rel, guint32 type, guint32 w, guint32 h,
                               guint32 iw, guint32 ih, const guint8 *px, guint32 len)
{
    GByteArray *a = g_byte_array_new ();
    g_byte_array_append (a, (const guint8 *) "TEXV0005\0TEXI0001\0", 18);
    put32 (a, type);
    put32 (a, 0);            /* flags */
    put32 (a, w);
    put32 (a, h);            /* texture size */
    put32 (a, iw);
    put32 (a, ih);           /* image size */
    put32 (a, 0);
    g_byte_array_append (a, (const guint8 *) "TEXB0003\0", 9);
    put32 (a, 1);            /* images */
    put32 (a, 0xffffffffu);  /* freeimage format: none (raw pixels) */
    put32 (a, 1);            /* mips */
    put32 (a, w);
    put32 (a, h);
    put32 (a, 0);            /* not LZ4 */
    put32 (a, len);
    put32 (a, len);
    g_byte_array_append (a, px, len);
    mkfile (rel, a->data, a->len);
    g_byte_array_unref (a);
}

static void test_tex (void)
{
    /* raw pixels that start like a JPEG (FF D8 FF): an RGBA texture whose
     * first pixel is #FFD8FF, an R8 mask - still textures */
    guint8 px[64];
    for (guint i = 0; i < sizeof px; i++)
        px[i] = (guint8) (i * 3);
    px[0] = 0xff; px[1] = 0xd8; px[2] = 0xff; px[3] = 0x80;
    write_raw_tex ("tex/rgba-ffd8.tex", 0, 4, 4, px, 64);
    char *rp = P ("tex/rgba-ffd8.tex");
    WalpeTex *rt = walpe_tex_load (rp);
    CHECK (rt && rt->width == 4 && rt->height == 4 && rt->len == 64);
    /* RGBA in the file, BGRA in memory */
    CHECK (rt && rt->data[0] == 0xff && rt->data[1] == 0xd8 && rt->data[2] == 0xff &&
           rt->data[3] == 0x80 && rt->data[4] == px[6] && rt->data[6] == px[4]);
    walpe_tex_free (rt);
    g_free (rp);
    write_raw_tex ("tex/r8-ffd8.tex", 9, 8, 8, px, 64);
    rp = P ("tex/r8-ffd8.tex");
    rt = walpe_tex_load_ex (rp, WALPE_TEX_LOAD_NATIVE_RG);
    CHECK (rt && rt->width == 8 && rt->height == 8 && rt->channels == 1 && rt->len == 64 &&
           !memcmp (rt->data, px, 64));
    walpe_tex_free (rt);
    g_free (rp);

    /* an image smaller than its (padded) texture is cropped to the image:
     * row y of the result = the first width pixels of row y of the texture */
    {
        enum { TW = 16, TH = 8, IW = 11, IH = 5 };
        guint8 big[TW * TH * 4];
        for (guint i = 0; i < sizeof big; i++)
            big[i] = (guint8) (i * 7 + i / 64);
        write_raw_tex_img ("tex/padded-rgba.tex", 0, TW, TH, IW, IH, big, sizeof big);
        char *pp = P ("tex/padded-rgba.tex");
        WalpeTex *pt = walpe_tex_load (pp);
        CHECK (pt && pt->width == IW && pt->height == IH && pt->len == IW * IH * 4 &&
               pt->full_width == IW && pt->full_height == IH);
        gboolean same = pt != NULL;
        for (int y = 0; pt && y < IH; y++)
            for (int x = 0; x < IW; x++) {
                const guint8 *src = big + (y * TW + x) * 4, *dst = pt->data + (y * IW + x) * 4;
                same &= dst[0] == src[2] && dst[1] == src[1] && dst[2] == src[0] && dst[3] == src[3];
            }
        CHECK (same);
        walpe_tex_free (pt);
        g_free (pp);
        /* one byte per pixel, kept native */
        write_raw_tex_img ("tex/padded-r8.tex", 9, TW, TH, IW, IH, big, TW * TH);
        pp = P ("tex/padded-r8.tex");
        pt = walpe_tex_load_ex (pp, WALPE_TEX_LOAD_NATIVE_RG);
        CHECK (pt && pt->width == IW && pt->height == IH && pt->channels == 1 && pt->len == IW * IH);
        same = pt != NULL;
        for (int y = 0; pt && y < IH; y++)
            same &= !memcmp (pt->data + y * IW, big + y * TW, IW);
        CHECK (same);
        walpe_tex_free (pt);
        g_free (pp);
    }

    /* a plain picture: loads, BGRA byte order */
    save_png ("tex/ok.png", 64, 32);
    char *p = P ("tex/ok.png");
    WalpeTex *t = walpe_tex_load (p);
    CHECK (t && t->width == 64 && t->height == 32 && t->len == 64 * 32 * 4);
    CHECK (t && t->data[0] == 0x30 && t->data[1] == 0x20 && t->data[2] == 0x10 && t->data[3] == 0xff);
    walpe_tex_free (t);
    g_free (p);

    /* wider than any texture: refused (before its pixels are decoded) */
    save_png ("tex/wide.png", 20000, 4);
    p = P ("tex/wide.png");
    CHECK (walpe_tex_load (p) == NULL);
    g_free (p);

    /* more pixels than one texture gets (8192 x 8192 in all): refused from
     * the header, before anything is decompressed; exactly at the limit it
     * still loads. One byte per pixel, all zero, as one LZ4 stream. */
    {
        static const struct { guint32 w, h; gboolean ok; } big[] = {
            { 8192, 8192, TRUE }, { 8200, 8200, FALSE }, { 16384, 5000, FALSE } };
        for (guint i = 0; i < G_N_ELEMENTS (big); i++) {
            gsize n = (gsize) big[i].w * big[i].h;
            char *zero = g_malloc0 (n);
            int cap = LZ4_compressBound ((int) n);
            char *lz = g_malloc ((gsize) cap);
            int cl = LZ4_compress_default (zero, lz, (int) n, cap);
            g_free (zero);
            GByteArray *a = g_byte_array_new ();
            g_byte_array_append (a, (const guint8 *) "TEXV0005\0TEXI0001\0", 18);
            put32 (a, 9);            /* R8 */
            put32 (a, 0);
            put32 (a, big[i].w);
            put32 (a, big[i].h);
            put32 (a, big[i].w);
            put32 (a, big[i].h);
            put32 (a, 0);
            g_byte_array_append (a, (const guint8 *) "TEXB0003\0", 9);
            put32 (a, 1);
            put32 (a, 0xffffffffu);
            put32 (a, 1);            /* mips */
            put32 (a, big[i].w);
            put32 (a, big[i].h);
            put32 (a, 1);            /* LZ4 */
            put32 (a, (guint32) n);
            put32 (a, (guint32) cl);
            g_byte_array_append (a, (const guint8 *) lz, (guint) cl);
            g_free (lz);
            mkfile ("tex/big.tex", a->data, a->len);
            g_byte_array_unref (a);
            p = P ("tex/big.tex");
            t = walpe_tex_load_ex (p, WALPE_TEX_LOAD_NATIVE_RG);
            if ((t != NULL) != big[i].ok) {
                g_printerr ("FAIL %u x %u texture: %s\n", big[i].w, big[i].h,
                            t ? "loaded" : "refused");
                failures++;
            }
            CHECK (!t || ((guint32) t->width == big[i].w && t->len == n));
            walpe_tex_free (t);
            g_free (p);
        }
    }

    /* a .tex container around the same PNG, with a sprite table whose
     * frame size is a float no int can hold */
    save_png ("tex/inner.png", 16, 16);
    p = P ("tex/inner.png");
    char *png = NULL;
    gsize plen = 0;
    if (!g_file_get_contents (p, &png, &plen, NULL))
        g_error ("no inner png");
    g_free (p);
    GByteArray *a = g_byte_array_new ();
    g_byte_array_append (a, (const guint8 *) "TEXV0005\0TEXI0001\0", 18);
    put32 (a, 0);            /* type RGBA8888 */
    put32 (a, 4);            /* flags: sprite sheet */
    put32 (a, 16);
    put32 (a, 16);           /* texture size */
    put32 (a, 16);
    put32 (a, 16);           /* image size */
    put32 (a, 0);
    g_byte_array_append (a, (const guint8 *) "TEXB0003\0", 9);
    put32 (a, 1);            /* images */
    put32 (a, 13);           /* freeimage format */
    put32 (a, 1);            /* mips */
    put32 (a, 16);
    put32 (a, 16);
    put32 (a, 0);            /* not LZ4 */
    put32 (a, 0);
    put32 (a, (guint32) plen);
    g_byte_array_append (a, (const guint8 *) png, (guint) plen);
    g_byte_array_append (a, (const guint8 *) "TEXS0002\0", 9);
    put32 (a, 1);            /* frames */
    put32 (a, 0);            /* image */
    const float fr[7] = { 0.1f, 0, 0, 1e30f, 0, 0, -1e30f };   /* time x y w1 w2 h2 h1 */
    g_byte_array_append (a, (const guint8 *) fr, sizeof fr);
    mkfile ("tex/sprite.tex", a->data, a->len);
    g_byte_array_unref (a);
    g_free (png);
    p = P ("tex/sprite.tex");
    t = walpe_tex_load (p);
    CHECK (t && t->width == 16 && t->height == 16 && t->nframes == 1);
    CHECK (t && t->frame_w > 0 && t->frame_w <= 16384 && t->frame_h > 0 && t->frame_h <= 16384);
    walpe_tex_free (t);
    g_free (p);
}

/* ---------------------------------------------------------- asset names */

/* names in a wallpaper's json resolve below the wallpaper (or the install's
 * assets) only: no "..", no absolute path, nothing that is not a regular
 * file - and ordinary names resolve to the same path as before */
static void test_assets (void)
{
    mkfile ("as/wp/materials/a.tex", "x", 1);
    mkfile ("as/wp/scene.json", "{}", 2);
    mkfile ("as/outside/secret.png", "x", 1);
    char *wp = P ("as/wp"), *outside = P ("as/outside/secret.png");

    char *got = walpe_asset_path (wp, "materials/a.tex");
    char *want = P ("as/wp/materials/a.tex");
    CHECK (!g_strcmp0 (got, want));
    g_free (got);
    g_free (want);
    got = walpe_asset_path (wp, "materials/../scene.json");   /* climbs, stays inside */
    CHECK (got && g_file_test (got, G_FILE_TEST_IS_REGULAR));
    g_free (got);
    CHECK (walpe_asset_path (wp, "../outside/secret.png") == NULL);
    CHECK (walpe_asset_path (wp, "materials/../../outside/secret.png") == NULL);
    CHECK (walpe_asset_path (wp, outside) == NULL);            /* absolute */
    CHECK (walpe_asset_path (wp, "/dev/zero") == NULL);
    CHECK (walpe_asset_path (wp, "materials") == NULL);        /* a directory */
    CHECK (walpe_asset_path (wp, "missing.tex") == NULL);
    CHECK (walpe_asset_path (wp, "..") == NULL);
    CHECK (walpe_asset_path (wp, "") == NULL && walpe_asset_path (NULL, "x") == NULL);
    char *fifo = P ("as/wp/materials/pipe.tex");
    if (mkfifo (fifo, 0600) == 0)
        CHECK (walpe_asset_path (wp, "materials/pipe.tex") == NULL);
    g_free (fifo);

    CHECK (walpe_path_is_inside (outside, wp) == FALSE);
    char *in = P ("as/wp/materials/a.tex"), *sib = P ("as/wp-other/x"), *wps = P ("as/wp/");
    CHECK (walpe_path_is_inside (in, wp));
    CHECK (walpe_path_is_inside (in, wps));                    /* root with a trailing '/' */
    CHECK (!walpe_path_is_inside (sib, wp));                   /* "wp-other" is not below "wp" */
    CHECK (!walpe_path_is_inside (wp, wp));                    /* the root itself */
    g_free (in);
    g_free (sib);
    g_free (wps);

    /* device files are not read: these returned after eating all memory */
    gsize len = 0;
    CHECK (walpe_read_file ("/dev/zero", &len) == NULL);
    CHECK (walpe_tex_load ("/dev/zero") == NULL);

    /* project.json "file" cannot name a file outside the wallpaper */
    mkfile ("as/vid/project.json", "{\"type\":\"video\",\"file\":\"../outside/secret.png\"}", -1);
    char *vid = P ("as/vid"), *media = (char *) 1;
    /* such an item is refused outright (type unsupported, warning): the
     * file is not used */
    CHECK (walpe_detect_type (vid, &media) == WALPE_TYPE_UNSUPPORTED);
    CHECK (media == NULL);
    mkfile ("as/vid/project.json", "{\"type\":\"video\",\"file\":\"clip.mp4\"}", -1);
    want = P ("as/vid/clip.mp4");
    CHECK (walpe_detect_type (vid, &media) == WALPE_TYPE_VIDEO);
    CHECK (!g_strcmp0 (media, want));
    g_free (media);
    g_free (want);
    g_free (vid);
    g_free (wp);
    g_free (outside);
}

/* ------------------------------------------------------- wallpaper types */

static const char *scan_type (GList *l, const char *id)
{
    for (; l; l = l->next)
        if (!g_strcmp0 (((WalpeEntry *) l->data)->id, id))
            return ((WalpeEntry *) l->data)->type;
    return NULL;
}

/* project.json "type" in any letter case (Workshop items carry "Scene",
 * "Video", "Web" as well as the lower-case words), no type at all, and
 * presets: the GUI's library scan and the engine name the same type */
static void test_types (void)
{
    static const struct { const char *id, *json, *want; WalpeType t; } it[] = {
        { "1001", "{\"type\":\"Scene\",\"file\":\"scene.json\"}", "scene", WALPE_TYPE_SCENE },
        { "1002", "{\"type\":\"scene\"}", "scene", WALPE_TYPE_SCENE },
        { "1003", "{\"type\":\"Video\",\"file\":\"a.mp4\"}", "video", WALPE_TYPE_VIDEO },
        { "1004", "{\"type\":\"VIDEO\",\"file\":\"a.mp4\"}", "video", WALPE_TYPE_VIDEO },
        { "1005", "{\"type\":\"Web\",\"file\":\"index.html\"}", "web", WALPE_TYPE_WEB },
        { "1006", "{\"title\":\"no type\"}", "scene", WALPE_TYPE_SCENE },
        { "1007", "{\"type\":\"Application\"}", "application", WALPE_TYPE_UNSUPPORTED },
        /* presets: the type of the base item */
        { "1008", "{\"dependency\":\"1005\",\"preset\":{\"x\":1}}", "web", WALPE_TYPE_WEB },
        { "1009", "{\"dependency\":\"1003\",\"preset\":{}}", "video", WALPE_TYPE_VIDEO },
        { "1010", "{\"dependency\":\"1001\",\"preset\":{}}", "scene", WALPE_TYPE_SCENE },
        /* base not installed: listed as a scene, as before (loading it
         * reports the missing item) */
        { "1011", "{\"dependency\":\"999999\",\"preset\":{}}", "scene", WALPE_TYPE_SCENE },
    };
    for (guint i = 0; i < G_N_ELEMENTS (it); i++) {
        char *rel = g_strdup_printf ("types/%s/project.json", it[i].id);
        mkfile (rel, it[i].json, -1);
        g_free (rel);
    }
    char *lib = P ("types");
    GList *l = walpe_store_scan (lib);
    CHECK (g_list_length (l) == G_N_ELEMENTS (it));
    for (guint i = 0; i < G_N_ELEMENTS (it); i++) {
        const char *got = scan_type (l, it[i].id);
        if (g_strcmp0 (got, it[i].want)) {
            g_printerr ("FAIL library scan: %s is listed as %s, expected %s\n", it[i].id,
                        got ? got : "(not listed)", it[i].want);
            failures++;
        }
        char *dir = g_build_filename (lib, it[i].id, NULL);
        WalpeType t = walpe_detect_type (dir, NULL);
        if (t != it[i].t) {
            g_printerr ("FAIL type detection: %s is %s\n", it[i].id, walpe_type_name (t));
            failures++;
        }
        g_free (dir);
    }
    g_list_free_full (l, g_object_unref);
    g_free (lib);

    /* a scene opened by its project.json or scene.json loads like its
     * folder (it was accepted and then failed: "scene.json not found in
     * .../project.json"); a scene.json without a project.json loads too */
    const char *scene = "{\"camera\":{\"center\":\"0 0 -1\",\"eye\":\"0 0 0\",\"up\":\"0 1 0\"},"
                        "\"general\":{\"orthogonalprojection\":{\"width\":64,\"height\":36}},"
                        "\"objects\":[]}";
    mkfile ("open/wp/project.json", "{\"type\":\"scene\",\"file\":\"scene.json\"}", -1);
    mkfile ("open/wp/scene.json", scene, -1);
    mkfile ("open/bare/scene.json", scene, -1);
    static const char *const how[] = { "open/wp", "open/wp/project.json", "open/wp/scene.json",
                                       "open/bare/scene.json" };
    for (guint i = 0; i < G_N_ELEMENTS (how); i++) {
        char *p = P (how[i]);
        GError *err = NULL;
        WalpeContent *c = walpe_content_new (p, NULL, &err);
        if (!c || c->type != WALPE_TYPE_SCENE) {
            g_printerr ("FAIL open %s: %s\n", how[i], err ? err->message : "no content");
            failures++;
        }
        g_clear_error (&err);
        if (c)
            c->free (c);
        g_free (p);
    }

    /* Import: a different file with the same name does not replace the one
     * already imported; the same file again does not multiply. Add folder:
     * the same folder twice is fine, another folder of that name is an
     * error (it was reported as added while the library kept the old one). */
    mkfile ("imp/a/video.mp4", "HOLIDAY", -1);
    mkfile ("imp/b/video.mp4", "WORK-other-content", -1);
    mkfile ("imp/setA/wallpaper/project.json", "{\"title\":\"A\"}", -1);
    mkfile ("imp/setB/wallpaper/project.json", "{\"title\":\"B\"}", -1);
    char *ilib = P ("imp/lib"), *fa = P ("imp/a/video.mp4"), *fb = P ("imp/b/video.mp4");
    char *da = P ("imp/setA/wallpaper"), *db = P ("imp/setB/wallpaper");
    g_mkdir_with_parents (ilib, 0755);
    GError *ierr = NULL;
    char *d1 = walpe_store_import_file (fa, ilib, &ierr);
    char *d2 = walpe_store_import_file (fb, ilib, &ierr);
    char *d3 = walpe_store_import_file (fa, ilib, &ierr);
    CHECK (d1 && d2 && d3 && !ierr);
    CHECK (d1 && d2 && strcmp (d1, d2) != 0);
    CHECK (!g_strcmp0 (d1, d3));
    char *c1 = NULL, *c2 = NULL;
    CHECK (d1 && g_file_get_contents (d1, &c1, NULL, NULL) && !g_strcmp0 (c1, "HOLIDAY"));
    CHECK (d2 && g_file_get_contents (d2, &c2, NULL, NULL) && !g_strcmp0 (c2, "WORK-other-content"));
    g_clear_error (&ierr);
    char *l1 = walpe_store_import_dir (da, ilib, &ierr);
    char *l2 = walpe_store_import_dir (da, ilib, &ierr);
    CHECK (l1 && l2 && !ierr);
    char *l3 = walpe_store_import_dir (db, ilib, &ierr);
    CHECK (!l3 && ierr);
    g_clear_error (&ierr);
    g_free (c1); g_free (c2);
    g_free (d1); g_free (d2); g_free (d3);
    g_free (l1); g_free (l2); g_free (l3);
    g_free (ilib); g_free (fa); g_free (fb); g_free (da); g_free (db);
}

int main (int argc, char **argv)
{
    char *tmp = g_dir_make_tmp ("walpe-formats-test-XXXXXX", NULL);
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
    g_setenv ("WALPE_NO_AUDIO", "1", TRUE);   /* scenes are created, never drawn */
    g_setenv ("WALPE_NO_MEDIA", "1", TRUE);
    g_mkdir_with_parents (home, 0755);

    /* formats-test [preset|particles|pkg|tex|assets]: one group only */
    const char *only = argc > 1 ? argv[1] : NULL;
    if (!only || !strcmp (only, "assets"))
        test_assets ();
    if (!only || !strcmp (only, "types"))
        test_types ();
    if (!only || !strcmp (only, "preset"))
        test_preset_cycle ();
    if (!only || !strcmp (only, "particles"))
        test_particles ();
    if (!only || !strcmp (only, "pkg"))
        test_pkg ();
    if (!only || !strcmp (only, "tex"))
        test_tex ();

    g_printerr ("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures,
                failures == 1 ? "" : "s");
    char *rm[] = { "rm", "-rf", T, NULL };
    g_spawn_sync (NULL, rm, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
    g_free (home);
    g_free (cfg);
    g_free (cache);
    g_free (data);
    free (T);
    return failures ? 1 : 0;
}
