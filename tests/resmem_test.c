/* resmem_test.c - the memory / disk reductions of the loader and the
 * package cache keep their results:
 *   levels    .tex: an LZ4 level decodes to the same pixels as the raw one
 *             although only the image's rows are decompressed; a stored mip
 *             level loaded by walpe_tex_load_level is exactly what
 *             walpe_tex_load_ex gives for a file holding that level alone,
 *             for every pixel format and flag set; the level is refused
 *             where the image does not halve exactly, for sprite sheets,
 *             for kept-compressed DXT and beyond the stored chain;
 *             walpe_tex_probe reports the loadable levels
 *   pkgcache  the extracted-package cache: least recently used copies go
 *             when the cache passes its limit, a copy in use never does, a
 *             copy whose package is gone or that has no stamp is removed
 *
 * Everything happens in a private temp dir that is also HOME and the XDG
 * dirs (set before GLib caches them).
 * Run: meson test -C build resmem -v   (resmem-test levels|pkgcache = one group) */
#include "engine/pkg.h"
#include "common/tex.h"
#include <glib/gstdio.h>
#include <lz4.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

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

/* ----------------------------------------------------------------- levels */

static gsize level_bytes (guint32 type, guint32 w, guint32 h)
{
    if (type == 0)
        return (gsize) w * h * 4;
    if (type == 8)
        return (gsize) w * h * 2;
    if (type == 9)
        return (gsize) w * h;
    gsize bw = (w + 3) / 4, bh = (h + 3) / 4;
    return bw * bh * (type == 7 ? 8 : 16);
}

/* pixel bytes of level k: the same for every file that holds that level */
static guint8 *level_data (guint32 type, guint32 w, guint32 h, guint k, gsize *len)
{
    *len = level_bytes (type, w, h);
    guint8 *d = g_malloc (*len);
    guint32 x = 0x9e3779b9u * (k + 1) + type;
    for (gsize i = 0; i < *len; i++) {
        /* runs and noise: LZ4 emits both literals and matches */
        if ((i & 63) < 40) {
            x = x * 1664525u + 1013904223u;
            d[i] = (guint8) (x >> 24);
        } else {
            d[i] = (guint8) (k * 17 + (i >> 6));
        }
    }
    return d;
}

/* a TEXB0003 container: the image iw x ih in the top-left corner of a
 * tw x th texture; `mips` levels starting at level `first` of that texture
 * (first > 0: a file that holds that level alone as its level 0) */
static void write_tex (const char *rel, guint32 type, guint32 flags, guint32 tw, guint32 th,
                       guint32 iw, guint32 ih, guint first, guint mips, gboolean lz4)
{
    GByteArray *a = g_byte_array_new ();
    g_byte_array_append (a, (const guint8 *) "TEXV0005\0TEXI0001\0", 18);
    put32 (a, type);
    put32 (a, flags);
    put32 (a, MAX (1u, tw >> first));
    put32 (a, MAX (1u, th >> first));    /* texture size */
    put32 (a, iw >> first);
    put32 (a, ih >> first);              /* image size */
    put32 (a, 0);
    g_byte_array_append (a, (const guint8 *) "TEXB0003\0", 9);
    put32 (a, 1);            /* images */
    put32 (a, 0);            /* freeimage format */
    put32 (a, mips);
    for (guint k = first; k < first + mips; k++) {
        guint32 w = MAX (1u, tw >> k), h = MAX (1u, th >> k);
        gsize len = 0;
        guint8 *d = level_data (type, w, h, k, &len);
        put32 (a, w);
        put32 (a, h);
        if (lz4) {
            int cap = LZ4_compressBound ((int) len);
            char *c = g_malloc ((gsize) cap);
            int n = LZ4_compress_default ((const char *) d, c, (int) len, cap);
            put32 (a, 1);
            put32 (a, (guint32) len);
            put32 (a, (guint32) n);
            g_byte_array_append (a, (const guint8 *) c, (guint) n);
            g_free (c);
        } else {
            put32 (a, 0);
            put32 (a, 0);
            put32 (a, (guint32) len);
            g_byte_array_append (a, d, (guint) len);
        }
        g_free (d);
    }
    mkfile (rel, a->data, a->len);
    g_byte_array_unref (a);
}

static gboolean same_tex (const WalpeTex *a, const WalpeTex *b)
{
    return a && b && a->format == b->format && a->width == b->width && a->height == b->height &&
           a->full_width == b->full_width && a->full_height == b->full_height &&
           a->channels == b->channels && a->we_format == b->we_format && a->len == b->len &&
           !memcmp (a->data, b->data, a->len);
}

static void test_levels (void)
{
    /* texture 64x32, image 48x24: halves exactly three times (6x3 at
     * level 3), not a fourth; five levels stored */
    enum { TW = 64, TH = 32, IW = 48, IH = 24, MIPS = 5 };
    static const guint32 types[] = { 0, 4, 6, 7, 8, 9 };
    static const guint flagsets[] = { 0, WALPE_TEX_LOAD_NATIVE_RG, WALPE_TEX_LOAD_WE_COLOR };
    for (guint ti = 0; ti < G_N_ELEMENTS (types); ti++) {
        guint32 type = types[ti];
        write_tex ("lv/lz4.tex", type, 0, TW, TH, IW, IH, 0, MIPS, TRUE);
        write_tex ("lv/raw.tex", type, 0, TW, TH, IW, IH, 0, MIPS, FALSE);
        char *lz = P ("lv/lz4.tex"), *raw = P ("lv/raw.tex");
        for (guint fi = 0; fi < G_N_ELEMENTS (flagsets); fi++) {
            guint fl = flagsets[fi];
            /* level 0: only the image's rows of the LZ4 level are
             * decompressed; the result is that of the raw payload */
            WalpeTex *a = walpe_tex_load_ex (lz, fl), *b = walpe_tex_load_ex (raw, fl);
            CHECK (same_tex (a, b));
            CHECK (a && a->width == IW && a->height == IH && a->mip_level == 0 &&
                   a->base_width == IW && a->base_height == IH && a->mips == MIPS);
            walpe_tex_free (a);
            walpe_tex_free (b);
            for (int k = 1; k < MIPS; k++) {
                WalpeTex *l = walpe_tex_load_level (lz, fl, k);
                WalpeTex *r = walpe_tex_load_level (raw, fl, k);
                if (k > 3) {
                    /* 48x24 does not halve a fourth time */
                    CHECK (!l && !r);
                    continue;
                }
                /* the same level as the only one of its own file */
                write_tex ("lv/one.tex", type, 0, TW, TH, IW, IH, (guint) k, 1, k & 1);
                char *one = P ("lv/one.tex");
                WalpeTex *o = walpe_tex_load_ex (one, fl);
                CHECK (same_tex (l, o) && same_tex (r, o));
                CHECK (l && l->width == IW >> k && l->height == IH >> k && l->mip_level == k &&
                       l->base_width == IW && l->base_height == IH && l->mips == MIPS);
                walpe_tex_free (l);
                walpe_tex_free (r);
                walpe_tex_free (o);
                g_free (one);
            }
        }
        /* kept compressed: level 0 carries the stored chain, no level alone */
        if (type == 4 || type == 6 || type == 7) {
            WalpeTex *n0 = walpe_tex_load_ex (lz, WALPE_TEX_LOAD_NATIVE_DXT);
            CHECK (n0 && n0->format != WALPE_TEX_RGBA8 && n0->width == IW && n0->height == IH);
            walpe_tex_free (n0);
            CHECK (!walpe_tex_load_level (lz, WALPE_TEX_LOAD_NATIVE_DXT, 1));
        }
        WalpeTexProbe pr;
        CHECK (walpe_tex_probe (lz, &pr) && pr.width == IW && pr.height == IH &&
               pr.tex_width == TW && pr.tex_height == TH && pr.we_format == (int) type &&
               pr.mips == MIPS && pr.levels == 4 && pr.lz4 && pr.level_bytes[0] > 0);
        CHECK (walpe_tex_probe (raw, &pr) && pr.levels == 4 && pr.levels_floor == 5 && !pr.lz4 &&
               !pr.picture && pr.level_bytes[1] == level_bytes (type, TW / 2, TH / 2));
        g_free (lz);
        g_free (raw);
    }

    /* beyond the stored chain, negative, absurd */
    write_tex ("lv/two.tex", 0, 0, 64, 32, 64, 32, 0, 2, TRUE);
    char *p = P ("lv/two.tex");
    WalpeTex *t = walpe_tex_load_level (p, 0, 1);
    CHECK (t && t->width == 32 && t->height == 16 && t->len == 32 * 16 * 4);
    walpe_tex_free (t);
    CHECK (!walpe_tex_load_level (p, 0, 2) && !walpe_tex_load_level (p, 0, -1) &&
           !walpe_tex_load_level (p, 0, 99));
    g_free (p);

    /* an image with an odd side: its level 1 texels would mix in the
     * padding; a sprite sheet: frame rects are level 0 pixels */
    write_tex ("lv/odd.tex", 0, 0, 64, 32, 47, 24, 0, 3, TRUE);
    write_tex ("lv/sheet.tex", 0, 4, 64, 32, 48, 24, 0, 3, TRUE);
    p = P ("lv/odd.tex");
    WalpeTexProbe pr;
    CHECK (!walpe_tex_load_level (p, 0, 1) && walpe_tex_probe (p, &pr) && pr.levels == 1 &&
           pr.levels_floor == 3 && !pr.picture);
    t = walpe_tex_load_ex (p, 0);
    CHECK (t && t->width == 47 && t->height == 24);
    walpe_tex_free (t);
    /* on request the level is given anyway, floored like the texture:
     * 23x12 from the top-left of the 32x16 level */
    t = walpe_tex_load_level (p, WALPE_TEX_LOAD_LEVEL_FLOOR, 1);
    CHECK (t && t->width == 23 && t->height == 12 && t->len == 23 * 12 * 4 && t->mip_level == 1 &&
           t->base_width == 47 && t->base_height == 24);
    if (t) {
        gsize ll = 0;
        guint8 *lv = level_data (0, 32, 16, 1, &ll);
        gboolean same = TRUE;
        for (int y = 0; y < 12; y++)
            for (int x = 0; x < 23; x++) {
                const guint8 *sp = lv + (y * 32 + x) * 4, *dp = t->data + (y * 23 + x) * 4;
                same &= dp[0] == sp[2] && dp[1] == sp[1] && dp[2] == sp[0] && dp[3] == sp[3];
            }
        CHECK (same);
        g_free (lv);
    }
    walpe_tex_free (t);
    /* the flag changes nothing for level 0 or for an image that halves exactly */
    {
        WalpeTex *f0 = walpe_tex_load_level (p, WALPE_TEX_LOAD_LEVEL_FLOOR, 0), *n0 = walpe_tex_load_ex (p, 0);
        CHECK (same_tex (f0, n0));
        walpe_tex_free (f0);
        walpe_tex_free (n0);
    }
    g_free (p);
    p = P ("lv/sheet.tex");
    CHECK (!walpe_tex_load_level (p, 0, 1) && walpe_tex_probe (p, &pr) && pr.levels == 1);
    g_free (p);

    /* a level whose stored size is not the texture halved */
    {
        GByteArray *a = g_byte_array_new ();
        g_byte_array_append (a, (const guint8 *) "TEXV0005\0TEXI0001\0", 18);
        put32 (a, 9);
        put32 (a, 0);
        put32 (a, 16);
        put32 (a, 16);
        put32 (a, 16);
        put32 (a, 16);
        put32 (a, 0);
        g_byte_array_append (a, (const guint8 *) "TEXB0003\0", 9);
        put32 (a, 1);
        put32 (a, 0);
        put32 (a, 2);
        guint8 px[256] = { 0 };
        put32 (a, 16); put32 (a, 16); put32 (a, 0); put32 (a, 0); put32 (a, 256);
        g_byte_array_append (a, px, 256);
        put32 (a, 4); put32 (a, 4); put32 (a, 0); put32 (a, 0); put32 (a, 16);   /* not 8x8 */
        g_byte_array_append (a, px, 16);
        mkfile ("lv/wrongsize.tex", a->data, a->len);
        g_byte_array_unref (a);
        p = P ("lv/wrongsize.tex");
        CHECK (!walpe_tex_load_level (p, 0, 1) && walpe_tex_probe (p, &pr) && pr.levels == 1);
        t = walpe_tex_load_ex (p, 0);
        CHECK (t && t->width == 16 && t->height == 16);
        walpe_tex_free (t);
        g_free (p);
    }

    /* not a container */
    mkfile ("lv/plain.bin", "GIF89a-not-really", 17);
    p = P ("lv/plain.bin");
    CHECK (!walpe_tex_probe (p, &pr) && !walpe_tex_load_level (p, 0, 1));
    g_free (p);
}

/* --------------------------------------------------------------- pkgcache */

/* <T>/wp/<name>/scene.pkg with one entry of `size` bytes + project.json */
static char *make_item (const char *name, guint size)
{
    GByteArray *a = g_byte_array_new ();
    put32 (a, 8);
    g_byte_array_append (a, (const guint8 *) "PKGV0016", 8);
    put32 (a, 1);
    put32 (a, 9);
    g_byte_array_append (a, (const guint8 *) "blob.data", 9);
    put32 (a, 0);
    put32 (a, size);
    guint8 *blob = g_malloc (size);
    guint32 x = size;
    for (guint i = 0; i < size; i++) {
        x = x * 1664525u + 1013904223u;
        blob[i] = (guint8) (x >> 24);
    }
    g_byte_array_append (a, blob, size);
    g_free (blob);
    char *rel = g_strdup_printf ("wp/%s/scene.pkg", name);
    mkfile (rel, a->data, a->len);
    g_byte_array_unref (a);
    g_free (rel);
    rel = g_strdup_printf ("wp/%s/project.json", name);
    mkfile (rel, "{\"type\":\"scene\"}", -1);
    g_free (rel);
    rel = g_strdup_printf ("wp/%s", name);
    char *dir = P (rel);
    g_free (rel);
    return dir;
}

static char *open_item (const char *dir)
{
    char *pkg = g_build_filename (dir, "scene.pkg", NULL);
    GError *err = NULL;
    char *cache = walpe_pkg_extract_cached (pkg, dir, &err);
    CHECK (cache && !err);
    g_clear_error (&err);
    g_free (pkg);
    return cache;
}

static gboolean copy_ok (const char *cache)
{
    char *f = g_build_filename (cache, "blob.data", NULL);
    char *pj = g_build_filename (cache, "project.json", NULL);
    gboolean ok = g_file_test (f, G_FILE_TEST_IS_REGULAR) && g_file_test (pj, G_FILE_TEST_IS_REGULAR);
    g_free (f);
    g_free (pj);
    return ok;
}

/* last use of a cached copy, as the cache records it */
static void set_used (const char *cache, time_t when)
{
    char *stamp = g_build_filename (cache, ".walpe-pkg-stamp", NULL);
    struct utimbuf ut = { when, when };
    CHECK (utime (stamp, &ut) == 0);
    g_free (stamp);
}

static void test_pkgcache (void)
{
    enum { SIZE = 400 * 1024 };            /* two copies fit in 1 MiB, three do not */
    g_setenv ("WALPE_PKG_CACHE_MB", "1", TRUE);
    char *da = make_item ("a", SIZE), *db = make_item ("b", SIZE), *dc = make_item ("c", SIZE);
    time_t now = time (NULL);
    GStatBuf st;

    char *ca = open_item (da);
    CHECK (ca && copy_ok (ca));
    walpe_pkg_cache_release (ca);
    set_used (ca, now - 3000);
    char *cb = open_item (db);
    CHECK (cb && copy_ok (cb) && copy_ok (ca));      /* under the limit: both stay */
    walpe_pkg_cache_release (cb);
    set_used (cb, now - 2000);

    /* a third: over the limit; the least recently used (a) goes */
    char *cc = open_item (dc);
    CHECK (cc && copy_ok (cc) && copy_ok (cb) && !g_file_test (ca, G_FILE_TEST_EXISTS));
    walpe_pkg_cache_release (cc);
    set_used (cc, now - 1000);

    /* opening an up-to-date copy is a use: b is now newer than c */
    char *cb2 = open_item (db);
    CHECK (cb2 && !strcmp (cb2, cb));
    char *stamp = g_build_filename (cb2, ".walpe-pkg-stamp", NULL);
    CHECK (g_stat (stamp, &st) == 0 && st.st_mtime >= now - 5);
    g_free (stamp);
    walpe_pkg_cache_release (cb2);

    /* an evicted wallpaper is extracted again when opened; c goes for it */
    char *ca2 = open_item (da);                      /* held from here */
    CHECK (ca2 && !strcmp (ca2, ca) && copy_ok (ca2) && copy_ok (cb) &&
           !g_file_test (cc, G_FILE_TEST_EXISTS));

    /* a copy in use is never removed, whatever its age: a is the oldest
     * now, c comes back, b (not in use) is the one that goes */
    set_used (ca2, now - 5000);
    char *cc2 = open_item (dc);
    CHECK (cc2 && copy_ok (cc2) && copy_ok (ca2) && !g_file_test (cb, G_FILE_TEST_EXISTS));
    walpe_pkg_cache_release (cc2);
    walpe_pkg_cache_release (ca2);

    /* a copy whose package is gone, and a dir without a stamp (an
     * extraction that was interrupted), are removed whatever the limit */
    g_setenv ("WALPE_PKG_CACHE_MB", "0", TRUE);      /* no limit */
    char *pkga = g_build_filename (da, "scene.pkg", NULL);
    g_unlink (pkga);
    g_free (pkga);
    char *root = g_path_get_dirname (ca);
    char *junk = g_build_filename (root, "half-done-12345678", NULL);
    char *junkf = g_build_filename (junk, "x.bin", NULL);
    g_mkdir_with_parents (junk, 0755);
    g_file_set_contents (junkf, "x", 1, NULL);
    char *cb3 = open_item (db);
    CHECK (cb3 && copy_ok (cb3) && copy_ok (cc) && !g_file_test (ca, G_FILE_TEST_EXISTS) &&
           !g_file_test (junk, G_FILE_TEST_EXISTS));
    walpe_pkg_cache_release (cb3);
    g_unsetenv ("WALPE_PKG_CACHE_MB");
    g_free (junkf);
    g_free (junk);
    g_free (root);
    g_free (ca); g_free (cb); g_free (cc); g_free (ca2); g_free (cb2); g_free (cb3); g_free (cc2);
    g_free (da); g_free (db); g_free (dc);
}

int main (int argc, char **argv)
{
    char *tmp = g_dir_make_tmp ("walpe-resmem-test-XXXXXX", NULL);
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
    g_mkdir_with_parents (home, 0755);

    const char *only = argc > 1 ? argv[1] : NULL;
    if (!only || !strcmp (only, "levels"))
        test_levels ();
    if (!only || !strcmp (only, "pkgcache"))
        test_pkgcache ();

    char *rm = g_strdup_printf ("rm -rf '%s'", T);
    if (strstr (T, "walpe-resmem-test-") && system (rm) != 0)
        g_printerr ("could not remove %s\n", T);
    g_free (rm);
    g_free (home);
    g_free (cfg);
    g_free (cache);
    g_free (data);
    if (failures)
        g_printerr ("%d check(s) failed\n", failures);
    else
        g_print ("resmem: all checks passed\n");
    return failures ? 1 : 0;
}
