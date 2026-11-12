/* mem_ctl.h - `walpe-ctl mem`: where the daemon's memory is. Included by
 * main.c only.
 *
 *   walpe-ctl mem                 the report as a table
 *   walpe-ctl mem --json          the report as the daemon sent it (one JSON object)
 *   walpe-ctl mem --deep          also the size of every program binary (asks the
 *                                 driver for each: slower)
 *   walpe-ctl mem --wait SECONDS  how long a monitor's render thread may take to
 *                                 answer (default 3, at most 60: a software
 *                                 renderer can be busy with one frame for longer)
 *   walpe-ctl mem --diff A B      what changed between two reports saved with
 *                                 --json (a local command: no daemon needed)
 *
 * The daemon computes the report when it is asked and at no other time
 * (engine/memreport.h): the process as the kernel and the allocator see
 * it, the counters, and per monitor the GL objects of its context and
 * what its wallpaper holds. */
#pragma once
#include <glib.h>
#include <json-glib/json-glib.h>
#include <math.h>
#include <string.h>

#include "common/ipc.h"
#include "common/util.h"

static double mem_n (JsonObject *o, const char *k)
{
    return o ? wpj_num (o, k, 0) : 0;
}

/* a.b.c of an object tree (NULL when a step is missing) */
static JsonObject *mem_o (JsonObject *o, const char *path)
{
    char **parts = g_strsplit (path, ".", -1);
    for (int i = 0; o && parts[i]; i++)
        o = wpj_obj (o, parts[i]);
    g_strfreev (parts);
    return o;
}

/* A text of the report as it may be printed: the report names files and
 * folders of wallpapers (the path a module was loaded for, the files the
 * daemon has mapped), and a name can hold anything but '/' and NUL - an
 * escape sequence too. tt () (main.c) makes control characters visible,
 * as for every other text walpe-ctl takes from a reply. */
static const char *mem_s (JsonObject *o, const char *key, const char *dflt)
{
    return tt (wpj_str (o, key, dflt));
}

#define MEM_MIB(bytes) ((double) (bytes) / 1048576.0)
#define MEM_KMIB(kb) ((double) (kb) / 1024.0)

static void mem_print_sizes (JsonObject *o, const char *indent, guint max)
{
    JsonArray *a = o ? wpj_arr (o, "by_size") : NULL;
    for (guint i = 0; i < wpj_arr_len (a) && i < max; i++) {
        JsonObject *r = wpj_arr_obj (a, i);
        g_print ("%s%5.0f x %-44s %9.1f MiB\n", indent, mem_n (r, "count"), mem_s (r, "what", "?"),
                 MEM_MIB (mem_n (r, "bytes")));
    }
    if (wpj_arr_len (a) > max)
        g_print ("%s      (%u more rows in --json)\n", indent, wpj_arr_len (a) - max);
}

static void mem_print_gl_shared (JsonObject *gl, const char *indent)
{
    JsonObject *t = wpj_obj (gl, "textures"), *bu = wpj_obj (gl, "buffers");
    const gboolean sizes = wpj_bool (gl, "sizes_known", FALSE);
    if (sizes)
        g_print ("%stextures %.0f = %.1f MiB (compressed %.0f = %.1f; render targets seen "
                 "from this monitor %.0f = %.1f)\n", indent, mem_n (t, "count"), MEM_MIB (mem_n (t, "bytes")),
                 mem_n (t, "compressed"), MEM_MIB (mem_n (t, "compressed_bytes")),
                 mem_n (t, "render_targets"), MEM_MIB (mem_n (t, "render_target_bytes")));
    else
        g_print ("%stextures %.0f (%s)\n", indent, mem_n (t, "count"), mem_s (gl, "sizes_note", ""));
    mem_print_sizes (t, indent, 14);
    if (sizes)
        g_print ("%sbuffers %.0f = %.2f MiB (largest %.2f MiB, mapped %.0f)", indent,
                 mem_n (bu, "count"), MEM_MIB (mem_n (bu, "bytes")),
                 MEM_MIB (mem_n (bu, "largest_bytes")), mem_n (bu, "mapped"));
    else
        g_print ("%sbuffers %.0f", indent, mem_n (bu, "count"));
    g_print ("; programs %.0f", mem_n (wpj_obj (gl, "programs"), "count"));
    if (json_object_has_member (wpj_obj (gl, "programs"), "binary_bytes"))
        g_print (" (binaries %.1f MiB)", MEM_MIB (mem_n (wpj_obj (gl, "programs"), "binary_bytes")));
    g_print (", shader objects %.0f (%.0f kB of source), renderbuffers %.0f, samplers %.0f\n",
             mem_n (wpj_obj (gl, "shaders"), "count"),
             mem_n (wpj_obj (gl, "shaders"), "source_bytes") / 1024.0,
             mem_n (wpj_obj (gl, "renderbuffers"), "count"),
             mem_n (wpj_obj (gl, "samplers"), "count"));
    JsonObject *vm = wpj_obj (gl, "video_memory");
    if (vm)
        g_print ("%svideo memory of the GPU: %.0f of %.0f MiB free (every program together), "
                 "%.0f evictions\n", indent, MEM_KMIB (mem_n (vm, "free_kb")),
                 MEM_KMIB (mem_n (vm, "total_kb")), mem_n (vm, "evictions"));
}

static void mem_print_content (JsonObject *c, const char *in)
{
    JsonObject *lt = wpj_obj (c, "layer_textures"), *tx = wpj_obj (c, "text"),
               *tg = wpj_obj (c, "targets"), *fx = wpj_obj (c, "effects"),
               *pt = wpj_obj (c, "particles"), *sc = wpj_obj (c, "script");
    if (!lt)
        return;     /* not a scene */
    g_print ("%sscene: %.0f layers (%.0f visible), %.0f frames drawn; layer textures %.0f = %.1f MiB"
             "%s; text %.0f layers, %.0f textures = %.1f MiB\n", in, mem_n (c, "layers"),
             mem_n (c, "layers_visible"), mem_n (c, "frames_drawn"), mem_n (lt, "count"),
             MEM_MIB (mem_n (lt, "bytes")), mem_n (lt, "not_loaded_yet") > 0 ? " (some not loaded yet)" : "",
             mem_n (tx, "layers"), mem_n (tx, "textures"), MEM_MIB (mem_n (tx, "bytes")));
    g_print ("%stargets: effects %.0f = %.1f MiB (pool %.0f = %.1f MiB, %.0f lent; most ever %.0f = "
             "%.1f MiB; owned by chains %.0f), captures %.0f = %.1f, bake %.1f, scaled frame %.1f\n",
             in, mem_n (fx, "thread_targets"), MEM_MIB (mem_n (fx, "thread_target_bytes")),
             mem_n (fx, "pool_targets"), MEM_MIB (mem_n (fx, "pool_bytes")), mem_n (fx, "pool_lent"),
             mem_n (fx, "pool_peak_targets"), MEM_MIB (mem_n (fx, "pool_peak_bytes")),
             mem_n (fx, "targets_owned_by_chains"), mem_n (tg, "captures"),
             MEM_MIB (mem_n (tg, "capture_bytes")), MEM_MIB (mem_n (tg, "bake_bytes")),
             MEM_MIB (mem_n (tg, "scaled_frame_bytes")));
    g_print ("%seffects: %.0f layers, %.0f chains (%.0f failed), %.0f passes holding %.0f programs "
             "+ %.0f generic kept + %.0f waiting; shared programs %.0f, source texts %.0f = %.1f MiB, "
             "pass texts %.1f MiB\n", in, mem_n (fx, "layers_with_effects"), mem_n (fx, "chains"),
             mem_n (fx, "chains_failed"), mem_n (fx, "passes"), mem_n (fx, "pass_programs"),
             mem_n (fx, "generic_programs_kept"), mem_n (fx, "candidate_programs"),
             mem_n (fx, "shared_programs"), mem_n (fx, "cached_sources"),
             MEM_MIB (mem_n (fx, "cached_source_bytes")), MEM_MIB (mem_n (fx, "pass_text_bytes")));
    g_print ("%sparticles: %.0f objects, %.0f systems, %.0f alive of %.0f allocated (max %.0f) = "
             "%.2f MiB; trail points %.0f = %.2f MiB; scratch %.2f MiB; legacy objects %.0f\n", in,
             mem_n (pt, "objects"), mem_n (pt, "systems"), mem_n (pt, "live"),
             mem_n (pt, "allocated"), mem_n (pt, "max"), MEM_MIB (mem_n (pt, "pool_bytes")),
             mem_n (pt, "trail_points"), MEM_MIB (mem_n (pt, "trail_bytes")),
             MEM_MIB (mem_n (pt, "thread_scratch_bytes")), mem_n (pt, "legacy_objects"));
    if (wpj_bool (sc, "measured", FALSE))
        g_print ("%sscripts: %.0f, heap %.2f MiB in %.0f blocks (limit %.0f MiB), live %.2f MiB; "
                 "%.0f objects, %.0f strings, %.0f functions%s%s\n", in, mem_n (sc, "scripts"),
                 MEM_MIB (mem_n (sc, "heap_bytes")), mem_n (sc, "heap_blocks"),
                 MEM_MIB (mem_n (sc, "heap_limit_bytes")), MEM_MIB (mem_n (sc, "live_bytes")),
                 mem_n (sc, "objects"), mem_n (sc, "strings"), mem_n (sc, "functions"),
                 json_object_has_member (sc, "state") ? "; " : "", mem_s (sc, "state", ""));
    else
        g_print ("%sscripts: %.0f%s%s\n", in, mem_n (sc, "scripts"),
                 json_object_has_member (sc, "state") ? ", " : "", mem_s (sc, "state", ""));
    JsonArray *vs = wpj_arr (c, "videos");
    for (guint i = 0; i < wpj_arr_len (vs); i++) {
        JsonObject *v = wpj_arr_obj (vs, i);
        g_print ("%svideo texture %u: %.0fx%.0f, texture %.0fx%.0f, %.0f frames shown%s\n", in, i,
                 mem_n (v, "width"), mem_n (v, "height"), mem_n (v, "texture_width"),
                 mem_n (v, "texture_height"), mem_n (v, "frames_shown"),
                 wpj_bool (v, "open", FALSE) ? "" : " (closed)");
    }
}

static void mem_print (JsonObject *m)
{
    JsonObject *p = wpj_obj (m, "process"), *ma = wpj_obj (m, "malloc"), *maps = wpj_obj (m, "mappings"),
               *fd = wpj_obj (m, "fds"), *ct = wpj_obj (m, "counters");
    const double up = mem_n (m, "uptime_s");
    g_print ("walpe-daemon %s, pid %.0f, running for %.0f h %02.0f min %02.0f s  (%s)\n",
             mem_s (m, "version", "?"), mem_n (p, "pid"), floor (up / 3600), floor (fmod (up, 3600) / 60),
             floor (fmod (up, 60)), mem_s (m, "time", ""));
    g_print ("\nprocess   resident %.1f MiB = anonymous %.1f + files %.1f + shared %.1f   (peak %.1f, "
             "swapped %.1f)\n          data %.1f MiB, address space %.0f MiB, threads %.0f, open "
             "descriptors %.0f\n", MEM_KMIB (mem_n (p, "rss_kb")), MEM_KMIB (mem_n (p, "rss_anon_kb")),
             MEM_KMIB (mem_n (p, "rss_file_kb")), MEM_KMIB (mem_n (p, "rss_shmem_kb")),
             MEM_KMIB (mem_n (p, "rss_peak_kb")), MEM_KMIB (mem_n (p, "swap_kb")),
             MEM_KMIB (mem_n (p, "data_kb")), MEM_KMIB (mem_n (p, "virtual_kb")), mem_n (p, "threads"),
             mem_n (fd, "count"));
    if (json_object_has_member (ma, "arena_kb"))
        g_print ("malloc    handed out %.1f MiB (heaps %.1f + own mappings %.1f in %.0f blocks), free "
                 "but kept %.1f MiB, heaps hold %.1f MiB, arenas %.0f\n",
                 MEM_KMIB (mem_n (ma, "in_use_kb") + mem_n (ma, "mmapped_kb")),
                 MEM_KMIB (mem_n (ma, "in_use_kb")), MEM_KMIB (mem_n (ma, "mmapped_kb")),
                 mem_n (ma, "mmapped_blocks"), MEM_KMIB (mem_n (ma, "free_kept_kb")),
                 MEM_KMIB (mem_n (ma, "arena_kb")), mem_n (ma, "arenas"));
    {
        /* anonymous memory nobody asked malloc for: a library's own
         * allocator (the GL driver), thread stacks */
        const double anon = mem_n (mem_o (maps, "heap"), "rss_kb") + mem_n (mem_o (maps, "anonymous"), "rss_kb");
        const double mal = mem_n (ma, "arena_kb") + mem_n (ma, "mmapped_kb");
        if (anon > 0 && json_object_has_member (ma, "arena_kb"))
            g_print ("          anonymous mappings hold %.1f MiB resident; malloc accounts for at most "
                     "%.1f MiB of it: at least %.1f MiB belong to something else (a GL driver's own "
                     "memory, thread stacks)\n", MEM_KMIB (anon), MEM_KMIB (mal),
                     MEM_KMIB (MAX (0.0, anon - mal)));
    }
    g_print ("\nmappings  %-10s %6s %12s %12s %12s\n", "kind", "count", "size MiB", "resident", "private");
    static const char *const kinds[] = { "heap", "anonymous", "stack", "nvidia", "dri", "memfd", "shm",
                                         "libraries", "files", "other" };
    for (guint i = 0; i < G_N_ELEMENTS (kinds); i++) {
        JsonObject *k = wpj_obj (maps, kinds[i]);
        if (!k || mem_n (k, "count") <= 0)
            continue;
        g_print ("          %-10s %6.0f %12.1f %12.1f %12.1f\n", kinds[i], mem_n (k, "count"),
                 MEM_KMIB (mem_n (k, "size_kb")), MEM_KMIB (mem_n (k, "rss_kb")),
                 MEM_KMIB (mem_n (k, "private_dirty_kb")));
    }
    JsonArray *la = wpj_arr (maps, "largest_anonymous");
    if (wpj_arr_len (la)) {
        g_print ("          largest anonymous (resident of size, MiB):");
        for (guint i = 0; i < wpj_arr_len (la); i++)
            g_print (" %.0f/%.0f", MEM_KMIB (mem_n (wpj_arr_obj (la, i), "rss_kb")),
                     MEM_KMIB (mem_n (wpj_arr_obj (la, i), "size_kb")));
        g_print ("\n");
    }
    JsonArray *lf = wpj_arr (maps, "largest_files");
    if (wpj_arr_len (lf)) {
        g_print ("          largest files (resident MiB):");
        for (guint i = 0; i < wpj_arr_len (lf) && i < 6; i++)
            g_print (" %s %.1f", mem_s (wpj_arr_obj (lf, i), "name", "?"),
                     MEM_KMIB (mem_n (wpj_arr_obj (lf, i), "rss_kb")));
        g_print ("\n");
    }
    JsonArray *bn = wpj_arr (maps, "by_name");
    if (wpj_arr_len (bn)) {
        JsonObject *sum = wpj_obj (maps, "by_name_sum");
        g_print ("          by name, the most resident (all of them, and by size, in --json)\n"
                 "            %-40s %5s %9s = %9s + %8s + %8s  %8s\n", "name", "count", "resident",
                 "anonymous", "file", "shared m", "of size");
        for (guint i = 0; i < wpj_arr_len (bn) && i < 14; i++) {
            JsonObject *r = wpj_arr_obj (bn, i);
            g_print ("            %-40.40s %5.0f %9.1f   %9.1f   %8.1f   %8.1f  %8.1f\n",
                     mem_s (r, "name", "?"), mem_n (r, "count"), MEM_KMIB (mem_n (r, "rss_kb")),
                     MEM_KMIB (mem_n (r, "anon_kb")), MEM_KMIB (mem_n (r, "file_kb")),
                     MEM_KMIB (mem_n (r, "shmem_kb")), MEM_KMIB (mem_n (r, "size_kb")));
        }
        if (sum)
            g_print ("            all rows: anonymous %.1f + file %.1f + shared memory %.1f MiB (the "
                     "kernel's own split is the first line)\n", MEM_KMIB (mem_n (sum, "anon_kb")),
                     MEM_KMIB (mem_n (sum, "file_kb")), MEM_KMIB (mem_n (sum, "shmem_kb")));
    }
    JsonObject *hp = wpj_obj (maps, "huge_pages");
    if (hp)
        g_print ("          in 2 MiB units (transparent huge pages, system setting \"%s\"): anonymous "
                 "%.1f, shared memory %.1f, file %.1f MiB\n", mem_s (hp, "setting", "?"),
                 MEM_KMIB (mem_n (hp, "anon_kb")), MEM_KMIB (mem_n (hp, "shmem_kb")),
                 MEM_KMIB (mem_n (hp, "file_kb")));
    JsonObject *th = wpj_obj (m, "threads");
    JsonArray *tn = wpj_arr (th, "by_name");
    if (wpj_arr_len (tn)) {
        g_print ("threads   %.0f:", mem_n (th, "count"));
        for (guint i = 0; i < wpj_arr_len (tn); i++)
            g_print ("%s %.0f x %s", i ? "," : "", mem_n (wpj_arr_obj (tn, i), "count"),
                     mem_s (wpj_arr_obj (tn, i), "name", "?"));
        g_print ("\n");
    }
    g_print ("fds       sockets %.0f, pipes %.0f, anonymous inodes %.0f, /dev/nvidia* %.0f, /dev/dri "
             "%.0f, memfd %.0f, files %.0f, other %.0f\n", mem_n (fd, "sockets"), mem_n (fd, "pipes"),
             mem_n (fd, "anon_inodes"), mem_n (fd, "nvidia"), mem_n (fd, "dri"), mem_n (fd, "memfd"),
             mem_n (fd, "files"), mem_n (fd, "other"));
    g_print ("\ncounters  requests %.0f (status built %.0f, of them %.0f for subscribers; %.0f "
             "subscribers now), memory reports %.0f\n          pauses %.0f, resumes %.0f, loads %.0f "
             "(failed %.0f), unloaded while paused %.0f, shared-rendering role changes %.0f\n"
             "          frames drawn while paused %.0f, scenes that gave back all targets %.0f, text "
             "renders %.0f, main-loop sources created %.0f, settings saved %.0f\n",
             mem_n (ct, "ipc_requests"), mem_n (ct, "status_built"),
             mem_n (ct, "status_built_for_subscribers"), mem_n (ct, "subscribers"),
             mem_n (ct, "mem_reports"), mem_n (ct, "pauses"), mem_n (ct, "resumes"), mem_n (ct, "loads"),
             mem_n (ct, "loads_failed"), mem_n (ct, "idle_releases"), mem_n (ct, "shared_role_changes"),
             mem_n (ct, "frames_drawn_while_paused"), mem_n (ct, "scene_target_drops"),
             mem_n (ct, "text_renders"), mem_n (ct, "main_loop_sources_created"),
             mem_n (ct, "config_saves"));
    if (json_object_has_member (ct, "fx_compares"))
        g_print ("          effect pass programs built %.0f, compared with the plain program %.0f "
                 "times (%.0f passes verified, %.0f rejected)\n", mem_n (ct, "fx_programs_compiled"),
                 mem_n (ct, "fx_compares"), mem_n (ct, "fx_verified"), mem_n (ct, "fx_rejected"));
    JsonObject *ev = wpj_obj (m, "gl_events");
    if (ev && wpj_bool (ev, "counting", FALSE)) {
        static const struct { const char *key, *label; } sized[] = {
            { "texture_stores_without_pixels", "texture storage without pixels (render targets)" },
            { "texture_stores_with_pixels", "texture storage with pixels" },
            { "texture_stores_compressed", "compressed texture storage" },
            { "buffer_storages", "buffers that stay mapped" },
            { "read_backs", "read-backs (glReadPixels)" },
        };
        g_print ("\nGL events counted for %.0f s (%.0f entry points): textures deleted %.0f (names "
                 "made %.0f), framebuffers made %.0f / deleted %.0f, buffers made %.0f / deleted "
                 "%.0f, programs linked %.0f / deleted %.0f, texture read-backs %.0f\n",
                 mem_n (ev, "seconds"), mem_n (ev, "entry_points"), mem_n (ev, "textures_deleted"),
                 mem_n (ev, "texture_names_made"), mem_n (ev, "framebuffers_made"),
                 mem_n (ev, "framebuffers_deleted"), mem_n (ev, "buffers_made"),
                 mem_n (ev, "buffers_deleted"), mem_n (ev, "programs_linked"),
                 mem_n (ev, "programs_deleted"), mem_n (ev, "texture_read_backs"));
        for (guint i = 0; i < G_N_ELEMENTS (sized); i++) {
            JsonObject *so = wpj_obj (ev, sized[i].key);
            if (!so || mem_n (so, "count") <= 0)
                continue;
            g_print ("          %s: %.0f = %.1f MiB (", sized[i].label, mem_n (so, "count"),
                     MEM_KMIB (mem_n (so, "total_kb")));
            JsonArray *bs = wpj_arr (so, "by_size");
            for (guint k = 0; k < wpj_arr_len (bs); k++)
                g_print ("%s%s: %.0f", k ? ", " : "", mem_s (wpj_arr_obj (bs, k), "what", "?"),
                         mem_n (wpj_arr_obj (bs, k), "count"));
            g_print (")\n");
        }
    } else if (ev) {
        g_print ("\nGL events not counted (`walpe-ctl mem --events on` starts counting how often "
                 "textures, framebuffers, buffers and programs are made and deleted)\n");
    }
    JsonObject *mods = wpj_obj (m, "modules");
    static const char *const mn[] = { "web", "video" };
    for (guint i = 0; mods && i < G_N_ELEMENTS (mn); i++) {
        JsonObject *mo = wpj_obj (mods, mn[i]);
        if (!mo)
            continue;
        if (wpj_bool (mo, "loaded", FALSE))
            g_print ("module    %s: loaded %.1f s after the start (%s); used %.0f times\n", mn[i],
                     mem_n (mo, "loaded_after_s"), mem_s (mo, "loaded_for", "?"), mem_n (mo, "uses"));
        else
            g_print ("module    %s: not loaded\n", mn[i]);
    }
    JsonObject *vm = wpj_obj (m, "video_module");
    if (vm)
        g_print ("video     %.0f clips for %.0f viewers: %.0f decoders open (%.0f hardware, %.0f "
                 "software threads), %.0f parked, %.0f paused; largest %.0fx%.0f\n          frames "
                 "queued %.0f of %.0f = %.1f MiB, scratch %.1f MiB, upload buffers %.0f = %.1f MiB; "
                 "the decoders' own pictures: up to %.0f MiB; %.0f frames decoded, %.0f decoders "
                 "opened so far\n", mem_n (vm, "clips"), mem_n (vm, "viewers"),
                 mem_n (vm, "decoders_open"), mem_n (vm, "hardware_decoders"),
                 mem_n (vm, "decode_threads"), mem_n (vm, "parked"), mem_n (vm, "paused"),
                 mem_n (vm, "largest_width"), mem_n (vm, "largest_height"), mem_n (vm, "frames_queued"),
                 mem_n (vm, "queue_slots"), MEM_MIB (mem_n (vm, "frame_bytes")),
                 MEM_MIB (mem_n (vm, "scratch_bytes")), mem_n (vm, "upload_buffers"),
                 MEM_MIB (mem_n (vm, "upload_buffer_bytes")),
                 MEM_MIB (mem_n (vm, "decoder_pool_estimate_bytes")), mem_n (vm, "frames_decoded"),
                 mem_n (vm, "decoders_opened"));
    if (*mem_s (m, "sound", ""))
        g_print ("sound     %s\n", mem_s (m, "sound", ""));

    const gboolean shared = wpj_bool (m, "gl_shared", FALSE);
    JsonArray *mons = wpj_arr (m, "monitors");
    g_print ("\nGL        %s / %s%s\n", mem_s (m, "gl_renderer", "?"), mem_s (m, "gl_version", "?"),
             shared ? "\n          one share group: textures, buffers, programs and shaders are the "
                      "same objects for every monitor (listed once)" : "");
    gboolean shared_done = FALSE;
    for (guint i = 0; i < wpj_arr_len (mons); i++) {
        JsonObject *mo = wpj_arr_obj (mons, i);
        if (wpj_bool (mo, "busy", FALSE)) {
            g_print ("\nmonitor %.0f %s: no answer (%s)\n", mem_n (mo, "index"), mem_s (mo, "name", "?"),
                     mem_s (mo, "note", ""));
            continue;
        }
        g_print ("\nmonitor %.0f %s %.0fx%.0f: %s%s, %s%s%s\n", mem_n (mo, "index"),
                 mem_s (mo, "name", "?"), mem_n (mo, "width"), mem_n (mo, "height"),
                 mem_s (mo, "type", "?"), wpj_bool (mo, "paused", FALSE) ? " (paused)" : "",
                 !g_strcmp0 (mem_s (mo, "role", "own"), "own") ? "its own wallpaper"
                 : !g_strcmp0 (mem_s (mo, "role", ""), "leads") ? "renders for other monitors too"
                 : "shows another monitor's frames",
                 json_object_has_member (mo, "note") ? "; " : "", mem_s (mo, "note", ""));
        g_print ("  frames %.0f, wake-ups %.0f, loads %.0f, pauses %.0f, resumes %.0f, frames drawn "
                 "while paused %.0f, unloaded while paused %.0f\n", mem_n (mo, "frames"),
                 mem_n (mo, "wakeups"), mem_n (mo, "loads"), mem_n (mo, "pauses"), mem_n (mo, "resumes"),
                 mem_n (mo, "frames_drawn_while_paused"), mem_n (mo, "idle_releases"));
        JsonObject *gl = wpj_obj (mo, "gl");
        if (gl) {
            if (!shared || !shared_done) {
                mem_print_gl_shared (gl, "  ");
                shared_done = TRUE;
            }
            g_print ("  of this monitor's context: framebuffers %.0f, vertex arrays %.0f, queries "
                     "%.0f; render targets %.0f = %.1f MiB; shared-frame slots %.0f = %.1f MiB "
                     "(census %.1f ms)\n", mem_n (wpj_obj (gl, "framebuffers"), "count"),
                     mem_n (wpj_obj (gl, "vertex_arrays"), "count"),
                     mem_n (wpj_obj (gl, "queries"), "count"),
                     mem_n (wpj_obj (gl, "textures"), "render_targets"),
                     MEM_MIB (mem_n (wpj_obj (gl, "textures"), "render_target_bytes")),
                     mem_n (wpj_obj (mo, "shared_frames"), "slot_textures"),
                     MEM_MIB (mem_n (wpj_obj (mo, "shared_frames"), "slot_bytes")),
                     mem_n (gl, "census_us") / 1000.0);
        }
        if (wpj_obj (mo, "content"))
            mem_print_content (wpj_obj (mo, "content"), "  ");
    }
    g_print ("\n(texture sizes are what the pictures take on the GPU, estimated from their formats; "
             "a driver may keep a copy of each in main memory as well. `walpe-ctl mem --json > "
             "FILE` twice and `walpe-ctl mem --diff FILE1 FILE2` show what grew.)\n");
}

/* ---- --diff: every number of two reports that differs */

/* how an element of an array is told from its neighbours: the member that
 * names it (so that a row keeps its name when the order changes) */
static char *mem_elem_key (JsonNode *n, guint pos)
{
    if (JSON_NODE_HOLDS_OBJECT (n)) {
        JsonObject *o = json_node_get_object (n);
        static const char *const ids[] = { "what", "name", "index" };
        for (guint i = 0; i < G_N_ELEMENTS (ids); i++) {
            JsonNode *v = json_object_get_member (o, ids[i]);
            if (!v || !JSON_NODE_HOLDS_VALUE (v))
                continue;
            if (json_node_get_value_type (v) == G_TYPE_STRING)
                return g_strdup_printf ("[%s]", json_node_get_string (v));
            return g_strdup_printf ("[%s %" G_GINT64_FORMAT "]", ids[i], json_node_get_int (v));
        }
    }
    return g_strdup_printf ("[%u]", pos);
}

static void mem_flatten (JsonNode *n, const char *path, GHashTable *out)
{
    if (!n)
        return;
    if (JSON_NODE_HOLDS_OBJECT (n)) {
        JsonObject *o = json_node_get_object (n);
        GList *ks = json_object_get_members (o);
        for (GList *l = ks; l; l = l->next) {
            char *p = *path ? g_strconcat (path, ".", (const char *) l->data, NULL)
                            : g_strdup (l->data);
            mem_flatten (json_object_get_member (o, l->data), p, out);
            g_free (p);
        }
        g_list_free (ks);
    } else if (JSON_NODE_HOLDS_ARRAY (n)) {
        JsonArray *a = json_node_get_array (n);
        for (guint i = 0; i < json_array_get_length (a); i++) {
            JsonNode *e = json_array_get_element (a, i);
            char *k = mem_elem_key (e, i), *p = g_strconcat (path, k, NULL);
            mem_flatten (e, p, out);
            g_free (k);
            g_free (p);
        }
    } else if (JSON_NODE_HOLDS_VALUE (n)) {
        GType t = json_node_get_value_type (n);
        if (t == G_TYPE_INT64 || t == G_TYPE_DOUBLE || t == G_TYPE_BOOLEAN) {
            double *v = g_new (double, 1);
            *v = t == G_TYPE_BOOLEAN ? (double) json_node_get_boolean (n) : json_node_get_double (n);
            g_hash_table_insert (out, g_strdup (path), v);
        }
    }
}

static JsonNode *mem_load (const char *file)
{
    char *text = NULL;
    if (!g_file_get_contents (file, &text, NULL, NULL)) {
        g_printerr ("mem --diff: cannot read %s\n", file);
        return NULL;
    }
    /* (a file may hold what was printed around the object: the first '{' on) */
    const char *st = strchr (text, '{');
    JsonNode *n = st ? wpj_parse_data (st, -1) : NULL;
    g_free (text);
    if (!n || !JSON_NODE_HOLDS_OBJECT (n)) {
        g_printerr ("mem --diff: %s is not a report saved with `walpe-ctl mem --json`\n", file);
        if (n)
            json_node_unref (n);
        return NULL;
    }
    return n;
}

static gint mem_key_cmp (gconstpointer a, gconstpointer b)
{
    return strcmp (*(const char *const *) a, *(const char *const *) b);
}

static int mem_diff (const char *fa, const char *fb)
{
    JsonNode *a = mem_load (fa), *b = a ? mem_load (fb) : NULL;
    if (!a || !b) {
        if (a)
            json_node_unref (a);
        return 1;
    }
    JsonObject *ao = json_node_get_object (a), *bo = json_node_get_object (b);
    /* (the report itself, with or without the reply around it) */
    JsonNode *am = json_object_has_member (ao, "mem") ? json_object_get_member (ao, "mem") : a;
    JsonNode *bm = json_object_has_member (bo, "mem") ? json_object_get_member (bo, "mem") : b;
    GHashTable *ha = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
    GHashTable *hb = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
    mem_flatten (am, "", ha);
    mem_flatten (bm, "", hb);
    GPtrArray *keys = g_ptr_array_new ();
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init (&it, ha);
    while (g_hash_table_iter_next (&it, &k, &v))
        g_ptr_array_add (keys, k);
    g_hash_table_iter_init (&it, hb);
    while (g_hash_table_iter_next (&it, &k, &v))
        if (!g_hash_table_contains (ha, k))
            g_ptr_array_add (keys, k);
    /* by path: the members of one object stay together */
    g_ptr_array_sort (keys, mem_key_cmp);
    guint shown = 0;
    g_print ("%-84s %14s %14s %14s\n", "what changed", "before", "after", "change");
    for (guint i = 0; i < keys->len; i++) {
        const char *key = keys->pdata[i];
        const double *x = g_hash_table_lookup (ha, key), *y = g_hash_table_lookup (hb, key);
        if (x && y && *x == *y)
            continue;
        /* how long the report took is not what it reports */
        if (g_str_has_suffix (key, "census_us") || g_str_has_suffix (key, "report_us"))
            continue;
        const double d = (y ? *y : 0) - (x ? *x : 0);
        char sa[32] = "-", sb[32] = "-", unit[40] = "";
        if (x)
            g_snprintf (sa, sizeof (sa), "%.0f", *x);
        if (y)
            g_snprintf (sb, sizeof (sb), "%.0f", *y);
        if (g_str_has_suffix (key, "bytes"))
            g_snprintf (unit, sizeof (unit), "  (%+.1f MiB)", d / 1048576.0);
        else if (g_str_has_suffix (key, "_kb"))
            g_snprintf (unit, sizeof (unit), "  (%+.1f MiB)", d / 1024.0);
        /* a row only one report has: a mapping, a size, a thread name that
         * appeared or is gone */
        if (!x || !y)
            g_strlcat (unit, x ? "  gone" : "  new", sizeof (unit));
        /* (the keys are names from the two files) */
        g_print ("%-84s %14s %14s %+14.0f%s\n", tt (key), sa, sb, d, unit);
        shown++;
    }
    if (!shown)
        g_print ("(no number differs)\n");
    g_ptr_array_unref (keys);
    g_hash_table_destroy (ha);
    g_hash_table_destroy (hb);
    json_node_unref (a);
    json_node_unref (b);
    return 0;
}

static int mem_main (int argc, char **argv)
{
    gboolean json = FALSE, deep = FALSE;
    const char *events = NULL;
    int wait_s = 3;
    for (int i = 2; i < argc; i++) {
        char *end = NULL;
        if (!strcmp (argv[i], "--json")) {
            json = TRUE;
        } else if (!strcmp (argv[i], "--deep")) {
            deep = TRUE;
        } else if (!strcmp (argv[i], "--wait") && i + 1 < argc &&
                   (wait_s = (int) g_ascii_strtoll (argv[i + 1], &end, 10)) >= 1 && wait_s <= 60 &&
                   end && !*end) {
            i++;
        } else if (!strcmp (argv[i], "--events") && i + 1 < argc &&
                   (!strcmp (argv[i + 1], "on") || !strcmp (argv[i + 1], "off"))) {
            events = argv[++i];
        } else if (!strcmp (argv[i], "--diff") && i == 2 && argc == 5) {
            return mem_diff (argv[3], argv[4]);
        } else {
            g_printerr ("walpe-ctl mem [--json] [--deep] [--wait SECONDS (1..60)] [--events on|off]\n"
                        "walpe-ctl mem --diff FILE1 FILE2   (two reports saved with --json)\n");
            return 1;
        }
    }
    char *sock = walpe_socket_path ();
    /* (each monitor's render thread writes its part between two frames:
     * the daemon waits that long for them) */
    char *req = g_strdup_printf ("{\"cmd\":\"mem\",\"deep\":%s,\"wait\":%d%s%s%s}",
                                 deep ? "true" : "false", wait_s, events ? ",\"events\":\"" : "",
                                 events ? events : "", events ? "\"" : "");
    char *reply = walpe_ipc_request (sock, req, wait_s * 1000 + 7000);
    g_free (req);
    int rc = 0;
    if (!reply) {
        no_reply (sock);
        g_free (sock);
        return 2;
    }
    JsonNode *rn = wpj_parse_data (reply, -1);
    JsonObject *ro = rn && JSON_NODE_HOLDS_OBJECT (rn) ? json_node_get_object (rn) : NULL;
    if (!ro || !wpj_bool (ro, "ok", FALSE) || !wpj_obj (ro, "mem")) {
        if (json)
            print_json (reply);
        else
            g_printerr ("%s\n", ro ? mem_s (ro, "error", "no memory report in the reply")
                                   : "the reply is not JSON");
        note_old_daemon (reply, "mem");
        rc = 3;
    } else if (json) {
        print_json (reply);
    } else {
        mem_print (wpj_obj (ro, "mem"));
    }
    if (rn)
        json_node_unref (rn);
    g_free (reply);
    g_free (sock);
    return rc;
}
