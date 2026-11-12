/* gpustat_ctl.h - `walpe-ctl gpu-stats`: GPU usage and memory as the kernel
 * and the drivers account them (common/gpustat.h), for any vendor. Included
 * by main.c only. (`walpe-ctl gpu` is a different command: which GPU the
 * daemon uses and may use.)
 *
 *   walpe-ctl gpu-stats [--sample SECONDS] [--interval SECONDS] [--pid PID | --no-process]
 *                       [--device card1|renderD129|0000:03:00.0] [--json] [--no-nvidia-smi]
 *
 * A local command: it reads /proc and /sys itself and works without a
 * running daemon (then it measures the GPU alone). With a daemon it reports
 * on the daemon's process and follows the device the daemon renders on.
 *
 * For walpe-gpu-report (not for people):
 *   --probe      one reading, no waiting: is there a device, are there figures
 *   --sections   the parts of the report between "@name" lines
 * For the tests: --proc-root DIR, --sys-root DIR (trees written by
 * tests/fake-gpustat-tree.sh), --nvidia-smi PROGRAM. */
#pragma once
#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common/gpustat.h"
#include "common/ipc.h"
#include "common/util.h"

/* the running walpe-daemon of this user: the lowest pid whose process name
 * is "walpe-daemon" (only names are read), 0 = none */
static int gpustat_find_daemon (const char *proc_root)
{
    GDir *dir = g_dir_open (proc_root, 0, NULL);
    if (!dir)
        return 0;
    int found = 0;
    const char *name;
    while ((name = g_dir_read_name (dir))) {
        if (!g_ascii_isdigit (name[0]))
            continue;
        int pid = atoi (name);
        if (pid <= 0 || (found && pid > found))
            continue;
        char *path = g_build_filename (proc_root, name, "comm", NULL);
        char *comm = NULL;
        GStatBuf st;
        char *pdir = g_build_filename (proc_root, name, NULL);
        gboolean mine = g_stat (pdir, &st) == 0 && st.st_uid == getuid ();
        g_free (pdir);
        if (mine && g_file_get_contents (path, &comm, NULL, NULL) && comm) {
            g_strchomp (comm);
            if (!strcmp (comm, "walpe-daemon"))
                found = pid;
        }
        g_free (comm);
        g_free (path);
    }
    g_dir_close (dir);
    return found;
}

static void gpustat_usage (void)
{
    g_printerr ("walpe-ctl gpu-stats [--sample SECONDS] [--interval SECONDS] [--pid PID | --no-process]\n"
                "                    [--device card1|renderD129|0000:03:00.0] [--json] [--no-nvidia-smi]\n");
}

static int gpustat_main (int argc, char **argv)
{
    double seconds = 2.0, interval = 1.0;
    int pid = -1;                 /* -1 = look for the daemon, 0 = no process */
    gboolean json = FALSE, sections = FALSE, probe = FALSE, spawn = TRUE;
    const char *device = NULL, *proc_root = NULL, *sys_root = NULL, *smi = NULL;

    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        const char *val = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp (a, "--json")) {
            json = TRUE;
        } else if (!strcmp (a, "--sections")) {
            sections = TRUE;
        } else if (!strcmp (a, "--probe")) {
            probe = TRUE;
        } else if (!strcmp (a, "--no-process")) {
            pid = 0;
        } else if (!strcmp (a, "--no-nvidia-smi")) {
            spawn = FALSE;
        } else if (!strcmp (a, "--sample") && val) {
            seconds = g_ascii_strtod (val, NULL);
            i++;
        } else if (!strcmp (a, "--interval") && val) {
            interval = g_ascii_strtod (val, NULL);
            i++;
        } else if (!strcmp (a, "--pid") && val) {
            pid = atoi (val);
            if (pid <= 0) {
                g_printerr ("--pid takes a process id\n");
                return 1;
            }
            i++;
        } else if (!strcmp (a, "--device") && val) {
            device = val;
            i++;
        } else if (!strcmp (a, "--proc-root") && val) {
            proc_root = val;
            i++;
        } else if (!strcmp (a, "--sys-root") && val) {
            sys_root = val;
            i++;
        } else if (!strcmp (a, "--nvidia-smi") && val) {
            smi = val;
            i++;
        } else {
            gpustat_usage ();
            return 1;
        }
    }
    if (!(seconds >= 0.2))
        seconds = 0.2;
    if (seconds > 3600.0)
        seconds = 3600.0;
    interval = CLAMP (interval, 0.1, 60.0);
    if (!(interval <= seconds))
        interval = seconds;

    gboolean real = !proc_root && !sys_root;
    if (pid < 0)
        pid = gpustat_find_daemon (proc_root ? proc_root : "/proc");

    /* what the daemon says: the device it renders on (status "gpu_in_use":
     * its PCI address, else its DRM node; "software" = it does not render
     * on a GPU at all), and its GL strings, which name the GPU in words and
     * tell two devices apart when the first is not there (an older daemon) */
    char *gl_vendor = NULL, *gl_renderer = NULL, *in_use = NULL;
    gboolean software = FALSE;
    if (real && pid > 0) {
        char *sock = walpe_socket_path ();
        char *reply = walpe_ipc_request (sock, "{\"cmd\":\"status\"}", 800);
        JsonNode *rn = reply ? wpj_parse_data (reply, -1) : NULL;
        JsonObject *ro = rn && JSON_NODE_HOLDS_OBJECT (rn) ? json_node_get_object (rn) : NULL;
        JsonObject *st = ro ? wpj_obj (ro, "state") : NULL;
        if (st) {
            gl_vendor = g_strdup (wpj_str (st, "gl_vendor", NULL));
            gl_renderer = g_strdup (wpj_str (st, "gl_renderer", NULL));
            JsonObject *giu = wpj_obj (st, "gpu_in_use");
            if (giu) {
                const char *pci = wpj_str (giu, "pci", NULL), *card = wpj_str (giu, "card_node", NULL);
                in_use = g_strdup (pci && *pci ? pci : card && *card ? card : NULL);
                software = wpj_bool (giu, "software", FALSE);
            }
            software = software || wpj_bool (st, "software_active", FALSE);
        }
        if (rn)
            json_node_unref (rn);
        g_free (reply);
        g_free (sock);
    }

    WalpeGpuStatOpts o = {
        .proc_root = proc_root,
        .sys_root = sys_root,
        .target_pid = pid,
        .flags = WALPE_GPUSTAT_OTHERS | (spawn && !probe ? WALPE_GPUSTAT_SPAWN : 0),
        .device = device ? device : in_use,
        .gl_vendor = software ? NULL : gl_vendor,
        .nvidia_smi = smi,
        .uid = -1,
    };
    WalpeGpuSampler *s = walpe_gpu_sampler_new (&o);
    walpe_gpu_sampler_tick (s);
    if (!probe) {
        gint64 t0 = g_get_monotonic_time ();
        gint64 end = t0 + (gint64) (seconds * G_USEC_PER_SEC);
        gint64 step = (gint64) (interval * G_USEC_PER_SEC);
        for (gint64 next = t0 + step;; next += step) {
            gint64 now = g_get_monotonic_time ();
            gint64 until = MIN (next, end);
            if (until > now)
                g_usleep ((gulong) (until - now));
            walpe_gpu_sampler_tick (s);
            if (until >= end)
                break;
            /* a sample that took longer than the interval (nvidia-smi): no
             * burst of samples afterwards */
            now = g_get_monotonic_time ();
            if (now > next + step)
                next = now - step;
        }
    }
    WalpeGpuStatReport *r = walpe_gpu_sampler_report (s);

    if (json) {
        JsonBuilder *b = json_builder_new ();
        walpe_gpu_stat_to_json (r, b);
        JsonNode *root = json_builder_get_root (b);
        JsonGenerator *g = json_generator_new ();
        json_generator_set_root (g, root);
        json_generator_set_pretty (g, TRUE);
        json_generator_set_indent (g, 1);
        char *txt = json_generator_to_data (g, NULL);
        print_json (txt);
        g_free (txt);
        g_object_unref (g);
        json_node_unref (root);
        g_object_unref (b);
    } else {
        GString *t = g_string_new (NULL);
        gboolean any = r->sources != 0 || r->fdinfo_engines || r->fdinfo_memory;
        if (sections)
            g_string_append (t, "@walpe-gpustat 1\n@device\n");
        g_string_append (t, "GPU: ");
        walpe_gpu_stat_format_device (r, t);
        if (gl_renderer && *gl_renderer && !r->name[0])
            g_string_append_printf (t, " | GL: %s", tt (gl_renderer));
        if (software)
            g_string_append (t, " | the daemon renders on the CPU (software rendering): the GPU's "
                             "figures are not its");
        g_string_append (t, "\nGPU statistics from: ");
        walpe_gpu_stat_format_sources (r, t);
        if (r->how_found[0])
            g_string_append_printf (t, " (device chosen as: %s%s)", r->how_found,
                                    r->n_devices > 1 ? "; this machine has several GPUs" : "");
        g_string_append_c (t, '\n');
        if (sections) {
            /* found: the kernel lists a GPU; stats: some source has figures
             * for it; nvidia: it is driven by the NVIDIA driver */
            gboolean found = r->driver[0] || r->pci_slot[0] || r->card[0] || r->name[0];
            g_string_append_printf (t, "@found %s\n@stats %s\n@nvidia %s\n", found ? "yes" : "no",
                                    any ? "yes" : "no",
                                    !strcmp (r->driver, "nvidia") || (r->vendor_id == 0x10de && !r->driver[0])
                                    ? "yes" : "no");
        } else {
            if (r->target_pid)
                g_string_append_printf (t, "process: %s pid %d%s\n",
                                        r->target_name[0] ? r->target_name : "?", r->target_pid,
                                        r->target_seen ? "" : " (not found)");
            else
                g_string_append (t, "process: none (no walpe-daemon runs: the GPU alone)\n");
        }
        if (!probe) {
            if (sections)
                g_string_append (t, "@rows\n");
            else
                g_string_append_printf (t, "\n%-26s %-7s %9s %9s %9s  %s\n", "metric", "unit", "min",
                                        "avg", "max", "samples");
            walpe_gpu_stat_format_rows (r, r->target_pid ? "daemon" : NULL, t);
            g_string_append (t, sections ? "@programs\n" : "\n");
            walpe_gpu_stat_format_programs (r, 8, t);
            g_string_append (t, sections ? "@notes\n" : "\nhow to read this\n");
            walpe_gpu_stat_format_notes (r, t);
        }
        if (sections)
            g_string_append (t, "@end\n");
        g_print ("%s", t->str);
        g_string_free (t, TRUE);
    }
    walpe_gpu_stat_report_free (r);
    walpe_gpu_sampler_free (s);
    g_free (gl_vendor);
    g_free (gl_renderer);
    g_free (in_use);
    return 0;
}
