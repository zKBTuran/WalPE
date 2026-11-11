/* gpu_ctl.h - `walpe-ctl gpu`: the graphics devices of this machine, which
 * one the wallpapers are rendered on, and what each value of the `gpu`
 * setting would pick here. Works without a running daemon (the device
 * list comes from sysfs, common/gpusel.h); with one, the device in use and
 * the daemon's note come from its status.
 *
 *   walpe-ctl gpu [--json]
 *
 * Included by main.c (static functions). */

static void gpu_ctl_usage (void)
{
    g_print ("walpe-ctl gpu [--json]\n"
             "  lists the graphics devices, says which one renders the wallpapers and what\n"
             "  the values of the gpu setting mean on this machine.\n"
             "walpe-ctl set gpu=auto|integrated|discrete|<device>\n"
             "  auto        (default) the GPU that drives the displays: frames reach the screen\n"
             "              without a copy between GPUs\n"
             "  integrated  the processor's own graphics: lowest power draw, shares system memory\n"
             "  discrete    the separate card / the laptop's second GPU: fastest, draws more power\n"
             "  <device>    a PCI address (01:00.0), a DRM node (card1, renderD129), ids\n"
             "              (10de:25a0) or a part of the name or driver (nvidia, radeon, arc)\n"
             "  Rendering on another GPU than the one that drives the displays costs one\n"
             "  full-screen copy per monitor and frame and keeps the second GPU awake. The\n"
             "  daemon restarts itself for a change. WALPE_GPU=<value> in the daemon's\n"
             "  environment overrides the saved setting.\n"
             "walpe-ctl set preset_follow_gpu=on|off\n"
             "  off (default). on: when the kind of GPU that renders changes, the quality preset\n"
             "  is set once to the one that suits it (discrete: high, integrated: balanced,\n"
             "  CPU rendering: low); a preset chosen afterwards stays.\n");
}

/* what a value of the setting would pick, as one line (g_free) */
static char *gpu_ctl_pick (GPtrArray *devs, const char *value, int display)
{
    char *why = NULL;
    int idx = walpe_gpu_match (devs, value, display, &why);
    if (!strcmp (value, "auto"))
        idx = display;
    char *line;
    if (idx < 0) {
        line = g_strdup_printf ("%s", !strcmp (value, "auto") ? "the GPU the window system uses"
                                                              : why ? why : "no such device");
    } else {
        const WalpeGpuDev *d = g_ptr_array_index (devs, idx);
        const char *cost = display < 0 || idx == display
                               ? "no copy between GPUs"
                               : "every frame is copied to the window system's GPU";
        line = g_strdup_printf ("%s%s%s%s (%s)", d->name, d->pci ? " at " : "",
                                d->pci ? d->pci : "", d->driver && *d->driver ? "" : ", NO DRIVER",
                                cost);
    }
    g_free (why);
    return line;
}

static int gpu_ctl_main (int argc, char **argv)
{
    gboolean json = FALSE;
    for (int i = 2; i < argc; i++) {
        if (!g_strcmp0 (argv[i], "--json")) {
            json = TRUE;
        } else {
            gpu_ctl_usage ();
            return !g_strcmp0 (argv[i], "--help") || !g_strcmp0 (argv[i], "-h") ? 0 : 1;
        }
    }
    GPtrArray *devs = walpe_gpu_enumerate (NULL);
    int display = walpe_gpu_default_renderer (devs, walpe_gpu_display_index (devs, NULL, NULL));

    /* the daemon's view, when one runs */
    char *sock = walpe_socket_path ();
    char *reply = walpe_ipc_request (sock, "{\"cmd\":\"status\"}", 2000);
    g_free (sock);
    JsonNode *rn = reply ? wpj_parse_data (reply, -1) : NULL;
    /* the reply is { "ok": true, "state": { ... } }: the members read below
     * (gpu, gpu_in_use, gpu_candidates, gl_renderer, ...) are those of "state" */
    JsonObject *ro = rn && JSON_NODE_HOLDS_OBJECT (rn) ? json_node_get_object (rn) : NULL;
    JsonObject *st = ro ? wpj_obj (ro, "state") : NULL;
    gboolean has_gpu = st && json_object_has_member (st, "gpu");
    JsonObject *use = has_gpu ? wpj_obj (st, "gpu_in_use") : NULL;
    if (has_gpu) {
        /* the daemon knows which device EGL renders for the displays on */
        JsonArray *ca = wpj_arr (st, "gpu_candidates");
        for (guint i = 0; i < wpj_arr_len (ca); i++) {
            JsonObject *c = wpj_arr_obj (ca, i);
            if (c && wpj_bool (c, "display", FALSE) && i < devs->len)
                display = (int) i;
        }
    }
    /* the saved setting when no daemon answers */
    WalpeConfig *cfg = has_gpu ? NULL : walpe_config_load ();
    const char *setting = has_gpu ? wpj_str (st, "gpu", "auto") : cfg && cfg->gpu ? cfg->gpu : "auto";

    if (json) {
        JsonBuilder *b = json_builder_new ();
        json_builder_begin_object (b);
        json_builder_set_member_name (b, "devices");
        json_builder_begin_array (b);
        for (guint i = 0; i < devs->len; i++) {
            json_builder_begin_object (b);
            walpe_gpu_dev_to_json (b, g_ptr_array_index (devs, i));
            json_builder_set_member_name (b, "display");
            json_builder_add_boolean_value (b, (int) i == display);
            json_builder_end_object (b);
        }
        json_builder_end_array (b);
        json_builder_set_member_name (b, "gpu");
        json_builder_add_string_value (b, setting);
        json_builder_set_member_name (b, "daemon");
        json_builder_add_boolean_value (b, st != NULL);
        if (has_gpu) {
            static const char *const copy[] = { "gpu_env", "gpu_offload", "gpu_note", "gpu_in_use",
                                                "gpu_environment", "gl_driver", "gl_renderer",
                                                "gl_error", "preset_follow_gpu" };
            for (guint i = 0; i < G_N_ELEMENTS (copy); i++)
                if (json_object_has_member (st, copy[i])) {
                    json_builder_set_member_name (b, copy[i]);
                    json_builder_add_value (b, json_node_copy (json_object_get_member (st, copy[i])));
                }
        }
        json_builder_end_object (b);
        JsonGenerator *g = json_generator_new ();
        JsonNode *root = json_builder_get_root (b);
        json_generator_set_root (g, root);
        char *txt = json_generator_to_data (g, NULL);
        g_print ("%s\n", txt);
        g_free (txt);
        json_node_unref (root);
        g_object_unref (g);
        g_object_unref (b);
    } else {
        if (!devs->len)
            g_print ("graphics devices: none found in sysfs (a virtual machine or a container "
                     "without a GPU)\n");
        else
            g_print ("graphics devices:\n");
        for (guint i = 0; i < devs->len; i++) {
            char *d = walpe_gpu_dev_describe (g_ptr_array_index (devs, i));
            g_print ("  %u  %s%s\n", i, d, (int) i == display ? "  <- the window system's GPU" : "");
            g_free (d);
        }
        g_print ("setting: gpu = %s%s\n", setting,
                 has_gpu && wpj_bool (st, "gpu_env", FALSE)
                     ? " (WALPE_GPU in the daemon's environment; the saved setting waits)" : "");
        if (!st) {
            g_print ("rendering now: walpe-daemon is not running; the setting applies when it "
                     "starts\n");
        } else if (!has_gpu) {
            g_print ("rendering now: %s (this daemon is older than the gpu setting)\n",
                     wpj_str (st, "gl_renderer", "?"));
        } else if (use && wpj_bool (use, "software", FALSE)) {
            g_print ("rendering now: on the CPU (%s)\n", wpj_str (st, "gl_renderer", "software renderer"));
        } else if (use) {
            const char *pci = wpj_str (use, "pci", NULL);
            g_print ("rendering now: %s%s%s, %s, driver %s%s\n", wpj_str (use, "name", "?"),
                     pci ? " at " : "", pci ? pci : "", wpj_str (use, "class", "?"),
                     wpj_str (use, "driver", "?"),
                     wpj_bool (st, "gpu_offload", FALSE) ? " (frames are copied to the display GPU)"
                                                         : "");
            if (json_object_has_member (st, "gl_renderer"))
                g_print ("               OpenGL: %s (%s)\n", wpj_str (st, "gl_renderer", "?"),
                         wpj_str (st, "gl_driver", "?"));
        } else {
            g_print ("rendering now: %s (the device could not be told)\n",
                     wpj_str (st, "gl_renderer", "?"));
        }
        if (has_gpu && json_object_has_member (st, "gpu_note"))
            g_print ("note: %s\n", wpj_str (st, "gpu_note", ""));
        if (has_gpu && json_object_has_member (st, "gl_error"))
            g_print ("ERROR: %s\n", wpj_str (st, "gl_error", ""));
        static const char *const words[] = { "auto", "integrated", "discrete" };
        g_print ("what the values mean here:\n");
        for (guint i = 0; i < G_N_ELEMENTS (words); i++) {
            char *line = gpu_ctl_pick (devs, words[i], display);
            g_print ("  %-11s %s\n", words[i], line);
            g_free (line);
        }
        g_print ("change it: walpe-ctl set gpu=auto|integrated|discrete|<PCI address or name>   "
                 "(walpe-ctl gpu --help)\n");
        g_print ("load and memory figures: walpe-ctl gpu-stats\n");
    }
    if (cfg)
        walpe_config_free (cfg);
    if (rn)
        json_node_unref (rn);
    g_free (reply);
    g_ptr_array_unref (devs);
    return 0;
}
