/* swrender.h - "Render on CPU": Mesa's software renderer (llvmpipe)
 * instead of the GPU driver.
 *
 * The GL renderer is not touched. The switch only decides which GL
 * implementation the daemon's EGL context comes from. EGL goes through
 * libglvnd, which asks the vendor libraries listed in
 * /etc/glvnd/egl_vendor.d and /usr/share/glvnd/egl_vendor.d in file-name
 * order (10_nvidia.json before 50_mesa.json): on an NVIDIA machine the
 * NVIDIA driver answers first. With __EGL_VENDOR_LIBRARY_FILENAMES set to
 * Mesa's file libglvnd loads Mesa alone, and LIBGL_ALWAYS_SOFTWARE=1 +
 * GALLIUM_DRIVER=llvmpipe make Mesa render on the CPU and present with
 * plain X requests (PutImage / MIT-SHM), which every X server takes.
 *
 * libglvnd reads those variables once, on the first EGL call of a
 * process, so the daemon sets them before it touches EGL and a change of
 * the setting restarts the daemon in place (exec).
 *
 * This file is the part without EGL / X11: finding Mesa's vendor file,
 * the environment, the thread count, the probe's report. */
#pragma once
#include <glib.h>

/* libglvnd's own search path for EGL vendor files */
#define WALPE_SW_VENDOR_DIRS "/etc/glvnd/egl_vendor.d:/usr/share/glvnd/egl_vendor.d"

/* Mesa's EGL vendor file (the *.json whose ICD.library_path names
 * libEGL_mesa): its path (g_free) or NULL. `dirs`: ':'-separated
 * directories, NULL = where libglvnd looks (__EGL_VENDOR_LIBRARY_DIRS,
 * else WALPE_SW_VENDOR_DIRS). *searched (optional, g_free): the
 * directories looked at, for the message when nothing was found. */
char *walpe_sw_find_mesa_vendor (const char *dirs, char **searched);

/* llvmpipe rasteriser threads (LP_NUM_THREADS). configured > 0: that many
 * (at most 32); 0 = automatic: half of the logical CPUs, at least 1, at
 * most 8, so that software rendering never takes every core. */
int walpe_sw_threads (int configured, int ncpu);

/* WALPE_SOFTWARE: "1" / "on" / "true" / "yes" -> 1, "0" / "off" / "false"
 * / "no" -> 0, unset / empty / anything else -> -1 (the config decides) */
int walpe_sw_env_override (const char *value);

/* The variables of software mode as key, value, key, value, ..., NULL
 * (g_strfreev). LP_NUM_THREADS is left out when `keep_lp_threads` (the
 * user set it). */
char **walpe_sw_env_pairs (const char *vendor_json, int threads, gboolean keep_lp_threads);

/* `env` (g_get_environ() form, consumed) with the pairs applied */
char **walpe_sw_environ (char **env, char **pairs);

/* a GL_RENDERER of a CPU rasteriser (llvmpipe, softpipe, swrast) */
gboolean walpe_sw_renderer_is_software (const char *renderer);
gboolean walpe_sw_renderer_is_llvmpipe (const char *renderer);

/* The probe child's report: lines "key=value" on stdout (vendor,
 * renderer, version, egl, fail). TRUE when it drew a frame with
 * llvmpipe; else *why (g_free) says what is missing. The strings are set
 * when the report has them (g_free), NULL otherwise. */
gboolean walpe_sw_parse_probe (const char *out, char **vendor, char **renderer,
                               char **version, char **why);
