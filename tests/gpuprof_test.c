/* gpuprof_test.c - the GPU profiler (src/engine/gpuprof.h)
 *
 * 1. helpers: effect label, quad pixel area, upload sizes, driver-message
 *    de-duplication (numbers collapsed, counts, monitors, table limit,
 *    shader-compiler text never kept)
 * 2. report formatting on hand-made reports: rows sorted by GPU time, the
 *    top 15 and the "(+N more rows" line, per-monitor and overall TOTAL
 *    lines, performance messages first, long / non-ASCII layer names,
 *    a monitor with no frame (no division by zero), the fallback without
 *    timer queries (sorted by CPU time), JSON that parses and agrees
 * 3. with an EGL device or Mesa's surfaceless platform, on GL 3.3 and on
 *    GLES 3: a real run - timer queries give GPU time per layer and pass,
 *    the counting wrappers see draws and uploads, the KHR_debug channel
 *    de-duplicates injected performance messages, the memory snapshot
 *    finds the render target; afterwards the context is as before (no
 *    query active, debug output off, libepoxy's pointers restored); the
 *    same run with WALPE_GPU_PROFILE_TIMER=0 falls back to CPU time; hooks
 *    do nothing while no run is active. Skipped without such a context.
 */
#include <glib.h>
#include <json-glib/json-glib.h>
#include <epoxy/gl.h>
#include <epoxy/egl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine/gpuprof.h"
#include "engine/glutil.h"

static int fails;

#define CHECK(cond, ...) \
    do { \
        if (!(cond)) { \
            fails++; \
            printf ("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf (__VA_ARGS__); \
            printf ("\n"); \
        } \
    } while (0)

/* --------------------------------------------------------------- helpers */

static void test_helpers (void)
{
    char b[48];
    walpe_prof_effect_label (b, sizeof (b), "effects/waterripple/effect.json");
    CHECK (!strcmp (b, "waterripple"), "label: %s", b);
    walpe_prof_effect_label (b, sizeof (b), "effects/workshop/2846673187/My Effect/effect.json");
    CHECK (!strcmp (b, "My Effect"), "label: %s", b);
    walpe_prof_effect_label (b, sizeof (b), "");
    CHECK (!strcmp (b, "effect"), "label of nothing: %s", b);
    walpe_prof_effect_label (b, sizeof (b), NULL);
    CHECK (!strcmp (b, "effect"), "label of NULL: %s", b);
    walpe_prof_effect_label (b, sizeof (b), "effect.json");
    CHECK (!strcmp (b, "effect"), "label of a bare effect.json: %s", b);
    walpe_prof_effect_label (b, sizeof (b), "shaders/foo.frag");
    CHECK (!strcmp (b, "foo"), "label of another file: %s", b);
    char s8[8];
    /* cut inside a 3-byte character: the whole character goes */
    walpe_prof_effect_label (s8, sizeof (s8), "effects/abcde\xe9\x9b\xa8\xe9\x9b\xa8/effect.json");
    CHECK (!strcmp (s8, "abcde"), "label cut at a character boundary: '%s'", s8);
    CHECK (g_utf8_validate (s8, -1, NULL), "label is valid UTF-8");
    walpe_prof_effect_label (s8, sizeof (s8), "effects/a\x01" "b\nc/effect.json");
    CHECK (!strcmp (s8, "a b c"), "control characters become spaces: '%s'", s8);
    walpe_prof_effect_label (s8, sizeof (s8), "effects/\xff\xfe/effect.json");
    CHECK (g_utf8_validate (s8, -1, NULL), "invalid UTF-8 is repaired");

    const float ident[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    CHECK (fabs (walpe_prof_quad_pixels (ident, 1920, 1080) - 1920.0 * 1080.0) < 1.0, "full quad");
    const float half[9] = { 0.5f, 0, 0, 0, 0.5f, 0, 0, 0, 1 };
    CHECK (fabs (walpe_prof_quad_pixels (half, 1920, 1080) - 1920.0 * 1080.0 / 4) < 1.0, "half quad");
    const float off[9] = { 0.5f, 0, 0, 0, 0.5f, 0, 3, 0, 1 };
    CHECK (walpe_prof_quad_pixels (off, 1920, 1080) == 0.0, "off-screen quad");
    const float part[9] = { 1, 0, 0, 0, 1, 0, 1, 0, 1 };   /* right half visible */
    CHECK (fabs (walpe_prof_quad_pixels (part, 1000, 500) - 500.0 * 500.0) < 1.0, "clipped quad");
    const float big[9] = { 4, 0, 0, 0, 4, 0, 0, 0, 1 };
    CHECK (fabs (walpe_prof_quad_pixels (big, 100, 100) - 10000.0) < 1.0, "oversized quad = target");
    const float nanm[9] = { NAN, 0, 0, 0, 1, 0, 0, 0, 1 };
    CHECK (walpe_prof_quad_pixels (nanm, 100, 100) == 0.0, "NaN matrix");
    CHECK (walpe_prof_quad_pixels (NULL, 100, 100) == 0.0, "no matrix");
    CHECK (walpe_prof_quad_pixels (ident, 0, 100) == 0.0, "no target");
    const float rot[9] = { 0, 0.5f, 0, -0.5f, 0, 0, 0, 0, 1 };   /* 90 degrees, half size */
    CHECK (fabs (walpe_prof_quad_pixels (rot, 800, 800) - 160000.0) < 1.0, "rotated quad");

    CHECK (walpe_prof_image_bytes (GL_RGBA, GL_UNSIGNED_BYTE, 10, 20) == 800, "RGBA8 bytes");
    CHECK (walpe_prof_image_bytes (GL_BGRA, GL_UNSIGNED_BYTE, 10, 20) == 800, "BGRA8 bytes");
    CHECK (walpe_prof_image_bytes (GL_RED, GL_UNSIGNED_BYTE, 10, 20) == 200, "R8 bytes");
    CHECK (walpe_prof_image_bytes (GL_RG, GL_UNSIGNED_BYTE, 10, 20) == 400, "RG8 bytes");
    CHECK (walpe_prof_image_bytes (GL_RGB, GL_UNSIGNED_BYTE, 10, 20) == 600, "RGB8 bytes");
    CHECK (walpe_prof_image_bytes (GL_RGBA, GL_HALF_FLOAT, 10, 20) == 1600, "RGBA16F bytes");
    CHECK (walpe_prof_image_bytes (GL_RGBA, GL_FLOAT, 10, 20) == 3200, "RGBA32F bytes");
    CHECK (walpe_prof_image_bytes (GL_RGB, GL_UNSIGNED_SHORT_5_6_5, 10, 20) == 400, "565 bytes");
    CHECK (walpe_prof_image_bytes (GL_RGBA, GL_UNSIGNED_BYTE, 0, 20) == 0, "empty image");
    CHECK (walpe_prof_image_bytes (GL_RGBA, GL_UNSIGNED_BYTE, -3, 20) == 0, "negative size");

    for (int st = 0; st < WALPE_PROF_N_STAGES; st++)
        CHECK (walpe_prof_stage_name (st)[0] != '?', "stage %d has a name", st);
    CHECK (!strcmp (walpe_prof_stage_name (99), "?"), "unknown stage");
}

static void test_messages (void)
{
    char *k1 = walpe_prof_msg_key (1, 2, 131186,
        "Buffer performance warning: Buffer object 3 (bound to GL_ARRAY_BUFFER_ARB, usage hint is "
        "GL_DYNAMIC_DRAW) is being copied/moved from VIDEO memory to HOST memory.");
    char *k2 = walpe_prof_msg_key (1, 2, 131186,
        "Buffer performance warning: Buffer object 17 (bound to GL_ARRAY_BUFFER_ARB, usage hint is "
        "GL_DYNAMIC_DRAW) is being copied/moved from VIDEO memory to HOST memory.");
    CHECK (!strcmp (k1, k2), "keys ignore numbers:\n %s\n %s", k1, k2);
    char *k3 = walpe_prof_msg_key (1, 2, 131186, "object 0x1f is slow");
    char *k4 = walpe_prof_msg_key (1, 2, 131186, "object 0x2ab3 is slow");
    CHECK (!strcmp (k3, k4), "keys ignore hexadecimal numbers: %s / %s", k3, k4);
    char *k5 = walpe_prof_msg_key (1, 2, 7, "object 0x2ab3 is slow");
    CHECK (strcmp (k4, k5), "another id is another key");
    char *k6 = walpe_prof_msg_key (1, 2, 7, NULL);
    CHECK (k6 && *k6, "key of no text");
    g_free (k1); g_free (k2); g_free (k3); g_free (k4); g_free (k5); g_free (k6);

    WalpeProfMsg tab[3];
    guint n = 0, dropped = 0;
    int a = walpe_prof_msg_add (tab, &n, 3, &dropped, GL_DEBUG_SOURCE_API, GL_DEBUG_TYPE_PERFORMANCE,
                                GL_DEBUG_SEVERITY_MEDIUM, 131218,
                                "Program/shader state performance warning: Vertex shader in program 12 "
                                "is being recompiled based on GL state.", 0);
    int b = walpe_prof_msg_add (tab, &n, 3, &dropped, GL_DEBUG_SOURCE_API, GL_DEBUG_TYPE_PERFORMANCE,
                                GL_DEBUG_SEVERITY_MEDIUM, 131218,
                                "Program/shader state performance warning: Vertex shader in program 40 "
                                "is being recompiled based on GL state.\n", 1);
    CHECK (a == 0 && b == 0 && n == 1, "the same message twice is one entry (%d %d %u)", a, b, n);
    CHECK (tab[0].count == 2, "count %u", tab[0].count);
    CHECK (tab[0].monitors == 3, "monitor mask %u", tab[0].monitors);
    CHECK (strstr (tab[0].text, "program 12"), "the first text is kept: %s", tab[0].text);
    CHECK (!strchr (tab[0].text, '\n'), "no line break in the text");
    int c = walpe_prof_msg_add (tab, &n, 3, &dropped, GL_DEBUG_SOURCE_API, GL_DEBUG_TYPE_ERROR,
                                GL_DEBUG_SEVERITY_HIGH, 1, "GL_INVALID_OPERATION in glDrawArrays", 0);
    CHECK (c == 1 && n == 2, "another message is another entry");
    int d = walpe_prof_msg_add (tab, &n, 3, &dropped, GL_DEBUG_SOURCE_SHADER_COMPILER,
                                GL_DEBUG_TYPE_ERROR, GL_DEBUG_SEVERITY_HIGH, 1,
                                "0:12(3): error: `secret_wallpaper_variable' undeclared", 0);
    CHECK (d == 2 && n == 3, "shader compiler message kept as an entry");
    CHECK (!strstr (tab[2].text, "secret"), "... without its text: %s", tab[2].text);
    int e = walpe_prof_msg_add (tab, &n, 3, &dropped, GL_DEBUG_SOURCE_API, GL_DEBUG_TYPE_OTHER,
                                GL_DEBUG_SEVERITY_LOW, 5, "one more", 0);
    CHECK (e == -1 && n == 3 && dropped == 1, "a full table drops and counts (%d %u %u)", e, n, dropped);
    int f = walpe_prof_msg_add (tab, &n, 3, &dropped, GL_DEBUG_SOURCE_API, GL_DEBUG_TYPE_ERROR,
                                GL_DEBUG_SEVERITY_HIGH, 1, "GL_INVALID_OPERATION in glDrawArrays", 40);
    CHECK (f == 1 && tab[1].count == 2 && dropped == 1, "a known message still counts in a full table");
    CHECK (tab[1].monitors == 1, "a monitor index beyond the mask is ignored");
    char longtext[2000];
    memset (longtext, 'x', sizeof (longtext) - 1);
    longtext[sizeof (longtext) - 1] = 0;
    n = 0;
    walpe_prof_msg_add (tab, &n, 3, &dropped, 1, 2, 3, 4, longtext, 0);
    CHECK (strlen (tab[0].text) < sizeof (tab[0].text), "long text is cut");
}

/* ------------------------------------------------------------ formatting */

static void set_row (WalpeProfRow *r, int li, const char *layer, const char *effect, int inst,
                     int pass, int tw, int th, double gpu_ms, double cpu_ms, guint64 frames)
{
    memset (r, 0, sizeof (*r));
    g_strlcpy (r->layer, layer, sizeof (r->layer));
    g_strlcpy (r->effect, effect, sizeof (r->effect));
    g_strlcpy (r->kind, "image", sizeof (r->kind));
    r->layer_index = li;
    r->inst = inst;
    r->pass = pass;
    r->tw = tw;
    r->th = th;
    r->gpu_ns = (guint64) (gpu_ms * 1e6 * frames);
    r->cpu_ns = (guint64) (cpu_ms * 1e6 * frames);
    r->runs = frames;
    r->draws = frames;
    r->pixels = (guint64) tw * th * frames;
}

static WalpeProfReport *make_report (gboolean timer)
{
    WalpeProfReport *r = g_new0 (WalpeProfReport, 1);
    g_strlcpy (r->when, "2026-11-10T12:00:00", sizeof (r->when));
    g_strlcpy (r->version, "0.12.0", sizeof (r->version));
    r->seconds = 10.0;
    r->shared = TRUE;
    r->n_mons = 3;
    r->mons = g_new0 (WalpeProfMon, 3);

    WalpeProfMon *m = &r->mons[0];
    m->monitor = 0;
    g_strlcpy (m->name, "DP-0", sizeof (m->name));
    m->w = 1920; m->h = 1080; m->hz = 144.0;
    g_strlcpy (m->type, "scene", sizeof (m->type));
    g_strlcpy (m->wallpaper, "2901582518", sizeof (m->wallpaper));
    g_strlcpy (m->gl_vendor, "NVIDIA Corporation", sizeof (m->gl_vendor));
    g_strlcpy (m->gl_renderer, "NVIDIA GeForce RTX 4050 Laptop GPU/PCIe/SSE2", sizeof (m->gl_renderer));
    g_strlcpy (m->gl_version, "4.6.0 NVIDIA 550.120", sizeof (m->gl_version));
    m->gl_ver = 46;
    g_strlcpy (m->timer, timer ? "GL_TIME_ELAPSED queries, GL 3.3 core, 64 bits"
                               : "none: this GL context lacks ARB_timer_query (CPU time only)",
               sizeof (m->timer));
    m->timer_ok = timer;
    g_strlcpy (m->debug, "KHR_debug on, debug context", sizeof (m->debug));
    m->hooks_ok = TRUE;
    m->attached = TRUE;
    m->seconds = 10.0;
    m->frames = 600;
    m->presented = 598;
    m->skipped = 2;
    m->gpu_frames = timer ? 600 : 0;
    m->n_rows = 20;
    m->rows = g_new0 (WalpeProfRow, 20);
    guint64 gpu = 0;
    for (int i = 0; i < 20; i++) {
        char layer[64];
        g_snprintf (layer, sizeof (layer), "Layer %02d", i);
        /* GPU time grows with i, CPU time shrinks: the two orders differ */
        set_row (&m->rows[i], i, layer, i % 2 ? "blur" : "", i % 2 ? 0 : -1, i % 2 ? 1 : -1,
                 i % 2 ? 960 : 1920, i % 2 ? 540 : 1080, timer ? 0.1 * (i + 1) : 0.0,
                 0.01 * (20 - i), 600);
        gpu += m->rows[i].gpu_ns;
    }
    /* a long name, a non-ASCII name */
    g_strlcpy (m->rows[19].layer, "A very long layer name that does not fit into the column at all",
               sizeof (m->rows[19].layer));
    g_strlcpy (m->rows[18].layer, "\xe8\x83\x8c\xe6\x99\xaf \"quoted\" back\\slash", sizeof (m->rows[18].layer));
    m->gpu_ns = gpu;
    m->gpu_max_ns = 30000000;
    m->counters[WALPE_PROF_C_PASSES] = 600 * 10;
    m->counters[WALPE_PROF_C_DRAWS] = 600 * 20;
    m->counters[WALPE_PROF_C_PIXELS] = (guint64) 600 * 1920 * 1080 * 12;
    m->counters[WALPE_PROF_C_TEX_UPLOAD_BYTES] = 600 * 2048;
    m->counters[WALPE_PROF_C_TEX_UPLOADS] = 600;
    m->counters[WALPE_PROF_C_BUF_UPLOAD_BYTES] = 600 * 512;
    m->fx_cached = 600 * 4;
    m->load_upload_bytes = 300 << 20;
    m->stage_wall_ns[WALPE_PROF_ST_LAYERS] = (guint64) 600 * 1500000;
    m->stage_cpu_ns[WALPE_PROF_ST_LAYERS] = (guint64) 600 * 1400000;
    m->stage_wall_ns[WALPE_PROF_ST_SWAP] = (guint64) 600 * 5000000;
    m->mem_bytes[WALPE_PROF_MEM_TEX_COMPRESSED] = 300 << 20;
    m->mem_count[WALPE_PROF_MEM_TEX_COMPRESSED] = 12;
    m->mem_bytes[WALPE_PROF_MEM_FX_TARGETS] = 64 << 20;
    m->mem_count[WALPE_PROF_MEM_FX_TARGETS] = 8;
    m->mem_bytes[WALPE_PROF_MEM_SURFACES] = 1920 * 1080 * 8;
    m->mem_shared = TRUE;
    m->vram_total_kb = 6141 * 1024;
    m->vram_avail_kb = 5000 * 1024;
    m->vram_evictions = 0;

    /* monitor 1: a follower that only presents; monitor 2: nothing drawn */
    m = &r->mons[1];
    m->monitor = 1;
    g_strlcpy (m->name, "HDMI-0", sizeof (m->name));
    m->w = 1920; m->h = 1080; m->hz = 60.0;
    g_strlcpy (m->type, "scene", sizeof (m->type));
    g_strlcpy (m->role, "follows a shared frame", sizeof (m->role));
    g_strlcpy (m->timer, "GL_TIME_ELAPSED queries, GL 3.3 core, 64 bits", sizeof (m->timer));
    m->timer_ok = timer;
    g_strlcpy (m->debug, "KHR_debug on, debug context", sizeof (m->debug));
    m->hooks_ok = TRUE;
    m->attached = TRUE;
    m->seconds = 10.0;
    m->frames = 600;
    m->presented = 600;
    m->gpu_frames = timer ? 600 : 0;
    m->n_rows = 1;
    m->rows = g_new0 (WalpeProfRow, 1);
    set_row (&m->rows[0], -3, "(shared frame: present)", "", -1, -1, 1920, 1080, timer ? 0.25 : 0.0,
             0.02, 600);
    m->gpu_ns = m->rows[0].gpu_ns;
    m->mem_bytes[WALPE_PROF_MEM_SURFACES] = 1920 * 1080 * 8;

    m = &r->mons[2];
    m->monitor = 2;
    g_strlcpy (m->name, "DP-2", sizeof (m->name));
    m->w = 2560; m->h = 1440;
    g_strlcpy (m->type, "image", sizeof (m->type));
    g_strlcpy (m->wallpaper, "(local item)", sizeof (m->wallpaper));
    g_strlcpy (m->timer, "GL_TIME_ELAPSED queries, GL 3.3 core, 64 bits", sizeof (m->timer));
    m->timer_ok = timer;
    m->attached = TRUE;
    m->seconds = 10.0;
    m->vram_evictions = -1;

    r->n_msgs = 4;
    r->msgs = g_new0 (WalpeProfMsg, 4);
    static const struct { guint source, type, severity, id, count, monitors; const char *text; } mm[4] = {
        { GL_DEBUG_SOURCE_API, GL_DEBUG_TYPE_OTHER, GL_DEBUG_SEVERITY_NOTIFICATION, 131185, 50, 1,
          "Buffer detailed info: Buffer object 1 will use VIDEO memory" },
        { GL_DEBUG_SOURCE_API, GL_DEBUG_TYPE_ERROR, GL_DEBUG_SEVERITY_HIGH, 1282, 1, 1,
          "GL_INVALID_OPERATION error generated." },
        { GL_DEBUG_SOURCE_API, GL_DEBUG_TYPE_PERFORMANCE, GL_DEBUG_SEVERITY_MEDIUM, 131218, 120, 3,
          "Program/shader state performance warning: Vertex shader in program 12 is being "
          "recompiled based on GL state." },
        { GL_DEBUG_SOURCE_API, GL_DEBUG_TYPE_PERFORMANCE, GL_DEBUG_SEVERITY_MEDIUM, 131154, 7, 1,
          "Pixel-path performance warning: Pixel transfer is synchronized with 3D rendering." },
    };
    for (int i = 0; i < 4; i++) {
        r->msgs[i].source = mm[i].source;
        r->msgs[i].type = mm[i].type;
        r->msgs[i].severity = mm[i].severity;
        r->msgs[i].id = mm[i].id;
        r->msgs[i].count = mm[i].count;
        r->msgs[i].monitors = mm[i].monitors;
        g_strlcpy (r->msgs[i].text, mm[i].text, sizeof (r->msgs[i].text));
    }
    return r;
}

static void free_report (WalpeProfReport *r)
{
    for (guint i = 0; i < r->n_mons; i++)
        g_free (r->mons[i].rows);
    g_free (r->mons);
    g_free (r->msgs);
    g_free (r);
}

/* the numbered table rows ("   NN  ...") of monitor `mon` in a text report */
static GPtrArray *table_rows (const char *text, int mon)
{
    GPtrArray *out = g_ptr_array_new_with_free_func (g_free);
    char **lines = g_strsplit (text, "\n", -1);
    char head[32];
    g_snprintf (head, sizeof (head), "== monitor %d ", mon);
    gboolean in = FALSE;
    for (char **l = lines; *l; l++) {
        if (g_str_has_prefix (*l, "== monitor "))
            in = g_str_has_prefix (*l, head);
        else if (in && strlen (*l) > 6 && (*l)[0] == ' ' && g_ascii_isdigit ((*l)[4]) &&
                 (*l)[5] == ' ' && ((*l)[3] == ' ' || g_ascii_isdigit ((*l)[3])))
            g_ptr_array_add (out, g_strdup (*l));
    }
    g_strfreev (lines);
    return out;
}

static void test_format_text (void)
{
    WalpeProfReport *r = make_report (TRUE);
    char *t = walpe_prof_format_text (r);
    CHECK (g_utf8_validate (t, -1, NULL), "text is valid UTF-8");
    CHECK (!strstr (t, "nan") && !strstr (t, "inf"), "no nan / inf in the text");
    CHECK (g_str_has_prefix (t, "WalPE GPU profile  2026-11-10T12:00:00  walpe 0.12.0  run 10.0 s"),
           "header line");
    CHECK (strstr (t, "== monitor 0 (DP-0, 1920x1080 @ 144.0 Hz): scene 2901582518"), "monitor 0 head");
    CHECK (strstr (t, "== monitor 1 (HDMI-0, 1920x1080 @ 60.0 Hz): scene, follows a shared frame"),
           "monitor 1 head");
    CHECK (strstr (t, "== monitor 2 (DP-2, 2560x1440): image (local item)"), "monitor 2 head");
    CHECK (strstr (t, "frames: 600 rendered (60.0/s), 598 presented, 2 skipped as unchanged"),
           "frames line");
    CHECK (strstr (t, "no frame was rendered during the run"), "monitor without frames says so");

    GPtrArray *rows = table_rows (t, 0);
    CHECK (rows->len == WALPE_PROF_TOP_ROWS, "top %d rows listed, got %u", WALPE_PROF_TOP_ROWS,
           rows->len);
    /* most GPU time first: Layer 19 (2.0 ms) ... Layer 05 (0.6 ms) */
    double prev = 1e9;
    for (guint i = 0; i < rows->len; i++) {
        const char *l = g_ptr_array_index (rows, i);
        int rank = 0;
        double gpu = -1, pct = -1, cpu = -1;
        CHECK (sscanf (l, " %d %lf %lf %lf", &rank, &gpu, &pct, &cpu) == 4, "row parses: %s", l);
        CHECK (rank == (int) i + 1, "rank %d at position %u", rank, i + 1);
        CHECK (gpu <= prev + 1e-9, "rows sorted by GPU time: %.3f after %.3f", gpu, prev);
        prev = gpu;
    }
    if (rows->len == WALPE_PROF_TOP_ROWS) {
        const char *first = g_ptr_array_index (rows, 0), *last = g_ptr_array_index (rows, 14);
        CHECK (strstr (first, "    2.000 "), "the most expensive row leads: %s", first);
        CHECK (strstr (first, "A very long layer name ~"), "long layer name is cut: %s", first);
        CHECK (strstr (first, "blur (e0 p1)"), "effect, its index and pass: %s", first);
        CHECK (strstr (first, "960x540"), "target size: %s", first);
        CHECK (strstr (last, "    0.600 "), "the 15th row: %s", last);
        const char *second = g_ptr_array_index (rows, 1);
        CHECK (strstr (second, "\xe8\x83\x8c\xe6\x99\xaf \"quoted\" back\\slash"),
               "non-ASCII layer name intact: %s", second);
        CHECK (strstr (second, "(layer draw)"), "a layer's own row: %s", second);
        CHECK (strstr (second, "1920x1080"), "the frame is its target: %s", second);
    }
    g_ptr_array_unref (rows);
    /* rows 16..20: 0.5 + 0.4 + 0.3 + 0.2 + 0.1 = 1.5 ms GPU */
    CHECK (strstr (t, "(+5 more rows: GPU 1.500 ms/f"), "the rest is summed");
    rows = table_rows (t, 1);
    CHECK (rows->len == 1, "the follower has one row");
    g_ptr_array_unref (rows);

    /* 0.1 * (1 + ... + 20) = 21 ms per frame at 60 fps = 126 % busy */
    /* rows 0-4 spend 0.20-0.16 ms on the CPU for 0.1-0.5 ms of "GPU
     * time": flagged, their possible waiting (0.10 + 0.19 + 0.18 + 0.17 +
     * 0.16 = 0.8 ms) is named as the lower bound */
    CHECK (strstr (t, "TOTAL mon0: GPU 21.000 ms/frame (max 30.000), 126.0 % GPU busy at 60.0 fps, "
                      "at least 20.200 ms of it GPU work (rows marked *); "
                      "10.0 passes, 20.0 draws, 24.88 Mpx shaded, 2.5 KiB uploaded per frame"),
           "per-monitor total");
    CHECK (strstr (t, "* 5 rows spent a large part of their GPU time on the CPU"), "footnote of the "
           "flagged rows");
    CHECK (strstr (t, "Up to 0.800 of the 21.000 ms/f may be waiting"), "possible waiting summed");
    CHECK (!strstr (t, "NOTE: monitor"), "no pause note for a run without a pause");
    CHECK (!strstr (t, "while drawing"), "no second frame rate for a run that drew all the time");
    CHECK (strstr (t, "TOTAL mon2: GPU time n/a, 0.0 fps"), "total of the idle monitor");
    CHECK (strstr (t, "passes 10.0 (+4.0 from the static cache)"), "cached passes");
    CHECK (strstr (t, "outside frames (wallpaper loads): 300 MiB uploaded"), "load uploads");
    CHECK (strstr (t, "render thread ms/frame (wall/cpu): layers 1.500/1.400 | swap 5.000/0.000"),
           "stages");
    CHECK (strstr (t, "compressed textures 12 / 300 MiB"), "memory categories");
    CHECK (strstr (t, "effect targets 8 / 64 MiB"), "effect targets");
    CHECK (strstr (t, "driver, mon0: 6141 MiB video memory, 5000 MiB free, 0 evictions"), "driver memory");
    CHECK (strstr (t, "driver messages (KHR_debug): 4 distinct, 178 in all; performance: 2 distinct, "
                      "127 in all"), "message summary");
    const char *p1 = strstr (t, "[performance] x120 mon0,1 api/medium id 131218: Program/shader");
    const char *p2 = strstr (t, "[performance] x7 mon0 api/medium id 131154: Pixel-path");
    const char *p3 = strstr (t, "[error] x1 mon0 api/high id 1282");
    CHECK (p1 && p2 && p3 && p1 < p3 && p2 < p3, "performance messages come first");
    CHECK (!strstr (t, "Buffer detailed info"), "notifications are counted, not listed");
    CHECK (strstr (t, "(+1 distinct notifications, 50 in all"), "notification count");

    char *total = walpe_prof_format_total (r);
    CHECK (!strchr (total, '\n'), "the total is one line");
    CHECK (g_str_has_prefix (total, "TOTAL: GPU 21.250 ms/frame, 127.5 % GPU busy (at least 20.450 ms "
                                    "GPU work, the rest may be the GPU waiting in rows marked *) over "
                                    "3 monitors (mon0 21.000 ms @ 60.0 fps, mon1 0.250 ms @ 60.0 fps, "
                                    "mon2 "),
           "overall total: %s", total);
    CHECK (strstr (total, "2 driver performance messages (127 in all)"), "total names the messages");
    /* the last line of the text is the total */
    char *want = g_strdup_printf ("\n%s\n", total);
    CHECK (g_str_has_suffix (t, want), "the report ends with the total line");
    g_free (want);
    g_free (total);
    g_free (t);
    free_report (r);

    /* no report content at all must not crash */
    WalpeProfReport empty = { 0 };
    t = walpe_prof_format_text (&empty);
    CHECK (t && strstr (t, "TOTAL: GPU time n/a") && !strstr (t, "nan"), "empty report");
    g_free (t);
    t = walpe_prof_format_json (&empty);
    JsonNode *n = json_from_string (t, NULL);
    CHECK (n != NULL, "empty report is JSON");
    if (n)
        json_node_unref (n);
    g_free (t);
}

static void test_format_fallback (void)
{
    WalpeProfReport *r = make_report (FALSE);
    char *t = walpe_prof_format_text (r);
    CHECK (strstr (t, "(no GPU timer: sorted by CPU time)"), "fallback header");
    CHECK (strstr (t, "timer: none: this GL context lacks ARB_timer_query"), "why there is no timer");
    CHECK (!strstr (t, "GPU ms/f "), "no GPU column");
    CHECK (!strstr (t, "nan") && !strstr (t, "inf"), "no nan / inf");
    GPtrArray *rows = table_rows (t, 0);
    CHECK (rows->len == WALPE_PROF_TOP_ROWS, "top rows listed, got %u", rows->len);
    double prev = 1e9;
    for (guint i = 0; i < rows->len; i++) {
        int rank = 0;
        double cpu = -1, pct = -1;
        CHECK (sscanf (g_ptr_array_index (rows, i), " %d %lf %lf", &rank, &cpu, &pct) == 3, "row parses");
        CHECK (cpu <= prev + 1e-9, "rows sorted by CPU time");
        prev = cpu;
    }
    if (rows->len)
        CHECK (strstr (g_ptr_array_index (rows, 0), "Layer 00"), "most CPU time first: %s",
               (char *) g_ptr_array_index (rows, 0));
    g_ptr_array_unref (rows);
    CHECK (strstr (t, "TOTAL mon0: GPU time n/a, 60.0 fps"), "per-monitor total without GPU time");
    CHECK (strstr (t, "\nTOTAL: GPU time n/a (no timer queries) over 3 monitors"), "overall total");
    g_free (t);
    free_report (r);
}

static void test_format_json (void)
{
    WalpeProfReport *r = make_report (TRUE);
    char *j = walpe_prof_format_json (r);
    CHECK (g_utf8_validate (j, -1, NULL), "JSON is valid UTF-8");
    GError *err = NULL;
    JsonNode *root = json_from_string (j, &err);
    CHECK (root && JSON_NODE_HOLDS_OBJECT (root), "JSON parses: %s", err ? err->message : "");
    g_clear_error (&err);
    if (root && JSON_NODE_HOLDS_OBJECT (root)) {
        JsonObject *o = json_node_get_object (root);
        CHECK (json_object_get_int_member (o, "walpe_gpu_profile") == 1, "format marker");
        CHECK (!g_strcmp0 (json_object_get_string_member (o, "version"), "0.12.0"), "version");
        JsonArray *mons = json_object_get_array_member (o, "monitors");
        CHECK (json_array_get_length (mons) == 3, "three monitors");
        JsonObject *m0 = json_array_get_object_element (mons, 0);
        CHECK (json_object_get_int_member (m0, "frames") == 600, "frames");
        CHECK (json_object_get_int_member (m0, "skipped_unchanged") == 2, "skipped");
        CHECK (fabs (json_object_get_double_member (m0, "gpu_ms_per_frame") - 21.0) < 1e-6, "gpu ms");
        CHECK (fabs (json_object_get_double_member (m0, "gpu_busy_percent") - 126.0) < 1e-6, "busy");
        JsonArray *rows = json_object_get_array_member (m0, "rows");
        CHECK (json_array_get_length (rows) == 20, "JSON has every row, got %u",
               json_array_get_length (rows));
        double prev = 1e9;
        for (guint i = 0; i < json_array_get_length (rows); i++) {
            double g = json_object_get_double_member (json_array_get_object_element (rows, i),
                                                      "gpu_ms_per_frame");
            CHECK (g <= prev + 1e-9, "JSON rows sorted by GPU time");
            prev = g;
        }
        JsonObject *r1 = json_array_get_object_element (rows, 1);
        CHECK (!g_strcmp0 (json_object_get_string_member (r1, "layer"),
                           "\xe8\x83\x8c\xe6\x99\xaf \"quoted\" back\\slash"), "layer name round-trips");
        JsonObject *r0 = json_array_get_object_element (rows, 0);
        CHECK (!g_strcmp0 (json_object_get_string_member (r0, "effect"), "blur"), "effect name");
        CHECK (json_object_get_int_member (r0, "target_w") == 960, "target size");
        CHECK (json_object_get_int_member (r0, "pass") == 1, "pass");
        JsonObject *pf = json_object_get_object_member (m0, "per_frame");
        CHECK (fabs (json_object_get_double_member (pf, "draws") - 20.0) < 1e-9, "draws per frame");
        CHECK (fabs (json_object_get_double_member (pf, "tex_upload_bytes") - 2048.0) < 1e-9, "uploads");
        CHECK (json_object_has_member (pf, "passes_cached"), "cached passes");
        JsonObject *st = json_object_get_object_member (m0, "stages_ms_per_frame");
        CHECK (fabs (json_object_get_double_member (json_object_get_object_member (st, "layers"),
                                                    "cpu") - 1.4) < 1e-9, "stage cpu");
        JsonObject *mem = json_object_get_object_member (o, "memory");
        CHECK (json_object_get_int_member (json_object_get_object_member (mem, "effect_targets"),
                                           "bytes") == 64 << 20, "memory by category");
        CHECK (json_object_get_int_member (mem, "total_bytes") ==
               (300 << 20) + (64 << 20) + 2 * 1920 * 1080 * 8, "memory total");
        JsonArray *msgs = json_object_get_array_member (o, "driver_messages");
        CHECK (json_array_get_length (msgs) == 4, "messages");
        JsonObject *tot = json_object_get_object_member (o, "total");
        char *line = walpe_prof_format_total (r);
        CHECK (!g_strcmp0 (json_object_get_string_member (tot, "line"), line), "total line in JSON");
        g_free (line);
        CHECK (json_object_get_int_member (tot, "performance_messages") == 2, "performance messages");
        JsonObject *m2 = json_array_get_object_element (mons, 2);
        CHECK (json_object_get_double_member (m2, "fps") == 0.0, "idle monitor: fps 0, not nan");
    }
    if (root)
        json_node_unref (root);
    g_free (j);
    free_report (r);
}

/* ------------------------------------------------------------- real GL */

/* a run in which a monitor was paused for a part, and a row whose
 * GPU timer brackets CPU time: a report from the maintainer's RTX 4050 (850
 * frames in a 30 s run read as "28.3 fps"; an upload row of 0.919 ms GPU /
 * 0.516 ms CPU read as GPU work) */
static void test_partial_run (void)
{
    WalpeProfReport *r = make_report (TRUE);
    WalpeProfMon *m = &r->mons[0];
    m->seconds = 30.0;
    m->frames = 850;
    m->presented = 850;
    m->skipped = 0;
    m->gpu_frames = 850;
    m->idle_seconds = 15.8;
    m->draw_runs = 1;        /* one run of 850 frames = 849 intervals in 14.2 s */
    m->paused_seconds = 15.8;
    g_strlcpy (m->pause_reason, "a fullscreen window has the focus", sizeof (m->pause_reason));
    /* the follower: its one row is CPU bound (0.2 of 0.25 ms) */
    WalpeProfMon *f = &r->mons[1];
    f->rows[0].cpu_ns = (guint64) (0.2 * 1e6 * 600);
    f->idle_seconds = 2.0;   /* not paused: nothing changed for 2 s */
    char *t = walpe_prof_format_text (r);
    CHECK (strstr (t, "\nNOTE: monitor 0 was paused for 15.8 s of this run (a fullscreen window has "
                      "the focus)"), "the pause is named under the header");
    CHECK (strstr (t, "NOTE: monitor 0") < strstr (t, "== monitor 0"), "... before the monitors");
    CHECK (!strstr (t, "NOTE: monitor 1"), "a monitor that was not paused has no note");
    CHECK (strstr (t, "frames: 850 rendered (28.3/s over the run, 59.8/s while drawing), 850 "
                      "presented, 0 skipped as unchanged"), "both frame rates");
    CHECK (strstr (t, "paused for 15.8 s of the 30.0 s (a fullscreen window has the focus); nothing "
                      "was drawn for 15.8 s in all"), "pause line of the monitor");
    CHECK (strstr (t, "nothing was drawn for 2.0 s of the 10.0 s (nothing changed"), "idle without "
           "a pause");
    CHECK (strstr (t, "frames: 600 rendered (60.0/s over the run, 75.0/s while drawing)"),
           "frame rate of the idle monitor");
    GPtrArray *frows = table_rows (t, 1);
    CHECK (frows->len == 1 && strstr (g_ptr_array_index (frows, 0), "(shared frame: present)") &&
           g_str_has_suffix (g_ptr_array_index (frows, 0), "  *"), "a CPU-bound row is marked: %s",
           frows->len ? (char *) g_ptr_array_index (frows, 0) : "(no row)");
    g_ptr_array_unref (frows);
    GPtrArray *mrows = table_rows (t, 0);
    CHECK (mrows->len > 0 && !g_str_has_suffix (g_ptr_array_index (mrows, 0), "*"), "a GPU-bound "
           "row is not marked");
    g_ptr_array_unref (mrows);
    CHECK (strstr (t, "* 1 row spent a large part of its GPU time on the CPU"), "its footnote");
    CHECK (strstr (t, "Up to 0.200 of the 0.250 ms/f may be waiting"), "its possible waiting");
    CHECK (strstr (t, "TOTAL mon1: GPU 0.250 ms/frame (max 0.000), 1.5 % GPU busy at 60.0 fps (1.9 % "
                      "at 75.0 fps while drawing), at least 0.050 ms of it GPU work (rows marked *)"),
           "total of the CPU-bound monitor");
    CHECK (fabs (walpe_prof_mon_fps_drawing (m) - 849 / 14.2) < 1e-6, "fps while drawing");
    {
        /* a scene that draws one frame a second: 30 runs of one frame, no
         * interval inside a run - its rate while drawing is its rate */
        WalpeProfMon slow = *m;
        slow.frames = 30;
        slow.draw_runs = 30;
        slow.idle_seconds = 29.9;
        CHECK (fabs (walpe_prof_mon_fps_drawing (&slow) - 1.0) < 1e-9, "one frame a second stays "
               "one frame a second: %.2f", walpe_prof_mon_fps_drawing (&slow));
    }
    CHECK (walpe_prof_mon_fps_drawing (&r->mons[2]) == 0.0, "... of a monitor without frames");
    CHECK (fabs (walpe_prof_row_cpu_wait_ms (f, &f->rows[0]) - 0.2) < 1e-9, "waiting of the row");
    CHECK (walpe_prof_row_cpu_wait_ms (m, &m->rows[19]) == 0.0, "a GPU-bound row is not flagged");
    CHECK (fabs (walpe_prof_mon_gpu_ms_min (f) - 0.05) < 1e-9, "lower bound of the monitor");
    /* a row whose CPU time is larger than its GPU time: all of it may be waiting */
    f->rows[0].cpu_ns = (guint64) (0.9 * 1e6 * 600);
    CHECK (fabs (walpe_prof_row_cpu_wait_ms (f, &f->rows[0]) - 0.25) < 1e-9, "waiting is at most "
           "the GPU time");
    CHECK (walpe_prof_mon_gpu_ms_min (f) == 0.0, "lower bound never negative");
    /* no timer: nothing to flag; software GL or --sync: GPU time is wall time anyway */
    f->timer_ok = FALSE;
    CHECK (walpe_prof_row_cpu_wait_ms (f, &f->rows[0]) == 0.0, "no timer, no flag");
    f->timer_ok = TRUE;
    f->wall_timer = TRUE;
    CHECK (walpe_prof_row_cpu_wait_ms (f, &f->rows[0]) == 0.0, "wall-time rows are not flagged");
    f->wall_timer = FALSE;
    char *j = walpe_prof_format_json (r);
    JsonNode *root = json_from_string (j, NULL);
    CHECK (root != NULL, "partial run: the JSON parses");
    if (root) {
        JsonArray *mons = json_object_get_array_member (json_node_get_object (root), "monitors");
        JsonObject *m0 = json_array_get_object_element (mons, 0);
        CHECK (fabs (json_object_get_double_member (m0, "paused_seconds") - 15.8) < 1e-9, "JSON: paused");
        CHECK (fabs (json_object_get_double_member (m0, "idle_seconds") - 15.8) < 1e-9, "JSON: idle");
        CHECK (!g_strcmp0 (json_object_get_string_member (m0, "pause_reason"),
                           "a fullscreen window has the focus"), "JSON: reason");
        CHECK (fabs (json_object_get_double_member (m0, "fps_while_drawing") - 849 / 14.2) < 1e-6,
               "JSON: fps while drawing");
        JsonObject *m1 = json_array_get_object_element (mons, 1);
        JsonObject *r0 = json_array_get_object_element (json_object_get_array_member (m1, "rows"), 0);
        CHECK (fabs (json_object_get_double_member (r0, "gpu_ms_maybe_waiting") - 0.25) < 1e-9,
               "JSON: waiting of the row");
        CHECK (json_object_get_double_member (m1, "gpu_ms_per_frame_min") == 0.0, "JSON: lower bound");
        json_node_unref (root);
    }
    g_free (j);
    g_free (t);
    free_report (r);
}

static void pump (int until_state, double max_s)
{
    gint64 end = g_get_monotonic_time () + (gint64) (max_s * 1e6);
    while (walpe_prof_state () != until_state && g_get_monotonic_time () < end) {
        g_main_context_iteration (NULL, FALSE);
        g_usleep (2000);
    }
}

static int woken;
static void waker (gpointer user)
{
    (void) user;
    woken++;
}

static void fill_info (WalpeProfThreadInfo *info)
{
    memset (info, 0, sizeof (*info));
    info->monitor = 0;
    info->name = "TEST-0";
    info->w = 256;
    info->h = 256;
    info->type = "scene";
    info->path = "/somewhere/wallpapers/1234567890";
    info->role = "";
    info->hz = 60.0;
    info->fx_bytes = 128 * 128 * 4;
    info->fx_count = 1;
}

/* one profiled run of `nframes` frames; returns the JSON root (unref) */
static JsonNode *gl_profile_run (const char *api, GLuint fbo, GLuint src_tex, GLuint rt_tex,
                                 int nframes, guint flags, char **text_out)
{
    const char *why = NULL;
    woken = 0;
    walpe_prof_set_waker (waker, NULL);
    walpe_prof_set_expected (1);
    gboolean ok = walpe_prof_start (1.0, TRUE, flags, &why);
    CHECK (ok, "%s: start: %s", api, why ? why : "?");
    if (!ok)
        return NULL;
    CHECK (woken == 1, "%s: the waker ran at the start", api);
    CHECK (walpe_prof_state () == 1, "%s: running", api);
    CHECK (!walpe_prof_start (1.0, TRUE, 0, &why) && why, "%s: a second start is refused", api);
    WalpeProfThreadInfo info;
    fill_info (&info);
    walpe_prof_thread_sync (&info);   /* attach */

    gboolean has_debug = epoxy_has_gl_extension ("GL_KHR_debug") ||
                         (epoxy_is_desktop_gl () ? epoxy_gl_version () >= 43 : epoxy_gl_version () >= 32);
    const float ident[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    const float half[9] = { 0.5f, 0, 0, 0, 0.5f, 0, 0, 0, 1 };
    static guint8 pix[16 * 16 * 4];
    for (int f = 0; f < nframes; f++) {
        gboolean prof = WALPE_PROF_ON ();
        CHECK (prof, "%s: the profile is on in frame %d", api, f);
        walpe_prof_frame_begin (0, 256, 256);
        WALPE_PROF_STAGE (WALPE_PROF_ST_UPDATE);
        glBindFramebuffer (GL_FRAMEBUFFER, fbo);
        glViewport (0, 0, 256, 256);
        glClearColor (0, 0, 0, 1);
        glClear (GL_COLOR_BUFFER_BIT);
        /* a per-frame upload, as a video texture does */
        glBindTexture (GL_TEXTURE_2D, src_tex);
        glTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, pix);
        /* ... and what a video frame draws there (its colour
         * conversion): a row of its own, not the upload's */
        glViewport (0, 0, 64, 64);
        walpe_glutil_draw_quad (src_tex, FALSE, 1.0f, ident, WALPE_BLEND_NONE);
        glViewport (0, 0, 256, 256);
        if (f == 1)
            walpe_prof_note_pause (0, TRUE, "test pause");   /* paused from frame 1 ... */
        if (f == 2) {
            walpe_prof_note_pause (0, TRUE, "test pause");   /* (said twice: one interval) */
            walpe_prof_note_pause (0, FALSE, "");            /* ... to frame 2 */
        }
        WALPE_PROF_STAGE (WALPE_PROF_ST_LAYERS);
        WALPE_PROF_LAYER_BEGIN (0, "Background", "image");
        walpe_glutil_draw_quad (src_tex, FALSE, 1.0f, ident, WALPE_BLEND_NONE);
        WALPE_PROF_LAYER_END ();
        WALPE_PROF_LAYER_BEGIN (1, "Girl \xe9\x9b\xa8", "image");
        /* the layer-colour pass: no effect / pass number, here of the
         * frame's size - still a row of its own, not the layer's */
        WALPE_PROF_PASS_BEGIN ("(layer colour)", -1, -1, 256, 256);
        WALPE_PROF_PASS_END ();
        WALPE_PROF_PASS_BEGIN ("blur", 0, 0, 128, 128);
        glViewport (0, 0, 128, 128);
        walpe_glutil_draw_quad (src_tex, FALSE, 1.0f, ident, WALPE_BLEND_NONE);
        WALPE_PROF_PASS_END ();
        WALPE_PROF_PASS_BEGIN ("blur", 0, 1, 128, 128);
        walpe_glutil_draw_quad (src_tex, FALSE, 1.0f, ident, WALPE_BLEND_NONE);
        walpe_glutil_draw_quad (src_tex, FALSE, 1.0f, ident, WALPE_BLEND_NONE);
        WALPE_PROF_PASS_END ();
        glViewport (0, 0, 256, 256);
        walpe_glutil_draw_quad (src_tex, FALSE, 0.5f, half, WALPE_BLEND_NORMAL);
        /* no LAYER_END: the next layer (or the stage) must close it */
        WALPE_PROF_LAYER_BEGIN (2, NULL, "particles");
        WALPE_PROF_LAYER_END ();
        WALPE_PROF_STAGE (WALPE_PROF_ST_POST);
        if (has_debug && f < 3) {
            char msg[160];
            g_snprintf (msg, sizeof (msg), "Buffer performance warning: Buffer object %d is being "
                        "copied/moved from VIDEO memory to HOST memory.", 3 + f * 7);
            glDebugMessageInsert (GL_DEBUG_SOURCE_APPLICATION, GL_DEBUG_TYPE_PERFORMANCE, 131186,
                                  GL_DEBUG_SEVERITY_MEDIUM, -1, msg);
        }
        WALPE_PROF_STAGE (WALPE_PROF_ST_SWAP);
        glFlush ();
        walpe_prof_frame_end (TRUE);
    }
    /* a wake-up that draws nothing and presents nothing: skipped, no frame */
    walpe_prof_frame_begin (0, 256, 256);
    WALPE_PROF_FRAME_SKIPPED (0);
    walpe_prof_frame_end (FALSE);
    glBindFramebuffer (GL_FRAMEBUFFER, 0);
    (void) rt_tex;

    pump (2, 10.0);   /* the run's second is over: collecting */
    CHECK (walpe_prof_state () == 2, "%s: collecting after the run (state %d)", api, walpe_prof_state ());
    CHECK (woken == 2, "%s: the waker ran at the end", api);
    /* no new frame is taken while collecting (a frame in progress would
     * still end normally: the hooks stay live until the report is built) */
    walpe_prof_frame_begin (0, 256, 256);
    WALPE_PROF_LAYER_BEGIN (7, "late layer", "image");
    walpe_glutil_draw_quad (src_tex, FALSE, 1.0f, ident, WALPE_BLEND_NONE);
    WALPE_PROF_LAYER_END ();
    walpe_prof_frame_end (TRUE);
    fill_info (&info);
    info.fx_cached = 5;
    walpe_prof_thread_sync (&info);   /* hand in */
    pump (3, 10.0);
    CHECK (walpe_prof_state () == 3, "%s: report ready (state %d)", api, walpe_prof_state ());
    CHECK (!WALPE_PROF_ON (), "%s: hooks are off once the report is ready", api);
    char *json = walpe_prof_last_json ();
    char *text = walpe_prof_last_text ();
    CHECK (json && text, "%s: text and JSON exist", api);
    const char *tp = walpe_prof_last_text_path (), *jp = walpe_prof_last_json_path ();
    CHECK (tp && g_file_test (tp, G_FILE_TEST_EXISTS), "%s: text file written", api);
    CHECK (jp && g_file_test (jp, G_FILE_TEST_EXISTS), "%s: JSON file written", api);
    if (tp) {
        char *dir = g_path_get_dirname (tp);
        char *want = g_build_filename (g_get_user_cache_dir (), "walpe", NULL);
        CHECK (!strcmp (dir, want), "%s: written under ~/.cache/walpe: %s", api, dir);
        char *latest = g_build_filename (want, "gpu-profile.txt", NULL);
        CHECK (g_file_test (latest, G_FILE_TEST_EXISTS), "%s: gpu-profile.txt (newest) written", api);
        g_free (latest);
        g_free (dir);
        g_free (want);
    }
    JsonNode *root = json ? json_from_string (json, NULL) : NULL;
    CHECK (root != NULL, "%s: the JSON parses", api);
    g_free (json);
    if (text_out)
        *text_out = text;
    else
        g_free (text);
    return root;
}

static JsonObject *find_row (JsonArray *rows, const char *layer, const char *effect, int pass)
{
    for (guint i = 0; i < json_array_get_length (rows); i++) {
        JsonObject *o = json_array_get_object_element (rows, i);
        if (!g_strcmp0 (json_object_get_string_member (o, "layer"), layer) &&
            !g_strcmp0 (json_object_get_string_member (o, "effect"), effect) &&
            json_object_get_int_member (o, "pass") == pass)
            return o;
    }
    return NULL;
}

static void gl_checks (const char *api)
{
    gboolean es = !epoxy_is_desktop_gl ();
    int ver = epoxy_gl_version ();
    printf ("%s: %s, version %d, %s\n", api, (const char *) glGetString (GL_RENDERER), ver,
            (const char *) glGetString (GL_VERSION));
    gboolean expect_timer = es ? epoxy_has_gl_extension ("GL_EXT_disjoint_timer_query")
                               : (ver >= 33 || epoxy_has_gl_extension ("GL_ARB_timer_query"));
    gboolean has_debug = epoxy_has_gl_extension ("GL_KHR_debug") || (es ? ver >= 32 : ver >= 43);

    walpe_glutil_init ();
    GLuint src = 0, rt = 0, fbo = 0;
    static guint8 pix[16 * 16 * 4];
    memset (pix, 200, sizeof (pix));
    glGenTextures (1, &src);
    glBindTexture (GL_TEXTURE_2D, src);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, pix);
    glGenTextures (1, &rt);
    glBindTexture (GL_TEXTURE_2D, rt);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, 256, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers (1, &fbo);
    glBindFramebuffer (GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
    CHECK (glCheckFramebufferStatus (GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "%s: FBO", api);
    glBindFramebuffer (GL_FRAMEBUFFER, 0);
    /* every wrapped entry point resolved by libepoxy before the run, so
     * that "restored" below compares against a settled value */
    const float ident[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    glBindFramebuffer (GL_FRAMEBUFFER, fbo);
    walpe_glutil_draw_quad (src, FALSE, 1.0f, ident, WALPE_BLEND_NONE);
    glBindFramebuffer (GL_FRAMEBUFFER, 0);
    while (glGetError () != GL_NO_ERROR)
        ;

    /* hooks while no run is active: nothing happens */
    CHECK (!WALPE_PROF_ON (), "%s: off before the first run", api);
    int state0 = walpe_prof_state ();
    walpe_prof_frame_begin (0, 256, 256);
    walpe_prof_layer_begin (0, "x", "image");
    walpe_prof_pass_begin ("blur", 0, 0, 8, 8);
    walpe_prof_pass_end ();
    walpe_prof_layer_end ();
    walpe_prof_quad (ident);
    walpe_prof_stage (WALPE_PROF_ST_SWAP);
    walpe_prof_frame_end (TRUE);
    WalpeProfThreadInfo info;
    fill_info (&info);
    walpe_prof_thread_sync (&info);
    CHECK (walpe_prof_state () == state0, "%s: hooks without a run change nothing", api);
    GLint cur = -1;
    glGetQueryiv (GL_TIME_ELAPSED, GL_CURRENT_QUERY, &cur);
    if (expect_timer)
        CHECK (cur == 0, "%s: no timer query was begun without a run", api);
    while (glGetError () != GL_NO_ERROR)
        ;

    void (*draw_before) (GLenum, GLint, GLsizei) = epoxy_glDrawArrays;
    void (*sub_before) (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum,
                        const void *) = epoxy_glTexSubImage2D;
    gboolean dbg_before = has_debug ? glIsEnabled (GL_DEBUG_OUTPUT) : FALSE;

    const int NF = 12;
    char *text = NULL;
    JsonNode *root = gl_profile_run (api, fbo, src, rt, NF, 0, &text);
    if (root) {
        JsonObject *o = json_node_get_object (root);
        JsonArray *mons = json_object_get_array_member (o, "monitors");
        CHECK (json_array_get_length (mons) == 1, "%s: one monitor in the report", api);
        JsonObject *m = json_array_get_object_element (mons, 0);
        CHECK (json_object_get_boolean_member (m, "attached"), "%s: attached", api);
        CHECK (!g_strcmp0 (json_object_get_string_member (m, "wallpaper"), "1234567890"),
               "%s: workshop id kept: %s", api, json_object_get_string_member (m, "wallpaper"));
        CHECK (json_object_get_boolean_member (m, "gl_es") == es, "%s: API recorded", api);
        CHECK (json_object_get_int_member (m, "frames") == NF, "%s: %d frames, got %d", api, NF,
               (int) json_object_get_int_member (m, "frames"));
        CHECK (json_object_get_int_member (m, "presented") == NF, "%s: presented", api);
        CHECK (json_object_get_int_member (m, "skipped_unchanged") == 1, "%s: skipped", api);
        gboolean timer_ok = json_object_get_boolean_member (m, "timer_ok");
        printf ("%s: timer: %s | debug: %s\n", api, json_object_get_string_member (m, "timer"),
                json_object_get_string_member (m, "debug"));
        CHECK (timer_ok == expect_timer, "%s: timer queries %s", api,
               expect_timer ? "expected" : "not expected");
        JsonArray *rows = json_object_get_array_member (m, "rows");
        JsonObject *bg = find_row (rows, "Background", "", -1);
        JsonObject *girl = find_row (rows, "Girl \xe9\x9b\xa8", "", -1);
        JsonObject *p0 = find_row (rows, "Girl \xe9\x9b\xa8", "blur", 0);
        JsonObject *p1 = find_row (rows, "Girl \xe9\x9b\xa8", "blur", 1);
        JsonObject *l2 = find_row (rows, "layer 2", "", -1);
        CHECK (bg && girl && p0 && p1 && l2, "%s: rows of the layers and passes (%p %p %p %p %p)", api,
               (void *) bg, (void *) girl, (void *) p0, (void *) p1, (void *) l2);
        CHECK (!find_row (rows, "late layer", "", -1), "%s: a frame begun after the end of the run "
               "is not in the report", api);
        JsonObject *tint = find_row (rows, "Girl \xe9\x9b\xa8", "(layer colour)", -1);
        /* the update stage: uploads in one row, what is drawn in another */
        JsonObject *upl = find_row (rows, "(scene update, uploads)", "", -1);
        JsonObject *upd = find_row (rows, "(scene update: draws)", "", -1);
        CHECK (upl && upd, "%s: the update stage has an upload row and a draw row", api);
        if (upl && upd) {
            CHECK (json_object_get_double_member (upl, "draws_per_frame") == 0.0, "%s: no draw in "
                   "the upload row: %.2f", api, json_object_get_double_member (upl, "draws_per_frame"));
            CHECK (fabs (json_object_get_double_member (upd, "draws_per_frame") - 1.0) < 1e-9,
                   "%s: the update stage's draw: %.2f", api,
                   json_object_get_double_member (upd, "draws_per_frame"));
            CHECK (json_object_get_int_member (upd, "target_w") == 64 &&
                   json_object_get_int_member (upd, "target_h") == 64, "%s: its target is the "
                   "viewport it was drawn with", api);
            CHECK (fabs (json_object_get_double_member (upd, "pixels_per_frame") - 64.0 * 64.0) < 1.0,
                   "%s: its pixels: %.0f", api, json_object_get_double_member (upd, "pixels_per_frame"));
            CHECK (fabs (json_object_get_double_member (upd, "runs_per_frame") - 1.0) < 1e-9 &&
                   fabs (json_object_get_double_member (upl, "runs_per_frame") - 1.0) < 1e-9,
                   "%s: each of the two rows once per frame", api);
        }
        CHECK (json_object_get_double_member (m, "paused_seconds") > 0.0 &&
               json_object_get_double_member (m, "paused_seconds") < 1.0, "%s: the pause between "
               "frame 1 and 2 was measured: %.4f s", api,
               json_object_get_double_member (m, "paused_seconds"));
        CHECK (!g_strcmp0 (json_object_get_string_member (m, "pause_reason"), "test pause"),
               "%s: its reason, once: %s", api, json_object_get_string_member (m, "pause_reason"));
        CHECK (tint != NULL, "%s: the layer-colour pass has its own row", api);
        if (tint)
            CHECK (fabs (json_object_get_double_member (tint, "pixels_per_frame") - 256.0 * 256.0) < 1.0 &&
                   json_object_get_double_member (tint, "draws_per_frame") == 0.0,
                   "%s: ... with its own pixels and no draw of the layer's", api);
        if (bg && girl && p0 && p1 && l2) {
            CHECK (json_object_get_int_member (p0, "target_w") == 128, "%s: pass target size", api);
            CHECK (fabs (json_object_get_double_member (p0, "runs_per_frame") - 1.0) < 1e-9,
                   "%s: one run per frame", api);
            CHECK (fabs (json_object_get_double_member (p0, "pixels_per_frame") - 128.0 * 128.0) < 1.0,
                   "%s: pass pixels = target area: %.0f", api,
                   json_object_get_double_member (p0, "pixels_per_frame"));
            CHECK (fabs (json_object_get_double_member (bg, "pixels_per_frame") - 256.0 * 256.0) < 1.0,
                   "%s: layer pixels = its quad: %.0f", api,
                   json_object_get_double_member (bg, "pixels_per_frame"));
            CHECK (fabs (json_object_get_double_member (girl, "pixels_per_frame") - 128.0 * 128.0) < 1.0,
                   "%s: half quad pixels: %.0f", api,
                   json_object_get_double_member (girl, "pixels_per_frame"));
            /* the counting wrappers attribute draws to the open row */
            CHECK (fabs (json_object_get_double_member (bg, "draws_per_frame") - 1.0) < 1e-9,
                   "%s: draws of the background: %.2f", api,
                   json_object_get_double_member (bg, "draws_per_frame"));
            CHECK (fabs (json_object_get_double_member (p1, "draws_per_frame") - 2.0) < 1e-9,
                   "%s: draws of pass 1: %.2f", api, json_object_get_double_member (p1, "draws_per_frame"));
            CHECK (fabs (json_object_get_double_member (girl, "draws_per_frame") - 1.0) < 1e-9,
                   "%s: draws of the layer itself: %.2f", api,
                   json_object_get_double_member (girl, "draws_per_frame"));
            CHECK (json_object_get_double_member (l2, "draws_per_frame") == 0.0,
                   "%s: the unclosed layer did not leak into the next", api);
            if (timer_ok) {
                CHECK (json_object_get_int_member (m, "gpu_frames") == NF, "%s: GPU time of every "
                       "frame read back: %d", api, (int) json_object_get_int_member (m, "gpu_frames"));
                double sum = 0;
                for (guint i = 0; i < json_array_get_length (rows); i++)
                    sum += json_object_get_double_member (json_array_get_object_element (rows, i),
                                                          "gpu_ms_per_frame");
                double tot = json_object_get_double_member (m, "gpu_ms_per_frame");
                CHECK (tot > 0.0, "%s: GPU time measured: %.6f ms/frame", api, tot);
                CHECK (fabs (sum - tot) <= tot * 1e-6 + 1e-9, "%s: rows add up to the frame (%.6f / "
                       "%.6f)", api, sum, tot);
                CHECK (json_object_get_double_member (p1, "gpu_ms_per_frame") > 0.0 ||
                       json_object_get_double_member (bg, "gpu_ms_per_frame") > 0.0,
                       "%s: a row has GPU time", api);
                printf ("%s: GPU %.4f ms/frame; background %.4f, pass0 %.4f, pass1 %.4f\n", api, tot,
                        json_object_get_double_member (bg, "gpu_ms_per_frame"),
                        json_object_get_double_member (p0, "gpu_ms_per_frame"),
                        json_object_get_double_member (p1, "gpu_ms_per_frame"));
            }
            CHECK (json_object_get_double_member (bg, "cpu_ms_per_frame") > 0.0, "%s: CPU time of a "
                   "row", api);
        }
        JsonObject *pf = json_object_get_object_member (m, "per_frame");
        CHECK (json_object_get_boolean_member (m, "counters_ok"), "%s: counters available", api);
        CHECK (fabs (json_object_get_double_member (pf, "passes") - 3.0) < 1e-9, "%s: passes/frame", api);
        CHECK (fabs (json_object_get_double_member (pf, "draws") - 6.0) < 1e-9, "%s: 6 draws per "
               "frame, got %.2f", api, json_object_get_double_member (pf, "draws"));
        CHECK (fabs (json_object_get_double_member (pf, "tex_upload_bytes") - 16.0 * 16 * 4) < 1e-9,
               "%s: 1024 bytes uploaded per frame, got %.1f", api,
               json_object_get_double_member (pf, "tex_upload_bytes"));
        CHECK (fabs (json_object_get_double_member (pf, "clears") - 1.0) < 1e-9, "%s: clears", api);
        CHECK (fabs (json_object_get_double_member (pf, "flushes") - 1.0) < 1e-9, "%s: flushes", api);
        CHECK (json_object_get_double_member (pf, "target_switches") >= 1.0, "%s: target switches", api);
        CHECK (fabs (json_object_get_double_member (pf, "passes_cached") - 5.0 / NF) < 1e-9,
               "%s: cached passes from the thread's totals", api);
        JsonObject *stg = json_object_get_object_member (m, "stages_ms_per_frame");
        CHECK (json_object_get_double_member (json_object_get_object_member (stg, "layers"), "wall") > 0,
               "%s: stage time", api);
        JsonObject *mem = json_object_get_object_member (o, "memory");
        CHECK (json_object_get_int_member (json_object_get_object_member (mem, "effect_targets"),
                                           "bytes") == 128 * 128 * 4, "%s: effect targets", api);
        CHECK (json_object_get_int_member (json_object_get_object_member (mem, "window_buffers_est"),
                                           "bytes") == 256 * 256 * 8, "%s: window buffers", api);
        if (!es) {
            gint64 other = json_object_get_int_member (
                json_object_get_object_member (mem, "other_targets"), "bytes");
            gint64 plain = json_object_get_int_member (
                json_object_get_object_member (mem, "other_textures"), "bytes");
            /* the 256x256 target (minus what "effect targets" claims) and
             * the 16x16 source texture */
            CHECK (other == 256 * 256 * 4 - 128 * 128 * 4, "%s: render target found: %" G_GINT64_FORMAT,
                   api, other);
            CHECK (plain >= 16 * 16 * 4, "%s: plain textures found: %" G_GINT64_FORMAT, api, plain);
            CHECK (json_object_get_int_member (json_object_get_object_member (mem, "buffers"),
                                               "count") >= 1, "%s: buffers found", api);
        }
        JsonArray *msgs = json_object_get_array_member (o, "driver_messages");
        if (has_debug) {
            guint perf = 0, count = 0;
            for (guint i = 0; i < json_array_get_length (msgs); i++) {
                JsonObject *g = json_array_get_object_element (msgs, i);
                if (!g_strcmp0 (json_object_get_string_member (g, "type"), "performance") &&
                    strstr (json_object_get_string_member (g, "text"), "VIDEO memory to HOST")) {
                    perf++;
                    count = (guint) json_object_get_int_member (g, "count");
                }
            }
            CHECK (perf == 1 && count == 3, "%s: three injected performance messages are one entry "
                   "with count 3 (%u entries, count %u)", api, perf, count);
            CHECK (strstr (text, "[performance] x3 mon0 application/medium id 131186: Buffer "
                                 "performance warning"), "%s: the message is in the text", api);
        }
        CHECK (strstr (text, "TOTAL mon0:") && strstr (text, "\nTOTAL: "), "%s: totals in the text", api);
        if (g_getenv ("WALPE_TEST_VERBOSE"))
            printf ("---- %s report ----\n%s----\n", api, text);
        CHECK (strstr (text, "Girl \xe9\x9b\xa8"), "%s: layer name in the text", api);
        CHECK (!strstr (text, "/somewhere"), "%s: no path in the report", api);
        json_node_unref (root);
    }
    g_free (text);

    /* the context is as it was */
    CHECK (epoxy_glDrawArrays == draw_before, "%s: glDrawArrays pointer restored", api);
    CHECK (epoxy_glTexSubImage2D == sub_before, "%s: glTexSubImage2D pointer restored", api);
    if (has_debug)
        CHECK (glIsEnabled (GL_DEBUG_OUTPUT) == dbg_before, "%s: debug output as before", api);
    if (expect_timer) {
        cur = -1;
        glGetQueryiv (GL_TIME_ELAPSED, GL_CURRENT_QUERY, &cur);
        CHECK (cur == 0, "%s: no timer query left active", api);
    }
    GLenum e = glGetError ();
    CHECK (e == GL_NO_ERROR, "%s: no GL error left behind (0x%x)", api, e);
    CHECK (!WALPE_PROF_ON (), "%s: off after the run", api);

    /* the same without timer queries: CPU time, same counters */
    g_setenv ("WALPE_GPU_PROFILE_TIMER", "0", TRUE);
    text = NULL;
    root = gl_profile_run (api, fbo, src, rt, 6, 0, &text);
    g_unsetenv ("WALPE_GPU_PROFILE_TIMER");
    if (root) {
        JsonObject *m = json_array_get_object_element (
            json_object_get_array_member (json_node_get_object (root), "monitors"), 0);
        CHECK (!json_object_get_boolean_member (m, "timer_ok"), "%s: fallback: no timer", api);
        CHECK (json_object_get_int_member (m, "gpu_frames") == 0, "%s: fallback: no GPU frames", api);
        CHECK (json_object_get_double_member (m, "gpu_ms_per_frame") == 0.0, "%s: fallback: 0 ms", api);
        CHECK (fabs (json_object_get_double_member (json_object_get_object_member (m, "per_frame"),
                                                    "draws") - 6.0) < 1e-9, "%s: fallback: draws", api);
        CHECK (strstr (text, "sorted by CPU time"), "%s: fallback table", api);
        CHECK (strstr (text, "TOTAL: GPU time n/a"), "%s: fallback total", api);
        json_node_unref (root);
    }
    g_free (text);

    /* sync mode: glFinish at the row boundaries, every row with draws has
     * time, the renderer's own glFinish count stays what it is (0) */
    text = NULL;
    root = gl_profile_run (api, fbo, src, rt, 6, WALPE_PROF_SYNC, &text);
    if (root) {
        JsonObject *m = json_array_get_object_element (
            json_object_get_array_member (json_node_get_object (root), "monitors"), 0);
        JsonArray *rows = json_object_get_array_member (m, "rows");
        CHECK (json_object_get_boolean_member (m, "timer_ok"), "%s: sync: GPU time available", api);
        CHECK (strstr (json_object_get_string_member (m, "timer"), "sync mode"), "%s: sync: said so", api);
        CHECK (json_object_get_int_member (m, "gpu_frames") == 6, "%s: sync: every frame timed", api);
        const char *lay[] = { "Background", "Girl \xe9\x9b\xa8", "Girl \xe9\x9b\xa8", "Girl \xe9\x9b\xa8" };
        const char *eff[] = { "", "", "blur", "blur" };
        const int pas[] = { -1, -1, 0, 1 };
        double sum = 0;
        for (int i = 0; i < 4; i++) {
            JsonObject *w = find_row (rows, lay[i], eff[i], pas[i]);
            CHECK (w && json_object_get_double_member (w, "gpu_ms_per_frame") > 0.0,
                   "%s: sync: row %d has time", api, i);
        }
        for (guint i = 0; i < json_array_get_length (rows); i++)
            sum += json_object_get_double_member (json_array_get_object_element (rows, i),
                                                  "gpu_ms_per_frame");
        double tot = json_object_get_double_member (m, "gpu_ms_per_frame");
        CHECK (tot > 0 && fabs (sum - tot) <= tot * 1e-6 + 1e-9, "%s: sync: rows add up (%.6f / %.6f)",
               api, sum, tot);
        CHECK (json_object_get_double_member (json_object_get_object_member (m, "per_frame"),
                                              "finishes") == 0.0, "%s: sync: its glFinish calls are "
               "not counted as the renderer's", api);
        if (g_getenv ("WALPE_TEST_VERBOSE"))
            printf ("---- %s sync report ----\n%s----\n", api, text);
        json_node_unref (root);
    }
    g_free (text);

    /* and without the counting wrappers */
    g_setenv ("WALPE_GPU_PROFILE_HOOKS", "0", TRUE);
    root = gl_profile_run (api, fbo, src, rt, 4, 0, NULL);
    g_unsetenv ("WALPE_GPU_PROFILE_HOOKS");
    if (root) {
        JsonObject *m = json_array_get_object_element (
            json_object_get_array_member (json_node_get_object (root), "monitors"), 0);
        CHECK (!json_object_get_boolean_member (m, "counters_ok"), "%s: no wrappers on request", api);
        CHECK (json_object_get_double_member (json_object_get_object_member (m, "per_frame"),
                                              "draws") == 0.0, "%s: no draw counts then", api);
        CHECK (fabs (json_object_get_double_member (json_object_get_object_member (m, "per_frame"),
                                                    "passes") - 3.0) < 1e-9, "%s: passes still", api);
        json_node_unref (root);
    }
    walpe_prof_thread_exit ();
    e = glGetError ();
    CHECK (e == GL_NO_ERROR, "%s: no GL error at the end (0x%x)", api, e);
    glDeleteFramebuffers (1, &fbo);
    glDeleteTextures (1, &src);
    glDeleteTextures (1, &rt);
}

static gboolean gl_run (EGLDisplay dpy, gboolean es)
{
    const char *api = es ? "GLES 3" : "GL 3.3";
    EGLint maj, min, nc = 0;
    EGLConfig cfg;
    const EGLint attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
                            es ? EGL_OPENGL_ES3_BIT : EGL_OPENGL_BIT, EGL_NONE };
    if (dpy == EGL_NO_DISPLAY || !eglInitialize (dpy, &maj, &min) ||
        !eglBindAPI (es ? EGL_OPENGL_ES_API : EGL_OPENGL_API) ||
        !eglChooseConfig (dpy, attr, &cfg, 1, &nc) || nc < 1) {
        printf ("skip %s checks (no usable config)\n", api);
        return FALSE;
    }
    const EGLint ca_gl[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
                             EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                             EGL_NONE };
    const EGLint ca_es[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE };
    EGLContext ctx = eglCreateContext (dpy, cfg, EGL_NO_CONTEXT, es ? ca_es : ca_gl);
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent (dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        printf ("skip %s checks (no surfaceless context)\n", api);
        if (ctx != EGL_NO_CONTEXT)
            eglDestroyContext (dpy, ctx);
        return FALSE;
    }
    gl_checks (api);
    eglMakeCurrent (dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext (dpy, ctx);
    return TRUE;
}

typedef struct { EGLDisplay dpy; gboolean es, ran; } GlRun;

static gpointer gl_thread (gpointer data)
{
    GlRun *r = data;
    r->ran = gl_run (r->dpy, r->es);
    eglReleaseThread ();
    return NULL;
}

static void test_gl (gboolean es)
{
    EGLDisplay dpy = EGL_NO_DISPLAY;
    if (epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_MESA_platform_surfaceless"))
        dpy = eglGetPlatformDisplayEXT (EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    if (dpy == EGL_NO_DISPLAY && epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_EXT_device_enumeration") &&
        epoxy_has_egl_extension (EGL_NO_DISPLAY, "EGL_EXT_platform_device")) {
        EGLDeviceEXT dev[8];
        EGLint nd = 0;
        if (eglQueryDevicesEXT (8, dev, &nd) && nd > 0)
            dpy = eglGetPlatformDisplayEXT (EGL_PLATFORM_DEVICE_EXT, dev[0], NULL);
    }
    if (dpy == EGL_NO_DISPLAY) {
        printf ("skip GL checks (no EGL device / surfaceless platform)\n");
        return;
    }
    /* a render thread of its own, as in the daemon (glutil and the
     * profiler keep their per-context state in thread-local storage) */
    GlRun r = { dpy, es, FALSE };
    g_thread_join (g_thread_new (es ? "gles" : "gl", gl_thread, &r));
    eglTerminate (dpy);
}

int main (int argc, char **argv)
{
    /* "--gles": only the GLES part. It runs in a process of its own: one
     * process uses one GL API (libepoxy and glutil resolve and cache per
     * process), as the daemon does. */
    gboolean only_es = argc > 1 && !strcmp (argv[1], "--gles");
    /* reports go to a private cache dir, not the user's */
    char *tmp = g_dir_make_tmp ("walpe-gpuprof-XXXXXX", NULL);
    g_setenv ("XDG_CACHE_HOME", tmp, TRUE);
    g_unsetenv ("WALPE_GPU_PROFILE_TIMER");
    g_unsetenv ("WALPE_GPU_PROFILE_HOOKS");
    g_unsetenv ("WALPE_GPU_PROFILE_EVERY");
    g_unsetenv ("WALPE_GPU_PROFILE_SYNC");

    if (only_es) {
        test_gl (TRUE);
    } else {
        test_helpers ();
        test_messages ();
        test_format_text ();
        test_partial_run ();
        test_format_fallback ();
        test_format_json ();
        test_gl (FALSE);
        fflush (stdout);
        char *child[] = { argv[0], (char *) "--gles", NULL };
        int status = -1;
        GError *err = NULL;
        if (!g_spawn_sync (NULL, child, NULL, G_SPAWN_CHILD_INHERITS_STDIN, NULL, NULL, NULL, NULL,
                           &status, &err)) {
            CHECK (FALSE, "GLES part did not start: %s", err ? err->message : "?");
            g_clear_error (&err);
        } else {
            CHECK (g_spawn_check_wait_status (status, NULL), "GLES part failed (status %d)", status);
        }
    }

    /* remove what the runs wrote */
    char *dir = g_build_filename (tmp, "walpe", NULL);
    GDir *d = g_dir_open (dir, 0, NULL);
    if (d) {
        const char *n;
        while ((n = g_dir_read_name (d))) {
            char *p = g_build_filename (dir, n, NULL);
            remove (p);
            g_free (p);
        }
        g_dir_close (d);
    }
    remove (dir);
    remove (tmp);
    g_free (dir);
    g_free (tmp);

    if (fails) {
        printf ("%d check(s) FAILED%s\n", fails, only_es ? " (GLES part)" : "");
        return 1;
    }
    if (!only_es)
        printf ("gpuprof: all checks passed\n");
    return 0;
}
