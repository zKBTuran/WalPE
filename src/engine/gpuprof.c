/* gpuprof.c - opt-in GPU profiler (see gpuprof.h).
 *
 * Layout of this file:
 *   1. pure helpers and the report formatting (no GL; tests/gpuprof_test.c)
 *   2. per-thread state: rows, timer queries, stages
 *   3. interposer: counting wrappers in libepoxy's dispatch pointers
 *   4. KHR_debug channel
 *   5. memory snapshot
 *   6. hooks
 *   7. control: start / stop / collect / files
 */
#include "walpe-build-config.h"
#include "gpuprof.h"
#include "glutil.h"   /* walpe_gl_is_software */
#include <epoxy/gl.h>
#include <epoxy/egl.h>
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <dlfcn.h>
#include <math.h>
#include <string.h>
#include <time.h>

int walpe_prof_enabled_ = 0;

/* ===================================================== 1. pure helpers */

/* copy into a fixed buffer: valid UTF-8, no control characters, cut at a
 * character boundary */
static void prof_copy (char *dst, gsize size, const char *src)
{
    if (!size)
        return;
    dst[0] = 0;
    if (!src || !*src)
        return;
    char *valid = g_utf8_make_valid (src, -1);
    gsize n = strlen (valid);
    if (n > size - 1) {
        n = size - 1;
        /* not in the middle of a character */
        while (n > 0 && ((guchar) valid[n] & 0xc0) == 0x80)
            n--;
    }
    memcpy (dst, valid, n);
    dst[n] = 0;
    for (char *p = dst; *p; p++)
        if ((guchar) *p < 0x20 || *p == 0x7f)
            *p = ' ';
    g_free (valid);
}

void walpe_prof_effect_label (char *out, gsize size, const char *effect_file)
{
    if (!size)
        return;
    out[0] = 0;
    if (!effect_file || !*effect_file) {
        prof_copy (out, size, "effect");
        return;
    }
    /* ".../<name>/effect.json" -> <name>; anything else: its last element
     * without the extension */
    char *dir = g_path_get_dirname (effect_file);
    char *base = g_path_get_basename (effect_file);
    if (!g_ascii_strcasecmp (base, "effect.json") && strcmp (dir, ".") && strcmp (dir, "/")) {
        char *name = g_path_get_basename (dir);
        prof_copy (out, size, name);
        g_free (name);
    } else {
        char *dot = strrchr (base, '.');
        if (dot && dot != base)
            *dot = 0;
        prof_copy (out, size, base);
    }
    g_free (dir);
    g_free (base);
    if (!out[0])
        prof_copy (out, size, "effect");
}

double walpe_prof_quad_pixels (const float *mvp, int w, int h)
{
    if (!mvp || w < 1 || h < 1)
        return 0.0;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (int k = 0; k < 4; k++) {
        float qx = (k & 1) ? 1.0f : -1.0f, qy = (k & 2) ? 1.0f : -1.0f;
        float cx = mvp[0] * qx + mvp[3] * qy + mvp[6];
        float cy = mvp[1] * qx + mvp[4] * qy + mvp[7];
        if (!isfinite (cx) || !isfinite (cy))
            return 0.0;
        x0 = fminf (x0, cx); x1 = fmaxf (x1, cx);
        y0 = fminf (y0, cy); y1 = fmaxf (y1, cy);
    }
    x0 = fmaxf (x0, -1.0f); y0 = fmaxf (y0, -1.0f);
    x1 = fminf (x1, 1.0f);  y1 = fminf (y1, 1.0f);
    if (x1 <= x0 || y1 <= y0)
        return 0.0;
    return (double) (x1 - x0) * 0.5 * w * (double) (y1 - y0) * 0.5 * h;
}

gsize walpe_prof_image_bytes (guint format, guint type, int w, int h)
{
    if (w < 1 || h < 1)
        return 0;
    gsize comps;
    switch (format) {
    case GL_RED: case GL_RED_INTEGER: case GL_ALPHA: case GL_LUMINANCE:
    case GL_DEPTH_COMPONENT:
        comps = 1; break;
    case GL_RG: case GL_RG_INTEGER: case GL_LUMINANCE_ALPHA:
        comps = 2; break;
    case GL_RGB: case GL_BGR: case GL_RGB_INTEGER:
        comps = 3; break;
    default: /* GL_RGBA, GL_BGRA, ... */
        comps = 4; break;
    }
    gsize per;   /* bytes per pixel */
    switch (type) {
    case GL_UNSIGNED_BYTE: case GL_BYTE:
        per = comps; break;
    case GL_UNSIGNED_SHORT: case GL_SHORT: case GL_HALF_FLOAT:
        per = comps * 2; break;
    case GL_UNSIGNED_INT: case GL_INT: case GL_FLOAT:
        per = comps * 4; break;
    case GL_UNSIGNED_SHORT_5_6_5: case GL_UNSIGNED_SHORT_4_4_4_4:
    case GL_UNSIGNED_SHORT_5_5_5_1:
        per = 2; break;
    case GL_UNSIGNED_INT_8_8_8_8: case GL_UNSIGNED_INT_8_8_8_8_REV:
    case GL_UNSIGNED_INT_2_10_10_10_REV: case GL_UNSIGNED_INT_24_8:
        per = 4; break;
    default:
        per = comps; break;
    }
    return (gsize) w * (gsize) h * per;
}

char *walpe_prof_msg_key (guint source, guint type, guint id, const char *text)
{
    GString *k = g_string_new (NULL);
    g_string_append_printf (k, "%x/%x/%u/", source, type, id);
    gboolean in_num = FALSE;
    for (const char *p = text ? text : ""; *p; p++) {
        /* a run of digits (also hexadecimal "0x1f") is one '#' */
        gboolean d = g_ascii_isdigit (*p) ||
                     (in_num && g_ascii_isxdigit (*p)) ||
                     (in_num && (*p == 'x' || *p == 'X') && p > text && p[-1] == '0');
        if (d) {
            if (!in_num)
                g_string_append_c (k, '#');
            in_num = TRUE;
        } else {
            in_num = FALSE;
            g_string_append_c (k, *p);
        }
    }
    return g_string_free (k, FALSE);
}

int walpe_prof_msg_add (WalpeProfMsg *tab, guint *n, guint max, guint *dropped,
                        guint source, guint type, guint severity, guint id,
                        const char *text, int monitor)
{
    char buf[sizeof (tab[0].text)];
    /* what a shader compiler says quotes the shader: wallpaper content */
    if (source == GL_DEBUG_SOURCE_SHADER_COMPILER)
        text = "(shader compiler message; its text is not recorded)";
    prof_copy (buf, sizeof (buf), text ? text : "");
    g_strchomp (buf);
    /* a driver can send thousands of these a second: one key per message,
     * the table is compared by its hash */
    char *key = walpe_prof_msg_key (source, type, id, buf);
    guint hash = g_str_hash (key);
    g_free (key);
    int at = -1;
    for (guint i = 0; i < *n && at < 0; i++)
        if (tab[i].key_hash == hash && tab[i].source == source && tab[i].type == type &&
            tab[i].id == id)
            at = (int) i;
    if (at < 0) {
        if (*n >= max) {
            if (dropped)
                (*dropped)++;
            return -1;
        }
        at = (int) (*n)++;
        memset (&tab[at], 0, sizeof (tab[at]));
        tab[at].source = source;
        tab[at].type = type;
        tab[at].severity = severity;
        tab[at].id = id;
        tab[at].key_hash = hash;
        memcpy (tab[at].text, buf, sizeof (buf));
    }
    tab[at].count++;
    if (monitor >= 0 && monitor < 32)
        tab[at].monitors |= 1u << monitor;
    return at;
}

const char *walpe_prof_stage_name (int stage)
{
    static const char *names[WALPE_PROF_N_STAGES] = {
        "render", "update", "layers", "post", "publish", "swap", "present", "profiler",
    };
    return stage >= 0 && stage < WALPE_PROF_N_STAGES ? names[stage] : "?";
}

static const char *prof_counter_name (int c)
{
    static const char *names[WALPE_PROF_N_COUNTERS] = {
        "passes", "draws", "pixels", "clears", "blits", "blit_pixels", "target_switches",
        "tex_uploads", "tex_upload_bytes", "buf_uploads", "buf_upload_bytes", "readbacks",
        "finishes", "flushes", "mipmap_gens", "tex_copies", "program_links",
    };
    return c >= 0 && c < WALPE_PROF_N_COUNTERS ? names[c] : "?";
}

static const char *prof_mem_name (int c)
{
    static const char *names[WALPE_PROF_N_MEM] = {
        "compressed_textures", "other_textures", "effect_targets", "other_targets", "buffers",
        "window_buffers_est",
    };
    return c >= 0 && c < WALPE_PROF_N_MEM ? names[c] : "?";
}

static const char *prof_mem_label (int c)
{
    static const char *names[WALPE_PROF_N_MEM] = {
        "compressed textures", "other textures", "effect targets", "other targets", "buffers",
        "window buffers (est.)",
    };
    return c >= 0 && c < WALPE_PROF_N_MEM ? names[c] : "?";
}

static const char *prof_dbg_type (guint t)
{
    switch (t) {
    case GL_DEBUG_TYPE_ERROR: return "error";
    case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR: return "deprecated";
    case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR: return "undefined";
    case GL_DEBUG_TYPE_PORTABILITY: return "portability";
    case GL_DEBUG_TYPE_PERFORMANCE: return "performance";
    case GL_DEBUG_TYPE_MARKER: return "marker";
    case GL_DEBUG_TYPE_OTHER: return "other";
    default: return "?";
    }
}

static const char *prof_dbg_severity (guint s)
{
    switch (s) {
    case GL_DEBUG_SEVERITY_HIGH: return "high";
    case GL_DEBUG_SEVERITY_MEDIUM: return "medium";
    case GL_DEBUG_SEVERITY_LOW: return "low";
    case GL_DEBUG_SEVERITY_NOTIFICATION: return "note";
    default: return "?";
    }
}

static const char *prof_dbg_source (guint s)
{
    switch (s) {
    case GL_DEBUG_SOURCE_API: return "api";
    case GL_DEBUG_SOURCE_WINDOW_SYSTEM: return "window-system";
    case GL_DEBUG_SOURCE_SHADER_COMPILER: return "shader-compiler";
    case GL_DEBUG_SOURCE_THIRD_PARTY: return "third-party";
    case GL_DEBUG_SOURCE_APPLICATION: return "application";
    case GL_DEBUG_SOURCE_OTHER: return "other";
    default: return "?";
    }
}

/* ---- derived numbers of one monitor ---- */

static double mon_fps (const WalpeProfMon *m)
{
    return m->seconds > 0 ? m->frames / m->seconds : 0.0;
}

/* average GPU ms of a rendered frame (timer queries), 0 = unknown */
static double mon_gpu_ms (const WalpeProfMon *m)
{
    return m->gpu_frames ? m->gpu_ns / 1e6 / m->gpu_frames : 0.0;
}

/* share of the run's wall time the GPU spent on this monitor's frames, % */
static double mon_gpu_busy (const WalpeProfMon *m)
{
    return mon_gpu_ms (m) * mon_fps (m) / 10.0;
}

static double per_frame (const WalpeProfMon *m, guint64 v)
{
    return m->frames ? (double) v / m->frames : 0.0;
}

/* ---- a run in which a monitor did not draw all the time ---- */
double walpe_prof_mon_fps_drawing (const WalpeProfMon *m)
{
    double active = m->seconds - m->idle_seconds;
    if (m->idle_seconds < WALPE_PROF_IDLE_GAP_S || active < 0.05 || m->frames <= m->draw_runs)
        return mon_fps (m);
    /* n frames of a run span n - 1 frame intervals */
    return (m->frames - m->draw_runs) / active;
}

/* ---- rows whose GPU timer brackets CPU work ---- */
#define PROF_WAIT_MIN_MS 0.05     /* less CPU time than this per frame is noise */
#define PROF_WAIT_SHARE 0.30      /* of the row's GPU time */
double walpe_prof_row_cpu_wait_ms (const WalpeProfMon *m, const WalpeProfRow *w)
{
    if (!m->timer_ok || !m->gpu_frames || !m->frames || m->wall_timer)
        return 0.0;
    double gpu = w->gpu_ns / 1e6 / m->gpu_frames, cpu = per_frame (m, w->cpu_ns) / 1e6;
    if (cpu < PROF_WAIT_MIN_MS || cpu < PROF_WAIT_SHARE * gpu)
        return 0.0;
    return MIN (cpu, gpu);
}

double walpe_prof_mon_gpu_ms_min (const WalpeProfMon *m)
{
    double ms = mon_gpu_ms (m);
    for (guint k = 0; k < m->n_rows; k++)
        ms -= walpe_prof_row_cpu_wait_ms (m, &m->rows[k]);
    return MAX (ms, 0.0);
}

static int row_cmp_gpu (gconstpointer a, gconstpointer b)
{
    const WalpeProfRow *x = *(const WalpeProfRow *const *) a, *y = *(const WalpeProfRow *const *) b;
    if (x->gpu_ns != y->gpu_ns)
        return x->gpu_ns > y->gpu_ns ? -1 : 1;
    if (x->cpu_ns != y->cpu_ns)
        return x->cpu_ns > y->cpu_ns ? -1 : 1;
    if (x->layer_index != y->layer_index)
        return x->layer_index < y->layer_index ? -1 : 1;
    if (x->inst != y->inst)
        return x->inst < y->inst ? -1 : 1;
    return x->pass < y->pass ? -1 : x->pass > y->pass;
}

static int row_cmp_cpu (gconstpointer a, gconstpointer b)
{
    const WalpeProfRow *x = *(const WalpeProfRow *const *) a, *y = *(const WalpeProfRow *const *) b;
    if (x->cpu_ns != y->cpu_ns)
        return x->cpu_ns > y->cpu_ns ? -1 : 1;
    return row_cmp_gpu (a, b);
}

/* the rows of a monitor, most expensive first (GPU time; CPU time when
 * the context has no timer queries). g_free the array. */
static const WalpeProfRow **mon_sorted_rows (const WalpeProfMon *m)
{
    const WalpeProfRow **v = g_new0 (const WalpeProfRow *, m->n_rows + 1);
    for (guint i = 0; i < m->n_rows; i++)
        v[i] = &m->rows[i];
    if (m->n_rows)
        qsort (v, m->n_rows, sizeof (v[0]), m->timer_ok ? row_cmp_gpu : row_cmp_cpu);
    return v;
}

/* "blur (e2 p1)": effect, its position in the layer's effects, the pass */
static void row_effect_text (const WalpeProfRow *r, char *out, gsize size)
{
    if (!r->effect[0])
        g_snprintf (out, size, "%s", r->layer_index >= 0 ? "(layer draw)" : "-");
    else if (r->inst >= 0 && r->pass >= 0)
        g_snprintf (out, size, "%s (e%d p%d)", r->effect, r->inst, r->pass);
    else if (r->inst >= 0)
        g_snprintf (out, size, "%s (e%d)", r->effect, r->inst);
    else
        g_snprintf (out, size, "%s", r->effect);
}

/* pad / cut a UTF-8 string to `cols` characters */
static void append_cell (GString *s, const char *text, int cols)
{
    glong n = g_utf8_strlen (text, -1);
    if (n > cols) {
        const char *end = g_utf8_offset_to_pointer (text, cols - 1);
        g_string_append_len (s, text, end - text);
        g_string_append_c (s, '~');
    } else {
        g_string_append (s, text);
        for (glong i = n; i < cols; i++)
            g_string_append_c (s, ' ');
    }
}

static void append_bytes (GString *s, double b)
{
    if (b >= 1048576.0 * 10)
        g_string_append_printf (s, "%.0f MiB", b / 1048576.0);
    else if (b >= 1048576.0)
        g_string_append_printf (s, "%.1f MiB", b / 1048576.0);
    else if (b >= 1024.0)
        g_string_append_printf (s, "%.1f KiB", b / 1024.0);
    else
        g_string_append_printf (s, "%.0f B", b);
}

static gint64 report_mem_total (const WalpeProfReport *r, gint64 by_cat[WALPE_PROF_N_MEM],
                                int count[WALPE_PROF_N_MEM])
{
    gint64 total = 0;
    for (int c = 0; c < WALPE_PROF_N_MEM; c++) {
        gint64 b = 0;
        int n = 0;
        for (guint i = 0; i < r->n_mons; i++) {
            b += r->mons[i].mem_bytes[c];
            n += r->mons[i].mem_count[c];
        }
        if (by_cat)
            by_cat[c] = b;
        if (count)
            count[c] = n;
        total += b;
    }
    return total;
}

static guint report_perf_msgs (const WalpeProfReport *r, guint *total)
{
    guint n = 0, t = 0;
    for (guint i = 0; i < r->n_msgs; i++)
        if (r->msgs[i].type == GL_DEBUG_TYPE_PERFORMANCE) {
            n++;
            t += r->msgs[i].count;
        }
    if (total)
        *total = t;
    return n;
}

char *walpe_prof_format_total (const WalpeProfReport *r)
{
    GString *s = g_string_new ("TOTAL: ");
    double gpu = 0, busy = 0, passes = 0, draws = 0, px = 0, up = 0;
    double gpu_min = 0;   /* without the possible CPU waiting of flagged rows */
    gboolean any_timer = FALSE, all_timer = r->n_mons > 0;
    for (guint i = 0; i < r->n_mons; i++) {
        const WalpeProfMon *m = &r->mons[i];
        any_timer |= m->timer_ok && m->gpu_frames > 0;
        all_timer &= (m->timer_ok && m->gpu_frames > 0) || m->frames == 0;
        gpu += mon_gpu_ms (m);
        gpu_min += walpe_prof_mon_gpu_ms_min (m);
        busy += mon_gpu_busy (m);
        passes += per_frame (m, m->counters[WALPE_PROF_C_PASSES]);
        draws += per_frame (m, m->counters[WALPE_PROF_C_DRAWS]);
        px += per_frame (m, m->counters[WALPE_PROF_C_PIXELS]);
        up += per_frame (m, m->counters[WALPE_PROF_C_TEX_UPLOAD_BYTES] +
                            m->counters[WALPE_PROF_C_BUF_UPLOAD_BYTES]);
    }
    if (any_timer)
    {
        g_string_append_printf (s, "GPU %.3f ms/frame, %.1f %% GPU busy%s", gpu, busy,
                                all_timer ? "" : " (not every monitor has timer queries)");
        if (gpu_min < gpu - 0.0005)
            g_string_append_printf (s, " (at least %.3f ms GPU work, the rest may be the GPU "
                                    "waiting in rows marked *)", gpu_min);
    }
    else
        g_string_append (s, "GPU time n/a (no timer queries)");
    g_string_append_printf (s, " over %u monitor%s (", r->n_mons, r->n_mons == 1 ? "" : "s");
    for (guint i = 0; i < r->n_mons; i++) {
        const WalpeProfMon *m = &r->mons[i];
        g_string_append_printf (s, "%smon%d ", i ? ", " : "", m->monitor);
        if (m->timer_ok && m->gpu_frames)
            g_string_append_printf (s, "%.3f ms ", mon_gpu_ms (m));
        g_string_append_printf (s, "@ %.1f fps", mon_fps (m));
    }
    g_string_append_printf (s, "); per frame %.1f passes, %.1f draws, %.2f Mpx shaded, ", passes,
                            draws, px / 1e6);
    append_bytes (s, up);
    g_string_append (s, " uploaded; ");
    append_bytes (s, (double) report_mem_total (r, NULL, NULL));
    guint ptotal = 0, pn = report_perf_msgs (r, &ptotal);
    g_string_append_printf (s, " textures+targets; %u driver performance message%s (%u in all)",
                            pn, pn == 1 ? "" : "s", ptotal);
    return g_string_free (s, FALSE);
}

char *walpe_prof_format_text (const WalpeProfReport *r)
{
    GString *s = g_string_new (NULL);
    g_string_append_printf (s, "WalPE GPU profile  %s  walpe %s  run %.1f s  %u monitor%s%s\n",
                            r->when, r->version, r->seconds, r->n_mons, r->n_mons == 1 ? "" : "s",
                            r->shared ? " (one share group)" : "");
    /* a monitor that was paused for part of the run: said first,
     * because every "per second" figure of that monitor is lower by it */
    for (guint i = 0; i < r->n_mons; i++) {
        const WalpeProfMon *m = &r->mons[i];
        if (m->paused_seconds >= 0.1)
            g_string_append_printf (s, "NOTE: monitor %d was paused for %.1f s of this run (%s): "
                                    "it drew nothing then. Run it again with the wallpapers "
                                    "visible for the whole time.\n", m->monitor, m->paused_seconds,
                                    m->pause_reason[0] ? m->pause_reason : "reason unknown");
    }
    for (guint i = 0; i < r->n_mons; i++) {
        const WalpeProfMon *m = &r->mons[i];
        g_string_append_printf (s, "\n== monitor %d (%s, %dx%d", m->monitor,
                                m->name[0] ? m->name : "?", m->w, m->h);
        if (m->hz > 0)
            g_string_append_printf (s, " @ %.1f Hz", m->hz);
        g_string_append_printf (s, "): %s", m->type[0] ? m->type : "none");
        if (m->wallpaper[0])
            g_string_append_printf (s, " %s", m->wallpaper);
        if (m->role[0])
            g_string_append_printf (s, ", %s", m->role);
        g_string_append_c (s, '\n');
        if (!m->attached) {
            g_string_append (s, "   the render thread did not report in time (busy loading?)\n");
            continue;
        }
        g_string_append_printf (s, "   GL: %s | %s | %s\n", m->gl_renderer, m->gl_version,
                                m->gl_vendor);
        g_string_append_printf (s, "   timer: %s | debug: %s%s\n", m->timer, m->debug,
                                m->hooks_ok ? "" : " | draw/upload counters: off");
        gboolean part = m->idle_seconds >= WALPE_PROF_IDLE_GAP_S && m->frames > 0;
        if (part)
            g_string_append_printf (s, "   frames: %u rendered (%.1f/s over the run, %.1f/s while "
                                    "drawing), %u presented, %u skipped as unchanged", m->frames,
                                    mon_fps (m), walpe_prof_mon_fps_drawing (m), m->presented,
                                    m->skipped);
        else
        g_string_append_printf (s, "   frames: %u rendered (%.1f/s), %u presented, %u skipped "
                                "as unchanged", m->frames, mon_fps (m), m->presented, m->skipped);
        if (m->timer_ok) {
            g_string_append_printf (s, " | GPU time read for %u", m->gpu_frames);
            if (m->disjoint)
                g_string_append_printf (s, ", %u dropped", m->disjoint);
        }
        g_string_append_c (s, '\n');
        if (m->frames == 0) {
            g_string_append (s, "   no frame was rendered during the run (static, paused or "
                             "covered wallpaper)\n");
        }
        if (m->paused_seconds >= 0.1)
            g_string_append_printf (s, "   paused for %.1f s of the %.1f s (%s); nothing was drawn "
                                    "for %.1f s in all\n", m->paused_seconds, m->seconds,
                                    m->pause_reason[0] ? m->pause_reason : "reason unknown",
                                    MAX (m->idle_seconds, m->paused_seconds));
        else if (part)
            g_string_append_printf (s, "   nothing was drawn for %.1f s of the %.1f s (nothing "
                                    "changed, a still picture, or the wallpaper was loading)\n",
                                    m->idle_seconds, m->seconds);
        if (m->n_rows) {
            const WalpeProfRow **v = mon_sorted_rows (m);
            double tot_gpu = (double) m->gpu_ns, tot_cpu = 0;
            for (guint k = 0; k < m->n_rows; k++)
                tot_cpu += (double) v[k]->cpu_ns;
            if (m->timer_ok)
                g_string_append (s, "    #  GPU ms/f      %  CPU ms/f  runs/f  draws/f    Mpx/f  "
                                 "target     layer                     effect\n");
            else
                g_string_append (s, "    #  CPU ms/f      %  runs/f  draws/f    Mpx/f  "
                                 "target     layer                     effect"
                                 "   (no GPU timer: sorted by CPU time)\n");
            guint shown = MIN (m->n_rows, (guint) WALPE_PROF_TOP_ROWS);
            double rest_gpu = 0, rest_cpu = 0;
            double wait_ms = 0;      /* possible GPU waiting in flagged rows */
            guint n_wait = 0;
            for (guint k = 0; k < m->n_rows; k++) {
                const WalpeProfRow *w = v[k];
                double gpu_f = m->gpu_frames ? w->gpu_ns / 1e6 / m->gpu_frames : 0.0;
                double cpu_f = per_frame (m, w->cpu_ns) / 1e6;
                double wait_f = walpe_prof_row_cpu_wait_ms (m, w);
                if (wait_f > 0) {
                    wait_ms += wait_f;
                    n_wait++;
                }
                if (k >= shown) {
                    rest_gpu += gpu_f;
                    rest_cpu += cpu_f;
                    continue;
                }
                char tgt[24], eff[96];
                if (w->tw > 0 && w->th > 0)
                    g_snprintf (tgt, sizeof (tgt), "%dx%d", w->tw, w->th);
                else
                    g_snprintf (tgt, sizeof (tgt), "-");
                row_effect_text (w, eff, sizeof (eff));
                g_string_append_printf (s, "   %2u  ", k + 1);
                if (m->timer_ok)
                    g_string_append_printf (s, "%8.3f  %5.1f  %8.3f", gpu_f,
                                            tot_gpu > 0 ? 100.0 * w->gpu_ns / tot_gpu : 0.0, cpu_f);
                else
                    g_string_append_printf (s, "%8.3f  %5.1f", cpu_f,
                                            tot_cpu > 0 ? 100.0 * w->cpu_ns / tot_cpu : 0.0);
                g_string_append_printf (s, "  %6.2f  %7.1f  %7.2f  ", per_frame (m, w->runs),
                                        per_frame (m, w->draws), per_frame (m, w->pixels) / 1e6);
                append_cell (s, tgt, 9);
                g_string_append (s, "  ");
                append_cell (s, w->layer[0] ? w->layer : "?", 24);
                g_string_append (s, "  ");
                g_string_append (s, eff);
                if (wait_f > 0)
                    g_string_append (s, "  *");
                g_string_append_c (s, '\n');
            }
            if (m->n_rows > shown) {
                g_string_append_printf (s, "       (+%u more rows", m->n_rows - shown);
                if (m->timer_ok)
                    g_string_append_printf (s, ": GPU %.3f ms/f, CPU %.3f ms/f)\n", rest_gpu,
                                            rest_cpu);
                else
                    g_string_append_printf (s, ": CPU %.3f ms/f)\n", rest_cpu);
            }
            if (n_wait)
                g_string_append_printf (s, "       * %u row%s spent a large part of %s GPU time "
                                        "on the CPU (the driver copying an upload, scripts): the "
                                        "timer counts the GPU waiting for that.\n         Up to "
                                        "%.3f of the %.3f ms/f may be waiting, not GPU work.\n",
                                        n_wait, n_wait == 1 ? "" : "s", n_wait == 1 ? "its" : "their",
                                        wait_ms, mon_gpu_ms (m));
            g_free (v);
        }
        const guint64 *c = m->counters;
        g_string_append_printf (s, "   per frame: passes %.1f", per_frame (m, c[WALPE_PROF_C_PASSES]));
        if (m->fx_cached)
            g_string_append_printf (s, " (+%.1f from the static cache)",
                                    per_frame (m, m->fx_cached));
        if (m->hooks_ok) {
            g_string_append_printf (s, " | draws %.1f | shaded %.2f Mpx | clears %.1f | blits %.1f "
                                    "(%.2f Mpx) | target switches %.1f\n",
                                    per_frame (m, c[WALPE_PROF_C_DRAWS]),
                                    per_frame (m, c[WALPE_PROF_C_PIXELS]) / 1e6,
                                    per_frame (m, c[WALPE_PROF_C_CLEARS]),
                                    per_frame (m, c[WALPE_PROF_C_BLITS]),
                                    per_frame (m, c[WALPE_PROF_C_BLIT_PIXELS]) / 1e6,
                                    per_frame (m, c[WALPE_PROF_C_FBO_BINDS]));
            g_string_append (s, "   per frame: uploads textures ");
            append_bytes (s, per_frame (m, c[WALPE_PROF_C_TEX_UPLOAD_BYTES]));
            g_string_append_printf (s, " (%.2f calls), buffers ",
                                    per_frame (m, c[WALPE_PROF_C_TEX_UPLOADS]));
            append_bytes (s, per_frame (m, c[WALPE_PROF_C_BUF_UPLOAD_BYTES]));
            g_string_append_printf (s, " (%.2f calls) | readbacks %.2f | glFinish %.2f | glFlush "
                                    "%.2f | mipmap builds %.2f | texture copies %.2f | program "
                                    "links %" G_GUINT64_FORMAT " in all\n",
                                    per_frame (m, c[WALPE_PROF_C_BUF_UPLOADS]),
                                    per_frame (m, c[WALPE_PROF_C_READBACKS]),
                                    per_frame (m, c[WALPE_PROF_C_FINISHES]),
                                    per_frame (m, c[WALPE_PROF_C_FLUSHES]),
                                    per_frame (m, c[WALPE_PROF_C_MIPGENS]),
                                    per_frame (m, c[WALPE_PROF_C_TEX_COPIES]),
                                    c[WALPE_PROF_C_PROGRAM_LINKS]);
            if (m->load_upload_bytes) {
                g_string_append (s, "   outside frames (wallpaper loads): ");
                append_bytes (s, (double) m->load_upload_bytes);
                g_string_append (s, " uploaded\n");
            }
        } else {
            g_string_append_printf (s, " | shaded %.2f Mpx (passes only)\n",
                                    per_frame (m, c[WALPE_PROF_C_PIXELS]) / 1e6);
        }
        g_string_append (s, "   render thread ms/frame (wall/cpu):");
        gboolean any = FALSE;
        for (int st = 0; st < WALPE_PROF_N_STAGES; st++) {
            if (!m->stage_wall_ns[st] && !m->stage_cpu_ns[st])
                continue;
            g_string_append_printf (s, "%s %s %.3f/%.3f", any ? " |" : "",
                                    walpe_prof_stage_name (st),
                                    per_frame (m, m->stage_wall_ns[st]) / 1e6,
                                    per_frame (m, m->stage_cpu_ns[st]) / 1e6);
            any = TRUE;
        }
        g_string_append (s, any ? "\n" : " -\n");
        g_string_append_printf (s, "   TOTAL mon%d: ", m->monitor);
        if (m->timer_ok && m->gpu_frames)
        {
            g_string_append_printf (s, "GPU %.3f ms/frame (max %.3f), %.1f %% GPU busy at %.1f fps",
                                    mon_gpu_ms (m), m->gpu_max_ns / 1e6, mon_gpu_busy (m),
                                    mon_fps (m));
            if (m->idle_seconds >= WALPE_PROF_IDLE_GAP_S && m->frames > 0)
                g_string_append_printf (s, " (%.1f %% at %.1f fps while drawing)",
                                        mon_gpu_ms (m) * walpe_prof_mon_fps_drawing (m) / 10.0,
                                        walpe_prof_mon_fps_drawing (m));
            double lo = walpe_prof_mon_gpu_ms_min (m);
            if (lo < mon_gpu_ms (m) - 0.0005)
                g_string_append_printf (s, ", at least %.3f ms of it GPU work (rows marked *)", lo);
        }
        else
            g_string_append_printf (s, "GPU time n/a, %.1f fps", mon_fps (m));
        g_string_append_printf (s, "; %.1f passes, %.1f draws, %.2f Mpx shaded, ",
                                per_frame (m, c[WALPE_PROF_C_PASSES]),
                                per_frame (m, c[WALPE_PROF_C_DRAWS]),
                                per_frame (m, c[WALPE_PROF_C_PIXELS]) / 1e6);
        append_bytes (s, per_frame (m, c[WALPE_PROF_C_TEX_UPLOAD_BYTES] +
                                       c[WALPE_PROF_C_BUF_UPLOAD_BYTES]));
        g_string_append (s, " uploaded per frame\n");
    }

    /* memory */
    gint64 by_cat[WALPE_PROF_N_MEM];
    int count[WALPE_PROF_N_MEM];
    gint64 total = report_mem_total (r, by_cat, count);
    g_string_append (s, "\nmemory at the end of the run");
    if (r->shared)
        g_string_append (s, " (shared textures counted once)");
    g_string_append (s, ":\n  ");
    for (int c = 0; c < WALPE_PROF_N_MEM; c++) {
        g_string_append_printf (s, "%s%s ", c ? " | " : " ", prof_mem_label (c));
        if (c != WALPE_PROF_MEM_SURFACES)
            g_string_append_printf (s, "%d / ", count[c]);
        append_bytes (s, (double) by_cat[c]);
    }
    g_string_append (s, " | total ");
    append_bytes (s, (double) total);
    g_string_append_c (s, '\n');
    for (guint i = 0; i < r->n_mons; i++) {
        const WalpeProfMon *m = &r->mons[i];
        if (m->vram_total_kb <= 0)
            continue;
        g_string_append_printf (s, "   driver, mon%d: %" G_GINT64_FORMAT " MiB video memory, %"
                                G_GINT64_FORMAT " MiB free", m->monitor, m->vram_total_kb / 1024,
                                m->vram_avail_kb / 1024);
        if (m->vram_evictions >= 0)
            g_string_append_printf (s, ", %" G_GINT64_FORMAT " evictions during the run",
                                    m->vram_evictions);
        g_string_append_c (s, '\n');
        break;   /* one GPU: the same numbers on every monitor */
    }

    /* driver messages: performance first, then by count */
    guint ptotal = 0, pn = report_perf_msgs (r, &ptotal), all = 0;
    for (guint i = 0; i < r->n_msgs; i++)
        all += r->msgs[i].count;
    g_string_append_printf (s, "\ndriver messages (KHR_debug): %u distinct, %u in all; "
                            "performance: %u distinct, %u in all", r->n_msgs, all, pn, ptotal);
    if (r->msgs_dropped)
        g_string_append_printf (s, "; %u more not kept (table full)", r->msgs_dropped);
    g_string_append_c (s, '\n');
    for (int pass = 0; pass < 2; pass++) {
        guint notes = 0, note_total = 0;
        for (guint i = 0; i < r->n_msgs; i++) {
            const WalpeProfMsg *g = &r->msgs[i];
            if ((g->type == GL_DEBUG_TYPE_PERFORMANCE) != (pass == 0))
                continue;
            if (pass == 1 && g->severity == GL_DEBUG_SEVERITY_NOTIFICATION) {
                notes++;   /* informational chatter: counted, not listed */
                note_total += g->count;
                continue;
            }
            g_string_append_printf (s, "   [%s] x%u mon", prof_dbg_type (g->type), g->count);
            gboolean first = TRUE;
            for (int b = 0; b < 32; b++)
                if (g->monitors & (1u << b)) {
                    g_string_append_printf (s, "%s%d", first ? "" : ",", b);
                    first = FALSE;
                }
            if (first)
                g_string_append_c (s, '?');
            g_string_append_printf (s, " %s/%s id %u: %s\n", prof_dbg_source (g->source),
                                    prof_dbg_severity (g->severity), g->id, g->text);
        }
        if (pass == 1 && notes)
            g_string_append_printf (s, "   (+%u distinct notifications, %u in all: see the JSON)\n",
                                    notes, note_total);
    }
    char *t = walpe_prof_format_total (r);
    g_string_append_printf (s, "\n%s\n", t);
    g_free (t);
    return g_string_free (s, FALSE);
}

static void jb_num (JsonBuilder *b, const char *k, double v)
{
    json_builder_set_member_name (b, k);
    json_builder_add_double_value (b, isfinite (v) ? v : 0.0);
}

static void jb_i (JsonBuilder *b, const char *k, gint64 v)
{
    json_builder_set_member_name (b, k);
    json_builder_add_int_value (b, v);
}

static void jb_s (JsonBuilder *b, const char *k, const char *v)
{
    json_builder_set_member_name (b, k);
    json_builder_add_string_value (b, v ? v : "");
}

static void jb_b (JsonBuilder *b, const char *k, gboolean v)
{
    json_builder_set_member_name (b, k);
    json_builder_add_boolean_value (b, v);
}

char *walpe_prof_format_json (const WalpeProfReport *r)
{
    JsonBuilder *b = json_builder_new ();
    json_builder_begin_object (b);
    jb_i (b, "walpe_gpu_profile", 1);
    jb_s (b, "when", r->when);
    jb_s (b, "version", r->version);
    jb_num (b, "seconds", r->seconds);
    jb_b (b, "share_group", r->shared);
    json_builder_set_member_name (b, "monitors");
    json_builder_begin_array (b);
    for (guint i = 0; i < r->n_mons; i++) {
        const WalpeProfMon *m = &r->mons[i];
        json_builder_begin_object (b);
        jb_i (b, "monitor", m->monitor);
        jb_s (b, "name", m->name);
        jb_i (b, "width", m->w);
        jb_i (b, "height", m->h);
        jb_num (b, "refresh_hz", m->hz);
        jb_s (b, "type", m->type);
        jb_s (b, "wallpaper", m->wallpaper);
        jb_s (b, "role", m->role);
        jb_b (b, "attached", m->attached);
        jb_s (b, "gl_vendor", m->gl_vendor);
        jb_s (b, "gl_renderer", m->gl_renderer);
        jb_s (b, "gl_version", m->gl_version);
        jb_b (b, "gl_es", m->gl_es);
        jb_i (b, "gl_version_number", m->gl_ver);
        jb_s (b, "timer", m->timer);
        jb_b (b, "timer_ok", m->timer_ok);
        jb_s (b, "debug", m->debug);
        jb_b (b, "counters_ok", m->hooks_ok);
        jb_num (b, "seconds", m->seconds);
        jb_i (b, "frames", m->frames);
        jb_i (b, "presented", m->presented);
        jb_i (b, "skipped_unchanged", m->skipped);
        jb_num (b, "fps", mon_fps (m));
        jb_i (b, "gpu_frames", m->gpu_frames);
        jb_i (b, "gpu_frames_dropped", m->disjoint);
        jb_num (b, "gpu_ms_per_frame", mon_gpu_ms (m));
        jb_num (b, "gpu_ms_max_frame", m->gpu_max_ns / 1e6);
        jb_num (b, "gpu_busy_percent", mon_gpu_busy (m));
        jb_num (b, "idle_seconds", m->idle_seconds);
        jb_i (b, "draw_runs", m->draw_runs);
        jb_num (b, "paused_seconds", m->paused_seconds);
        jb_s (b, "pause_reason", m->pause_reason);
        jb_num (b, "fps_while_drawing", walpe_prof_mon_fps_drawing (m));
        jb_num (b, "gpu_ms_per_frame_min", walpe_prof_mon_gpu_ms_min (m));
        jb_b (b, "gpu_time_is_wall_time", m->wall_timer);
        json_builder_set_member_name (b, "per_frame");
        json_builder_begin_object (b);
        for (int c = 0; c < WALPE_PROF_N_COUNTERS; c++)
            jb_num (b, prof_counter_name (c), per_frame (m, m->counters[c]));
        jb_num (b, "passes_cached", per_frame (m, m->fx_cached));
        json_builder_end_object (b);
        json_builder_set_member_name (b, "totals");
        json_builder_begin_object (b);
        for (int c = 0; c < WALPE_PROF_N_COUNTERS; c++)
            jb_i (b, prof_counter_name (c), (gint64) m->counters[c]);
        jb_i (b, "passes_cached", m->fx_cached);
        jb_i (b, "upload_bytes_outside_frames", (gint64) m->load_upload_bytes);
        json_builder_end_object (b);
        json_builder_set_member_name (b, "stages_ms_per_frame");
        json_builder_begin_object (b);
        for (int st = 0; st < WALPE_PROF_N_STAGES; st++) {
            json_builder_set_member_name (b, walpe_prof_stage_name (st));
            json_builder_begin_object (b);
            jb_num (b, "wall", per_frame (m, m->stage_wall_ns[st]) / 1e6);
            jb_num (b, "cpu", per_frame (m, m->stage_cpu_ns[st]) / 1e6);
            json_builder_end_object (b);
        }
        json_builder_end_object (b);
        json_builder_set_member_name (b, "memory");
        json_builder_begin_object (b);
        for (int c = 0; c < WALPE_PROF_N_MEM; c++) {
            json_builder_set_member_name (b, prof_mem_name (c));
            json_builder_begin_object (b);
            jb_i (b, "bytes", m->mem_bytes[c]);
            jb_i (b, "count", m->mem_count[c]);
            json_builder_end_object (b);
        }
        jb_b (b, "textures_are_the_share_groups", m->mem_shared);
        jb_i (b, "driver_video_memory_kb", m->vram_total_kb);
        jb_i (b, "driver_video_memory_free_kb", m->vram_avail_kb);
        jb_i (b, "driver_evictions_during_run", m->vram_evictions);
        json_builder_end_object (b);
        json_builder_set_member_name (b, "rows");
        json_builder_begin_array (b);
        const WalpeProfRow **v = mon_sorted_rows (m);
        for (guint k = 0; k < m->n_rows; k++) {
            const WalpeProfRow *w = v[k];
            json_builder_begin_object (b);
            jb_s (b, "layer", w->layer);
            jb_i (b, "layer_index", w->layer_index);
            jb_s (b, "kind", w->kind);
            jb_s (b, "effect", w->effect);
            jb_i (b, "effect_index", w->inst);
            jb_i (b, "pass", w->pass);
            jb_i (b, "target_w", w->tw);
            jb_i (b, "target_h", w->th);
            jb_num (b, "gpu_ms_per_frame", m->gpu_frames ? w->gpu_ns / 1e6 / m->gpu_frames : 0.0);
            jb_num (b, "cpu_ms_per_frame", per_frame (m, w->cpu_ns) / 1e6);
            jb_num (b, "gpu_ms_maybe_waiting", walpe_prof_row_cpu_wait_ms (m, w));
            jb_num (b, "runs_per_frame", per_frame (m, w->runs));
            jb_num (b, "draws_per_frame", per_frame (m, w->draws));
            jb_num (b, "pixels_per_frame", per_frame (m, w->pixels));
            jb_num (b, "upload_bytes_per_frame", per_frame (m, w->upload_bytes));
            json_builder_end_object (b);
        }
        g_free (v);
        json_builder_end_array (b);
        json_builder_end_object (b);
    }
    json_builder_end_array (b);

    gint64 by_cat[WALPE_PROF_N_MEM];
    int count[WALPE_PROF_N_MEM];
    gint64 total = report_mem_total (r, by_cat, count);
    json_builder_set_member_name (b, "memory");
    json_builder_begin_object (b);
    for (int c = 0; c < WALPE_PROF_N_MEM; c++) {
        json_builder_set_member_name (b, prof_mem_name (c));
        json_builder_begin_object (b);
        jb_i (b, "bytes", by_cat[c]);
        jb_i (b, "count", count[c]);
        json_builder_end_object (b);
    }
    jb_i (b, "total_bytes", total);
    json_builder_end_object (b);

    json_builder_set_member_name (b, "driver_messages");
    json_builder_begin_array (b);
    for (guint i = 0; i < r->n_msgs; i++) {
        const WalpeProfMsg *g = &r->msgs[i];
        json_builder_begin_object (b);
        jb_s (b, "type", prof_dbg_type (g->type));
        jb_s (b, "source", prof_dbg_source (g->source));
        jb_s (b, "severity", prof_dbg_severity (g->severity));
        jb_i (b, "id", g->id);
        jb_i (b, "count", g->count);
        jb_i (b, "monitors_mask", g->monitors);
        jb_s (b, "text", g->text);
        json_builder_end_object (b);
    }
    json_builder_end_array (b);
    jb_i (b, "driver_messages_not_kept", r->msgs_dropped);

    double gpu = 0, busy = 0;
    for (guint i = 0; i < r->n_mons; i++) {
        gpu += mon_gpu_ms (&r->mons[i]);
        busy += mon_gpu_busy (&r->mons[i]);
    }
    guint ptotal = 0, pn = report_perf_msgs (r, &ptotal);
    json_builder_set_member_name (b, "total");
    json_builder_begin_object (b);
    jb_num (b, "gpu_ms_per_frame", gpu);
    jb_num (b, "gpu_busy_percent", busy);
    jb_i (b, "memory_bytes", total);
    jb_i (b, "performance_messages", pn);
    jb_i (b, "performance_messages_count", ptotal);
    char *line = walpe_prof_format_total (r);
    jb_s (b, "line", line);
    g_free (line);
    json_builder_end_object (b);
    json_builder_end_object (b);

    JsonNode *root = json_builder_get_root (b);
    JsonGenerator *g = json_generator_new ();
    json_generator_set_root (g, root);
    json_generator_set_pretty (g, TRUE);
    json_generator_set_indent (g, 1);
    char *out = json_generator_to_data (g, NULL);
    g_object_unref (g);
    json_node_unref (root);
    g_object_unref (b);
    return out;
}

/* ============================================ 2. per-thread state */

#define PROF_MAX_ROWS 4096
#define PROF_MAX_PENDING 240      /* frames whose queries are not read back yet */
#define PROF_MAX_MSGS 96
#define PROF_STACK 8
#define PROF_MAX_PAUSE_MONS 16    /* monitors whose pauses are followed */

/* special rows (layer_index < 0) */
enum {
    PROF_ROW_FRAME = -1,     /* content render without finer stages */
    PROF_ROW_PUBLISH = -2, PROF_ROW_PRESENT = -3, PROF_ROW_OTHER = -4,
    PROF_ROW_UPDATE = -5,    /* scene: before the layers */
    PROF_ROW_BETWEEN = -6,   /* scene: between the layers */
    PROF_ROW_POST = -7,      /* scene: after the layers */
    PROF_ROW_UPDATE_DRAW = -8,   /* scene: draws before the layers (video conversion) */
};

typedef struct {
    gint64 key;
    gboolean is_pass;
    WalpeProfRow r;
} ProfRowX;

typedef struct { GLuint q; ProfRowX *row; } ProfSeg;

typedef struct {
    ProfSeg *seg;
    guint n;
    gboolean skipped;   /* a wake-up that drew nothing: its time counts, it is no frame */
} ProfBatch;

typedef struct { GLuint name; gint64 bytes; guint8 compressed, target; } ProfTexEnt;
typedef struct { GLuint name; gint64 bytes; } ProfBufEnt;

typedef struct ProfThread {
    guint gen;                  /* the run this state belongs to */
    gboolean attached;          /* context facts read, debug channel on */
    gboolean finalized;         /* guarded by P.lock: the collector may read it */
    gboolean counting;          /* wrappers and hooks record (attached, not finished) */
    WalpeProfMon mon;           /* rows are filled in by the collector */
    GHashTable *row_by_key;     /* gint64 key -> ProfRowX */
    GPtrArray *rows;            /* ProfRowX, owned */
    ProfRowX *row_frame, *row_other;
    /* timer queries */
    int tq_mode;                /* 0 = none, 1 = core / ARB_timer_query, 2 = EXT_disjoint,
                                 * 3 = sync (glFinish at row boundaries) */
    ProfRowX *sync_row;         /* sync: the row being timed */
    gint64 sync_t0;             /* sync: since when (ns) */
    guint64 sync_frame_ns;      /* sync: this frame so far */
    GArray *q_free;             /* GLuint */
    GArray *cur;                /* ProfSeg of the frame being rendered */
    GQueue pending;             /* ProfBatch, oldest first */
    gboolean q_active;
    /* frame */
    gboolean in_frame, sampled;
    gboolean frame_skipped;     /* walpe_prof_frame_skipped () inside this frame */
    int fw, fh;
    int vw, vh;                 /* the viewport in effect (frame size without the wrappers) */
    ProfRowX *stack[PROF_STACK];
    int depth;
    gint64 seg_t0;              /* ns: start of the current row's CPU segment */
    gint64 frame_t0;            /* ns: frame_begin */
    gint64 frame_t1;            /* ns: end of the last frame */
    int stage;
    gint64 stage_t0, stage_c0;
    gint64 t_attach_ns;
    guint frame_no;
    gint64 last_drawn_ns;       /* end of the last frame that drew something (0 = none yet) */
    gint64 idle_ns;             /* gaps without a drawn frame longer than WALPE_PROF_IDLE_GAP_S */
    ProfRowX *row_update, *row_update_draw;   /* the update stage's two rows of this frame */
    gboolean quad_seen;         /* walpe_prof_quad () ran for the draw call that comes next */
    /* debug channel */
    gboolean dbg_installed, dbg_was_enabled;
    /* running totals at attach */
    guint fx_run0, fx_cached0;
    gint64 evict0;
    /* memory snapshot (finalize) */
    GArray *tex;                /* ProfTexEnt */
    GArray *buf;                /* ProfBufEnt */
} ProfThread;

static __thread ProfThread *prof_tls;

enum { PROF_IDLE = 0, PROF_RUNNING = 1, PROF_COLLECTING = 2, PROF_READY = 3 };

static struct {
    GMutex lock;
    gint state;                 /* atomic */
    gint gen;                   /* atomic */
    double seconds;
    gboolean shared;
    int expected;               /* render threads of the daemon */
    gint64 t_start_us;
    GPtrArray *threads;         /* ProfThread (registered; lock) */
    WalpeProfWaker wake;
    gpointer wake_user;
    guint stop_id, collect_id;
    gint64 collect_deadline_us;
    gint64 max_frame_ns;        /* atomic: the longest frame of this run */
    gint64 t_stop_ns;           /* atomic: when the run ended (0 = running) */
    char *text, *json, *text_path, *json_path;
    /* driver messages (lock) */
    WalpeProfMsg msgs[PROF_MAX_MSGS];
    guint n_msgs, msgs_dropped;
    gboolean msgs_open;
    /* options read at start */
    gboolean no_timer, no_hooks, sync;
    guint every;
    gboolean hooks_installed;
    /* pause intervals of the monitors during the run (lock) */
    struct {
        gboolean paused;
        gint64 since_ns, total_ns;
        char reason[64];
    } pause[PROF_MAX_PAUSE_MONS];
} P;

/* drop pending GL errors (bounded: a lost context reports one forever) */
static void prof_clear_errors (void)
{
    for (int i = 0; i < 64 && glGetError () != GL_NO_ERROR; i++)
        ;
}

static inline gint64 prof_ns (void)
{
    struct timespec ts;
    clock_gettime (CLOCK_MONOTONIC, &ts);
    return (gint64) ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static inline gint64 prof_thread_cpu_ns (void)
{
    struct timespec ts;
    clock_gettime (CLOCK_THREAD_CPUTIME_ID, &ts);
    return (gint64) ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/* a pass row never shares a key with a layer row (the layer-colour pass has
 * no effect / pass number and may have the frame's size) */
static gint64 row_key (int layer_index, int inst, int pass, int tw, int th, gboolean is_pass)
{
    return (is_pass ? (gint64) 1 << 62 : 0) |
           ((gint64) ((layer_index + 16) & 0x3fff) << 48) |
           ((gint64) ((inst + 1) & 0xff) << 40) |
           ((gint64) ((pass + 2) & 0xff) << 32) |
           ((gint64) (CLAMP (tw, 0, 0xffff)) << 16) |
           (gint64) (CLAMP (th, 0, 0xffff));
}

static ProfRowX *row_get (ProfThread *T, int layer_index, int inst, int pass, int tw, int th,
                          gboolean is_pass)
{
    gint64 key = row_key (layer_index, inst, pass, tw, th, is_pass);
    ProfRowX *row = g_hash_table_lookup (T->row_by_key, &key);
    if (row)
        return row;
    if (T->rows->len >= PROF_MAX_ROWS && T->row_other)
        return T->row_other;
    row = g_new0 (ProfRowX, 1);
    row->key = key;
    row->is_pass = is_pass;
    row->r.layer_index = layer_index;
    row->r.inst = inst;
    row->r.pass = pass;
    row->r.tw = tw;
    row->r.th = th;
    g_ptr_array_add (T->rows, row);
    g_hash_table_insert (T->row_by_key, &row->key, row);
    return row;
}

static void thread_reset (ProfThread *T)
{
    if (T->row_by_key)
        g_hash_table_destroy (T->row_by_key);
    if (T->rows)
        g_ptr_array_unref (T->rows);
    T->row_by_key = g_hash_table_new (g_int64_hash, g_int64_equal);
    T->rows = g_ptr_array_new_with_free_func (g_free);
    if (!T->q_free)
        T->q_free = g_array_new (FALSE, FALSE, sizeof (GLuint));
    if (!T->cur)
        T->cur = g_array_new (FALSE, FALSE, sizeof (ProfSeg));
    if (T->tex)
        g_array_set_size (T->tex, 0);
    if (T->buf)
        g_array_set_size (T->buf, 0);
    memset (&T->mon, 0, sizeof (T->mon));
    T->attached = T->counting = T->in_frame = T->sampled = T->q_active = FALSE;
    T->sync_row = NULL;
    T->sync_frame_ns = 0;
    T->frame_t1 = 0;
    T->depth = 0;
    T->frame_no = 0;
    T->last_drawn_ns = T->idle_ns = 0;
    T->row_update = T->row_update_draw = NULL;
    T->quad_seen = FALSE;
    T->row_frame = row_get (T, PROF_ROW_FRAME, -1, -1, 0, 0, FALSE);
    g_strlcpy (T->row_frame->r.layer, "(frame)", sizeof (T->row_frame->r.layer));
    T->row_other = row_get (T, PROF_ROW_OTHER, -1, -1, 0, 0, FALSE);
    g_strlcpy (T->row_other->r.layer, "(other rows)", sizeof (T->row_other->r.layer));
}

/* ---- timer queries ---- */

static void tq_detect (ProfThread *T)
{
    WalpeProfMon *m = &T->mon;
    T->tq_mode = 0;
    if (P.sync) {
        T->tq_mode = 3;
        m->timer_ok = TRUE;
        g_strlcpy (m->timer, "sync mode: glFinish after every row, wall time (slows rendering)",
                   sizeof (m->timer));
        return;
    }
    if (P.no_timer) {
        g_strlcpy (m->timer, "off (WALPE_GPU_PROFILE_TIMER=0): CPU time only", sizeof (m->timer));
        return;
    }
    gboolean ext = FALSE;
    if (!m->gl_es) {
        if (m->gl_ver >= 33 || epoxy_has_gl_extension ("GL_ARB_timer_query"))
            T->tq_mode = 1;
    } else if (m->gl_ver >= 30 && epoxy_has_gl_extension ("GL_EXT_disjoint_timer_query")) {
        T->tq_mode = 2;
        ext = TRUE;
    }
    if (!T->tq_mode) {
        g_snprintf (m->timer, sizeof (m->timer), "none: %s lacks %s (CPU time only)",
                    m->gl_es ? "this GLES context" : "this GL context",
                    m->gl_es ? "EXT_disjoint_timer_query" : "ARB_timer_query");
        return;
    }
    /* an implementation may expose the query with a counter of 0 bits */
    prof_clear_errors ();
    GLint bits = -1;
    glGetQueryiv (GL_TIME_ELAPSED, GL_QUERY_COUNTER_BITS, &bits);
    if (glGetError () != GL_NO_ERROR)
        bits = -1;   /* the question itself is unknown here: assume it works */
    if (bits == 0) {
        T->tq_mode = 0;
        g_strlcpy (m->timer, "none: GL_TIME_ELAPSED has 0 counter bits (CPU time only)",
                   sizeof (m->timer));
        return;
    }
    if (ext)
        (void) glGetError ();
    g_snprintf (m->timer, sizeof (m->timer), "GL_TIME_ELAPSED queries, %s%s",
                ext ? "EXT_disjoint_timer_query" : m->gl_ver >= 33 ? "GL 3.3 core" : "ARB_timer_query",
                P.every > 1 ? " (sampled frames)" : "");
    if (bits > 0) {
        gsize n = strlen (m->timer);
        g_snprintf (m->timer + n, sizeof (m->timer) - n, ", %d bits", bits);
    }
    if (walpe_gl_is_software ()) {
        /* a software rasterizer works through its queries tile by tile,
         * not along the draw order: totals are rough and rows are not
         * comparable (seen with llvmpipe) */
        gsize n = strlen (m->timer);
        g_snprintf (m->timer + n, sizeof (m->timer) - n, "; software GL: use --sync for rows");
    }
    m->timer_ok = TRUE;
}

static guint64 tq_result (ProfThread *T, GLuint q)
{
    GLuint64 v = 0;
    if (T->tq_mode == 2)
        glGetQueryObjectui64vEXT (q, GL_QUERY_RESULT, &v);
    else
        glGetQueryObjectui64v (q, GL_QUERY_RESULT, &v);
    return v;
}

static void batch_free (ProfThread *T, ProfBatch *b, gboolean recycle)
{
    for (guint i = 0; i < b->n; i++) {
        if (recycle)
            g_array_append_val (T->q_free, b->seg[i].q);
        else
            glDeleteQueries (1, &b->seg[i].q);
    }
    g_free (b->seg);
    g_free (b);
}

/* add the results of one frame (all its queries are complete) */
static void batch_read (ProfThread *T, ProfBatch *b, gboolean keep)
{
    guint64 total = 0;
    for (guint i = 0; i < b->n; i++) {
        guint64 ns = tq_result (T, b->seg[i].q);
        if (ns > G_GUINT64_CONSTANT (20000000000))
            ns = 0;   /* > 20 s for a piece of one frame: not a measurement */
        if (keep) {
            b->seg[i].row->r.gpu_ns += ns;
            total += ns;
        }
    }
    if (keep) {
        T->mon.gpu_ns += total;
        if (!b->skipped)
            T->mon.gpu_frames++;
        if (total > T->mon.gpu_max_ns)
            T->mon.gpu_max_ns = total;
    } else {
        T->mon.disjoint++;
    }
    batch_free (T, b, TRUE);
}

/* read back what the GPU has finished. block = everything (end of the run) */
static void tq_resolve (ProfThread *T, gboolean block)
{
    if (!T->tq_mode || T->tq_mode == 3 || g_queue_is_empty (&T->pending))
        return;
    gboolean keep = TRUE;
    ProfBatch *b;
    while ((b = g_queue_peek_head (&T->pending))) {
        if (!block) {
            GLuint avail = 0;
            glGetQueryObjectuiv (b->seg[b->n - 1].q, GL_QUERY_RESULT_AVAILABLE, &avail);
            if (!avail)
                break;
        }
        if (T->tq_mode == 2) {
            /* a disjoint event (power state change, context loss ...) since
             * the last check makes the timings read now meaningless */
            GLint dj = 0;
            glGetIntegerv (GL_GPU_DISJOINT_EXT, &dj);
            if (dj)
                keep = FALSE;
        }
        g_queue_pop_head (&T->pending);
        batch_read (T, b, keep);
    }
    /* a driver that never completes them must not make the queue grow */
    while (g_queue_get_length (&T->pending) > PROF_MAX_PENDING) {
        b = g_queue_pop_head (&T->pending);
        T->mon.disjoint++;
        batch_free (T, b, FALSE);
    }
}

static void prof_finish (void);

/* the GPU timer moves to `row` (NULL = nothing is timed) */
static void seg_switch (ProfThread *T, ProfRowX *row)
{
    if (T->tq_mode == 3) {
        /* sync mode: everything issued so far is done after the glFinish;
         * the time since the last boundary belongs to the row left */
        if (T->sync_row) {
            prof_finish ();
            guint64 d = (guint64) MAX (0, prof_ns () - T->sync_t0);
            T->sync_row->r.gpu_ns += d;
            T->sync_frame_ns += d;
        }
        T->sync_row = row && T->sampled ? row : NULL;
        T->sync_t0 = prof_ns ();
        return;
    }
    if (T->q_active) {
        glEndQuery (GL_TIME_ELAPSED);
        T->q_active = FALSE;
    }
    if (!row || !T->sampled)
        return;
    GLuint q = 0;
    if (T->q_free->len) {
        q = g_array_index (T->q_free, GLuint, T->q_free->len - 1);
        g_array_set_size (T->q_free, T->q_free->len - 1);
    } else {
        glGenQueries (1, &q);
    }
    if (!q)
        return;
    glBeginQuery (GL_TIME_ELAPSED, q);
    T->q_active = TRUE;
    ProfSeg sg = { q, row };
    g_array_append_val (T->cur, sg);
}

static void scope_push (ProfThread *T, ProfRowX *row)
{
    gint64 now = prof_ns ();
    if (T->depth > 0)
        T->stack[T->depth - 1]->r.cpu_ns += (guint64) (now - T->seg_t0);
    if (T->depth >= PROF_STACK) {
        T->depth++;   /* deeper than expected: time stays with the row above */
        return;
    }
    T->stack[T->depth++] = row;
    row->r.runs++;
    seg_switch (T, row);
    T->seg_t0 = prof_ns ();
}

static void scope_pop (ProfThread *T)
{
    if (T->depth <= 0)
        return;
    if (T->depth > PROF_STACK) {
        T->depth--;
        return;
    }
    gint64 now = prof_ns ();
    T->stack[T->depth - 1]->r.cpu_ns += (guint64) (now - T->seg_t0);
    T->depth--;
    seg_switch (T, T->depth > 0 ? T->stack[T->depth - 1] : NULL);
    T->seg_t0 = prof_ns ();
}

static inline ProfRowX *scope_cur (ProfThread *T)
{
    int d = MIN (T->depth, PROF_STACK);
    return d > 0 ? T->stack[d - 1] : NULL;
}

/* the row on top is replaced by `row`: CPU time and GPU timer move
 * to it from here on */
static void scope_swap (ProfThread *T, ProfRowX *row)
{
    if (T->depth <= 0 || T->depth > PROF_STACK || T->stack[T->depth - 1] == row)
        return;
    gint64 now = prof_ns ();
    T->stack[T->depth - 1]->r.cpu_ns += (guint64) (now - T->seg_t0);
    T->stack[T->depth - 1] = row;
    if (!row->r.runs || row->r.runs < T->mon.frames)
        row->r.runs++;        /* once per frame at most (the rows alternate) */
    seg_switch (T, row);
    T->seg_t0 = prof_ns ();
}

/* The update stage of a scene uploads (video planes, text) and
 * may draw (a video frame's YUV -> RGBA conversion pass). One row for both
 * made the conversion's GPU time indistinguishable from the GPU waiting
 * for the CPU copy of the upload, so what is DRAWN there has a row of its
 * own: the first draw call inside "(scene update, uploads)" moves the timer
 * to "(scene update: draws)", the next upload moves it back. Called by the
 * draw and upload wrappers (interposer below). */
static void update_split (ProfThread *T, gboolean draw)
{
    if (!T->row_update || T->depth != 1)
        return;
    ProfRowX *cur = T->stack[0];
    if (draw && cur == T->row_update) {
        if (!T->row_update_draw || T->row_update_draw->r.tw != T->vw ||
            T->row_update_draw->r.th != T->vh) {
            T->row_update_draw = row_get (T, PROF_ROW_UPDATE_DRAW, -1, -1, T->vw, T->vh, FALSE);
            if (!T->row_update_draw->r.layer[0])
                g_strlcpy (T->row_update_draw->r.layer, "(scene update: draws)",
                           sizeof (T->row_update_draw->r.layer));
        }
        scope_swap (T, T->row_update_draw);
    } else if (!draw && T->row_update_draw && cur == T->row_update_draw) {
        scope_swap (T, T->row_update);
    }
}

/* ============================================== 3. interposer
 * While a run is active, the libepoxy dispatch pointers of a few GL entry
 * points (every `glFoo (...)` in this process is `epoxy_glFoo (...)`) point
 * at the counting wrappers below; these call the real entry point, taken
 * from eglGetProcAddress. So draw calls, uploads, blits and readbacks are
 * counted wherever they are issued - the dlopen'd video / web modules
 * included - without a hook at each call site, and with no cost at all
 * while no run is active (the pointers are put back). A wrapper that sees
 * no profiled frame on its thread (GTK in the web module, another thread)
 * only forwards the call. WALPE_GPU_PROFILE_HOOKS=0 keeps the pointers
 * untouched; the report then has no draw / upload counters. */

#define PROF_T() ProfThread *T = prof_tls
#define PROF_C(T, c, n) \
    do { if ((T) && (T)->in_frame) (T)->mon.counters[(c)] += (guint64) (n); } while (0)

static void (*o_glDrawArrays) (GLenum, GLint, GLsizei);
static void (*o_glDrawElements) (GLenum, GLsizei, GLenum, const void *);
static void (*o_glDrawArraysInstanced) (GLenum, GLint, GLsizei, GLsizei);
static void (*o_glDrawElementsInstanced) (GLenum, GLsizei, GLenum, const void *, GLsizei);
static void (*o_glClear) (GLbitfield);
static void (*o_glBlitFramebuffer) (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint,
                                    GLbitfield, GLenum);
static void (*o_glBindFramebuffer) (GLenum, GLuint);
static void (*o_glTexImage2D) (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum,
                               const void *);
static void (*o_glTexSubImage2D) (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum,
                                  const void *);
static void (*o_glCompressedTexImage2D) (GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei,
                                         const void *);
static void (*o_glBufferData) (GLenum, GLsizeiptr, const void *, GLenum);
static void (*o_glBufferSubData) (GLenum, GLintptr, GLsizeiptr, const void *);
static void (*o_glReadPixels) (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
static void (*o_glGetTexImage) (GLenum, GLint, GLenum, GLenum, void *);
static void (*o_glFinish) (void);
static void (*o_glFlush) (void);
static void (*o_glGenerateMipmap) (GLenum);
static void (*o_glCopyTexSubImage2D) (GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei);
static void (*o_glLinkProgram) (GLuint);
static void (*o_glViewport) (GLint, GLint, GLsizei, GLsizei);

static inline void prof_draw (ProfThread *T)
{
    if (T && T->in_frame) {
        if (T->row_update) {
            /* a draw of the update stage has its own row; one that
             * told no quad size (the video module's conversion pass) fills
             * its viewport: the row gets those pixels (the frame's "shaded"
             * total stays as it was counted before) */
            update_split (T, TRUE);
            if (!T->quad_seen && T->row_update_draw && T->depth == 1 &&
                T->stack[0] == T->row_update_draw)
                T->row_update_draw->r.pixels += (guint64) MAX (0, T->vw) * (guint64) MAX (0, T->vh);
        }
        T->quad_seen = FALSE;
        T->mon.counters[WALPE_PROF_C_DRAWS]++;
        ProfRowX *row = scope_cur (T);
        if (row)
            row->r.draws++;
    }
}

static inline void prof_upload (ProfThread *T, int cnt, int bytes_c, gsize bytes)
{
    if (!T || !T->counting)
        return;
    if (T->in_frame) {
        if (T->row_update && cnt == WALPE_PROF_C_TEX_UPLOADS)
            update_split (T, FALSE);
        T->mon.counters[cnt]++;
        T->mon.counters[bytes_c] += bytes;
        ProfRowX *row = scope_cur (T);
        if (row)
            row->r.upload_bytes += bytes;
    } else {
        T->mon.load_upload_bytes += bytes;
    }
}

static void w_glDrawArrays (GLenum mode, GLint first, GLsizei count)
{
    PROF_T ();
    prof_draw (T);
    o_glDrawArrays (mode, first, count);
}

static void w_glDrawElements (GLenum mode, GLsizei count, GLenum type, const void *indices)
{
    PROF_T ();
    prof_draw (T);
    o_glDrawElements (mode, count, type, indices);
}

static void w_glDrawArraysInstanced (GLenum mode, GLint first, GLsizei count, GLsizei n)
{
    PROF_T ();
    prof_draw (T);
    o_glDrawArraysInstanced (mode, first, count, n);
}

static void w_glDrawElementsInstanced (GLenum mode, GLsizei count, GLenum type,
                                       const void *indices, GLsizei n)
{
    PROF_T ();
    prof_draw (T);
    o_glDrawElementsInstanced (mode, count, type, indices, n);
}

static void w_glClear (GLbitfield mask)
{
    PROF_T ();
    PROF_C (T, WALPE_PROF_C_CLEARS, 1);
    o_glClear (mask);
}

static void w_glBlitFramebuffer (GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0,
                                 GLint dx1, GLint dy1, GLbitfield mask, GLenum filter)
{
    PROF_T ();
    PROF_C (T, WALPE_PROF_C_BLITS, 1);
    PROF_C (T, WALPE_PROF_C_BLIT_PIXELS, (gint64) ABS (dx1 - dx0) * ABS (dy1 - dy0));
    o_glBlitFramebuffer (sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter);
}

static void w_glBindFramebuffer (GLenum target, GLuint fb)
{
    PROF_T ();
    if (target != GL_READ_FRAMEBUFFER)
        PROF_C (T, WALPE_PROF_C_FBO_BINDS, 1);
    o_glBindFramebuffer (target, fb);
}

static void w_glTexImage2D (GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h,
                            GLint border, GLenum format, GLenum type, const void *pixels)
{
    PROF_T ();
    if (pixels)
        prof_upload (T, WALPE_PROF_C_TEX_UPLOADS, WALPE_PROF_C_TEX_UPLOAD_BYTES,
                     walpe_prof_image_bytes (format, type, w, h));
    o_glTexImage2D (target, level, ifmt, w, h, border, format, type, pixels);
}

static void w_glTexSubImage2D (GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h,
                               GLenum format, GLenum type, const void *pixels)
{
    PROF_T ();
    prof_upload (T, WALPE_PROF_C_TEX_UPLOADS, WALPE_PROF_C_TEX_UPLOAD_BYTES,
                 walpe_prof_image_bytes (format, type, w, h));
    o_glTexSubImage2D (target, level, x, y, w, h, format, type, pixels);
}

static void w_glCompressedTexImage2D (GLenum target, GLint level, GLenum ifmt, GLsizei w,
                                      GLsizei h, GLint border, GLsizei size, const void *data)
{
    PROF_T ();
    if (data && size > 0)
        prof_upload (T, WALPE_PROF_C_TEX_UPLOADS, WALPE_PROF_C_TEX_UPLOAD_BYTES, (gsize) size);
    o_glCompressedTexImage2D (target, level, ifmt, w, h, border, size, data);
}

static void w_glBufferData (GLenum target, GLsizeiptr size, const void *data, GLenum usage)
{
    PROF_T ();
    if (data && size > 0)
        prof_upload (T, WALPE_PROF_C_BUF_UPLOADS, WALPE_PROF_C_BUF_UPLOAD_BYTES, (gsize) size);
    o_glBufferData (target, size, data, usage);
}

static void w_glBufferSubData (GLenum target, GLintptr off, GLsizeiptr size, const void *data)
{
    PROF_T ();
    if (size > 0)
        prof_upload (T, WALPE_PROF_C_BUF_UPLOADS, WALPE_PROF_C_BUF_UPLOAD_BYTES, (gsize) size);
    o_glBufferSubData (target, off, size, data);
}

static void w_glReadPixels (GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type,
                            void *pixels)
{
    PROF_T ();
    PROF_C (T, WALPE_PROF_C_READBACKS, 1);
    o_glReadPixels (x, y, w, h, format, type, pixels);
}

static void w_glGetTexImage (GLenum target, GLint level, GLenum format, GLenum type, void *pixels)
{
    PROF_T ();
    PROF_C (T, WALPE_PROF_C_READBACKS, 1);
    o_glGetTexImage (target, level, format, type, pixels);
}

static void w_glFinish (void)
{
    PROF_T ();
    PROF_C (T, WALPE_PROF_C_FINISHES, 1);
    o_glFinish ();
}

static void w_glFlush (void)
{
    PROF_T ();
    PROF_C (T, WALPE_PROF_C_FLUSHES, 1);
    o_glFlush ();
}

static void w_glGenerateMipmap (GLenum target)
{
    PROF_T ();
    PROF_C (T, WALPE_PROF_C_MIPGENS, 1);
    o_glGenerateMipmap (target);
}

static void w_glCopyTexSubImage2D (GLenum target, GLint level, GLint xo, GLint yo, GLint x,
                                   GLint y, GLsizei w, GLsizei h)
{
    PROF_T ();
    PROF_C (T, WALPE_PROF_C_TEX_COPIES, 1);
    o_glCopyTexSubImage2D (target, level, xo, yo, x, y, w, h);
}

static void w_glLinkProgram (GLuint program)
{
    PROF_T ();
    PROF_C (T, WALPE_PROF_C_PROGRAM_LINKS, 1);
    o_glLinkProgram (program);
}

/* not counted: the size a quad drawn next is measured against */
static void w_glViewport (GLint x, GLint y, GLsizei w, GLsizei h)
{
    PROF_T ();
    if (T && T->in_frame) {
        T->vw = w;
        T->vh = h;
    }
    o_glViewport (x, y, w, h);
}

typedef void (*ProfFn) (void);
typedef struct {
    const char *name;
    void *slot;        /* &epoxy_glFoo */
    ProfFn wrap;
    void *orig;        /* &o_glFoo */
    ProfFn saved;      /* what the slot held before */
    gboolean on;
} ProfHook;

#define PROF_HOOK(fn) { #fn, &epoxy_##fn, (ProfFn) w_##fn, &o_##fn, NULL, FALSE }
static ProfHook prof_hooks[] = {
    PROF_HOOK (glDrawArrays), PROF_HOOK (glDrawElements), PROF_HOOK (glDrawArraysInstanced),
    PROF_HOOK (glDrawElementsInstanced), PROF_HOOK (glClear), PROF_HOOK (glBlitFramebuffer),
    PROF_HOOK (glBindFramebuffer), PROF_HOOK (glTexImage2D), PROF_HOOK (glTexSubImage2D),
    PROF_HOOK (glCompressedTexImage2D), PROF_HOOK (glBufferData), PROF_HOOK (glBufferSubData),
    PROF_HOOK (glReadPixels), PROF_HOOK (glGetTexImage), PROF_HOOK (glFinish), PROF_HOOK (glFlush),
    PROF_HOOK (glGenerateMipmap), PROF_HOOK (glCopyTexSubImage2D), PROF_HOOK (glLinkProgram),
    PROF_HOOK (glViewport),
};

/* the slots are function pointers of different types: read and written as
 * bytes (one aligned pointer store each; readers see the old or the new) */
static inline ProfFn slot_get (void *slot)
{
    ProfFn f;
    memcpy (&f, slot, sizeof (f));
    return f;
}

static inline void slot_set (void *slot, ProfFn f)
{
    memcpy (slot, &f, sizeof (f));
}

/* the profiler's own glFinish (sync mode): not one of the renderer's */
static void prof_finish (void)
{
    if (P.hooks_installed && o_glFinish)
        o_glFinish ();
    else
        glFinish ();
}

/* main thread. FALSE = the real entry points cannot be looked up here */
static gboolean hooks_install (void)
{
    if (P.no_hooks)
        return FALSE;
    /* all or nothing: the real entry points first */
    ProfFn real[G_N_ELEMENTS (prof_hooks)];
    for (guint i = 0; i < G_N_ELEMENTS (prof_hooks); i++) {
        real[i] = (ProfFn) eglGetProcAddress (prof_hooks[i].name);
        if (!real[i]) {
            g_message ("gpuprof: no address for %s: draw / upload counters are off",
                       prof_hooks[i].name);
            return FALSE;
        }
    }
    for (guint i = 0; i < G_N_ELEMENTS (prof_hooks); i++) {
        ProfHook *h = &prof_hooks[i];
        slot_set (h->orig, real[i]);
        h->saved = slot_get (h->slot);
        slot_set (h->slot, h->wrap);
        h->on = TRUE;
    }
    return TRUE;
}

static void hooks_remove (void)
{
    for (guint i = 0; i < G_N_ELEMENTS (prof_hooks); i++) {
        ProfHook *h = &prof_hooks[i];
        if (!h->on)
            continue;
        /* libepoxy resolves an entry point at its first call and stores
         * the result in the slot: if that happened meanwhile (the slot is
         * not ours any more) its value is the one to keep */
        if (slot_get (h->slot) == h->wrap)
            slot_set (h->slot, h->saved);
        h->on = FALSE;
    }
}

/* render thread, once per frame: a slot libepoxy rewrote (see above) gets
 * the wrapper again */
static void hooks_check (void)
{
    for (guint i = 0; i < G_N_ELEMENTS (prof_hooks); i++) {
        ProfHook *h = &prof_hooks[i];
        if (h->on && slot_get (h->slot) != h->wrap)
            slot_set (h->slot, h->wrap);
    }
}

/* ============================================== 4. KHR_debug channel */

static void prof_debug_cb (GLenum source, GLenum type, GLuint id, GLenum severity,
                           GLsizei length, const GLchar *message, const void *user)
{
    char *text = message ? (length > 0 ? g_strndup (message, (gsize) length) : g_strdup (message))
                         : NULL;
    g_mutex_lock (&P.lock);
    if (P.msgs_open)
        walpe_prof_msg_add (P.msgs, &P.n_msgs, PROF_MAX_MSGS, &P.msgs_dropped, source, type,
                            severity, id, text, GPOINTER_TO_INT (user));
    g_mutex_unlock (&P.lock);
    g_free (text);
}

static void debug_install (ProfThread *T)
{
    WalpeProfMon *m = &T->mon;
    gboolean core = (!m->gl_es && m->gl_ver >= 43) || (m->gl_es && m->gl_ver >= 32);
    gboolean ext = epoxy_has_gl_extension ("GL_KHR_debug");
    T->dbg_installed = FALSE;
    if (!core && !ext) {
        g_strlcpy (m->debug, "none (this context has no KHR_debug)", sizeof (m->debug));
        return;
    }
    gboolean suffixed = m->gl_es && !core;   /* GLES < 3.2: the ...KHR entry points */
    T->dbg_was_enabled = glIsEnabled (GL_DEBUG_OUTPUT);
    gpointer user = GINT_TO_POINTER (m->monitor);
    if (suffixed) {
        glDebugMessageCallbackKHR ((GLDEBUGPROCKHR) prof_debug_cb, user);
        glDebugMessageControlKHR (GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, NULL, GL_TRUE);
    } else {
        glDebugMessageCallback ((GLDEBUGPROC) prof_debug_cb, user);
        glDebugMessageControl (GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, NULL, GL_TRUE);
    }
    glEnable (GL_DEBUG_OUTPUT);
    GLint flags = 0;
    if (!m->gl_es || m->gl_ver >= 32)
        glGetIntegerv (GL_CONTEXT_FLAGS, &flags);
    prof_clear_errors ();
    T->dbg_installed = TRUE;
    g_snprintf (m->debug, sizeof (m->debug), "KHR_debug on, %s",
                (flags & GL_CONTEXT_FLAG_DEBUG_BIT)
                    ? "debug context"
                    : "no debug context (the driver may say less; WALPE_GL_DEBUG=1)");
}

static void debug_remove (ProfThread *T)
{
    if (!T->dbg_installed)
        return;
    gboolean suffixed = T->mon.gl_es && T->mon.gl_ver < 32;
    if (suffixed)
        glDebugMessageCallbackKHR (NULL, NULL);
    else
        glDebugMessageCallback (NULL, NULL);
    if (!T->dbg_was_enabled)
        glDisable (GL_DEBUG_OUTPUT);
    T->dbg_installed = FALSE;
}

/* ============================================== 5. memory snapshot */

static int mem_fmt_bpp (GLint f)
{
    switch (f) {
    case GL_R8: return 1;
    case GL_RG8: case GL_R16F: case GL_R16: return 2;
    case GL_RGB8: case GL_SRGB8: return 3;
    case GL_RGBA16F: case GL_RGBA16: return 8;
    case GL_RGB16F: return 6;
    case GL_RGBA32F: return 16;
    default: return 4;
    }
}

#define NVX_VIDMEM_TOTAL 0x9048       /* GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX */
#define NVX_VIDMEM_AVAIL 0x9049       /* GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX */
#define NVX_EVICTION_COUNT 0x904A     /* GPU_MEMORY_INFO_EVICTION_COUNT_NVX */

/* textures, render targets and buffers of this context. GL cannot list its
 * objects: the name space is probed, as walpe_gl_mem_report does. Texture
 * levels and framebuffer attachments are only queried on desktop GL (GLES
 * has no such queries for all of it): on GLES the categories stay empty
 * except the effect targets. Runs once, at the end of a run. */
static void mem_snapshot (ProfThread *T)
{
    if (!T->tex)
        T->tex = g_array_new (FALSE, TRUE, sizeof (ProfTexEnt));
    if (!T->buf)
        T->buf = g_array_new (FALSE, TRUE, sizeof (ProfBufEnt));
    g_array_set_size (T->tex, 0);
    g_array_set_size (T->buf, 0);
    T->mon.vram_evictions = -1;
    /* (a software rasterizer answers with the machine's RAM: left out) */
    if (!walpe_gl_is_software () && epoxy_has_gl_extension ("GL_NVX_gpu_memory_info")) {
        GLint v = 0;
        glGetIntegerv (NVX_VIDMEM_TOTAL, &v);
        T->mon.vram_total_kb = v;
        v = 0;
        glGetIntegerv (NVX_VIDMEM_AVAIL, &v);
        T->mon.vram_avail_kb = v;
        v = 0;
        glGetIntegerv (NVX_EVICTION_COUNT, &v);
        T->mon.vram_evictions = T->evict0 >= 0 ? MAX (0, (gint64) v - T->evict0) : -1;
    }
    if (T->mon.gl_es)
        goto done;

    GLint prev_tex = 0, prev_read = 0, prev_buf = 0;
    glGetIntegerv (GL_TEXTURE_BINDING_2D, &prev_tex);
    glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &prev_read);
    glGetIntegerv (GL_COPY_READ_BUFFER_BINDING, &prev_buf);

    /* textures some framebuffer of this context renders into */
    GHashTable *targets = g_hash_table_new (g_direct_hash, g_direct_equal);
    for (GLuint name = 1, misses = 0; misses < 1024; name++) {
        if (!glIsFramebuffer (name)) {
            misses++;
            continue;
        }
        misses = 0;
        glBindFramebuffer (GL_READ_FRAMEBUFFER, name);
        GLint type = GL_NONE, obj = 0;
        glGetFramebufferAttachmentParameteriv (GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                               GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
        if (type == GL_TEXTURE) {
            glGetFramebufferAttachmentParameteriv (GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                                   GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &obj);
            if (obj > 0)
                g_hash_table_add (targets, GUINT_TO_POINTER ((guint) obj));
        }
    }
    glBindFramebuffer (GL_READ_FRAMEBUFFER, (GLuint) prev_read);

    for (GLuint name = 1, misses = 0; misses < 4096; name++) {
        if (!glIsTexture (name)) {
            misses++;
            continue;
        }
        misses = 0;
        prof_clear_errors ();
        glBindTexture (GL_TEXTURE_2D, name);
        if (glGetError () != GL_NO_ERROR)
            continue;   /* not a 2D texture */
        ProfTexEnt e = { name, 0, 0, 0 };
        for (GLint lv = 0; lv < 16; lv++) {
            GLint w = 0, h = 0, f = 0, c = 0, cs = 0;
            glGetTexLevelParameteriv (GL_TEXTURE_2D, lv, GL_TEXTURE_WIDTH, &w);
            glGetTexLevelParameteriv (GL_TEXTURE_2D, lv, GL_TEXTURE_HEIGHT, &h);
            if (w < 1 || h < 1)
                break;
            glGetTexLevelParameteriv (GL_TEXTURE_2D, lv, GL_TEXTURE_INTERNAL_FORMAT, &f);
            glGetTexLevelParameteriv (GL_TEXTURE_2D, lv, GL_TEXTURE_COMPRESSED, &c);
            if (c) {
                glGetTexLevelParameteriv (GL_TEXTURE_2D, lv, GL_TEXTURE_COMPRESSED_IMAGE_SIZE, &cs);
                e.bytes += cs;
                e.compressed = 1;
            } else {
                e.bytes += (gint64) w * h * mem_fmt_bpp (f);
            }
        }
        e.target = g_hash_table_contains (targets, GUINT_TO_POINTER (name)) ? 1 : 0;
        g_array_append_val (T->tex, e);
    }
    glBindTexture (GL_TEXTURE_2D, (GLuint) prev_tex);
    g_hash_table_destroy (targets);

    for (GLuint name = 1, misses = 0; misses < 1024; name++) {
        if (!glIsBuffer (name)) {
            misses++;
            continue;
        }
        misses = 0;
        GLint size = 0;
        glBindBuffer (GL_COPY_READ_BUFFER, name);
        glGetBufferParameteriv (GL_COPY_READ_BUFFER, GL_BUFFER_SIZE, &size);
        ProfBufEnt e = { name, size };
        g_array_append_val (T->buf, e);
    }
    glBindBuffer (GL_COPY_READ_BUFFER, (GLuint) prev_buf);
done:
    prof_clear_errors ();
}

/* ============================================== 6. hooks */

static void info_copy (ProfThread *T, const WalpeProfThreadInfo *info)
{
    WalpeProfMon *m = &T->mon;
    m->monitor = info->monitor;
    prof_copy (m->name, sizeof (m->name), info->name);
    m->w = info->w;
    m->h = info->h;
    m->hz = info->hz;
    prof_copy (m->type, sizeof (m->type), info->type);
    prof_copy (m->role, sizeof (m->role), info->role);
    /* of the wallpaper only a workshop id (an all-digit folder name) */
    m->wallpaper[0] = 0;
    if (info->path && *info->path) {
        char *base = g_path_get_basename (info->path);
        gboolean digits = *base != 0;
        for (const char *p = base; *p; p++)
            digits &= g_ascii_isdigit (*p);
        prof_copy (m->wallpaper, sizeof (m->wallpaper), digits ? base : "(local item)");
        g_free (base);
    }
}

/* nothing of an earlier run stays in the context: an open query, queries
 * not read back, the debug callback */
static void thread_gl_cleanup (ProfThread *T)
{
    if (T->q_active) {
        glEndQuery (GL_TIME_ELAPSED);
        T->q_active = FALSE;
    }
    T->in_frame = FALSE;
    ProfBatch *b;
    while ((b = g_queue_pop_head (&T->pending)))
        batch_free (T, b, FALSE);
    if (T->cur && T->cur->len) {
        for (guint i = 0; i < T->cur->len; i++)
            glDeleteQueries (1, &g_array_index (T->cur, ProfSeg, i).q);
        g_array_set_size (T->cur, 0);
    }
    if (T->q_free && T->q_free->len) {
        glDeleteQueries ((GLsizei) T->q_free->len, (const GLuint *) T->q_free->data);
        g_array_set_size (T->q_free, 0);
    }
    debug_remove (T);
}

static void thread_attach (ProfThread *T, const WalpeProfThreadInfo *info)
{
    thread_gl_cleanup (T);
    thread_reset (T);
    WalpeProfMon *m = &T->mon;
    info_copy (T, info);
    prof_copy (m->gl_vendor, sizeof (m->gl_vendor), (const char *) glGetString (GL_VENDOR));
    prof_copy (m->gl_renderer, sizeof (m->gl_renderer), (const char *) glGetString (GL_RENDERER));
    prof_copy (m->gl_version, sizeof (m->gl_version), (const char *) glGetString (GL_VERSION));
    m->gl_es = !epoxy_is_desktop_gl ();
    m->gl_ver = epoxy_gl_version ();
    tq_detect (T);
    m->wall_timer = T->tq_mode == 3 || walpe_gl_is_software ();
    debug_install (T);
    m->hooks_ok = P.hooks_installed;
    T->fx_run0 = info->fx_run;
    T->fx_cached0 = info->fx_cached;
    T->evict0 = -1;
    if (epoxy_has_gl_extension ("GL_NVX_gpu_memory_info")) {
        GLint v = 0;
        glGetIntegerv (NVX_EVICTION_COUNT, &v);
        T->evict0 = v;
    }
    prof_clear_errors ();
    T->t_attach_ns = prof_ns ();
    T->attached = TRUE;
    T->counting = TRUE;
    m->attached = TRUE;
}

static void thread_finalize (ProfThread *T, const WalpeProfThreadInfo *info)
{
    WalpeProfMon *m = &T->mon;
    T->counting = FALSE;
    if (T->q_active) {
        glEndQuery (GL_TIME_ELAPSED);
        T->q_active = FALSE;
    }
    T->in_frame = FALSE;
    tq_resolve (T, TRUE);
    thread_gl_cleanup (T);   /* also before the snapshot: its probing raises GL errors */
    info_copy (T, info);
    /* the run on this thread: from its attach to the end of the run, or
     * to the end of the frame that was being rendered then (it is counted) */
    gint64 t_end = __atomic_load_n (&P.t_stop_ns, __ATOMIC_RELAXED);
    if (!t_end || t_end > prof_ns ())
        t_end = prof_ns ();
    if (T->frame_t1 > t_end)
        t_end = T->frame_t1;
    m->seconds = MAX (0, t_end - T->t_attach_ns) / 1e9;
    {
        /* the time since the last drawn frame, when it is long */
        gint64 ref = T->last_drawn_ns ? T->last_drawn_ns : T->t_attach_ns;
        gint64 idle = T->idle_ns;
        if (t_end - ref > (gint64) (WALPE_PROF_IDLE_GAP_S * 1e9))
            idle += t_end - ref;
        m->idle_seconds = CLAMP (idle / 1e9, 0.0, m->seconds);
    }
    m->fx_run = info->fx_run >= T->fx_run0 ? info->fx_run - T->fx_run0 : info->fx_run;
    m->fx_cached = info->fx_cached >= T->fx_cached0 ? info->fx_cached - T->fx_cached0
                                                    : info->fx_cached;
    m->mem_bytes[WALPE_PROF_MEM_FX_TARGETS] = info->fx_bytes;
    m->mem_count[WALPE_PROF_MEM_FX_TARGETS] = info->fx_count;
    m->mem_bytes[WALPE_PROF_MEM_SURFACES] = (gint64) info->w * info->h * 4 * 2;
    m->mem_count[WALPE_PROF_MEM_SURFACES] = 2;
    mem_snapshot (T);
    g_mutex_lock (&P.lock);
    T->finalized = TRUE;
    g_mutex_unlock (&P.lock);
}

void walpe_prof_thread_sync (const WalpeProfThreadInfo *info)
{
    int state = g_atomic_int_get (&P.state);
    guint gen = (guint) g_atomic_int_get (&P.gen);
    ProfThread *T = prof_tls;
    if (state == PROF_RUNNING) {
        if (!T) {
            T = g_new0 (ProfThread, 1);
            g_queue_init (&T->pending);
            prof_tls = T;
            g_mutex_lock (&P.lock);
            g_ptr_array_add (P.threads, T);
            g_mutex_unlock (&P.lock);
        }
        if (T->gen != gen || !T->attached) {
            g_mutex_lock (&P.lock);
            T->finalized = FALSE;
            T->gen = gen;
            g_mutex_unlock (&P.lock);
            thread_attach (T, info);
        }
    } else if (T && T->attached && !T->finalized) {
        if (state == PROF_COLLECTING && T->gen == gen) {
            thread_finalize (T, info);
        } else {
            /* too late for its run's report: just leave the context clean */
            T->counting = FALSE;
            thread_gl_cleanup (T);
            T->attached = FALSE;
        }
    }
}

void walpe_prof_thread_exit (void)
{
    ProfThread *T = prof_tls;
    if (!T)
        return;
    prof_tls = NULL;
    g_mutex_lock (&P.lock);
    gboolean in_use = g_atomic_int_get (&P.state) == PROF_COLLECTING &&
                      T->gen == (guint) g_atomic_int_get (&P.gen) && T->finalized;
    if (!in_use)
        g_ptr_array_remove (P.threads, T);
    g_mutex_unlock (&P.lock);
    if (in_use)
        return;   /* the collector is reading it: left to it (a few KiB, once) */
    thread_gl_cleanup (T);
    if (T->q_free)
        g_array_unref (T->q_free);
    if (T->cur)
        g_array_unref (T->cur);
    if (T->tex)
        g_array_unref (T->tex);
    if (T->buf)
        g_array_unref (T->buf);
    if (T->row_by_key)
        g_hash_table_destroy (T->row_by_key);
    if (T->rows)
        g_ptr_array_unref (T->rows);
    g_free (T);
}

static inline ProfThread *prof_live (void)
{
    ProfThread *T = prof_tls;
    return T && T->counting ? T : NULL;
}

static void stage_close (ProfThread *T, gint64 now, gint64 cpu)
{
    T->mon.stage_wall_ns[T->stage] += (guint64) MAX (0, now - T->stage_t0);
    T->mon.stage_cpu_ns[T->stage] += (guint64) MAX (0, cpu - T->stage_c0);
}

void walpe_prof_frame_begin (int monitor, int w, int h)
{
    ProfThread *T = prof_live ();
    (void) monitor;
    if (!T || T->in_frame || T->gen != (guint) g_atomic_int_get (&P.gen) ||
        g_atomic_int_get (&P.state) != PROF_RUNNING)
        return;
    gint64 t0 = prof_ns (), c0 = prof_thread_cpu_ns ();
    tq_resolve (T, FALSE);
    if (P.hooks_installed)
        hooks_check ();
    gint64 t1 = prof_ns (), c1 = prof_thread_cpu_ns ();
    T->mon.stage_wall_ns[WALPE_PROF_ST_PROFILER] += (guint64) (t1 - t0);
    T->mon.stage_cpu_ns[WALPE_PROF_ST_PROFILER] += (guint64) MAX (0, c1 - c0);
    T->fw = T->vw = w;
    T->fh = T->vh = h;
    T->frame_no++;
    T->mon.frames++;
    T->sampled = T->tq_mode != 0 && (P.every <= 1 || T->frame_no % P.every == 0);
    T->sync_frame_ns = 0;
    T->sync_row = NULL;
    T->depth = 0;
    g_array_set_size (T->cur, 0);
    T->in_frame = TRUE;
    T->frame_skipped = FALSE;
    T->frame_t0 = t0;
    T->row_update = NULL;
    T->stage = WALPE_PROF_ST_RENDER;
    T->stage_t0 = t1;
    T->stage_c0 = c1;
    T->row_frame->r.tw = w;
    T->row_frame->r.th = h;
    scope_push (T, T->row_frame);
}

/* no GPU timer runs from here on (the swap waits for the display) */
static void scopes_close (ProfThread *T)
{
    while (T->depth > 0)
        scope_pop (T);
    if (T->q_active) {
        glEndQuery (GL_TIME_ELAPSED);
        T->q_active = FALSE;
    }
}

void walpe_prof_frame_end (gboolean presented)
{
    ProfThread *T = prof_tls;
    if (!T || !T->in_frame)
        return;
    scopes_close (T);
    gint64 now = prof_ns ();
    stage_close (T, now, prof_thread_cpu_ns ());
    if (now - T->frame_t0 > __atomic_load_n (&P.max_frame_ns, __ATOMIC_RELAXED))
        __atomic_store_n (&P.max_frame_ns, now - T->frame_t0, __ATOMIC_RELAXED);
    T->frame_t1 = now;
    T->in_frame = FALSE;
    if (presented)
        T->mon.presented++;
    /* a wake-up that found nothing to draw and presented nothing is not a
     * frame: what it cost stays in the totals, spread over the real frames */
    gboolean skipped = T->frame_skipped && !presented;
    if (skipped && T->mon.frames > 0)
        T->mon.frames--;
    T->row_update = NULL;
    if (!skipped) {
        /* a long gap since the last drawn frame (or since the run
         * began on this thread) is time in which nothing was drawn */
        gint64 ref = T->last_drawn_ns ? T->last_drawn_ns : T->t_attach_ns;
        gboolean gap = T->frame_t0 - ref > (gint64) (WALPE_PROF_IDLE_GAP_S * 1e9);
        if (gap)
            T->idle_ns += T->frame_t0 - ref;
        if (gap || !T->last_drawn_ns)
            T->mon.draw_runs++;   /* a run of frames begins */
        T->last_drawn_ns = now;
    }
    T->frame_skipped = FALSE;
    if (T->tq_mode == 3 && T->sampled) {
        T->mon.gpu_ns += T->sync_frame_ns;
        if (!skipped)
            T->mon.gpu_frames++;
        if (T->sync_frame_ns > T->mon.gpu_max_ns)
            T->mon.gpu_max_ns = T->sync_frame_ns;
    }
    T->sync_frame_ns = 0;
    T->sync_row = NULL;
    if (T->cur->len) {
        ProfBatch *b = g_new0 (ProfBatch, 1);
        b->skipped = skipped;
        b->n = T->cur->len;
        b->seg = g_memdup2 (T->cur->data, sizeof (ProfSeg) * b->n);
        g_queue_push_tail (&T->pending, b);
        g_array_set_size (T->cur, 0);
    }
}

void walpe_prof_stage (int stage)
{
    ProfThread *T = prof_tls;
    if (!T || !T->in_frame || stage < 0 || stage >= WALPE_PROF_N_STAGES)
        return;
    gint64 now = prof_ns (), cpu = prof_thread_cpu_ns ();
    stage_close (T, now, cpu);
    T->stage = stage;
    T->stage_t0 = now;
    T->stage_c0 = cpu;
    T->row_update = NULL;   /* set again below for the update stage */
    if (stage == WALPE_PROF_ST_SWAP) {
        scopes_close (T);
        return;
    }
    /* what is drawn outside the layers has a row per stage (a layer whose
     * end was not marked ends here too) */
    static const struct { int stage, id; const char *label; } rows[] = {
        { WALPE_PROF_ST_RENDER, PROF_ROW_FRAME, "(frame)" },
        { WALPE_PROF_ST_UPDATE, PROF_ROW_UPDATE, "(scene update, uploads)" },
        { WALPE_PROF_ST_LAYERS, PROF_ROW_BETWEEN, "(between layers)" },
        { WALPE_PROF_ST_POST, PROF_ROW_POST, "(after the layers)" },
        { WALPE_PROF_ST_PUBLISH, PROF_ROW_PUBLISH, "(shared frame: publish)" },
        { WALPE_PROF_ST_PRESENT, PROF_ROW_PRESENT, "(shared frame: present)" },
    };
    for (guint i = 0; i < G_N_ELEMENTS (rows); i++) {
        if (rows[i].stage != stage)
            continue;
        while (T->depth > 0)
            scope_pop (T);
        ProfRowX *row = row_get (T, rows[i].id, -1, -1, T->fw, T->fh, FALSE);
        if (!row->r.layer[0])
            g_strlcpy (row->r.layer, rows[i].label, sizeof (row->r.layer));
        scope_push (T, row);
        if (stage == WALPE_PROF_ST_UPDATE && row != T->row_other)
            T->row_update = row;
        break;
    }
}

void walpe_prof_frame_skipped (int monitor)
{
    ProfThread *T = prof_live ();
    (void) monitor;
    if (T && g_atomic_int_get (&P.state) == PROF_RUNNING) {
        T->mon.skipped++;
        if (T->in_frame)
            T->frame_skipped = TRUE;
    }
}

void walpe_prof_layer_begin (int index, const char *name, const char *kind)
{
    ProfThread *T = prof_tls;
    if (!T || !T->in_frame)
        return;
    /* layers do not nest: a layer whose end was not marked (a `continue`
     * added to the loop) ends here */
    while (T->depth > 1)
        scope_pop (T);
    ProfRowX *row = row_get (T, CLAMP (index, 0, 16000), -1, -1, T->fw, T->fh, FALSE);
    if (!row->r.layer[0] && row != T->row_other) {
        if (name && *name)
            prof_copy (row->r.layer, sizeof (row->r.layer), name);
        else
            g_snprintf (row->r.layer, sizeof (row->r.layer), "layer %d", index);
        prof_copy (row->r.kind, sizeof (row->r.kind), kind ? kind : "");
    }
    scope_push (T, row);
}

void walpe_prof_layer_end (void)
{
    ProfThread *T = prof_tls;
    /* depth 1 is the frame's own row: never popped by a layer */
    if (!T || !T->in_frame)
        return;
    while (T->depth > 1)
        scope_pop (T);
}

void walpe_prof_pass_begin (const char *effect, int inst, int pass, int tw, int th)
{
    ProfThread *T = prof_tls;
    if (!T || !T->in_frame)
        return;
    ProfRowX *parent = scope_cur (T);
    if (parent && parent->is_pass && T->depth > 1) {
        scope_pop (T);   /* passes do not nest either */
        parent = scope_cur (T);
    }
    int li = parent && parent->r.layer_index >= 0 ? parent->r.layer_index : PROF_ROW_FRAME;
    ProfRowX *row = row_get (T, li, CLAMP (inst, -1, 250), CLAMP (pass, -1, 250), tw, th, TRUE);
    if (!row->r.effect[0] && row != T->row_other) {
        if (parent) {
            memcpy (row->r.layer, parent->r.layer, sizeof (row->r.layer));
            memcpy (row->r.kind, parent->r.kind, sizeof (row->r.kind));
        }
        prof_copy (row->r.effect, sizeof (row->r.effect), effect && *effect ? effect : "effect");
    }
    guint64 px = (guint64) MAX (0, tw) * (guint64) MAX (0, th);
    row->r.pixels += px;
    T->mon.counters[WALPE_PROF_C_PIXELS] += px;
    T->mon.counters[WALPE_PROF_C_PASSES]++;
    scope_push (T, row);
}

/* the pass opened last shaded only drawn_px of the full_px its begin
 * counted (a pass restricted to a part of its target: fx_region.h) */
void walpe_prof_pass_region (guint64 full_px, guint64 drawn_px)
{
    ProfThread *T = prof_tls;
    if (!T || !T->in_frame || drawn_px >= full_px)
        return;
    ProfRowX *row = scope_cur (T);
    if (!row || !row->is_pass)
        return;
    guint64 d = full_px - drawn_px;
    row->r.pixels -= MIN (row->r.pixels, d);
    T->mon.counters[WALPE_PROF_C_PIXELS] -= MIN (T->mon.counters[WALPE_PROF_C_PIXELS], d);
}

void walpe_prof_pass_end (void)
{
    ProfThread *T = prof_tls;
    if (!T || !T->in_frame || T->depth <= 1)
        return;
    ProfRowX *row = scope_cur (T);
    if (row && row->is_pass)
        scope_pop (T);
}

void walpe_prof_quad (const float *mvp)
{
    ProfThread *T = prof_tls;
    if (!T || !T->in_frame)
        return;
    if (T->row_update)
        update_split (T, TRUE);   /* the quad belongs to the update stage's draw row */
    T->quad_seen = TRUE;
    ProfRowX *row = scope_cur (T);
    if (row && row->is_pass)
        return;   /* a pass fills its target: counted at its begin */
    /* against the target it is drawn into: the viewport (a video frame's
     * colour conversion fills a target of the video's size, not the frame's) */
    guint64 px = (guint64) walpe_prof_quad_pixels (mvp, T->vw > 0 ? T->vw : T->fw,
                                                   T->vh > 0 ? T->vh : T->fh);
    if (row)
        row->r.pixels += px;
    T->mon.counters[WALPE_PROF_C_PIXELS] += px;
}

/* ============================================== 7. control */

static gboolean prof_collect_cb (gpointer user);

/* main thread */
void walpe_prof_note_pause (int monitor, gboolean paused, const char *reason)
{
    if (monitor < 0 || monitor >= PROF_MAX_PAUSE_MONS ||
        g_atomic_int_get (&P.state) != PROF_RUNNING)
        return;
    gint64 now = prof_ns ();
    g_mutex_lock (&P.lock);
    if (paused && !P.pause[monitor].paused) {
        P.pause[monitor].paused = TRUE;
        P.pause[monitor].since_ns = now;
    } else if (!paused && P.pause[monitor].paused) {
        P.pause[monitor].paused = FALSE;
        P.pause[monitor].total_ns += MAX (0, now - P.pause[monitor].since_ns);
    }
    if (paused && reason && *reason && !strstr (P.pause[monitor].reason, reason)) {
        if (P.pause[monitor].reason[0])
            g_strlcat (P.pause[monitor].reason, ", ", sizeof (P.pause[monitor].reason));
        g_strlcat (P.pause[monitor].reason, reason, sizeof (P.pause[monitor].reason));
    }
    g_mutex_unlock (&P.lock);
}

void walpe_prof_set_waker (WalpeProfWaker wake, gpointer user)
{
    P.wake = wake;
    P.wake_user = user;
}

int walpe_prof_state (void)
{
    return g_atomic_int_get (&P.state);
}

double walpe_prof_remaining (void)
{
    if (g_atomic_int_get (&P.state) != PROF_RUNNING)
        return 0.0;
    double left = P.seconds - (g_get_monotonic_time () - P.t_start_us) / 1e6;
    return left > 0 ? left : 0.0;
}

char *walpe_prof_last_text (void)
{
    g_mutex_lock (&P.lock);
    char *s = g_strdup (P.text);
    g_mutex_unlock (&P.lock);
    return s;
}

char *walpe_prof_last_json (void)
{
    g_mutex_lock (&P.lock);
    char *s = g_strdup (P.json);
    g_mutex_unlock (&P.lock);
    return s;
}

const char *walpe_prof_last_text_path (void)
{
    return P.text_path;
}

const char *walpe_prof_last_json_path (void)
{
    return P.json_path;
}

static gboolean env_off (const char *name)
{
    const char *e = g_getenv (name);
    return e && (*e == '0' || !g_ascii_strcasecmp (e, "off") || !g_ascii_strcasecmp (e, "no"));
}

static gboolean prof_stop_cb (gpointer user)
{
    (void) user;
    P.stop_id = 0;
    if (g_atomic_int_get (&P.state) != PROF_RUNNING)
        return G_SOURCE_REMOVE;
    /* no new frame is profiled from here on; the hooks stay live until
     * every thread has handed in, so that a frame which is being rendered
     * right now ends as it began (rows closed, counters attributed) */
    __atomic_store_n (&P.t_stop_ns, prof_ns (), __ATOMIC_RELAXED);
    g_atomic_int_set (&P.state, PROF_COLLECTING);
    /* a thread hands in between two frames: wait a few of the longest
     * frame seen (seconds on a software rasterizer, a blink on a GPU) */
    gint64 longest_us = __atomic_load_n (&P.max_frame_ns, __ATOMIC_RELAXED) / 1000;
    P.collect_deadline_us = g_get_monotonic_time () +
                            CLAMP (5 * G_USEC_PER_SEC + 3 * longest_us, 5 * G_USEC_PER_SEC,
                                   120 * G_USEC_PER_SEC);
    if (P.wake)
        P.wake (P.wake_user);
    P.collect_id = g_timeout_add (25, prof_collect_cb, NULL);
    return G_SOURCE_REMOVE;
}

gboolean walpe_prof_start (double seconds, gboolean shared, guint flags, const char **why)
{
    int state = g_atomic_int_get (&P.state);
    if (state == PROF_RUNNING || state == PROF_COLLECTING) {
        if (why)
            *why = "a profile is running already";
        return FALSE;
    }
    if (!(seconds >= 1.0))
        seconds = 1.0;
    if (seconds > 600.0)
        seconds = 600.0;
    g_mutex_lock (&P.lock);
    if (!P.threads)
        P.threads = g_ptr_array_new ();
    P.n_msgs = P.msgs_dropped = 0;
    P.msgs_open = TRUE;
    memset (P.pause, 0, sizeof (P.pause));
    g_mutex_unlock (&P.lock);
    P.seconds = seconds;
    P.shared = shared;
    const char *es = g_getenv ("WALPE_GPU_PROFILE_SYNC");
    P.sync = (flags & WALPE_PROF_SYNC) != 0 || (es && *es && *es != '0');
    P.no_timer = env_off ("WALPE_GPU_PROFILE_TIMER");
    P.no_hooks = env_off ("WALPE_GPU_PROFILE_HOOKS");
    const char *ev = g_getenv ("WALPE_GPU_PROFILE_EVERY");
    P.every = ev ? (guint) CLAMP (atoi (ev), 1, 1000) : 1;
    P.t_start_us = g_get_monotonic_time ();
    __atomic_store_n (&P.max_frame_ns, 0, __ATOMIC_RELAXED);
    __atomic_store_n (&P.t_stop_ns, 0, __ATOMIC_RELAXED);
    P.hooks_installed = hooks_install ();
    g_atomic_int_inc (&P.gen);
    g_atomic_int_set (&P.state, PROF_RUNNING);
    __atomic_store_n (&walpe_prof_enabled_, 1, __ATOMIC_RELAXED);
    if (P.wake)
        P.wake (P.wake_user);   /* every render thread attaches now */
    P.stop_id = g_timeout_add ((guint) (seconds * 1000.0), prof_stop_cb, NULL);
    return TRUE;
}

static int mon_cmp (gconstpointer a, gconstpointer b)
{
    const WalpeProfMon *x = a, *y = b;
    return x->monitor - y->monitor;
}

/* memory of all threads into the per-monitor categories: with one share
 * group a texture / buffer name is the same object on every monitor and is
 * counted once, on the first monitor that reports it */
static void collect_memory (GPtrArray *threads, WalpeProfMon *mons, gboolean shared)
{
    GHashTable *seen_tex = g_hash_table_new (g_direct_hash, g_direct_equal);
    GHashTable *seen_buf = g_hash_table_new (g_direct_hash, g_direct_equal);
    /* a texture is a render target when any context renders into it */
    GHashTable *is_target = g_hash_table_new (g_direct_hash, g_direct_equal);
    if (shared)
        for (guint t = 0; t < threads->len; t++) {
            ProfThread *T = g_ptr_array_index (threads, t);
            for (guint i = 0; T->tex && i < T->tex->len; i++) {
                ProfTexEnt *e = &g_array_index (T->tex, ProfTexEnt, i);
                if (e->target)
                    g_hash_table_add (is_target, GUINT_TO_POINTER (e->name));
            }
        }
    gint64 fx_all = 0, tgt_all = 0;
    for (guint t = 0; t < threads->len; t++) {
        ProfThread *T = g_ptr_array_index (threads, t);
        WalpeProfMon *m = &mons[t];
        gint64 tgt_bytes = 0;
        int tgt_count = 0;
        for (guint i = 0; T->tex && i < T->tex->len; i++) {
            ProfTexEnt *e = &g_array_index (T->tex, ProfTexEnt, i);
            if (shared && !g_hash_table_add (seen_tex, GUINT_TO_POINTER (e->name)))
                continue;
            gboolean target = e->target ||
                              (shared && g_hash_table_contains (is_target, GUINT_TO_POINTER (e->name)));
            if (target) {
                tgt_bytes += e->bytes;
                tgt_count++;
            } else {
                int c = e->compressed ? WALPE_PROF_MEM_TEX_COMPRESSED : WALPE_PROF_MEM_TEX_PLAIN;
                m->mem_bytes[c] += e->bytes;
                m->mem_count[c]++;
            }
        }
        for (guint i = 0; T->buf && i < T->buf->len; i++) {
            ProfBufEnt *e = &g_array_index (T->buf, ProfBufEnt, i);
            if (shared && !g_hash_table_add (seen_buf, GUINT_TO_POINTER (e->name)))
                continue;
            m->mem_bytes[WALPE_PROF_MEM_BUFFERS] += e->bytes;
            m->mem_count[WALPE_PROF_MEM_BUFFERS]++;
        }
        /* render targets seen = effect targets (known exactly per thread)
         * + the rest; with a share group the split is done over the sum */
        m->mem_bytes[WALPE_PROF_MEM_OTHER_TARGETS] = tgt_bytes;
        m->mem_count[WALPE_PROF_MEM_OTHER_TARGETS] = tgt_count;
        m->mem_shared = shared && t == 0;
        fx_all += m->mem_bytes[WALPE_PROF_MEM_FX_TARGETS];
        tgt_all += tgt_bytes;
    }
    /* take the effect targets out of "other targets" (GLES: nothing was
     * enumerated, the effect targets stand alone) */
    gint64 fx_left = MIN (fx_all, tgt_all);
    int fxn_left = 0;
    for (guint t = 0; t < threads->len; t++)
        fxn_left += mons[t].mem_count[WALPE_PROF_MEM_FX_TARGETS];
    for (guint t = 0; t < threads->len && tgt_all > 0; t++) {
        WalpeProfMon *m = &mons[t];
        gint64 take = MIN (fx_left, m->mem_bytes[WALPE_PROF_MEM_OTHER_TARGETS]);
        m->mem_bytes[WALPE_PROF_MEM_OTHER_TARGETS] -= take;
        fx_left -= take;
        int tn = MIN (fxn_left, m->mem_count[WALPE_PROF_MEM_OTHER_TARGETS]);
        m->mem_count[WALPE_PROF_MEM_OTHER_TARGETS] -= tn;
        fxn_left -= tn;
    }
    g_hash_table_destroy (seen_tex);
    g_hash_table_destroy (seen_buf);
    g_hash_table_destroy (is_target);
}

static void prof_write (const char *dir, const char *name, const char *data, char **path_out)
{
    char *path = g_build_filename (dir, name, NULL);
    GError *err = NULL;
    if (!g_file_set_contents (path, data, -1, &err)) {
        g_warning ("gpuprof: %s: %s", path, err ? err->message : "write failed");
        g_clear_error (&err);
        g_free (path);
        path = NULL;
    }
    if (path_out) {
        g_free (*path_out);
        *path_out = path;
    } else {
        g_free (path);
    }
}

static gboolean prof_collect_cb (gpointer user)
{
    (void) user;
    guint gen = (guint) g_atomic_int_get (&P.gen);
    gboolean late = g_get_monotonic_time () >= P.collect_deadline_us;
    g_mutex_lock (&P.lock);
    guint waiting = 0;
    for (guint i = 0; i < P.threads->len; i++) {
        ProfThread *T = g_ptr_array_index (P.threads, i);
        if (T->gen == gen && T->attached && !T->finalized)
            waiting++;
    }
    if (waiting && !late) {
        g_mutex_unlock (&P.lock);
        return G_SOURCE_CONTINUE;
    }
    /* the finished threads: nothing else touches their state now */
    GPtrArray *done = g_ptr_array_new ();
    guint n_late = 0;
    for (guint i = 0; i < P.threads->len; i++) {
        ProfThread *T = g_ptr_array_index (P.threads, i);
        if (T->gen != gen || !T->attached)
            continue;
        if (T->finalized)
            g_ptr_array_add (done, T);
        else
            n_late++;
    }
    P.msgs_open = FALSE;
    g_mutex_unlock (&P.lock);
    P.collect_id = 0;
    /* every hook is one untaken branch again, libepoxy's pointers are its own */
    __atomic_store_n (&walpe_prof_enabled_, 0, __ATOMIC_RELAXED);
    hooks_remove ();
    P.hooks_installed = FALSE;

    WalpeProfReport r = { 0 };
    GDateTime *now = g_date_time_new_now_local ();
    char *when = g_date_time_format (now, "%Y-%m-%dT%H:%M:%S");
    char *stamp = g_date_time_format (now, "%Y%m%d-%H%M%S");
    g_date_time_unref (now);
    g_strlcpy (r.when, when, sizeof (r.when));
    g_free (when);
    g_strlcpy (r.version, WALPE_VERSION, sizeof (r.version));
    r.seconds = P.seconds;
    r.shared = P.shared;
    r.n_mons = done->len;
    r.mons = g_new0 (WalpeProfMon, done->len + 1);
    for (guint i = 0; i < done->len; i++) {
        ProfThread *T = g_ptr_array_index (done, i);
        r.mons[i] = T->mon;
        r.mons[i].n_rows = 0;
        r.mons[i].rows = g_new0 (WalpeProfRow, T->rows->len + 1);
        for (guint k = 0; k < T->rows->len; k++) {
            ProfRowX *x = g_ptr_array_index (T->rows, k);
            if (!x->r.runs && !x->r.gpu_ns)
                continue;   /* never entered (the catch-all row) */
            r.mons[i].rows[r.mons[i].n_rows++] = x->r;
        }
    }
    /* how long each monitor was paused during the run, and why */
    g_mutex_lock (&P.lock);
    gint64 t_stop = __atomic_load_n (&P.t_stop_ns, __ATOMIC_RELAXED);
    for (guint i = 0; i < r.n_mons; i++) {
        int idx = r.mons[i].monitor;
        if (idx < 0 || idx >= PROF_MAX_PAUSE_MONS)
            continue;
        gint64 tot = P.pause[idx].total_ns;
        if (P.pause[idx].paused && t_stop > P.pause[idx].since_ns)
            tot += t_stop - P.pause[idx].since_ns;
        r.mons[i].paused_seconds = CLAMP (tot / 1e9, 0.0, P.seconds);
        g_strlcpy (r.mons[i].pause_reason, P.pause[idx].reason, sizeof (r.mons[i].pause_reason));
    }
    g_mutex_unlock (&P.lock);
    collect_memory (done, r.mons, P.shared);
    g_ptr_array_unref (done);
    if (r.n_mons > 1)
        qsort (r.mons, r.n_mons, sizeof (r.mons[0]), mon_cmp);
    g_mutex_lock (&P.lock);
    r.msgs = g_memdup2 (P.msgs, sizeof (WalpeProfMsg) * MAX (1, P.n_msgs));
    r.n_msgs = P.n_msgs;
    r.msgs_dropped = P.msgs_dropped;
    g_mutex_unlock (&P.lock);

    char *text = walpe_prof_format_text (&r);
    if (n_late || (P.expected > 0 && (int) r.n_mons + (int) n_late < P.expected)) {
        char *t2 = g_strdup_printf ("%snote: %d of %d render threads are missing from this report "
                                    "(they did not answer in time: loading or blocked)\n", text,
                                    MAX ((int) n_late, P.expected - (int) r.n_mons), P.expected);
        g_free (text);
        text = t2;
    }
    char *json = walpe_prof_format_json (&r);
    for (guint i = 0; i < r.n_mons; i++)
        g_free (r.mons[i].rows);
    g_free (r.mons);
    g_free (r.msgs);

    char *dir = g_build_filename (g_get_user_cache_dir (), "walpe", NULL);
    g_mkdir_with_parents (dir, 0700);
    char *tname = g_strdup_printf ("gpu-profile-%s.txt", stamp);
    char *jname = g_strdup_printf ("gpu-profile-%s.json", stamp);
    g_mutex_lock (&P.lock);
    g_free (P.text);
    g_free (P.json);
    P.text = text;
    P.json = json;
    prof_write (dir, tname, text, &P.text_path);
    prof_write (dir, jname, json, &P.json_path);
    /* the newest report under a fixed name too */
    prof_write (dir, "gpu-profile.txt", text, NULL);
    prof_write (dir, "gpu-profile.json", json, NULL);
    g_mutex_unlock (&P.lock);
    g_free (tname);
    g_free (jname);
    g_free (stamp);
    g_free (dir);
    g_message ("gpuprof: report written to %s", P.text_path ? P.text_path : "(not written)");
    g_atomic_int_set (&P.state, PROF_READY);
    return G_SOURCE_REMOVE;
}

void walpe_prof_set_expected (int n)
{
    P.expected = n;
}
