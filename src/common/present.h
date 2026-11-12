/* present.h - how a monitor's frames reach the screen: the swap interval
 * of its window, the tuning file (gpu.conf) and the hints handed to the
 * GL driver before EGL starts. Pure functions, no state, no GL: shared by
 * the daemon and tests/present_test.c (like pace.h).
 *
 * Two ways to present a frame, one per monitor, never both pacing:
 *   vsync   - swap interval 1. The frame clock decides when a frame is
 *             rendered (it sleeps to the deadline, it never spins); the
 *             driver shows it at the display's next refresh and makes the
 *             thread wait only when it is ahead of the display.
 *   limiter - swap interval 0. The frame clock alone paces (sleep to the
 *             deadline, render, swap, no wait in the driver). Right where
 *             something else puts the picture on the screen at the
 *             display's refresh anyway: a compositing manager.
 *   auto    - limiter where a vsync'd swap cannot improve the picture
 *             (walpe_present_interval), vsync elsewhere. */
#pragma once
#include <glib.h>
#include <string.h>

typedef enum {
    WALPE_PRESENT_VSYNC = 0,
    WALPE_PRESENT_LIMITER,
    WALPE_PRESENT_AUTO,
} WalpePresentMode;

static inline const char *walpe_present_mode_name (WalpePresentMode m)
{
    return m == WALPE_PRESENT_LIMITER ? "limiter" : m == WALPE_PRESENT_AUTO ? "auto" : "vsync";
}

static inline gboolean walpe_present_mode_parse (const char *s, WalpePresentMode *out)
{
    static const struct { const char *name; WalpePresentMode mode; } names[] = {
        { "vsync", WALPE_PRESENT_VSYNC }, { "limiter", WALPE_PRESENT_LIMITER },
        { "auto", WALPE_PRESENT_AUTO },
    };
    for (guint i = 0; s && i < G_N_ELEMENTS (names); i++)
        if (!g_ascii_strcasecmp (s, names[i].name)) {
            *out = names[i].mode;
            return TRUE;
        }
    return FALSE;
}

/* the mode of one monitor from a setting "MODE[,NAME=MODE]...": a bare
 * mode is the default of every monitor, NAME=MODE (the output name from
 * `walpe-ctl status`, or monN) overrides it for one, e.g.
 * "auto,HDMI-0=limiter". NULL / "" = vsync (what every round up to 12
 * did). A token that is not understood is skipped; *bad (may be NULL)
 * receives a copy of the first one (g_free). */
static inline WalpePresentMode walpe_present_spec_mode (const char *spec, const char *name,
                                                        int index, char **bad)
{
    WalpePresentMode all = WALPE_PRESENT_VSYNC, own = WALPE_PRESENT_VSYNC;
    gboolean have_own = FALSE;
    if (bad)
        *bad = NULL;
    char **tok = g_strsplit (spec ? spec : "", ",", -1);
    char monn[24];
    g_snprintf (monn, sizeof monn, "mon%d", index);
    for (int i = 0; tok[i]; i++) {
        char *t = g_strstrip (tok[i]);
        if (!*t)
            continue;
        const char *eq = strchr (t, '=');
        WalpePresentMode m;
        gboolean ok;
        if (eq) {
            char *n = g_strstrip (g_strndup (t, (gsize) (eq - t)));
            char *v = g_strstrip (g_strdup (eq + 1));
            ok = *n && walpe_present_mode_parse (v, &m);
            if (ok && ((name && !g_ascii_strcasecmp (n, name)) || !g_ascii_strcasecmp (n, monn))) {
                own = m;
                have_own = TRUE;
            }
            g_free (n);
            g_free (v);
        } else {
            ok = walpe_present_mode_parse (t, &m);
            if (ok)
                all = m;
        }
        if (!ok && bad && !*bad)
            *bad = g_strdup (t);
    }
    g_strfreev (tok);
    return have_own ? own : all;
}

/* what the swap interval of a monitor depends on besides its mode */
typedef struct {
    gboolean composited;     /* a compositing manager owns _NET_WM_CM_S<screen> */
    const char *sync_device; /* __GL_SYNC_DISPLAY_DEVICE as the driver will see it,
                              * NULL = unset */
    gboolean sync_is_ours;   /* ... and it names one of this daemon's monitors */
    int min_interval;        /* EGL_MIN_SWAP_INTERVAL of the config */
    int max_interval;        /* EGL_MAX_SWAP_INTERVAL */
} WalpePresentEnv;

/* the swap interval of monitor `name`, and (why, may be NULL) the reason
 * as a static string for the log.
 *  auto: 0 under a compositing manager - the window is redirected into an
 *        off-screen pixmap and the compositor shows it at the display's
 *        refresh, so a swap that also waits for a vertical blank only
 *        adds a wait; 0 on a monitor the driver is told not to
 *        synchronise to (NVIDIA: OpenGL can sync to one display device,
 *        __GL_SYNC_DISPLAY_DEVICE names it) - its swaps would wait for
 *        another display's blank, which neither stops tearing here nor
 *        matches this monitor's rate; 1 otherwise.
 * A config that cannot do the interval (EGL_MIN/MAX_SWAP_INTERVAL) gets
 * the nearest it can. */
static inline int walpe_present_interval (WalpePresentMode mode, const char *name,
                                          const WalpePresentEnv *e, const char **why)
{
    int iv = 1;
    const char *w = "vsync: the swap waits for the display";
    if (mode == WALPE_PRESENT_LIMITER) {
        iv = 0;
        w = "limiter: the frame clock paces, the swap does not wait";
    } else if (mode == WALPE_PRESENT_AUTO) {
        if (e->composited) {
            iv = 0;
            w = "auto: a compositing manager presents the window, the swap does not wait";
        } else if (e->sync_device && e->sync_is_ours && name &&
                   g_ascii_strcasecmp (e->sync_device, name)) {
            iv = 0;
            w = "auto: the driver synchronises to another display, the swap does not wait";
        } else {
            w = "auto: no compositing manager, the swap waits for the display";
        }
    }
    int lo = e->min_interval, hi = MAX (e->min_interval, e->max_interval);
    if (iv < lo) {
        iv = lo;
        w = "the EGL config has no swap interval 0: the swap waits for the display";
    } else if (iv > hi) {
        iv = hi;
        w = "the EGL config has no swap interval 1: the swap does not wait";
    }
    if (why)
        *why = w;
    return iv;
}

/* ------------------------------------------------------------ gpu.conf
 * ~/.config/walpe/gpu.conf: "key = value" lines, '#' comments. The same
 * keys can be given in the environment as WALPE_<KEY> (upper case); the
 * environment wins. Keys: see walpe_gpuconf_keys. Unknown keys are
 * reported by the daemon and ignored. */
static inline GHashTable *walpe_gpuconf_parse (const char *text)
{
    GHashTable *h = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
    char **lines = g_strsplit (text ? text : "", "\n", -1);
    for (int i = 0; lines[i]; i++) {
        char *hash = strchr (lines[i], '#');
        if (hash)
            *hash = 0;
        char *eq = strchr (lines[i], '=');
        if (!eq)
            continue;
        *eq = 0;
        char *k = g_ascii_strdown (g_strstrip (lines[i]), -1);
        char *v = g_strdup (g_strstrip (eq + 1));
        if (*k)
            g_hash_table_replace (h, k, v);
        else {
            g_free (k);
            g_free (v);
        }
    }
    g_strfreev (lines);
    return h;
}

/* every key gpu.conf understands (NULL-terminated) */
static inline const char *const *walpe_gpuconf_keys (void)
{
    static const char *const keys[] = {
        "present", "idle_release", "egl_display", "swap_damage",
        "frame_align",
        "wayland_reconnect",    /* seconds to wait for a Wayland compositor that went away */
        "yield", "threaded", "max_frames", "sync_display", "shader_cache", "shader_cache_size",
        NULL
    };
    return keys;
}

static inline gboolean walpe_conf_bool (const char *v, gboolean *out)
{
    static const char *const on[] = { "1", "on", "true", "yes" };
    static const char *const off[] = { "0", "off", "false", "no" };
    for (guint i = 0; v && i < G_N_ELEMENTS (on); i++) {
        if (!g_ascii_strcasecmp (v, on[i])) {
            *out = TRUE;
            return TRUE;
        }
        if (!g_ascii_strcasecmp (v, off[i])) {
            *out = FALSE;
            return TRUE;
        }
    }
    return FALSE;
}

/* a driver hint from gpu.conf -> the environment variable(s) the NVIDIA
 * driver reads (its README, "Specifying OpenGL Environment Variable
 * Settings"; __GL_MaxFramesAllowed is not in it). Appends "NAME=VALUE"
 * strings (g_free) to out. Returns FALSE when key is no driver hint or
 * the value is not understood; an empty value or "default" sets nothing
 * and returns TRUE. Other drivers ignore these variables. */
static inline gboolean walpe_driver_hint (const char *key, const char *value, GPtrArray *out)
{
    gboolean b = FALSE;
    if (!key || !value)
        return FALSE;
    gboolean unset = !*value || !g_ascii_strcasecmp (value, "default");
    if (!strcmp (key, "yield")) {
        /* how the driver waits when it has to: sched_yield() (driver
         * default), usleep(0), or no yield at all */
        if (unset)
            return TRUE;
        if (!g_ascii_strcasecmp (value, "usleep"))
            g_ptr_array_add (out, g_strdup ("__GL_YIELD=USLEEP"));
        else if (!g_ascii_strcasecmp (value, "nothing"))
            g_ptr_array_add (out, g_strdup ("__GL_YIELD=NOTHING"));
        else
            return FALSE;
        return TRUE;
    }
    if (!strcmp (key, "threaded")) {
        if (unset)
            return TRUE;
        if (!walpe_conf_bool (value, &b))
            return FALSE;
        g_ptr_array_add (out, g_strdup_printf ("__GL_THREADED_OPTIMIZATIONS=%d", b ? 1 : 0));
        return TRUE;
    }
    if (!strcmp (key, "max_frames") || !strcmp (key, "shader_cache_size")) {
        if (unset)
            return TRUE;
        char *end = NULL;
        gint64 n = g_ascii_strtoll (value, &end, 10);
        gboolean frames = key[0] == 'm';
        if (!end || *end || n < 1 || (frames && n > 8))
            return FALSE;
        g_ptr_array_add (out, g_strdup_printf ("%s=%" G_GINT64_FORMAT,
                                               frames ? "__GL_MaxFramesAllowed"
                                                      : "__GL_SHADER_DISK_CACHE_SIZE", n));
        return TRUE;
    }
    if (!strcmp (key, "sync_display")) {
        if (unset)
            return TRUE;
        for (const char *p = value; *p; p++)
            if (!g_ascii_isalnum (*p) && *p != '-' && *p != '_' && *p != '.')
                return FALSE;
        g_ptr_array_add (out, g_strdup_printf ("__GL_SYNC_DISPLAY_DEVICE=%s", value));
        return TRUE;
    }
    if (!strcmp (key, "shader_cache")) {
        /* on | off | a directory (absolute): the driver's own disk cache
         * of compiled shaders */
        if (unset)
            return TRUE;
        if (value[0] == '/') {
            g_ptr_array_add (out, g_strdup ("__GL_SHADER_DISK_CACHE=1"));
            g_ptr_array_add (out, g_strdup_printf ("__GL_SHADER_DISK_CACHE_PATH=%s", value));
            return TRUE;
        }
        if (!walpe_conf_bool (value, &b))
            return FALSE;
        g_ptr_array_add (out, g_strdup_printf ("__GL_SHADER_DISK_CACHE=%d", b ? 1 : 0));
        return TRUE;
    }
    return FALSE;
}

/* The same hints for Mesa's drivers (radeonsi, iris, crocus, nouveau,
 * ...), where Mesa has a setting that means the same. Appends
 * "NAME=VALUE" strings (g_free) to out:
 *   threaded          -> mesa_glthread=true|false
 *   shader_cache      -> MESA_SHADER_CACHE_DISABLE=false|true, and for a
 *                        directory MESA_SHADER_CACHE_DIR=<dir>
 *   shader_cache_size -> MESA_SHADER_CACHE_MAX_SIZE=<n>M (the hint is in
 *                        bytes, as the NVIDIA driver takes it)
 * yield, max_frames and sync_display have no Mesa equivalent (every
 * window is synchronised to the display it is on): *none = TRUE and
 * nothing is appended. Returns FALSE when key is no driver hint or the
 * value is not understood (walpe_driver_hint says the same). */
static inline gboolean walpe_driver_hint_mesa (const char *key, const char *value,
                                               GPtrArray *out, gboolean *none)
{
    gboolean b = FALSE;
    if (none)
        *none = FALSE;
    if (!key || !value)
        return FALSE;
    gboolean unset = !*value || !g_ascii_strcasecmp (value, "default");
    if (!strcmp (key, "yield") || !strcmp (key, "max_frames") || !strcmp (key, "sync_display")) {
        if (!unset && none)
            *none = TRUE;
        return TRUE;
    }
    if (!strcmp (key, "threaded")) {
        if (unset)
            return TRUE;
        if (!walpe_conf_bool (value, &b))
            return FALSE;
        g_ptr_array_add (out, g_strdup_printf ("mesa_glthread=%s", b ? "true" : "false"));
        return TRUE;
    }
    if (!strcmp (key, "shader_cache_size")) {
        if (unset)
            return TRUE;
        char *end = NULL;
        gint64 n = g_ascii_strtoll (value, &end, 10);
        if (!end || *end || n < 1)
            return FALSE;
        gint64 mb = (n + (1 << 20) - 1) >> 20;
        g_ptr_array_add (out, g_strdup_printf ("MESA_SHADER_CACHE_MAX_SIZE=%" G_GINT64_FORMAT "M",
                                               CLAMP (mb, (gint64) 1, (gint64) 65536)));
        return TRUE;
    }
    if (!strcmp (key, "shader_cache")) {
        if (unset)
            return TRUE;
        if (value[0] == '/') {
            g_ptr_array_add (out, g_strdup ("MESA_SHADER_CACHE_DISABLE=false"));
            g_ptr_array_add (out, g_strdup_printf ("MESA_SHADER_CACHE_DIR=%s", value));
            return TRUE;
        }
        if (!walpe_conf_bool (value, &b))
            return FALSE;
        g_ptr_array_add (out, g_strdup_printf ("MESA_SHADER_CACHE_DISABLE=%s", b ? "false" : "true"));
        return TRUE;
    }
    return FALSE;
}

/* can the NVIDIA driver's shader disk cache work in this environment?
 * (the driver's README: off for root unless forced, and when none of
 * __GL_SHADER_DISK_CACHE_PATH, XDG_CACHE_HOME, HOME is set.) NULL = yes,
 * else why not, as a static string. Says nothing about other drivers. */
static inline const char *walpe_shader_cache_blocker (gboolean is_root, gboolean forced_on,
                                                      gboolean have_path, gboolean have_xdg_cache,
                                                      gboolean have_home)
{
    if (is_root && !forced_on)
        return "running as root (the driver keeps its shader disk cache off for root unless "
               "__GL_SHADER_DISK_CACHE=1)";
    if (!have_path && !have_xdg_cache && !have_home)
        return "none of __GL_SHADER_DISK_CACHE_PATH, XDG_CACHE_HOME and HOME is set (the driver "
               "has nowhere to keep its shader disk cache)";
    return NULL;
}
