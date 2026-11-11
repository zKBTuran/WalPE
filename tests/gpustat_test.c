/* gpustat_test.c - src/common/gpustat.c: the fdinfo parser on hand-written
 * texts, and the sampler on fake /proc and /sys trees.
 *
 *   gpustat-test <tests/fake-gpustat-tree.sh> <tests/fake-nvidia-smi>
 *
 * The trees are written by the shell script (one place for the samples; it
 * says where they come from: written by hand from the documented format,
 * not captured). The clock is stepped by the test: sample N is taken at N
 * seconds, so every share is known exactly.
 *
 * Covered: Intel i915 and xe key names, amdgpu, a driver without fdinfo
 * and one with identity keys only, a client seen through several
 * descriptors and through a second process, counters that go backwards /
 * wrap / jump, a process and the target that exit between samples,
 * permission denied, processes of other users, two GPUs, a GPU that is
 * powered down, a platform device, no GPU, nvidia-smi through a stand-in,
 * and what a profile run samples when nothing is set (nothing on NVIDIA's
 * kernel driver, everything elsewhere).
 * No GPU and no DRM device is needed (or used). */
#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "common/gpustat.h"

static int fails, checks;
#define CHECK(cond, ...) \
    do { \
        checks++; \
        if (!(cond)) { \
            fails++; \
            fprintf (stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf (stderr, __VA_ARGS__); \
            fputc ('\n', stderr); \
        } \
    } while (0)
#define NEAR(a, b, eps) (fabs ((double) (a) - (double) (b)) <= (eps))
#define HAS(r, metric) ((r)->m[metric].src != WALPE_GPU_SRC_NONE)

#define DAEMON 4242

static const char *gen;      /* tests/fake-gpustat-tree.sh */
static const char *fake_smi; /* tests/fake-nvidia-smi */
static char *top;            /* a temporary directory for the trees */

/* write (or move on) the tree `name` to `step` seconds of a scenario */
static void tree (const char *name, const char *scen, int step)
{
    char *dir = g_build_filename (top, name, NULL);
    char num[16], pid[16];
    g_snprintf (num, sizeof (num), "%d", step);
    g_snprintf (pid, sizeof (pid), "%d", DAEMON);
    const char *argv[] = { "sh", gen, dir, scen, num, pid, NULL };
    int status = -1;
    GError *err = NULL;
    gboolean ok = g_spawn_sync (NULL, (char **) argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL,
                                NULL, &status, &err);
    CHECK (ok && status == 0, "tree %s %s %d: %s", name, scen, step, err ? err->message : "failed");
    g_clear_error (&err);
    g_free (dir);
}

static WalpeGpuSampler *sampler (const char *name, int pid, guint flags, const char *device,
                                 gint64 uid)
{
    char *proc = g_build_filename (top, name, "proc", NULL);
    char *sys = g_build_filename (top, name, "sys", NULL);
    WalpeGpuStatOpts o = { .proc_root = proc, .sys_root = sys, .target_pid = pid, .flags = flags,
                           .device = device, .uid = uid };
    WalpeGpuSampler *s = walpe_gpu_sampler_new (&o);
    g_free (proc);
    g_free (sys);
    return s;
}

/* a scenario sampled at 0, 1, ... steps seconds */
static WalpeGpuStatReport *run (const char *scen, int steps, int pid, guint flags, const char *device)
{
    tree (scen, scen, 0);
    WalpeGpuSampler *s = sampler (scen, pid, flags, device, -1);
    for (int i = 0; i <= steps; i++) {
        if (i)
            tree (scen, scen, i);
        walpe_gpu_sampler_tick_at (s, (gint64) i * G_GINT64_CONSTANT (1000000000));
    }
    WalpeGpuStatReport *r = walpe_gpu_sampler_report (s);
    walpe_gpu_sampler_free (s);
    return r;
}

static const WalpeGpuEngineStat *engine (const WalpeGpuStatReport *r, const char *name)
{
    for (guint i = 0; i < r->n_engines; i++)
        if (!strcmp (r->engines[i].name, name))
            return &r->engines[i];
    return NULL;
}

static const WalpeGpuProcStat *prog (const WalpeGpuStatReport *r, const char *name)
{
    for (guint i = 0; i < r->n_procs; i++)
        if (!strcmp (r->procs[i].name, name))
            return &r->procs[i];
    return NULL;
}

static const WalpeGpuFdRegion *region (const WalpeGpuStatReport *r, const char *name)
{
    for (guint i = 0; i < r->n_regions; i++)
        if (!strcmp (r->regions[i].name, name))
            return &r->regions[i];
    return NULL;
}

static char *rows_text (const WalpeGpuStatReport *r)
{
    GString *s = g_string_new (NULL);
    walpe_gpu_stat_format_rows (r, "daemon", s);
    return g_string_free (s, FALSE);
}

/* ------------------------------------------------------------ the parser */
static void test_parser (void)
{
    WalpeGpuFdinfo f;
    /* illustrative texts, written by hand from the documented key names */
    CHECK (walpe_gpu_fdinfo_parse (
               "pos:\t0\nflags:\t02100002\nmnt_id:\t27\nino:\t412\n"
               "drm-driver:\ti915\ndrm-client-id:\t21\ndrm-pdev:\t0000:00:02.0\n"
               "drm-total-system0:\t286720 KiB\ndrm-shared-system0:\t16384 KiB\n"
               "drm-active-system0:\t4096 KiB\ndrm-resident-system0:\t245760 KiB\n"
               "drm-purgeable-system0:\t0\ndrm-total-stolen-system0:\t0\n"
               "drm-engine-render:\t3200000000 ns\ndrm-engine-copy:\t0 ns\n"
               "drm-engine-video:\t140000000 ns\ndrm-engine-capacity-video:\t2\n"
               "drm-engine-video-enhance:\t0 ns\n", &f), "i915 text is a DRM client");
    CHECK (!strcmp (f.driver, "i915") && f.has_client_id && f.client_id == 21 &&
           !strcmp (f.pdev, "0000:00:02.0"), "i915: who it is");
    CHECK (f.n_engines == 4, "i915: 4 engines, got %u", f.n_engines);
    CHECK (!strcmp (f.engines[0].name, "render") && f.engines[0].has_ns &&
           f.engines[0].ns == G_GUINT64_CONSTANT (3200000000) && !f.engines[0].has_cycles,
           "i915: render in ns");
    CHECK (!strcmp (f.engines[2].name, "video") && f.engines[2].capacity == 2, "i915: two video engines");
    CHECK (!strcmp (f.engines[3].name, "video-enhance"), "i915: a name with a dash");
    CHECK (f.n_regions == 2 && !strcmp (f.regions[0].name, "system0") &&
           f.regions[0].resident_kb == 245760 && f.regions[0].total_kb == 286720 &&
           f.regions[0].shared_kb == 16384 && f.regions[0].active_kb == 4096 &&
           f.regions[0].purgeable_kb == 0, "i915: region system0");
    CHECK (!strcmp (f.regions[1].name, "stolen-system0") && f.regions[1].total_kb == 0 &&
           f.regions[1].resident_kb == -1, "i915: region stolen-system0, resident not given");

    CHECK (walpe_gpu_fdinfo_parse (
               "drm-driver:\txe\ndrm-client-id:\t12\ndrm-pdev:\t0000:00:02.0\n"
               "drm-total-vram0:\t204800 KiB\ndrm-shared-vram0:\t8 MiB\ndrm-resident-vram0:\t196608 KiB\n"
               "drm-cycles-rcs:\t5800000\ndrm-total-cycles-rcs:\t519200000\n"
               "drm-cycles-vcs:\t3842000\ndrm-total-cycles-vcs:\t519200000\n"
               "drm-engine-capacity-vcs:\t2\n", &f), "xe text is a DRM client");
    CHECK (f.n_engines == 2 && !strcmp (f.engines[0].name, "rcs") && !f.engines[0].has_ns &&
           f.engines[0].has_cycles && f.engines[0].has_total_cycles && f.engines[0].cycles == 5800000 &&
           f.engines[0].total_cycles == 519200000, "xe: rcs in cycles against total cycles");
    CHECK (f.engines[1].capacity == 2, "xe: capacity of vcs");
    CHECK (f.n_regions == 1 && f.regions[0].shared_kb == 8192 && f.regions[0].resident_kb == 196608,
           "xe: MiB and KiB units");

    CHECK (walpe_gpu_fdinfo_parse (
               "drm-driver: amdgpu\ndrm-client-id: 7\ndrm-pdev: 0000:03:00.0\n"
               "drm-memory-vram: 307200 KiB\ndrm-memory-gtt: 8192 KiB\ndrm-memory-cpu: 2048 KiB\n"
               "amd-evicted-vram: 512 KiB\namd-requested-vram: 307200 KiB\n"
               "drm-engine-gfx: 5000000000 ns\ndrm-engine-compute: 0 ns\n", &f), "amdgpu text");
    CHECK (f.n_regions == 3 && !strcmp (f.regions[0].name, "vram") && f.regions[0].resident_kb == 307200 &&
           f.regions[0].total_kb == -1, "amdgpu: drm-memory-<region> is the resident figure");
    CHECK (f.evicted_kb == 512, "amdgpu: evicted VRAM");
    CHECK (f.n_engines == 2 && !strcmp (f.engines[0].name, "gfx"), "amdgpu: engines");

    /* values that are not what the format says: skipped, never fatal */
    CHECK (walpe_gpu_fdinfo_parse (
               "drm-driver:\tsome\ndrm-client-id:\tabc\ndrm-pdev:\tnot-an-address\n"
               "drm-engine-a:\t-5 ns\ndrm-engine-b:\t12 us\ndrm-engine-c:\t99999999999999999999999 ns\n"
               "drm-engine-d:\t7\ndrm-engine-capacity-d:\t0\ndrm-engine-:\t3 ns\n"
               "drm-memory-x:\t5 GiB\ndrm-total-y:\t1048576\ndrm-resident-z:\t\n"
               "drm-cycles-e:\t10\nnocolon\n:\n\n", &f), "odd values: still a client");
    CHECK (!f.has_client_id && !f.pdev[0], "odd values: no id, no address");
    CHECK (f.n_engines == 2, "odd values: engines d and e only, got %u", f.n_engines);
    CHECK (!strcmp (f.engines[0].name, "d") && f.engines[0].has_ns && f.engines[0].ns == 7 &&
           f.engines[0].capacity == 0, "a value without a unit is ns; capacity 0 is not taken");
    CHECK (f.engines[1].has_cycles && !f.engines[1].has_total_cycles, "cycles without a total");
    CHECK (f.n_regions == 1 && !strcmp (f.regions[0].name, "y") && f.regions[0].total_kb == 1024,
           "bytes become KiB; an unknown unit and an empty value are skipped");
    CHECK (f.evicted_kb == -1, "no evicted figure");

    /* figures beyond what any device holds are limited (2^40 KiB), so that
     * the sums made of them cannot leave the range of the type */
    CHECK (walpe_gpu_fdinfo_parse (
               "drm-driver: big\ndrm-memory-a: 18446744073709551615 KiB\n"
               "drm-total-a: 18446744073709551615 MiB\ndrm-shared-a: 18446744073709551615\n", &f) &&
           f.n_regions == 1 && f.regions[0].resident_kb == (gint64) 1 << 40 &&
           f.regions[0].total_kb == (gint64) 1 << 40 && f.regions[0].shared_kb == (gint64) 1 << 40,
           "memory figures out of any range are limited");

    GString *many = g_string_new ("drm-driver: many\n");
    for (int i = 0; i < 40; i++)
        g_string_append_printf (many, "drm-engine-e%d: %d ns\ndrm-total-r%d: 4 KiB\n", i, i, i);
    g_string_append (many, "drm-engine-");
    for (int i = 0; i < 500; i++)
        g_string_append_c (many, 'x');
    g_string_append (many, ": 1 ns\n");
    CHECK (walpe_gpu_fdinfo_parse (many->str, &f) && f.n_engines == WALPE_GPU_MAX_ENGINES &&
           f.n_regions == WALPE_GPU_MAX_REGIONS, "more engines / regions than the tables hold");
    g_string_free (many, TRUE);

    CHECK (!walpe_gpu_fdinfo_parse ("pos:\t0\nflags:\t02\nmnt_id:\t27\nino:\t9\n", &f), "an ordinary file");
    CHECK (!walpe_gpu_fdinfo_parse ("drm-client-id: 3\ndrm-engine-gfx: 5 ns\n", &f), "no drm-driver: not a client");
    CHECK (!walpe_gpu_fdinfo_parse ("", &f) && !walpe_gpu_fdinfo_parse (NULL, &f), "empty and NULL");
}

/* ----------------------------------------------------------------- amdgpu */
static void test_amdgpu (void)
{
    WalpeGpuStatReport *r = run ("amdgpu", 3, DAEMON, WALPE_GPUSTAT_OTHERS, NULL);
    CHECK (!strcmp (r->driver, "amdgpu") && !strcmp (r->pci_slot, "0000:03:00.0") &&
           !strcmp (r->card, "card0") && !strcmp (r->render_node, "renderD128") &&
           !strcmp (r->vendor_name, "AMD") && r->vendor_id == 0x1002 && r->device_id == 0x73df,
           "amdgpu: the device: %s %s %s %s", r->driver, r->pci_slot, r->card, r->vendor_name);
    CHECK (!strcmp (r->how_found, "the process's DRM client") && r->n_devices == 1,
           "amdgpu: how it was chosen: %s, %u", r->how_found, r->n_devices);
    CHECK ((r->sources & (1u << WALPE_GPU_SRC_FDINFO)) && (r->sources & (1u << WALPE_GPU_SRC_AMD_SYSFS)) &&
           (r->sources & (1u << WALPE_GPU_SRC_HWMON)), "amdgpu: three sources, 0x%x", r->sources);
    CHECK (r->samples == 4 && NEAR (r->seconds, 3.0, 1e-6), "amdgpu: 4 samples over 3 s");
    CHECK (!strcmp (r->main_engine, "gfx"), "amdgpu: main engine %s", r->main_engine);
    const WalpeGpuStatVal *v = &r->m[WALPE_GPU_M_PROC_BUSY];
    CHECK (v->src == WALPE_GPU_SRC_FDINFO && v->n == 3 && NEAR (v->avg, 10.0, 0.01) &&
           NEAR (v->min, 10.0, 0.01) && NEAR (v->max, 10.0, 0.01),
           "amdgpu: the daemon's share %.3f (%.3f .. %.3f) x%u", v->avg, v->min, v->max, v->n);
    const WalpeGpuEngineStat *e = engine (r, "dec");
    CHECK (e && e->has_target && NEAR (e->proc_pct, 2.0, 0.01), "amdgpu: its decode engine");
    v = &r->m[WALPE_GPU_M_BUSY];
    CHECK (v->src == WALPE_GPU_SRC_AMD_SYSFS && v->n == 4 && NEAR (v->min, 20, 0.01) && NEAR (v->max, 30, 0.01),
           "amdgpu: gpu_busy_percent %.1f .. %.1f x%u", v->min, v->max, v->n);
    CHECK (NEAR (r->m[WALPE_GPU_M_BUSY_CLIENTS].avg, 21.0, 0.01) && !r->m[WALPE_GPU_M_BUSY_CLIENTS].lower_bound,
           "amdgpu: all visible clients %.3f", r->m[WALPE_GPU_M_BUSY_CLIENTS].avg);
    CHECK (NEAR (r->m[WALPE_GPU_M_OTHERS_BUSY].avg, 11.0, 0.01), "amdgpu: the others %.3f",
           r->m[WALPE_GPU_M_OTHERS_BUSY].avg);
    /* one client through two descriptors: counted once */
    CHECK (r->target_clients == 1 && r->clients_shared == 1, "amdgpu: one client, seen twice (%u, %u)",
           r->target_clients, r->clients_shared);
    CHECK (NEAR (r->m[WALPE_GPU_M_PROC_MEM].last, 310.0, 0.01) && !r->mem_is_total,
           "amdgpu: the daemon's memory %.2f", r->m[WALPE_GPU_M_PROC_MEM].last);
    const WalpeGpuFdRegion *g = region (r, "vram");
    CHECK (g && g->resident_kb == 307200 && region (r, "gtt") && region (r, "cpu"), "amdgpu: regions");
    CHECK (r->proc_evicted_kb == 0, "amdgpu: nothing evicted");
    CHECK (NEAR (r->m[WALPE_GPU_M_VRAM_USED].last, 1203.0, 0.01) &&
           NEAR (r->m[WALPE_GPU_M_VRAM_TOTAL].last, 8176.0, 0.01) &&
           NEAR (r->m[WALPE_GPU_M_GTT_USED].last, 300.0, 0.01), "amdgpu: whole-GPU memory");
    CHECK (r->m[WALPE_GPU_M_POWER].src == WALPE_GPU_SRC_HWMON && NEAR (r->m[WALPE_GPU_M_POWER].avg, 24.5, 0.01) &&
           NEAR (r->m[WALPE_GPU_M_TEMP].avg, 51.0, 0.01) && NEAR (r->m[WALPE_GPU_M_CLOCK].avg, 1287.0, 0.01) &&
           NEAR (r->m[WALPE_GPU_M_CLOCK_MAX].last, 2575.0, 0.01) &&
           NEAR (r->m[WALPE_GPU_M_MEM_CLOCK].avg, 875.0, 0.01), "amdgpu: power, temperature, clocks");
    CHECK (r->m[WALPE_GPU_M_BUSY_SCALED].src == WALPE_GPU_SRC_DERIVED &&
           NEAR (r->m[WALPE_GPU_M_BUSY_SCALED].min, 20.0 * 1287 / 2575, 0.05), "amdgpu: busy at full clock");
    CHECK (r->n_procs == 3 && r->procs[0].target && !strcmp (r->procs[0].name, "walpe-daemon") &&
           !strcmp (r->procs[1].name, "picom") && !strcmp (r->procs[2].name, "Xorg"),
           "amdgpu: three programs, busiest first");
    const WalpeGpuProcStat *p = prog (r, "picom");
    CHECK (p && p->has_busy && NEAR (p->busy_pct, 8.0, 0.01) && p->mem_kb == 62464 && !p->exited,
           "amdgpu: picom");
    p = prog (r, "Xorg");
    CHECK (p && NEAR (p->busy_pct, 3.0, 0.01), "amdgpu: Xorg");
    CHECK (r->procs_other_users == 0 && r->procs_denied == 0 && r->counter_resets == 0, "amdgpu: nothing hidden");

    char *t = rows_text (r);
    CHECK (strstr (t, "GPU busy                   %            20.0"), "rows: GPU busy:\n%s", t);
    CHECK (strstr (t, "(amdgpu sysfs: gpu_busy_percent)"), "rows: its source");
    CHECK (strstr (t, "daemon GPU share           %            10.0      10.0      10.0  3/3  (fdinfo: engine gfx"),
           "rows: the daemon's share:\n%s", t);
    CHECK (strstr (t, "daemon engine dec"), "rows: its second engine");
    CHECK (strstr (t, "daemon GPU memory          MiB         310.0"), "rows: its memory");
    CHECK (strstr (t, "resident: vram 300.0 + gtt 8.0 + cpu 2.0"), "rows: memory by region");
    CHECK (strstr (t, "VRAM used                  MiB") && strstr (t, "of 8176 MiB"), "rows: VRAM");
    CHECK (strstr (t, "power                      W            24.5") && strstr (t, "(hwmon)"), "rows: power");
    CHECK (strstr (t, "clock                      MHz") && strstr (t, "highest 2575 MHz"), "rows: clock");
    CHECK (strstr (t, "other programs' GPU share  %            11.0"), "rows: the others");
    CHECK (!strstr (t, "not available") && !strstr (t, "nan"), "rows: nothing missing");
    g_free (t);

    GString *s = g_string_new (NULL);
    walpe_gpu_stat_format_device (r, s);
    CHECK (!strcmp (s->str, "AMD [1002:73df] at 0000:03:00.0 (card0, renderD128), driver amdgpu"),
           "device line: %s", s->str);
    g_string_set_size (s, 0);
    walpe_gpu_stat_format_sources (r, s);
    CHECK (!strcmp (s->str, "fdinfo, amdgpu sysfs, hwmon"), "sources: %s", s->str);
    g_string_set_size (s, 0);
    walpe_gpu_stat_format_programs (r, 8, s);
    CHECK (strstr (s->str, "programs on the GPU (kernel fdinfo; share = time on engine gfx"), "programs: head");
    CHECK (strstr (s->str, "  walpe-daemon            310.0 MiB    10.0 %   <- measured"), "programs: daemon:\n%s", s->str);
    CHECK (strstr (s->str, "  Xorg                    184.0 MiB     3.0 %"), "programs: Xorg");
    CHECK (!strstr (s->str, "not in this list"), "programs: nothing hidden");
    g_string_set_size (s, 0);
    walpe_gpu_stat_format_programs (r, 2, s);
    CHECK (strstr (s->str, "(+1 more)"), "programs: cut after 2");
    g_string_set_size (s, 0);
    walpe_gpu_stat_format_notes (r, s);
    CHECK (strstr (s->str, "\"GPU share\" from fdinfo") && strstr (s->str, "amdgpu"), "notes");
    g_string_set_size (s, 0);
    walpe_gpu_stat_format_profile (r, s);
    CHECK (g_str_has_prefix (s->str, "GPU as the kernel accounts it: AMD [1002:73df]"), "profile: head");
    CHECK (strstr (s->str, "walpe-daemon: engine gfx busy for it 10.0 % of the time, dec 2.0 %"),
           "profile: the daemon:\n%s", s->str);
    CHECK (strstr (s->str, "GPU memory 310.0 MiB resident (vram 300.0, gtt 8.0, cpu 2.0)"), "profile: memory");
    CHECK (strstr (s->str, "busy 2") && strstr (s->str, "[amdgpu sysfs]") && strstr (s->str, "[hwmon]"),
           "profile: whole GPU with sources");
    CHECK (strstr (s->str, "other programs: picom 8.0 % 61 MiB, Xorg 3.0 % 184 MiB"), "profile: the others");
    CHECK (!strstr (s->str, "not available"), "profile: nothing missing");
    g_string_free (s, TRUE);

    JsonBuilder *b = json_builder_new ();
    walpe_gpu_stat_to_json (r, b);
    JsonNode *root = json_builder_get_root (b);
    JsonObject *o = JSON_NODE_HOLDS_OBJECT (root) ? json_node_get_object (root) : NULL;
    CHECK (o && json_object_has_member (o, "device") && json_object_has_member (o, "values") &&
           json_array_get_length (json_object_get_array_member (o, "programs")) == 3 &&
           json_array_get_length (json_object_get_array_member (o, "sources")) == 3, "JSON: members");
    if (o) {
        JsonObject *vals = json_object_get_object_member (o, "values");
        JsonObject *pb = json_object_get_object_member (vals, "process_busy_percent");
        CHECK (pb && !strcmp (json_object_get_string_member (pb, "source"), "fdinfo") &&
               NEAR (json_object_get_double_member (pb, "avg"), 10.0, 0.01), "JSON: the daemon's share");
    }
    json_node_unref (root);
    g_object_unref (b);
    walpe_gpu_stat_report_free (r);

    /* without the other processes: the daemon's rows only */
    r = run ("amdgpu", 2, DAEMON, 0, NULL);
    CHECK (NEAR (r->m[WALPE_GPU_M_PROC_BUSY].avg, 10.0, 0.01) && r->n_procs == 1 &&
           !HAS (r, WALPE_GPU_M_BUSY_CLIENTS) && strstr (r->m[WALPE_GPU_M_BUSY_CLIENTS].why, "not read"),
           "amdgpu, own process only");
    walpe_gpu_stat_report_free (r);

    /* no process: the GPU alone (a baseline) */
    r = run ("amdgpu", 2, 0, WALPE_GPUSTAT_OTHERS, NULL);
    CHECK (!strcmp (r->driver, "amdgpu") && !strcmp (r->how_found, "the only GPU") && r->target_pid == 0 &&
           r->n_procs == 3 && NEAR (r->m[WALPE_GPU_M_BUSY_CLIENTS].avg, 21.0, 0.01), "amdgpu, baseline");
    t = rows_text (r);
    CHECK (!strstr (t, "daemon"), "baseline: no daemon rows");
    g_free (t);
    walpe_gpu_stat_report_free (r);

    /* one sample: no share yet, memory and the device's figures are there */
    r = run ("amdgpu", 0, DAEMON, WALPE_GPUSTAT_OTHERS, NULL);
    CHECK (!HAS (r, WALPE_GPU_M_PROC_BUSY) && strstr (r->m[WALPE_GPU_M_PROC_BUSY].why, "two samples") &&
           HAS (r, WALPE_GPU_M_PROC_MEM) && HAS (r, WALPE_GPU_M_BUSY), "amdgpu, one sample");
    walpe_gpu_stat_report_free (r);
}

/* ------------------------------------------------------------------ Intel */
static void test_intel (void)
{
    WalpeGpuStatReport *r = run ("i915", 3, DAEMON, WALPE_GPUSTAT_OTHERS, NULL);
    CHECK (!strcmp (r->driver, "i915") && !strcmp (r->vendor_name, "Intel") && !strcmp (r->main_engine, "render"),
           "i915: device and engine");
    CHECK (NEAR (r->m[WALPE_GPU_M_PROC_BUSY].avg, 20.0, 0.01), "i915: the daemon's render share %.3f",
           r->m[WALPE_GPU_M_PROC_BUSY].avg);
    const WalpeGpuEngineStat *e = engine (r, "video");
    CHECK (e && e->capacity == 2 && NEAR (e->proc_pct, 5.0, 0.01), "i915: 100 ms/s on two video engines = 5 %%");
    CHECK (!HAS (r, WALPE_GPU_M_BUSY) && strstr (r->m[WALPE_GPU_M_BUSY].why, "no whole-GPU figure"),
           "i915: no device-level busy: %s", r->m[WALPE_GPU_M_BUSY].why);
    CHECK (NEAR (r->m[WALPE_GPU_M_BUSY_CLIENTS].avg, 25.0, 0.01), "i915: the clients' sum");
    CHECK (!HAS (r, WALPE_GPU_M_POWER) && strstr (r->m[WALPE_GPU_M_POWER].why, "integrated") &&
           !HAS (r, WALPE_GPU_M_TEMP) && !HAS (r, WALPE_GPU_M_VRAM_USED), "i915: power, temperature, VRAM not available");
    CHECK (r->m[WALPE_GPU_M_CLOCK].src == WALPE_GPU_SRC_INTEL_SYSFS && NEAR (r->m[WALPE_GPU_M_CLOCK].avg, 650, 0.01) &&
           NEAR (r->m[WALPE_GPU_M_CLOCK_MAX].last, 1300, 0.01), "i915: clocks");
    CHECK (NEAR (r->m[WALPE_GPU_M_BUSY_SCALED].avg, 12.5, 0.05), "i915: busy at full clock %.2f",
           r->m[WALPE_GPU_M_BUSY_SCALED].avg);
    CHECK (NEAR (r->m[WALPE_GPU_M_PROC_MEM].last, 240.0, 0.01), "i915: resident memory");
    const WalpeGpuFdRegion *g = region (r, "system0");
    CHECK (g && g->total_kb == 286720 && g->shared_kb == 16384, "i915: region system0");
    char *t = rows_text (r);
    CHECK (strstr (t, "GPU busy                   %             n/a       n/a       n/a  -  (not available: the i915 driver"),
           "i915 rows: GPU busy not available:\n%s", t);
    CHECK (strstr (t, "GPU busy, visible clients  %            25.0"), "i915 rows: the clients' sum");
    CHECK (strstr (t, "power                      W             n/a") && strstr (t, "temperature                C             n/a"),
           "i915 rows: power and temperature not available");
    CHECK (strstr (t, "daemon engine video") && strstr (t, "share of all engines of this kind"), "i915 rows: video");
    g_free (t);
    GString *s = g_string_new (NULL);
    walpe_gpu_stat_format_profile (r, s);
    CHECK (strstr (s->str, "not available: busy (the i915 driver") && strstr (s->str, "power ("), "i915 profile: what is missing");
    g_string_free (s, TRUE);
    walpe_gpu_stat_report_free (r);

    r = run ("xe", 3, DAEMON, WALPE_GPUSTAT_OTHERS, NULL);
    CHECK (!strcmp (r->driver, "xe") && !strcmp (r->main_engine, "rcs"), "xe: device and engine");
    const WalpeGpuEngineStat *rcs = engine (r, "rcs");
    CHECK (rcs && rcs->cycles && NEAR (r->m[WALPE_GPU_M_PROC_BUSY].avg, 25.0, 0.01),
           "xe: cycles / total cycles = 25 %%, got %.3f", r->m[WALPE_GPU_M_PROC_BUSY].avg);
    e = engine (r, "vcs");
    CHECK (e && e->capacity == 2 && NEAR (e->proc_pct, 10.0, 0.01), "xe: vcs with two engines");
    const WalpeGpuProcStat *p = prog (r, "Xorg");
    CHECK (p && NEAR (p->busy_pct, 5.0, 0.01), "xe: Xorg");
    CHECK (NEAR (r->m[WALPE_GPU_M_CLOCK].avg, 800, 0.01) && NEAR (r->m[WALPE_GPU_M_CLOCK_MAX].last, 2050, 0.01),
           "xe: clocks below the device");
    CHECK (NEAR (r->m[WALPE_GPU_M_PROC_MEM].last, 193.0, 0.01), "xe: resident in gtt + vram0");
    walpe_gpu_stat_report_free (r);
}

/* ------------------------------------------- drivers without statistics */
static void test_none (void)
{
    WalpeGpuStatReport *r = run ("nofdinfo", 2, DAEMON, WALPE_GPUSTAT_OTHERS, NULL);
    CHECK (!strcmp (r->driver, "nvidia") && !strcmp (r->vendor_name, "NVIDIA") &&
           !strcmp (r->how_found, "the only GPU"), "no fdinfo: the device is still named (%s)", r->how_found);
    CHECK (r->target_seen && r->target_clients == 0 && r->n_procs == 0 && !r->fdinfo_engines && r->sources == 0,
           "no fdinfo: no client, no source");
    CHECK (!HAS (r, WALPE_GPU_M_PROC_BUSY) && strstr (r->m[WALPE_GPU_M_PROC_BUSY].why, "NVIDIA driver") &&
           strstr (r->m[WALPE_GPU_M_PROC_BUSY].why, "nvidia-smi"), "no fdinfo: why: %s", r->m[WALPE_GPU_M_PROC_BUSY].why);
    char *t = rows_text (r);
    CHECK (strstr (t, "daemon GPU share           %             n/a       n/a       n/a  -  (not available: "),
           "no fdinfo: rows say so:\n%s", t);
    g_free (t);
    GString *s = g_string_new (NULL);
    walpe_gpu_stat_format_sources (r, s);
    CHECK (!strcmp (s->str, "none"), "no fdinfo: sources none");
    g_string_free (s, TRUE);
    walpe_gpu_stat_report_free (r);

    r = run ("nostats", 2, DAEMON, 0, NULL);
    CHECK (!strcmp (r->driver, "radeon") && r->target_clients == 1, "identity keys only: a client is seen");
    CHECK (!HAS (r, WALPE_GPU_M_PROC_BUSY) && strstr (r->m[WALPE_GPU_M_PROC_BUSY].why, "no engine time") &&
           !HAS (r, WALPE_GPU_M_PROC_MEM) && strstr (r->m[WALPE_GPU_M_PROC_MEM].why, "no memory"),
           "identity keys only: both rows say why");
    walpe_gpu_stat_report_free (r);

    r = run ("empty", 1, DAEMON, WALPE_GPUSTAT_OTHERS, NULL);
    GString *d = g_string_new (NULL);
    walpe_gpu_stat_format_device (r, d);
    CHECK (!strcmp (d->str, "none found (the kernel lists no GPU)") && r->n_devices == 0, "no GPU: %s", d->str);
    g_string_free (d, TRUE);
    t = rows_text (r);
    CHECK (strstr (t, "not available: the kernel lists no GPU"), "no GPU: rows say so");
    g_free (t);
    walpe_gpu_stat_report_free (r);
}

/* -------------------------- one client in two processes; odd counters */
static void test_shared_and_counters (void)
{
    WalpeGpuStatReport *r = run ("shared", 3, DAEMON, WALPE_GPUSTAT_OTHERS, NULL);
    const WalpeGpuProcStat *p = prog (r, "picom");
    CHECK (NEAR (r->m[WALPE_GPU_M_PROC_BUSY].avg, 10.0, 0.01), "shared: the daemon keeps its client");
    CHECK (p && NEAR (p->busy_pct, 8.0, 0.01) && p->clients == 1, "shared: picom is not given the copy (%.2f)",
           p ? p->busy_pct : -1.0);
    CHECK (NEAR (r->m[WALPE_GPU_M_BUSY_CLIENTS].avg, 21.0, 0.01), "shared: the sum counts the client once (%.2f)",
           r->m[WALPE_GPU_M_BUSY_CLIENTS].avg);
    CHECK (r->clients_shared == 1 && r->target_clients == 1, "shared: noted once");
    walpe_gpu_stat_report_free (r);

    r = run ("backwards", 5, DAEMON, WALPE_GPUSTAT_OTHERS, NULL);
    const WalpeGpuStatVal *v = &r->m[WALPE_GPU_M_PROC_BUSY];
    /* 10 % a second; one reading 50 ms low: that second counts 0, the next 20 */
    CHECK (NEAR (v->avg, 10.0, 0.01) && NEAR (v->min, 0.0, 0.01) && NEAR (v->max, 20.0, 0.01) && v->n == 5,
           "backwards: held and caught up: %.2f (%.2f .. %.2f) x%u", v->avg, v->min, v->max, v->n);
    CHECK (r->counter_resets == 2, "backwards: a wrap and a jump = 2 resets, got %u", r->counter_resets);
    p = prog (r, "picom");
    CHECK (p && NEAR (p->busy_pct, 8.0, 0.01), "wrap: picom's share over the counted time %.2f", p ? p->busy_pct : -1.0);
    p = prog (r, "Xorg");
    CHECK (p && NEAR (p->busy_pct, 3.0, 0.01), "jump: Xorg's share without the jump %.2f", p ? p->busy_pct : -1.0);
    CHECK (r->m[WALPE_GPU_M_BUSY_CLIENTS].max <= 100.0, "no share above 100 %%");
    GString *s = g_string_new (NULL);
    walpe_gpu_stat_format_notes (r, s);
    CHECK (strstr (s->str, "2 times a kernel counter went backwards"), "backwards: the note");
    g_string_free (s, TRUE);
    walpe_gpu_stat_report_free (r);
}

/* ------------------------------------------------------ processes that go */
static void test_exits (void)
{
    WalpeGpuStatReport *r = run ("exit", 4, DAEMON, WALPE_GPUSTAT_OTHERS, NULL);
    const WalpeGpuProcStat *p = prog (r, "picom");
    CHECK (p && p->exited && p->has_busy && NEAR (p->busy_pct, 8.0, 0.01), "exit: picom's share before it went");
    CHECK (NEAR (r->m[WALPE_GPU_M_PROC_BUSY].avg, 10.0, 0.01) && !r->target_gone, "exit: the daemon is not touched");
    p = prog (r, "Xorg");
    CHECK (p && !p->exited, "exit: Xorg stays");
    GString *s = g_string_new (NULL);
    walpe_gpu_stat_format_programs (r, 8, s);
    CHECK (strstr (s->str, "(exited during the sampling)"), "exit: said in the list");
    g_string_free (s, TRUE);
    walpe_gpu_stat_report_free (r);

    r = run ("daemon-exit", 5, DAEMON, WALPE_GPUSTAT_OTHERS, NULL);
    const WalpeGpuStatVal *v = &r->m[WALPE_GPU_M_PROC_BUSY];
    CHECK (r->target_seen && r->target_gone, "daemon exit: noticed");
    CHECK (v->n == 2 && NEAR (v->max, 10.0, 0.01) && NEAR (v->avg, 10.0, 0.01),
           "daemon exit: two intervals were measured, the average is over them (x%u, %.2f)", v->n, v->avg);
    char *t = rows_text (r);
    CHECK (strstr (t, "(the daemon went away during the sampling"), "daemon exit: said under the rows");
    g_free (t);
    walpe_gpu_stat_report_free (r);

    /* a pid that never existed */
    r = run ("amdgpu", 1, 77777, WALPE_GPUSTAT_OTHERS, NULL);
    CHECK (!r->target_seen && !HAS (r, WALPE_GPU_M_PROC_BUSY) &&
           strstr (r->m[WALPE_GPU_M_PROC_BUSY].why, "not found"), "no such process");
    walpe_gpu_stat_report_free (r);
}

/* -------------------------------------------------- what may not be read */
static void test_permissions (void)
{
    tree ("denied", "denied", 0);
    char *probe = g_build_filename (top, "denied", "proc", "950", "fdinfo", NULL);
    DIR *d = opendir (probe);
    if (d) {
        /* this user may read everything (root): the case cannot be made */
        closedir (d);
        printf ("gpustat-test: permission-denied case skipped (running as a user that may read all)\n");
    } else {
        WalpeGpuStatReport *r = run ("denied", 2, DAEMON, WALPE_GPUSTAT_OTHERS, NULL);
        CHECK (r->procs_denied == 1 && !prog (r, "secretive"), "denied: counted, not listed (%u)", r->procs_denied);
        CHECK (r->m[WALPE_GPU_M_BUSY_CLIENTS].lower_bound, "denied: the sum is a lower bound");
        CHECK (NEAR (r->m[WALPE_GPU_M_PROC_BUSY].avg, 10.0, 0.01), "denied: the rest is measured");
        GString *s = g_string_new (NULL);
        walpe_gpu_stat_format_programs (r, 8, s);
        CHECK (strstr (s->str, "not in this list: 1 of your processes that do not allow"), "denied: said:\n%s", s->str);
        g_string_free (s, TRUE);
        char *t = rows_text (r);
        CHECK (strstr (t, "a lower bound: some clients cannot be read"), "denied: said in the row");
        g_free (t);
        walpe_gpu_stat_report_free (r);
    }
    g_free (probe);

    /* processes of another user: the test cannot make any, so it claims to
     * be someone else - every process of the tree is then "another user's"
     * and nothing of them may be read, the target included */
    tree ("amdgpu", "amdgpu", 0);
    WalpeGpuSampler *s = sampler ("amdgpu", DAEMON, WALPE_GPUSTAT_OTHERS, NULL, (gint64) getuid () + 1);
    walpe_gpu_sampler_tick_at (s, 0);
    tree ("amdgpu", "amdgpu", 1);
    walpe_gpu_sampler_tick_at (s, G_GINT64_CONSTANT (1000000000));
    WalpeGpuStatReport *r = walpe_gpu_sampler_report (s);
    CHECK (r->n_procs == 0 && r->procs_other_users == 2, "other users: none read, two counted (%u, %u)",
           r->n_procs, r->procs_other_users);
    CHECK (!HAS (r, WALPE_GPU_M_PROC_BUSY) && strstr (r->m[WALPE_GPU_M_PROC_BUSY].why, "another user"),
           "other users: the target is not read either: %s", r->m[WALPE_GPU_M_PROC_BUSY].why);
    CHECK (!strcmp (r->driver, "amdgpu") && HAS (r, WALPE_GPU_M_BUSY), "other users: the device's own figures stay");
    GString *g = g_string_new (NULL);
    walpe_gpu_stat_format_programs (r, 8, g);
    CHECK (strstr (g->str, "2 processes of other users (never read"), "other users: said:\n%s", g->str);
    CHECK (!strstr (g->str, "Xorg") && !strstr (g->str, "picom"), "other users: no name of theirs");
    g_string_free (g, TRUE);
    walpe_gpu_stat_report_free (r);
    walpe_gpu_sampler_free (s);
}

/* ------------------------------------------------------- which device */
static void test_devices (void)
{
    WalpeGpuStatReport *r = run ("twogpu", 3, DAEMON, WALPE_GPUSTAT_OTHERS, NULL);
    CHECK (!strcmp (r->driver, "amdgpu") && !strcmp (r->card, "card1") && r->n_devices == 2 &&
           !strcmp (r->how_found, "the process's DRM client"),
           "two GPUs: the one the daemon works on (%s %s, %s)", r->driver, r->card, r->how_found);
    CHECK (NEAR (r->m[WALPE_GPU_M_PROC_BUSY].avg, 10.0, 0.01) && !strcmp (r->main_engine, "gfx") &&
           r->target_clients == 1, "two GPUs: its share there");
    CHECK (!prog (r, "Xorg"), "two GPUs: Xorg is on the other one");
    walpe_gpu_stat_report_free (r);
    static const char *const spell[] = { "card0", "/dev/dri/card0", "renderD128", "/dev/dri/renderD128",
                                         "0000:00:02.0", NULL };
    for (int i = 0; spell[i]; i++) {
        r = run ("twogpu", 2, DAEMON, WALPE_GPUSTAT_OTHERS, spell[i]);
        CHECK (!strcmp (r->driver, "i915") && !strcmp (r->how_found, "named by the caller") &&
               NEAR (r->m[WALPE_GPU_M_PROC_BUSY].avg, 0.5, 0.01) && prog (r, "Xorg"),
               "two GPUs: named as %s -> %s, %.2f", spell[i], r->driver, r->m[WALPE_GPU_M_PROC_BUSY].avg);
        walpe_gpu_stat_report_free (r);
    }
    /* no process: the boot GPU */
    r = run ("twogpu", 1, 0, WALPE_GPUSTAT_OTHERS, NULL);
    CHECK (!strcmp (r->driver, "i915") && !strcmp (r->how_found, "the boot GPU"), "two GPUs, baseline: %s", r->how_found);
    walpe_gpu_stat_report_free (r);

    r = run ("suspended", 2, DAEMON, WALPE_GPUSTAT_OTHERS, NULL);
    CHECK (!strcmp (r->driver, "amdgpu") && !HAS (r, WALPE_GPU_M_BUSY) && !HAS (r, WALPE_GPU_M_VRAM_USED) &&
           !HAS (r, WALPE_GPU_M_POWER) && strstr (r->m[WALPE_GPU_M_BUSY].why, "powered down"),
           "a GPU that sleeps is not read: %s", r->m[WALPE_GPU_M_BUSY].why);
    walpe_gpu_stat_report_free (r);

    /* a sysfs tree of plain files: driver, ids and address from uevent */
    r = run ("flat", 2, DAEMON, 0, NULL);
    CHECK (!strcmp (r->driver, "amdgpu") && !strcmp (r->pci_slot, "0000:03:00.0") && r->vendor_id == 0x1002 &&
           r->device_id == 0x73df && !strcmp (r->card, "card0") && !strcmp (r->render_node, "renderD128"),
           "plain-file tree: the device (%s %s %04x:%04x)", r->driver, r->pci_slot, r->vendor_id, r->device_id);
    CHECK (NEAR (r->m[WALPE_GPU_M_PROC_BUSY].avg, 10.0, 0.01) && HAS (r, WALPE_GPU_M_BUSY) &&
           NEAR (r->m[WALPE_GPU_M_POWER].avg, 24.5, 0.01) && NEAR (r->m[WALPE_GPU_M_VRAM_TOTAL].last, 8176.0, 0.01),
           "plain-file tree: fdinfo, sysfs and hwmon are found");
    walpe_gpu_stat_report_free (r);

    r = run ("platform", 3, DAEMON, 0, NULL);
    CHECK (!strcmp (r->driver, "v3d") && !r->pci_slot[0] && !strcmp (r->card, "card0") &&
           !strcmp (r->main_engine, "render"), "platform device: %s %s", r->driver, r->main_engine);
    const WalpeGpuEngineStat *e = engine (r, "bin");
    CHECK (NEAR (r->m[WALPE_GPU_M_PROC_BUSY].avg, 15.0, 0.01) && e && NEAR (e->proc_pct, 1.0, 0.01),
           "platform device: engines");
    CHECK (!HAS (r, WALPE_GPU_M_PROC_MEM), "platform device: no memory keys");
    GString *s = g_string_new (NULL);
    walpe_gpu_stat_format_device (r, s);
    CHECK (!strcmp (s->str, "GPU (card0, renderD128), driver v3d"), "platform device line: %s", s->str);
    g_string_free (s, TRUE);
    walpe_gpu_stat_report_free (r);
}

/* ---------------------------------------- restart, and the status figures */
static void test_restart_and_self (void)
{
    tree ("restart", "amdgpu", 0);
    WalpeGpuSampler *s = sampler ("restart", DAEMON, 0, NULL, -1);
    walpe_gpu_sampler_tick_at (s, 0);
    tree ("restart", "amdgpu", 1);
    walpe_gpu_sampler_tick_at (s, G_GINT64_CONSTANT (1000000000));
    WalpeGpuStatReport *r = walpe_gpu_sampler_report (s);
    CHECK (r->samples == 2 && NEAR (r->m[WALPE_GPU_M_PROC_BUSY].avg, 10.0, 0.01), "before the restart");
    walpe_gpu_stat_report_free (r);
    walpe_gpu_sampler_restart (s);
    tree ("restart", "amdgpu", 3);
    walpe_gpu_sampler_tick_at (s, G_GINT64_CONSTANT (3000000000));
    r = walpe_gpu_sampler_report (s);
    CHECK (r->samples == 2 && NEAR (r->seconds, 2.0, 1e-6) && r->m[WALPE_GPU_M_PROC_BUSY].n == 1 &&
           NEAR (r->m[WALPE_GPU_M_PROC_BUSY].avg, 10.0, 0.01),
           "after the restart: the time since, %.2f over %.1f s", r->m[WALPE_GPU_M_PROC_BUSY].avg, r->seconds);
    walpe_gpu_stat_report_free (r);
    walpe_gpu_sampler_free (s);

    /* the real machine, this process: whatever is there, it must answer */
    WalpeGpuSelfStat a, b;
    walpe_gpustat_self (&a);
    walpe_gpustat_self (&b);
    CHECK (a.sources[0] && !a.has_busy, "self: first call has no share yet (sources: %s)", a.sources);
    CHECK (!strcmp (a.driver, b.driver), "self: a second call within a second repeats the first");
    CHECK (walpe_gpustat_self_readings () == 1, "self: one reading so far (%u)", walpe_gpustat_self_readings ());
    if (!a.driver[0] || !strcmp (a.driver, "nvidia")) {
        /* no GPU listed, or NVIDIA's driver: fdinfo never holds a figure,
         * so the second reading is the last one */
        g_usleep (1100000);
        walpe_gpustat_self (&b);
        CHECK (walpe_gpustat_self_readings () == 2, "self: a second later it reads once more (%u)",
               walpe_gpustat_self_readings ());
        WalpeGpuSelfStat c;
        g_usleep (1100000);
        walpe_gpustat_self (&c);
        g_usleep (1100000);
        walpe_gpustat_self (&c);
        CHECK (walpe_gpustat_self_readings () == 2,
               "self: nothing can come from driver \"%s\": not read again (%u readings)", b.driver,
               walpe_gpustat_self_readings ());
        CHECK (!c.has_busy && c.mem_kb < 0 && !strcmp (c.sources, b.sources) && !strcmp (c.note, b.note) &&
               c.note[0], "self: the answer stays, with its reason: %s", c.note);
        /* the renderer is told anew: asked again */
        walpe_gpustat_set_render_device ("0000:ff:1f.7");
        g_usleep (1100000);
        walpe_gpustat_self (&c);
        CHECK (walpe_gpustat_self_readings () >= 3, "self: another renderer is looked at again (%u)",
               walpe_gpustat_self_readings ());
        walpe_gpustat_set_render_device (NULL);
    } else {
        printf ("  (self: driver %s publishes figures: the settled case is not tested here)\n", a.driver);
    }
    WalpeGpuSampler *n = walpe_gpu_sampler_new (NULL);
    walpe_gpu_sampler_tick (n);
    r = walpe_gpu_sampler_report (n);
    CHECK (r->target_pid == (int) getpid () && r->target_seen, "defaults: the calling process");
    walpe_gpu_stat_report_free (r);
    walpe_gpu_sampler_free (n);
}

/* ------------- what a profile run in the daemon samples when nothing is set */
static WalpeGpuProfileKernel prof_mode (const char *name, const char *setting, char drv[32])
{
    char *sys = g_build_filename (top, name, "sys", NULL);
    drv[0] = '?';
    drv[1] = 0;
    walpe_gpustat_render_driver (sys, drv, 32);
    WalpeGpuProfileKernel m = walpe_gpustat_profile_kernel (setting, sys);
    g_free (sys);
    return m;
}

/* a display device on the PCI bus of tree `name` that has a driver and no
 * DRM node: NVIDIA's driver without its DRM module */
static void add_pci_only (const char *name, const char *slot, const char *driver, const char *vendor)
{
    char *dir = g_build_filename (top, name, "sys", "bus", "pci", "devices", slot, NULL);
    char *ue = g_strdup_printf ("DRIVER=%s\nPCI_SLOT_NAME=%s\n", driver, slot);
    static const char *const file[] = { "class", "vendor", "device", "boot_vga", "uevent" };
    const char *text[] = { "0x030200\n", vendor, "0x28a1\n", "0\n", ue };
    gboolean ok = g_mkdir_with_parents (dir, 0700) == 0;
    for (guint i = 0; ok && i < G_N_ELEMENTS (file); i++) {
        char *p = g_build_filename (dir, file[i], NULL);
        ok = g_file_set_contents (p, text[i], -1, NULL);
        g_free (p);
    }
    CHECK (ok, "cannot write the device %s into tree %s", slot, name);
    g_free (ue);
    g_free (dir);
}

static void test_profile_default (void)
{
    const WalpeGpuProfileKernel OFF = WALPE_GPU_PROFILE_KERNEL_OFF, SELF = WALPE_GPU_PROFILE_KERNEL_SELF,
                                ALL = WALPE_GPU_PROFILE_KERNEL_ALL;
    char drv[32];
    walpe_gpustat_set_render_device (NULL);
    walpe_gpustat_set_gl_vendor (NULL);

    /* NVIDIA's kernel driver: nothing is sampled unless the switch asks for it */
    tree ("nofdinfo", "nofdinfo", 0);
    CHECK (prof_mode ("nofdinfo", NULL, drv) == OFF && !strcmp (drv, "nvidia"),
           "profile on the NVIDIA driver: off when nothing is set (driver %s)", drv);
    CHECK (prof_mode ("nofdinfo", "", drv) == OFF, "NVIDIA driver: an empty value is no value");
    CHECK (prof_mode ("nofdinfo", "1", drv) == ALL && prof_mode ("nofdinfo", "on", drv) == ALL &&
           prof_mode ("nofdinfo", "all", drv) == ALL && prof_mode ("nofdinfo", "self", drv) == SELF,
           "NVIDIA driver: the switch turns it on");
    CHECK (prof_mode ("nofdinfo", "0", drv) == OFF && prof_mode ("nofdinfo", "off", drv) == OFF,
           "NVIDIA driver: and off");

    /* every other driver: all of it unless the switch says otherwise */
    static const struct { const char *scen, *driver; } other[] = {
        { "amdgpu", "amdgpu" }, { "i915", "i915" }, { "xe", "xe" }, { "nostats", "radeon" },
        { "platform", "v3d" }, { "flat", "amdgpu" },
    };
    for (guint i = 0; i < G_N_ELEMENTS (other); i++) {
        tree (other[i].scen, other[i].scen, 0);
        CHECK (prof_mode (other[i].scen, NULL, drv) == ALL && !strcmp (drv, other[i].driver),
               "profile on %s: everything when nothing is set (driver %s)", other[i].scen, drv);
        CHECK (prof_mode (other[i].scen, "self", drv) == SELF && prof_mode (other[i].scen, "0", drv) == OFF &&
               prof_mode (other[i].scen, "No", drv) == OFF && prof_mode (other[i].scen, "1", drv) == ALL,
               "profile on %s: the switch as before", other[i].scen);
    }

    /* no GPU in sysfs: nothing to hold it back ... */
    tree ("empty", "empty", 0);
    CHECK (prof_mode ("empty", NULL, drv) == ALL && !drv[0], "no GPU listed: no driver, the default");
    /* ... unless the process draws with NVIDIA's OpenGL: its kernel driver all the same */
    walpe_gpustat_set_gl_vendor ("NVIDIA Corporation");
    CHECK (prof_mode ("empty", NULL, drv) == OFF && !strcmp (drv, "nvidia"),
           "no GPU listed, NVIDIA's GL: off (driver %s)", drv);
    walpe_gpustat_set_gl_vendor (NULL);

    /* Intel (the boot GPU) and AMD */
    tree ("twogpu", "twogpu", 0);
    CHECK (prof_mode ("twogpu", NULL, drv) == ALL && !strcmp (drv, "i915"), "two GPUs: the boot GPU (%s)", drv);
    walpe_gpustat_set_gl_vendor ("AMD");
    CHECK (prof_mode ("twogpu", NULL, drv) == ALL && !strcmp (drv, "amdgpu"), "two GPUs: by GL vendor (%s)", drv);
    walpe_gpustat_set_render_device ("/dev/dri/renderD128");
    CHECK (prof_mode ("twogpu", NULL, drv) == ALL && !strcmp (drv, "i915"),
           "two GPUs: the device the daemon named comes first (%s)", drv);
    walpe_gpustat_set_render_device (NULL);
    walpe_gpustat_set_gl_vendor (NULL);

    /* a laptop: Intel drives the panel, an NVIDIA GPU with its own driver
     * (no DRM node) sits next to it. What counts is where the daemon draws. */
    tree ("laptop-nv", "i915", 0);
    add_pci_only ("laptop-nv", "0000:01:00.0", "nvidia", "0x10de\n");
    CHECK (prof_mode ("laptop-nv", NULL, drv) == ALL && !strcmp (drv, "i915"),
           "Intel + NVIDIA, nothing known: the boot GPU (%s)", drv);
    walpe_gpustat_set_gl_vendor ("Intel");
    CHECK (prof_mode ("laptop-nv", NULL, drv) == ALL && !strcmp (drv, "i915"), "Intel + NVIDIA, drawing on Intel");
    walpe_gpustat_set_gl_vendor ("NVIDIA Corporation");
    CHECK (prof_mode ("laptop-nv", NULL, drv) == OFF && !strcmp (drv, "nvidia"),
           "Intel + NVIDIA, drawing on NVIDIA: off (%s)", drv);
    CHECK (prof_mode ("laptop-nv", "self", drv) == SELF, "Intel + NVIDIA, drawing on NVIDIA: the switch");
    walpe_gpustat_set_gl_vendor (NULL);
    walpe_gpustat_set_render_device ("0000:01:00.0");
    CHECK (prof_mode ("laptop-nv", NULL, drv) == OFF && !strcmp (drv, "nvidia"),
           "Intel + NVIDIA, the daemon named the NVIDIA GPU: off (%s)", drv);
    walpe_gpustat_set_render_device (NULL);
}

/* ------------------------------------------- nvidia-smi through a stand-in */
static WalpeGpuStatReport *run_smi (const char *mode, guint flags)
{
    g_setenv ("FAKE_SMI", mode, TRUE);
    g_setenv ("FAKE_SMI_PID", G_STRINGIFY (DAEMON), TRUE);
    char *proc = g_build_filename (top, "smi", "proc", NULL);
    char *sys = g_build_filename (top, "smi", "sys", NULL);
    g_mkdir_with_parents (proc, 0700);
    g_mkdir_with_parents (sys, 0700);
    char *tool = g_build_filename (top, "nvidia-smi", NULL);
    WalpeGpuStatOpts o = { .proc_root = proc, .sys_root = sys, .target_pid = DAEMON, .flags = flags,
                           .nvidia_smi = tool, .uid = -1 };
    WalpeGpuSampler *s = walpe_gpu_sampler_new (&o);
    for (int i = 0; i < 3; i++)
        walpe_gpu_sampler_tick_at (s, (gint64) i * G_GINT64_CONSTANT (1000000000));
    WalpeGpuStatReport *r = walpe_gpu_sampler_report (s);
    walpe_gpu_sampler_free (s);
    g_free (proc);
    g_free (sys);
    g_free (tool);
    return r;
}

static void test_nvidia_smi (void)
{
    /* the stand-in as an executable file of its own name */
    char *tool = g_build_filename (top, "nvidia-smi", NULL);
    char *text = NULL;
    gsize len = 0;
    if (!g_file_get_contents (fake_smi, &text, &len, NULL) || !g_file_set_contents (tool, text, len, NULL) ||
        g_chmod (tool, 0700) != 0) {
        CHECK (FALSE, "cannot set up the stand-in nvidia-smi");
        g_free (text);
        g_free (tool);
        return;
    }
    g_free (text);
    g_free (tool);

    WalpeGpuStatReport *r = run_smi ("full", WALPE_GPUSTAT_OTHERS | WALPE_GPUSTAT_SPAWN);
    CHECK (!strcmp (r->driver, "nvidia") && !strcmp (r->name, "NVIDIA GeForce RTX 4050 Laptop GPU") &&
           !strcmp (r->driver_version, "550.120") && !strcmp (r->how_found, "the GPU nvidia-smi lists"),
           "nvidia-smi: the device (%s, %s, %s)", r->name, r->driver_version, r->how_found);
    CHECK (r->sources == (1u << WALPE_GPU_SRC_NVIDIA_SMI), "nvidia-smi: the only source");
    CHECK (r->m[WALPE_GPU_M_BUSY].src == WALPE_GPU_SRC_NVIDIA_SMI && NEAR (r->m[WALPE_GPU_M_BUSY].avg, 20.0, 0.01) &&
           NEAR (r->m[WALPE_GPU_M_VRAM_USED].avg, 900.0, 0.01) && NEAR (r->m[WALPE_GPU_M_VRAM_TOTAL].last, 6141.0, 0.01) &&
           NEAR (r->m[WALPE_GPU_M_POWER].avg, 12.5, 0.01) && NEAR (r->m[WALPE_GPU_M_TEMP].avg, 51.0, 0.01) &&
           NEAR (r->m[WALPE_GPU_M_CLOCK].avg, 600.0, 0.01) && NEAR (r->m[WALPE_GPU_M_CLOCK_MAX].last, 2400.0, 0.01),
           "nvidia-smi: whole-GPU values");
    CHECK (NEAR (r->m[WALPE_GPU_M_BUSY_SCALED].avg, 5.0, 0.01), "nvidia-smi: busy at full clock");
    CHECK (r->m[WALPE_GPU_M_PROC_BUSY].src == WALPE_GPU_SRC_NVIDIA_SMI && NEAR (r->m[WALPE_GPU_M_PROC_BUSY].avg, 10.0, 0.01) &&
           NEAR (r->m[WALPE_GPU_M_PROC_MEM].avg, 400.0, 0.01), "nvidia-smi: the daemon from pmon");
    const WalpeGpuProcStat *p = prog (r, "Xorg");
    CHECK (r->n_procs == 3 && p && NEAR (p->busy_pct, 3.0, 0.01) && p->mem_kb == 180 * 1024, "nvidia-smi: programs");
    char *t = rows_text (r);
    CHECK (strstr (t, "(nvidia-smi: utilization.gpu)") && strstr (t, "(nvidia-smi: pmon sm)") &&
           strstr (t, "(nvidia-smi: pmon fb)"), "nvidia-smi rows name it:\n%s", t);
    g_free (t);
    GString *s = g_string_new (NULL);
    walpe_gpu_stat_format_programs (r, 8, s);
    CHECK (g_str_has_prefix (s->str, "programs on the GPU (nvidia-smi pmon"), "nvidia-smi programs head");
    g_string_free (s, TRUE);
    walpe_gpu_stat_report_free (r);

    /* a laptop GPU: [N/A] fields, an unknown field, pmon without figures */
    r = run_smi ("laptop", WALPE_GPUSTAT_OTHERS | WALPE_GPUSTAT_SPAWN);
    CHECK (HAS (r, WALPE_GPU_M_BUSY) && !HAS (r, WALPE_GPU_M_POWER) && !HAS (r, WALPE_GPU_M_CLOCK) &&
           !HAS (r, WALPE_GPU_M_CLOCK_MAX) && !HAS (r, WALPE_GPU_M_PROC_BUSY) && !HAS (r, WALPE_GPU_M_PROC_MEM),
           "nvidia-smi laptop: what it does not report is not available");
    CHECK (strstr (r->m[WALPE_GPU_M_POWER].why, "nvidia-smi"), "nvidia-smi laptop: why");
    walpe_gpu_stat_report_free (r);

    /* the tool cannot reach the driver: no device from it, nothing invented */
    r = run_smi ("broken", WALPE_GPUSTAT_OTHERS | WALPE_GPUSTAT_SPAWN);
    CHECK (!r->driver[0] && r->sources == 0 && !HAS (r, WALPE_GPU_M_BUSY), "nvidia-smi broken: nothing");
    walpe_gpu_stat_report_free (r);

    /* without leave to run a program the tool is never started */
    r = run_smi ("full", WALPE_GPUSTAT_OTHERS);
    CHECK (!r->driver[0] && r->sources == 0, "nvidia-smi is not run without WALPE_GPUSTAT_SPAWN");
    walpe_gpu_stat_report_free (r);
}

int main (int argc, char **argv)
{
    if (argc < 3) {
        fprintf (stderr, "usage: gpustat-test <fake-gpustat-tree.sh> <fake-nvidia-smi>\n");
        return 2;
    }
    gen = argv[1];
    fake_smi = argv[2];
    GError *err = NULL;
    top = g_dir_make_tmp ("walpe-gpustat-XXXXXX", &err);
    if (!top) {
        fprintf (stderr, "no temporary directory: %s\n", err ? err->message : "?");
        return 2;
    }

    test_parser ();
    test_amdgpu ();
    test_intel ();
    test_none ();
    test_shared_and_counters ();
    test_exits ();
    test_permissions ();
    test_devices ();
    test_profile_default ();
    test_restart_and_self ();
    test_nvidia_smi ();

    /* the trees: one of them has a directory without permissions */
    const char *cargv[] = { "sh", gen, top, "clean", NULL };
    g_spawn_sync (NULL, (char **) cargv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
    const char *rargv[] = { "rm", "-rf", top, NULL };
    g_spawn_sync (NULL, (char **) rargv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
    g_free (top);

    if (fails) {
        fprintf (stderr, "gpustat: %d of %d checks FAILED\n", fails, checks);
        return 1;
    }
    printf ("gpustat: all %d checks passed\n", checks);
    return 0;
}
