/* gpu_glue.h - the daemon's side of the GPU choice (common/gpusel.h).
 *
 * Included by main.c (static functions, like layers_ipc.h). It needs D,
 * Sw, init_egl (), create_thread_context (), ipc_set () and the jb_*
 * helpers of main.c.
 *
 * Two calls meet the window-system code:
 *
 *   gpu_setup ()     before the first EGL call of the process and before
 *                    any thread exists: resolves the `gpu` setting and,
 *                    when another GPU than the display's is asked for,
 *                    sets the driver's environment variables - after a
 *                    child process (`walpe-daemon --gpu-probe`) has shown
 *                    that a window can be drawn that way. With gpu =
 *                    display it reads the device list (not the PCI id
 *                    list) and sets nothing. So it does with gpu = auto
 *                    on every machine but one where an integrated GPU
 *                    drives the displays and a discrete one that can be
 *                    used sits beside it: there auto is tried as
 *                    `discrete` is, and the window system's GPU renders
 *                    when the test fails.
 *   gpu_note_egl ()  after eglInitialize: which device EGL really uses,
 *                    for `status`, the log and the modules.
 *
 * Nothing here creates an EGL display in the daemon itself, and nothing
 * here is asked while frames are drawn.
 *
 * WALPE_GPU=<value>       overrides the saved setting for this run
 * WALPE_GPU_QUERY=0       do not ask EGL for its device (EGL_EXT_device_
 *                         query); the device is then taken from the EGL
 *                         vendor and the device list
 * WALPE_GPU_SYSFS_ROOT    another sysfs (tests) */

#ifndef EGL_DRM_RENDER_NODE_FILE_EXT
#define EGL_DRM_RENDER_NODE_FILE_EXT 0x3377
#endif

static struct {
    char *setting;          /* what decides this run: WALPE_GPU, else config "gpu" */
    gboolean env_forced;    /* WALPE_GPU decides, not the config */
    GPtrArray *devs;        /* the machine's graphics devices (WalpeGpuDev) */
    gboolean named;         /* their names are those of the PCI id list (gpu_names) */
    int display;            /* the device that drives the displays, -1 = not known */
    int target;             /* the device the setting names or auto chose, -1 = none */
    gboolean by_auto;       /* ... auto chose it (the setting names no device) */
    int in_use;             /* the device EGL renders on, -1 = not known */
    gboolean offload;       /* variables for another GPU than the display's are set */
    char *env_text;         /* those variables, "NAME=VALUE ..." */
    char *note;             /* why the setting is not honoured, or what it costs */
    WalpeGpuWinsys winsys;
    gboolean probing;       /* this process is the --gpu-probe child */
    guint restart_id;
} Gpu = { .display = -1, .target = -1, .in_use = -1 };

static WalpeGpuDev *gpu_dev (int idx)
{
    return Gpu.devs && idx >= 0 && (guint) idx < Gpu.devs->len ? g_ptr_array_index (Gpu.devs, idx)
                                                               : NULL;
}

static void gpu_set_note (char *note)   /* takes note */
{
    g_free (Gpu.note);
    Gpu.note = note;
}

/* The devices' names from the PCI id list (1.5 MB of text, read once).
 * Only when a value that names a device is in play: it may name it by a
 * part of its name, and the notes about it carry the names. With gpu =
 * auto or display the daemon never reads the list: its log lines and its
 * status name a PCI device by vendor and ids, and `walpe-ctl gpu` reads
 * the list in its own process. */
static void gpu_names (void)
{
    if (Gpu.named || !Gpu.devs)
        return;
    Gpu.named = TRUE;
    walpe_gpu_name_devices (Gpu.devs);
}

/* ------------------------------------------------- what EGL renders on */

/* The DRM nodes of the device behind an initialised EGL display, and
 * Mesa's name for its driver; each NULL when EGL does not say. Asked
 * through eglGetProcAddress, so a library without the extension is not a
 * problem. Call it while no context is current on this thread. */
static void gpu_egl_device (EGLDisplay dpy, char **card, char **render, char **gl_driver)
{
    *card = *render = *gl_driver = NULL;
    const char *q = g_getenv ("WALPE_GPU_QUERY");
    if (q && *q == '0')
        return;
    const char *client = eglQueryString (EGL_NO_DISPLAY, EGL_EXTENSIONS);
    if (!client)
        (void) eglGetError ();      /* EGL 1.4 without client extensions */
    const char *dext = eglQueryString (dpy, EGL_EXTENSIONS);
    if (client && (epoxy_extension_in_string (client, "EGL_EXT_device_query") ||
                   epoxy_extension_in_string (client, "EGL_EXT_device_base"))) {
        EGLBoolean (*query_display) (EGLDisplay, EGLint, EGLAttrib *) =
            (EGLBoolean (*) (EGLDisplay, EGLint, EGLAttrib *))
                eglGetProcAddress ("eglQueryDisplayAttribEXT");
        const char *(*query_device) (EGLDeviceEXT, EGLint) =
            (const char *(*) (EGLDeviceEXT, EGLint)) eglGetProcAddress ("eglQueryDeviceStringEXT");
        EGLAttrib dev = 0;
        if (query_display && query_device && query_display (dpy, EGL_DEVICE_EXT, &dev) && dev) {
            const char *de = query_device ((EGLDeviceEXT) dev, EGL_EXTENSIONS);
            if (de && epoxy_extension_in_string (de, "EGL_EXT_device_drm")) {
                const char *f = query_device ((EGLDeviceEXT) dev, EGL_DRM_DEVICE_FILE_EXT);
                if (f && *f)
                    *card = g_strdup (f);
            }
            if (de && epoxy_extension_in_string (de, "EGL_EXT_device_drm_render_node")) {
                const char *f = query_device ((EGLDeviceEXT) dev, EGL_DRM_RENDER_NODE_FILE_EXT);
                if (f && *f)
                    *render = g_strdup (f);
            }
        }
        (void) eglGetError ();      /* a refused query is not an error of ours */
    }
    if (dext && epoxy_extension_in_string (dext, "EGL_MESA_query_driver")) {
        const char *(*driver_name) (EGLDisplay) =
            (const char *(*) (EGLDisplay)) eglGetProcAddress ("eglGetDisplayDriverName");
        const char *n = driver_name ? driver_name (dpy) : NULL;
        if (n && *n)
            *gl_driver = g_strdup (n);
        (void) eglGetError ();
    }
}

/* Mesa's name for a CPU rasteriser */
static gboolean gpu_gl_driver_is_software (const char *gl_driver)
{
    return gl_driver && (!strcmp (gl_driver, "swrast") || !strcmp (gl_driver, "kms_swrast") ||
                         !strcmp (gl_driver, "llvmpipe") || !strcmp (gl_driver, "softpipe"));
}

/* -------------------------------------------------------------- probe */

/* How long one test process may take before it is ended (SIGALRM), in
 * seconds: for a GPU the user named, and for the discrete GPU gpu = auto
 * found. A GPU that was powered down has to wake within this; when it
 * does not, the window system's GPU renders for this run. */
#define GPU_PROBE_LIMIT_S      10
#define GPU_PROBE_LIMIT_AUTO_S 5

/* `walpe-daemon --gpu-probe`, run by gpu_setup () in a child that has the
 * environment to be tried: the daemon's own path once - the display of
 * the window system the parent chose (--backend, as for --sw-probe), EGL
 * on it, its config, a context from its ladder, the platform layer's
 * probe surface (X11: a window as the desktop windows are made, never
 * mapped), one frame. Reports key=value lines on stdout
 * (walpe_gpu_parse_probe). On X11 an X error ends the child through
 * Xlib's default handler: no "drew=1".
 * (The steps are sw_probe_run ()'s, plus the device lines.) */
static int gpu_probe_run (int argc, char **argv)
{
    /* never hold up the daemon's start: 10 s, or what the parent allows
     * (--gpu-probe-limit S: a choice nobody asked for by name gets less) */
    guint limit = GPU_PROBE_LIMIT_S;
    for (int i = 1; i + 1 < argc; i++)
        if (!g_strcmp0 (argv[i], "--gpu-probe-limit"))
            limit = (guint) CLAMP (atoi (argv[i + 1]), 1, GPU_PROBE_LIMIT_S);
    alarm (limit);
    Gpu.probing = TRUE;
    char *perr = NULL;
    if (!probe_open (argc, argv, &perr)) {
        g_print ("fail=%s\n", perr ? perr : "cannot open the display");
        return 1;
    }
    if (walpe_platform_egl_display () == EGL_NO_DISPLAY) {
        g_print ("fail=no EGL display on this window system with these settings\n");
        return 1;
    }
    if (!init_egl ()) {
        g_print ("fail=EGL does not start on this display with these settings\n");
        return 1;
    }
    char *card = NULL, *render = NULL, *gl_driver = NULL;
    gpu_egl_device (D.egl_dpy, &card, &render, &gl_driver);
    if (card)
        g_print ("device=%s\n", card);
    if (render)
        g_print ("render_node=%s\n", render);
    if (gl_driver)
        g_print ("egl_driver=%s\n", gl_driver);
    const char *ev = eglQueryString (D.egl_dpy, EGL_VENDOR);
    g_print ("egl=%s\n", ev ? ev : "?");
    WalpeSurfaceSpec spec = { .kind = WALPE_SURFACE_PROBE, .width = 64, .height = 64 };
    EGLSurface surf = EGL_NO_SURFACE;
    int pw = 0, ph = 0;
    WalpeSurface *ps = walpe_surface_new (&spec, D.egl_dpy, D.egl_conf, &surf, &pw, &ph);
    EGLContext ctx = ps ? create_thread_context () : EGL_NO_CONTEXT;
    if (!ps || ctx == EGL_NO_CONTEXT ||
        !eglMakeCurrent (D.egl_dpy, surf, surf, ctx)) {
        g_print ("fail=no window surface or no GL 3.3 / GLES 3 context on this display (EGL "
                 "error 0x%x)\n", eglGetError ());
        return 1;
    }
    const char *v = (const char *) glGetString (GL_VENDOR);
    const char *r = (const char *) glGetString (GL_RENDERER);
    const char *ver = (const char *) glGetString (GL_VERSION);
    g_print ("vendor=%s\nrenderer=%s\nversion=%s\n", v ? v : "?", r ? r : "?", ver ? ver : "?");
    glViewport (0, 0, 64, 64);
    glClearColor (0, 0, 0, 1);
    glClear (GL_COLOR_BUFFER_BIT);
    gboolean drew = eglSwapBuffers (D.egl_dpy, surf) && glGetError () == GL_NO_ERROR;
    walpe_platform_sync ();   /* an X error of the presentation arrives here */
    g_print ("drew=%d\n", drew ? 1 : 0);
    return 0;
}

/* Runs the probe with `pairs` (key, value, ..., NULL; may be NULL) on top
 * of this process's environment, for at most limit_s seconds. TRUE: a
 * hardware renderer drew a frame; *idx = the device it reports (-1 = it
 * does not say), *renderer its GL_RENDERER (g_free). FALSE: *why (g_free). */
static gboolean gpu_probe (char **pairs, guint limit_s, int *idx, char **renderer, char **why)
{
    *idx = -1;
    *renderer = *why = NULL;
    char **envp = walpe_sw_environ (g_get_environ (), pairs);
    char lim[16];
    g_snprintf (lim, sizeof lim, "%u", limit_s);
    /* the child draws on the window system this process chose */
    char *pargv[] = { Sw.exe, (char *) "--gpu-probe", (char *) "--backend",
                      (char *) walpe_backend_name (D.backend), (char *) "--gpu-probe-limit", lim,
                      NULL };
    char *out = NULL, *errout = NULL;
    GError *e = NULL;
    gboolean ok = FALSE;
    int wstatus = 0;
    if (!g_spawn_sync (NULL, pargv, envp, G_SPAWN_DEFAULT, NULL, NULL, &out, &errout, &wstatus,
                       &e)) {
        *why = g_strdup_printf ("the test could not be run (%s)", e ? e->message : "?");
        g_clear_error (&e);
    } else if (WIFSIGNALED (wstatus) && WTERMSIG (wstatus) == SIGALRM) {
        /* (its own alarm: gpu_probe_run) */
        *why = g_strdup_printf ("the test did not finish within %u s", limit_s);
    } else {
        char *dev = NULL, *rn = NULL;
        ok = walpe_gpu_parse_probe (out, &dev, &rn, renderer, why);
        *idx = walpe_gpu_index_of_node (Gpu.devs, rn);
        if (*idx < 0)
            *idx = walpe_gpu_index_of_node (Gpu.devs, dev);
        if (!ok && errout && *errout)
            g_debug ("gpu test, stderr: %s", errout);
        g_free (dev);
        g_free (rn);
    }
    g_free (out);
    g_free (errout);
    g_strfreev (envp);
    return ok;
}

/* ------------------------------------------------------- driver hints */

/* gpu.conf's driver hints (present.h) are settings of the NVIDIA driver.
 * When a Mesa driver will render, the ones that have a Mesa equivalent
 * are handed to Mesa as well; the others are named once as having no
 * effect. The NVIDIA variables set by present_settings_load () stay as
 * they are in every case. */
static void gpu_mesa_hints (void)
{
    const WalpeGpuDev *d = gpu_dev (Gpu.offload ? Gpu.target : Gpu.display);
    if (Sw.active || !d || !d->driver || !*d->driver || walpe_gpu_driver_is_nvidia (d->driver))
        return;
    static const char *const hints[] = { "yield", "threaded", "max_frames", "sync_display",
                                         "shader_cache", "shader_cache_size" };
    for (guint i = 0; i < G_N_ELEMENTS (hints); i++) {
        const char *val = present_setting (hints[i]);
        if (!val)
            continue;
        GPtrArray *ev = g_ptr_array_new_with_free_func (g_free);
        gboolean none = FALSE;
        if (walpe_driver_hint_mesa (hints[i], val, ev, &none)) {
            if (none)
                g_message ("present: %s is a setting of the NVIDIA driver: no effect on %s (%s)",
                           hints[i], d->name, d->driver);
            for (guint j = 0; j < ev->len; j++) {
                char *nv = g_ptr_array_index (ev, j), *eq = strchr (nv, '=');
                *eq = 0;
                if (g_getenv (nv)) {
                    g_message ("present: %s is set in the environment: kept (%s ignored)", nv,
                               hints[i]);
                } else {
                    g_setenv (nv, eq + 1, FALSE);
                    g_message ("present: driver hint %s=%s (%s, Mesa)", nv, eq + 1, hints[i]);
                }
            }
        }
        g_ptr_array_unref (ev);
    }
}

/* -------------------------------------------------------------- set-up */

/* GPU choice: hand-over point for the window-system layer (call 1 of 2).
 * Main thread, after sw_setup (), before the first EGL call and before
 * any thread exists (setenv). outputs / display_node: see gpusel.h
 * walpe_gpu_display_index (); both may be NULL. */
static void gpu_setup (WalpeGpuWinsys winsys, const char *const *outputs, const char *display_node)
{
    Gpu.winsys = winsys;
    if (!Gpu.devs)
        Gpu.devs = walpe_gpu_enumerate_plain (NULL);
    const char *e = g_getenv ("WALPE_GPU");
    char *env_val = e && *e ? walpe_gpu_value_normalize (e) : NULL;
    if (e && *e && !env_val)
        g_warning ("gpu: WALPE_GPU is not a usable value: ignored");
    Gpu.env_forced = env_val != NULL;
    g_free (Gpu.setting);
    Gpu.setting = env_val ? env_val : g_strdup (D.cfg && D.cfg->gpu ? D.cfg->gpu : "auto");
    const gboolean is_auto = !strcmp (Gpu.setting, "auto");
    if (!is_auto && !walpe_gpu_value_is_display (Gpu.setting))
        gpu_names ();
    /* the device that renders for the displays when nothing is set (on a
     * system-on-chip not the display controller itself) */
    Gpu.display = walpe_gpu_default_renderer (
        Gpu.devs, walpe_gpu_display_index (Gpu.devs, outputs, display_node));
    for (guint i = 0; i < Gpu.devs->len; i++) {
        char *d = walpe_gpu_dev_describe (g_ptr_array_index (Gpu.devs, i));
        g_message ("gpu: device %u: %s%s", i, d,
                   (int) i == Gpu.display ? " - the window system's GPU" : "");
        g_free (d);
    }
    if (walpe_gpu_value_is_display (Gpu.setting)) {
        /* nothing is set: EGL renders on the GPU of the window system */
        gpu_mesa_hints ();
        return;
    }
    char *why = NULL;
    if (is_auto) {
        /* A discrete GPU beside an integrated one that drives the displays?
         * Told from the list read above. On every other machine - one GPU,
         * or a card that drives the displays itself - auto is `display`:
         * nothing more is read, nothing is started, nothing is set. */
        Gpu.target = Sw.active ? -1 : walpe_gpu_auto_target (Gpu.devs, Gpu.display, &why);
        if (Gpu.target < 0) {
            if (why) {
                g_message ("gpu = auto: %s", why);
                gpu_set_note (why);
            }
            gpu_mesa_hints ();
            return;
        }
        Gpu.by_auto = TRUE;
    } else {
        if (Sw.active) {
            gpu_set_note (g_strdup_printf ("gpu = %s has no effect while \"Render on CPU\" is on",
                                           Gpu.setting));
            g_message ("gpu: %s", Gpu.note);
            return;
        }
        Gpu.target = walpe_gpu_match (Gpu.devs, Gpu.setting, Gpu.display, &why);
        if (Gpu.target < 0) {
            g_warning ("gpu = %s: %s. Rendering on the GPU that drives the displays.", Gpu.setting,
                       why ? why : "no such device");
            gpu_set_note (why);
            gpu_mesa_hints ();
            return;
        }
    }
    WalpeGpuDev *t = gpu_dev (Gpu.target);
    /* a variable of the kind set below that the user has set is the
     * user's own choice of GPU: it is not replaced */
    for (const char *const *n = walpe_gpu_offload_env_names (); *n; n++)
        if (g_getenv (*n)) {
            gpu_set_note (g_strdup_printf ("%s is set in the environment: gpu = %s is not applied "
                                           "(unset the variable to let WalPE choose)", *n,
                                           Gpu.setting));
            if (is_auto) {
                /* (nobody asked for a device: nothing went wrong) */
                g_message ("gpu: %s", Gpu.note);
                Gpu.target = -1;
            } else {
                g_warning ("gpu: %s", Gpu.note);
            }
            gpu_mesa_hints ();
            return;
        }
    /* auto: a choice nobody asked for by name may hold up the start for
     * less time than one that was asked for */
    const guint limit_s = is_auto ? GPU_PROBE_LIMIT_AUTO_S : GPU_PROBE_LIMIT_S;
    const gint64 t0 = g_get_monotonic_time ();
    /* which device renders when nothing is set: EGL's own answer, from a
     * child process, beats what sysfs suggests */
    char *base_renderer = NULL;
    gboolean base_unnamed = FALSE;      /* a GPU drew, and EGL does not say which */
    {
        int idx = -1;
        char *w = NULL;
        if (gpu_probe (NULL, limit_s, &idx, &base_renderer, &w)) {
            if (idx >= 0)
                Gpu.display = idx;
            else
                base_unnamed = TRUE;
        }
        g_free (w);
    }
    if (is_auto && Gpu.display != Gpu.target) {
        /* The same question again with EGL's answer: the window system may
         * render on the discrete GPU although another one has the outputs
         * (an X server set up that way) - then that is a card driving the
         * displays, and nothing is set. Where EGL names no device, its
         * renderer's name is taken for the answer when it carries the
         * vendor of the discrete GPU and not the integrated one's. */
        int again = walpe_gpu_auto_target (Gpu.devs, Gpu.display, NULL);
        const WalpeGpuDev *dd = gpu_dev (Gpu.display);
        const gboolean on_target = base_unnamed && dd &&
                                   walpe_gpu_renderer_is_vendor (base_renderer, t->vendor) &&
                                   !walpe_gpu_renderer_is_vendor (base_renderer, dd->vendor);
        if (again < 0 || on_target) {
            g_message ("gpu = auto: the window system renders on %s, nothing to set (asked in "
                       "%d ms)", on_target ? base_renderer : dd ? dd->name : "its own GPU",
                       (int) ((g_get_monotonic_time () - t0) / 1000));
            Gpu.target = -1;
            Gpu.by_auto = FALSE;
            g_free (base_renderer);
            gpu_mesa_hints ();
            return;
        }
        Gpu.target = again;
        t = gpu_dev (Gpu.target);
    }
    g_free (base_renderer);
    if (Gpu.display == Gpu.target) {
        g_message ("gpu = %s: %s is the GPU that drives the displays, nothing to set",
                   Gpu.setting, t->name);
        if (is_auto) {
            Gpu.target = -1;
            Gpu.by_auto = FALSE;
        }
        gpu_mesa_hints ();
        return;
    }
    char *mesa_json = walpe_gpu_find_egl_vendor (NULL, "egl_mesa");
    char *nv_json = walpe_gpu_find_egl_vendor (NULL, "egl_nvidia");
    char *env_note = NULL;
    char **pairs = walpe_gpu_offload_env (gpu_dev (Gpu.display), t, winsys, mesa_json, nv_json,
                                          &env_note);
    g_free (mesa_json);
    g_free (nv_json);
    if (!pairs[0]) {
        g_log (G_LOG_DOMAIN, is_auto ? G_LOG_LEVEL_MESSAGE : G_LOG_LEVEL_WARNING,
               "gpu = %s: %s. Rendering on the GPU that drives the displays.", Gpu.setting,
               env_note ? env_note : "nothing can be set for this device");
        gpu_set_note (env_note ? env_note : g_strdup ("nothing can be set for this device"));
        g_strfreev (pairs);
        if (is_auto) {
            Gpu.target = -1;
            Gpu.by_auto = FALSE;
        }
        gpu_mesa_hints ();
        return;
    }
    int idx = -1;
    char *renderer = NULL;
    why = NULL;
    gboolean ok = gpu_probe (pairs, limit_s, &idx, &renderer, &why);
    const int took_ms = (int) ((g_get_monotonic_time () - t0) / 1000);
    if (ok && idx >= 0 && idx != Gpu.target) {
        ok = FALSE;
        why = g_strdup_printf ("the driver still renders on %s", gpu_dev (idx)->name);
    }
    if (ok) {
        GString *txt = g_string_new (NULL);
        for (int i = 0; pairs[i] && pairs[i + 1]; i += 2) {
            g_setenv (pairs[i], pairs[i + 1], TRUE);
            g_string_append_printf (txt, "%s%s=%s", txt->len ? " " : "", pairs[i], pairs[i + 1]);
        }
        Gpu.offload = TRUE;
        Gpu.env_text = g_string_free (txt, FALSE);
        const WalpeGpuDev *dd = gpu_dev (Gpu.display);
        gpu_set_note (g_strdup_printf ("rendering on %s while %s drives the displays: every "
                                       "frame is copied from one GPU to the other%s",
                                       t->name, dd ? dd->name : "another GPU",
                                       is_auto ? " (gpu = auto takes a discrete GPU when there is "
                                                 "one; gpu = display renders on the displays' GPU "
                                                 "and lets this one sleep)" : ""));
        g_message ("gpu = %s: %s (%s; renderer: %s; tested in %d ms)", Gpu.setting, Gpu.note,
                   Gpu.env_text, renderer ? renderer : "?", took_ms);
        if (env_note)
            g_message ("gpu: %s", env_note);
        g_free (env_note);
    } else {
        const WalpeGpuDev *dd = gpu_dev (Gpu.display);
        gpu_set_note (g_strdup_printf ("%s could not be made the renderer (%s%s%s): rendering on "
                                       "%s%s", t->name, why ? why : "the test failed",
                                       env_note ? "; " : "", env_note ? env_note : "",
                                       dd ? dd->name : "the GPU that drives the displays",
                                       is_auto ? " (gpu = auto tried the discrete GPU; gpu = "
                                                 "display does not try)" : ""));
        if (is_auto)
            /* (nobody asked for that device: the start goes on as it did
             * before auto looked for one) */
            g_message ("gpu = auto: %s (tested in %d ms)", Gpu.note, took_ms);
        else
            g_warning ("gpu = %s: %s", Gpu.setting, Gpu.note);
        g_free (env_note);
        Gpu.target = -1;
        Gpu.by_auto = FALSE;
    }
    g_free (why);
    g_free (renderer);
    g_strfreev (pairs);
    gpu_mesa_hints ();
}

/* ------------------------------------------- the preset follows the GPU */

static gboolean gpu_follow_preset_cb (gpointer user)
{
    char *preset = user;
    /* exactly what `walpe-ctl set preset=...` does */
    JsonObject *o = json_object_new ();
    json_object_set_string_member (o, "quality_preset", preset);
    JsonBuilder *b = json_builder_new ();
    json_builder_begin_object (b);
    ipc_set (b, o);
    json_builder_end_object (b);
    g_object_unref (b);
    json_object_unref (o);
    g_free (preset);
    return G_SOURCE_REMOVE;
}

/* The kernel-side figures (common/gpustat.h: status "gpu_stats", the
 * profile section) are told which device renders, so that they are about
 * it also when the daemon has DRM clients on two GPUs. A CPU rasteriser:
 * no device. */
static void gpu_tell_stats (void)
{
    WalpeGpuInUse u;
    if (!walpe_gpu_in_use (&u))
        return;
    walpe_gpustat_set_render_device (u.software ? NULL : u.pci ? u.pci : u.card_node);
    walpe_gpu_in_use_clear (&u);
}

/* preset_follow_gpu (off by default): when the kind of renderer is not
 * the one the preset was last set for, the preset is set once to the one
 * that suits it. The kind is remembered, so a preset the user chooses
 * afterwards stays until the kind changes again. */
static void gpu_follow_preset (void)
{
    if (!D.cfg || !D.cfg->preset_follow_gpu || Gpu.probing)
        return;
    WalpeGpuInUse u;
    if (!walpe_gpu_in_use (&u))
        return;
    const char *kind = u.software ? "software" : walpe_gpu_class_name (u.klass);
    const char *preset = walpe_gpu_class_preset (u.klass, u.software);
    if (preset && g_strcmp0 (kind, D.cfg->preset_gpu_class)) {
        g_message ("gpu: preset_follow_gpu: the renderer is %s (%s): quality preset %s", kind,
                   u.name ? u.name : "?", preset);
        g_free (D.cfg->preset_gpu_class);
        D.cfg->preset_gpu_class = g_strdup (kind);
        walpe_config_save (D.cfg);
        /* from the main loop, like a request of a client */
        g_idle_add (gpu_follow_preset_cb, g_strdup (preset));
    }
    walpe_gpu_in_use_clear (&u);
}

/* ------------------------------------------------------- after EGL init */

/* does device d fit an EGL vendor string? (NVIDIA's EGL drives NVIDIA's
 * kernel driver, Mesa's everything else.) A device without a DRM card
 * renders nothing - except under NVIDIA's driver, which draws without its
 * DRM module as well: sysfs then lists the device on the PCI bus only,
 * with the driver's name. */
static gboolean gpu_fits_vendor (const WalpeGpuDev *d, gboolean egl_is_nvidia)
{
    if (!d || d->klass == WALPE_GPU_VIRTUAL)
        return FALSE;
    const gboolean nv = walpe_gpu_driver_is_nvidia (d->driver);
    return (d->card >= 0 || (nv && egl_is_nvidia)) && !nv == !egl_is_nvidia;
}

/* GPU choice: hand-over point for the window-system layer (call 2 of 2).
 * Main thread, right after eglInitialize, no context current. */
static void gpu_note_egl (EGLDisplay dpy)
{
    if (Gpu.probing || !D.cfg)
        return;     /* a probe child (--gpu-probe, --sw-probe): nothing to record */
    if (!Gpu.devs)
        Gpu.devs = walpe_gpu_enumerate_plain (NULL);
    if (Sw.active) {
        walpe_gpu_in_use_set (NULL, TRUE);
        gpu_tell_stats ();
        gpu_follow_preset ();
        return;
    }
    char *card = NULL, *render = NULL, *gl_driver = NULL;
    gpu_egl_device (dpy, &card, &render, &gl_driver);
    const char *how = "named by EGL";
    int idx = walpe_gpu_index_of_node (Gpu.devs, render);
    if (idx < 0)
        idx = walpe_gpu_index_of_node (Gpu.devs, card);
    if (idx < 0 && gpu_gl_driver_is_software (gl_driver)) {
        /* no GPU driver answered: Mesa's CPU rasteriser renders */
        Gpu.in_use = -1;
        walpe_gpu_in_use_set (NULL, TRUE);
        g_message ("gpu: no hardware driver answered for this display: rendering on the CPU "
                   "(%s)", gl_driver);
        goto done;
    }
    if (idx < 0) {
        const char *ev = eglQueryString (dpy, EGL_VENDOR);
        gboolean nv = ev && strstr (ev, "NVIDIA") != NULL;
        int only = -1, n = 0;
        for (guint i = 0; i < Gpu.devs->len; i++)
            if (gpu_fits_vendor (g_ptr_array_index (Gpu.devs, i), nv)) {
                only = (int) i;
                n++;
            }
        how = "taken from the EGL vendor and the device list";
        if (Gpu.offload && gpu_fits_vendor (gpu_dev (Gpu.target), nv))
            idx = Gpu.target;
        else if (gpu_fits_vendor (gpu_dev (Gpu.display), nv))
            idx = Gpu.display;
        else if (n == 1)
            idx = only;
    }
    Gpu.in_use = idx;
    if (idx >= 0 && !Gpu.offload) {
        /* nothing was set by WalPE: EGL renders on the window system's
         * GPU - unless the user's own environment sends it elsewhere */
        gboolean user_env = FALSE;
        for (const char *const *n = walpe_gpu_offload_env_names (); *n; n++)
            user_env |= g_getenv (*n) != NULL;
        if (!user_env)
            Gpu.display = idx;
    }
    if (idx >= 0) {
        WalpeGpuDev *d = gpu_dev (idx);
        walpe_gpu_in_use_set (d, FALSE);
        char *txt = walpe_gpu_dev_describe (d);
        g_message ("gpu: rendering on %s (%s)", txt, how);
        g_free (txt);
        if (Gpu.target >= 0 && idx != Gpu.target) {
            gpu_set_note (g_strdup_printf ("gpu = %s %s %s, but EGL renders on %s", Gpu.setting,
                                           Gpu.by_auto ? "chose" : "names",
                                           gpu_dev (Gpu.target)->name, d->name));
            g_warning ("gpu: %s", Gpu.note);
        }
    } else {
        walpe_gpu_in_use_set (NULL, FALSE);
        if (Gpu.devs->len)
            g_message ("gpu: the device EGL renders on could not be told (%u graphics devices)",
                       Gpu.devs->len);
    }
done:
    g_free (card);
    g_free (render);
    g_free (gl_driver);
    gpu_tell_stats ();
    gpu_follow_preset ();
}

/* first monitor thread, from sw_note_gl (): GL_RENDERER is a CPU
 * rasteriser although no software mode was asked for */
static void gpu_note_software_renderer (void)
{
    WalpeGpuInUse u;
    gboolean known = walpe_gpu_in_use (&u);
    if (!known || !u.software)
        walpe_gpu_in_use_set (NULL, TRUE);
    if (known)
        walpe_gpu_in_use_clear (&u);
    gpu_tell_stats ();      /* (thread-safe: the hint is under a lock) */
}

/* --------------------------------------------------------------- status */

static void gpu_status_json (JsonBuilder *b)
{
    /* the saved setting (or WALPE_GPU while it is set): a change shows
     * here at once, in gpu_in_use after the restart */
    jb_str (b, "gpu", Gpu.env_forced ? Gpu.setting : D.cfg->gpu ? D.cfg->gpu : "auto");
    jb_bool (b, "gpu_env", Gpu.env_forced);
    jb_bool (b, "gpu_offload", Gpu.offload);
    if (Gpu.note)
        jb_str (b, "gpu_note", Gpu.note);
    if (Gpu.env_text)
        jb_str (b, "gpu_environment", Gpu.env_text);
    jb_bool (b, "preset_follow_gpu", D.cfg->preset_follow_gpu);
    WalpeGpuInUse u;
    gboolean known = walpe_gpu_in_use (&u);
    if (known) {
        json_builder_set_member_name (b, "gpu_in_use");
        json_builder_begin_object (b);
        walpe_gpu_in_use_to_json (b, &u);
        json_builder_end_object (b);
    }
    json_builder_set_member_name (b, "gpu_candidates");
    json_builder_begin_array (b);
    for (guint i = 0; Gpu.devs && i < Gpu.devs->len; i++) {
        const WalpeGpuDev *d = g_ptr_array_index (Gpu.devs, i);
        json_builder_begin_object (b);
        walpe_gpu_dev_to_json (b, d);
        jb_bool (b, "display", (int) i == Gpu.display);
        jb_bool (b, "in_use", known && !u.software && (int) i == Gpu.in_use);
        json_builder_end_object (b);
    }
    json_builder_end_array (b);
    if (known)
        walpe_gpu_in_use_clear (&u);
    {
        /* the GL driver behind it, and what it cannot do at all */
        const WalpeGlCaps *c = walpe_gl_caps ();
        if (c)
            jb_str (b, "gl_driver", walpe_gl_driver_name (c->driver));
        if (walpe_gl_caps_error ())
            jb_str (b, "gl_error", walpe_gl_caps_error ());
    }
}

/* ------------------------------------------------------------------ set */

static gboolean gpu_restart_cb (gpointer user)
{
    (void) user;
    Gpu.restart_id = 0;
    if (!restart_begin ("gpu setting changed"))
        return G_SOURCE_REMOVE;
    Sw.restart = TRUE;      /* the restart in place of main () */
    g_message ("gpu setting changed: restarting in place");
    g_main_loop_quit (D.loop);
    return G_SOURCE_REMOVE;
}

/* the device that renders under a setting value, as far as it can be
 * told here: the device it names (auto: the discrete GPU it would try),
 * else the display device */
static int gpu_effective (const char *value)
{
    int t = !strcmp (value, "auto") ? walpe_gpu_auto_target (Gpu.devs, Gpu.display, NULL)
                                    : walpe_gpu_match (Gpu.devs, value, Gpu.display, NULL);
    return t >= 0 ? t : Gpu.display;
}

/* "gpu" / "preset_follow_gpu" of a set request. FALSE + *err (g_free):
 * nothing was changed. *restart: the engine has to restart in place for
 * it (the caller schedules it with gpu_schedule_restart). *note
 * (g_free): a sentence for the reply. */
static gboolean gpu_ipc_set (JsonObject *o, gboolean busy, gboolean *restart, char **note,
                             char **err)
{
    *restart = FALSE;
    *note = *err = NULL;
    if (json_object_has_member (o, "gpu")) {
        char *v = walpe_gpu_value_normalize (wpj_str (o, "gpu", NULL));
        if (!v) {
            *err = g_strdup ("gpu: auto, display, integrated, discrete, or one device by PCI "
                             "address or name (at most 127 characters); nothing was changed");
            return FALSE;
        }
        if (strcmp (v, "auto") && !walpe_gpu_value_is_display (v)) {
            /* a value that names nothing here would silently mean auto
             * (it may name a device by a part of its name) */
            gpu_names ();
            char *why = NULL;
            if (walpe_gpu_match (Gpu.devs, v, Gpu.display, &why) < 0) {
                *err = g_strdup_printf ("gpu: %s; nothing was changed (`walpe-ctl gpu` lists the "
                                        "devices)", why ? why : "no such device");
                g_free (why);
                g_free (v);
                return FALSE;
            }
        }
        /* a restart only when another device should render than the one
         * that does now (so a value that failed its test is tried again
         * when it is set again, and going back to what runs costs nothing) */
        int running = Gpu.offload ? Gpu.target : Gpu.in_use >= 0 ? Gpu.in_use : Gpu.display;
        gboolean need = !Gpu.env_forced && !Sw.active && gpu_effective (v) != running;
        if (need && busy) {
            *err = g_strdup ("gpu: the engine has to restart for this and an import / Workshop "
                             "download is running; change it when that has finished");
            g_free (v);
            return FALSE;
        }
        g_free (D.cfg->gpu);
        D.cfg->gpu = v;
        *restart = need;
        if (Gpu.env_forced)
            *note = g_strdup ("saved; WALPE_GPU in the daemon's environment decides while it is "
                              "set");
        else if (Sw.active)
            *note = g_strdup ("saved; it has no effect while \"Render on CPU\" is on");
        else if (!need && strcmp (v, Gpu.setting ? Gpu.setting : "auto")) {
            /* the same device under another name: taken over without a restart */
            g_free (Gpu.setting);
            Gpu.setting = g_strdup (v);
            Gpu.by_auto = Gpu.target >= 0 && !strcmp (v, "auto");
        }
    }
    if (json_object_has_member (o, "preset_follow_gpu")) {
        D.cfg->preset_follow_gpu = wpj_bool (o, "preset_follow_gpu", D.cfg->preset_follow_gpu);
        if (D.cfg->preset_follow_gpu)
            gpu_follow_preset ();   /* for the renderer of now, when it was never set for it */
    }
    return TRUE;
}

/* a little later, so that the reply to `set` is out first */
static void gpu_schedule_restart (void)
{
    if (!Gpu.restart_id && !Sw.restart_id)
        Gpu.restart_id = g_timeout_add (200, gpu_restart_cb, NULL);
}
