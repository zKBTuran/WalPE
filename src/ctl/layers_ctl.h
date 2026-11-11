/* layers_ctl.h - `walpe-ctl layers ...` ("Adjust layers"; included by
 * main.c). Requests: src/daemon/layers_ipc.h. */
#pragma once
#include <math.h>

static void layers_usage (void)
{
    g_print ("usage: walpe-ctl layers [list]            the layers of the running scene\n"
             "       walpe-ctl layers move DX DY KEY...  move layers by an offset, together\n"
             "       walpe-ctl layers scale FACTOR KEY...  resize them as a group\n"
             "       walpe-ctl layers set KEY [dx=N] [dy=N] [scale=F] [visible=default|hidden|shown]\n"
             "       walpe-ctl layers hide KEY... | show KEY...\n"
             "       walpe-ctl layers reset [KEY...]     back to the wallpaper's own placement\n"
             "                                           (no KEY: every layer)\n"
             "       walpe-ctl layers highlight [KEY...] outline them on the wallpaper for a few\n"
             "                                           seconds (no KEY: off)\n"
             "       walpe-ctl layers get                the saved adjustments\n"
             "  KEY          as printed by `layers` (\"123:Name\"), a bare object id (123) or a\n"
             "               layer name\n"
             "  DX DY        scene pixels, added to what is saved: +x = right, +y = up\n"
             "  --children   also every layer below the named ones\n"
             "  --monitor N  the wallpaper on monitor N (default: the default wallpaper)\n"
             "  --path P     a wallpaper by its path\n"
             "  --seconds S  how long `highlight` stays (default 3)\n"
             "  --json       print the daemon's reply as it is\n");
}

static gboolean layers_number (const char *s, double *out)
{
    char *end = NULL;
    double d = g_ascii_strtod (s, &end);
    if (!s || !*s || !end || *end || !isfinite (d))
        return FALSE;
    *out = d;
    return TRUE;
}

static void layers_keys (JsonBuilder *b, GPtrArray *keys, guint from)
{
    json_builder_set_member_name (b, "keys");
    json_builder_begin_array (b);
    for (guint i = from; i < keys->len; i++)
        json_builder_add_string_value (b, keys->pdata[i]);
    json_builder_end_array (b);
}

static void layers_print_adjust (JsonObject *a, GString *out)
{
    double dx = wpj_num (a, "dx", 0), dy = wpj_num (a, "dy", 0), sc = wpj_num (a, "scale", 1);
    const char *v = wpj_str (a, "visible", "default");
    if (dx != 0 || dy != 0)
        g_string_append_printf (out, "dx=%+g dy=%+g ", dx, dy);
    if (sc != 1)
        g_string_append_printf (out, "x%g ", sc);
    if (strcmp (v, "default"))
        g_string_append_printf (out, "%s ", v);
    if (out->len)
        g_string_truncate (out, out->len - 1);
}

static void layers_print_list (JsonObject *ro)
{
    JsonArray *la = wpj_arr (ro, "layers"), *sz = wpj_arr (ro, "size");
    guint n = wpj_arr_len (la);
    if (!wpj_bool (ro, "scene", FALSE)) {
        g_print ("%s is not a scene: only scenes have layers\n", wpj_str (ro, "path", "?"));
        return;
    }
    if (!n && wpj_bool (ro, "loading", FALSE)) {
        g_print ("%s has not drawn a frame yet (still loading?): try again in a moment\n",
                 wpj_str (ro, "path", "?"));
    } else if (!wpj_bool (ro, "running", FALSE)) {
        g_print ("%s is not shown on a monitor: its layers are listed while it runs\n",
                 wpj_str (ro, "path", "?"));
    } else if (!n && !wpj_bool (ro, "fresh", FALSE)) {
        g_print ("%s did not draw a frame in time (still loading?): try again in a moment\n",
                 wpj_str (ro, "path", "?"));
    } else {
        g_print ("%s\nscene %g x %g, %u layers, back to front. Positions in scene pixels: "
                 "x from the left, y from the BOTTOM.%s\n", wpj_str (ro, "path", "?"),
                 wpj_arr_len (sz) >= 2 ? json_array_get_double_element (sz, 0) : 0.0,
                 wpj_arr_len (sz) >= 2 ? json_array_get_double_element (sz, 1) : 0.0, n,
                 wpj_bool (ro, "fresh", FALSE) ? "" : " (not from the current frame)");
        g_print ("%-30s %-10s %-5s %-13s %-11s %-22s %s\n", "KEY", "TYPE", "SHOWN", "POS", "SIZE",
                 "PARENT", "ADJUSTED");
    }
    for (guint i = 0; i < n; i++) {
        JsonObject *lo = wpj_arr_obj (la, i);
        if (!lo)
            continue;
        JsonArray *ps = wpj_arr (lo, "pos"), *ss = wpj_arr (lo, "size");
        char pos[48] = "-", size[48] = "-";
        if (wpj_arr_len (ps) >= 2)
            g_snprintf (pos, sizeof pos, "%.0f,%.0f", json_array_get_double_element (ps, 0),
                        json_array_get_double_element (ps, 1));
        if (wpj_arr_len (ss) >= 2)
            g_snprintf (size, sizeof size, "%.0fx%.0f", json_array_get_double_element (ss, 0),
                        json_array_get_double_element (ss, 1));
        int par = (int) wpj_num (lo, "parent", -1);
        const char *pk = par >= 0 && (guint) par < n ? wpj_str (wpj_arr_obj (la, par), "key", "-")
                                                    : "-";
        GString *adj = g_string_new (NULL);
        JsonObject *ao = wpj_obj (lo, "adjust");
        if (ao)
            layers_print_adjust (ao, adj);
        g_print ("%-30s %-10s %-5s %-13s %-11s %-22s %s\n", wpj_str (lo, "key", "?"),
                 wpj_str (lo, "type", "?"), wpj_bool (lo, "shown", FALSE) ? "yes" : "no", pos, size,
                 pk, adj->len ? adj->str : "-");
        g_string_free (adj, TRUE);
        const char *t = wpj_str (lo, "text", NULL), *f = wpj_str (lo, "file", NULL);
        if (t) {
            char *one = g_strdup (t);
            g_strdelimit (one, "\n\r\t", ' ');
            g_print ("    text \"%s\"\n", one);
            g_free (one);
        } else if (f) {
            g_print ("    file %s\n", f);
        }
    }
    JsonArray *aa = wpj_arr (ro, "adjustments");
    for (guint i = 0; i < wpj_arr_len (aa); i++) {
        JsonObject *ao = wpj_arr_obj (aa, i);
        if (!ao || (json_object_has_member (ao, "missing") && !wpj_bool (ao, "missing", FALSE)))
            continue;
        GString *adj = g_string_new (NULL);
        layers_print_adjust (ao, adj);
        g_print ("%s \"%s\": %s\n",
                 json_object_has_member (ao, "missing")
                     ? "saved, but no layer of this scene has the key (ignored)" : "saved",
                 wpj_str (ao, "key", "?"), adj->str);
        g_string_free (adj, TRUE);
    }
}

/* walpe-ctl layers ...: 0 ok, 1 usage, 2 no daemon, 3 refused */
static int layers_main (int argc, char **argv)
{
    const char *sub = "list", *path = NULL;
    int mon = -1;
    gboolean json = FALSE, children = FALSE, have_secs = FALSE;
    double secs = 0;
    GPtrArray *pos = g_ptr_array_new ();   /* positional arguments after the sub-command */
    int first = 2;
    if (argc > 2 && argv[2][0] != '-') {
        sub = argv[2];
        first = 3;
    }
    for (int i = first; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp (a, "--json")) {
            json = TRUE;
        } else if (!strcmp (a, "--children")) {
            children = TRUE;
        } else if (!strcmp (a, "--monitor") && i + 1 < argc) {
            mon = atoi (argv[++i]);
        } else if (g_str_has_prefix (a, "--monitor=")) {
            mon = atoi (a + 10);
        } else if (!strcmp (a, "--path") && i + 1 < argc) {
            path = argv[++i];
        } else if (g_str_has_prefix (a, "--path=")) {
            path = a + 7;
        } else if ((!strcmp (a, "--seconds") && i + 1 < argc) || g_str_has_prefix (a, "--seconds=")) {
            const char *v = a[9] == '=' ? a + 10 : argv[++i];
            if (!layers_number (v, &secs)) {
                g_printerr ("--seconds needs a number\n");
                return 1;
            }
            have_secs = TRUE;
        } else if (!strcmp (a, "--help") || !strcmp (a, "-h")) {
            layers_usage ();
            return 0;
        } else if (g_str_has_prefix (a, "--")) {
            g_printerr ("unknown option %s\n", a);
            layers_usage ();
            return 1;
        } else {
            g_ptr_array_add (pos, argv[i]);
        }
    }

    JsonBuilder *b = json_builder_new ();
    json_builder_begin_object (b);
    json_builder_set_member_name (b, "cmd");
    gboolean list = !strcmp (sub, "list");
    json_builder_add_string_value (b, list ? "layers" : "layeradj");
    if (path) {
        char *abs = walpe_expand_path (path);
        json_builder_set_member_name (b, "path");
        json_builder_add_string_value (b, abs);
        g_free (abs);
    } else if (mon >= 0) {
        json_builder_set_member_name (b, "monitor");
        json_builder_add_int_value (b, mon);
    }
    gboolean bad = FALSE;
    if (!list) {
        const char *op = !strcmp (sub, "hide") || !strcmp (sub, "show") ? "set" : sub;
        json_builder_set_member_name (b, "op");
        json_builder_add_string_value (b, op);
        if (children) {
            json_builder_set_member_name (b, "children");
            json_builder_add_boolean_value (b, TRUE);
        }
    }
    if (list || !strcmp (sub, "get")) {
        bad = pos->len > 0;
    } else if (!strcmp (sub, "move")) {
        double dx, dy;
        if (pos->len < 3 || !layers_number (pos->pdata[0], &dx) ||
            !layers_number (pos->pdata[1], &dy)) {
            g_printerr ("move needs DX DY (numbers, scene pixels) and at least one KEY\n");
            bad = TRUE;
        } else {
            json_builder_set_member_name (b, "dx");
            json_builder_add_double_value (b, dx);
            json_builder_set_member_name (b, "dy");
            json_builder_add_double_value (b, dy);
            layers_keys (b, pos, 2);
        }
    } else if (!strcmp (sub, "scale")) {
        double f;
        if (pos->len < 2 || !layers_number (pos->pdata[0], &f)) {
            g_printerr ("scale needs FACTOR (a number, 1 = unchanged) and at least one KEY\n");
            bad = TRUE;
        } else {
            json_builder_set_member_name (b, "factor");
            json_builder_add_double_value (b, f);
            layers_keys (b, pos, 1);
        }
    } else if (!strcmp (sub, "set") || !strcmp (sub, "hide") || !strcmp (sub, "show")) {
        gboolean is_set = !strcmp (sub, "set");
        if (!pos->len) {
            g_printerr ("%s needs a KEY\n", sub);
            bad = TRUE;
        }
        json_builder_set_member_name (b, "layers");
        json_builder_begin_array (b);
        for (guint i = 0; !bad && i < (is_set ? 1 : pos->len); i++) {
            json_builder_begin_object (b);
            json_builder_set_member_name (b, "key");
            json_builder_add_string_value (b, pos->pdata[i]);
            if (!is_set) {
                json_builder_set_member_name (b, "visible");
                json_builder_add_string_value (b, !strcmp (sub, "hide") ? "hidden" : "shown");
            }
            guint nset = 0;
            for (guint k = 1; is_set && k < pos->len; k++, nset++) {
                char **kv = g_strsplit (pos->pdata[k], "=", 2);
                double d;
                if (kv[0] && kv[1] && (!strcmp (kv[0], "dx") || !strcmp (kv[0], "dy") ||
                                       !strcmp (kv[0], "scale")) && layers_number (kv[1], &d)) {
                    json_builder_set_member_name (b, kv[0]);
                    json_builder_add_double_value (b, d);
                } else if (kv[0] && kv[1] && !strcmp (kv[0], "visible")) {
                    json_builder_set_member_name (b, "visible");
                    json_builder_add_string_value (b, kv[1]);
                } else {
                    g_printerr ("set: \"%s\" is not dx=N, dy=N, scale=F or visible=default|hidden|"
                                "shown\n", (const char *) pos->pdata[k]);
                    bad = TRUE;
                }
                g_strfreev (kv);
            }
            if (is_set && !nset && !bad) {
                g_printerr ("set needs at least one of dx=N dy=N scale=F visible=...\n");
                bad = TRUE;
            }
            json_builder_end_object (b);
        }
        json_builder_end_array (b);
    } else if (!strcmp (sub, "reset")) {
        if (pos->len)
            layers_keys (b, pos, 0);
    } else if (!strcmp (sub, "highlight")) {
        layers_keys (b, pos, 0);
        if (have_secs) {
            json_builder_set_member_name (b, "seconds");
            json_builder_add_double_value (b, secs);
        }
    } else {
        g_printerr ("unknown: layers %s\n", sub);
        bad = TRUE;
    }
    json_builder_end_object (b);
    JsonNode *rn = json_builder_get_root (b);
    char *req = json_to_string (rn, FALSE);
    json_node_unref (rn);
    g_object_unref (b);
    g_ptr_array_unref (pos);
    if (bad) {
        g_free (req);
        layers_usage ();
        return 1;
    }
    char *sock = walpe_socket_path ();
    char *reply = walpe_ipc_request (sock, req, 4000);
    g_free (req);
    if (!reply) {
        g_printerr ("walpe-daemon is not running (no reply on %s)\n", sock);
        g_free (sock);
        return 2;
    }
    g_free (sock);
    JsonNode *pn = wpj_parse_data (reply, -1);
    JsonObject *ro = pn && JSON_NODE_HOLDS_OBJECT (pn) ? json_node_get_object (pn) : NULL;
    gboolean ok = ro && wpj_bool (ro, "ok", FALSE);
    if (json || !ro) {
        g_print ("%s\n", reply);
    } else if (!ok) {
        g_printerr ("%s\n", wpj_str (ro, "error", "refused"));
    } else if (list) {
        layers_print_list (ro);
    } else {
        JsonArray *aa = wpj_arr (ro, "adjustments");
        if (strcmp (sub, "get") && strcmp (sub, "highlight"))
            g_print ("%d layer(s) changed%s\n", (int) wpj_num (ro, "changed", 0),
                     wpj_bool (ro, "running", FALSE) ? ""
                         : " (saved; the wallpaper is not shown right now)");
        for (guint i = 0; strcmp (sub, "highlight") && i < wpj_arr_len (aa); i++) {
            JsonObject *ao = wpj_arr_obj (aa, i);
            GString *adj = g_string_new (NULL);
            layers_print_adjust (ao, adj);
            g_print ("  %-30s %s%s\n", wpj_str (ao, "key", "?"), adj->str,
                     wpj_bool (ao, "missing", FALSE) ? "   (no such layer in the scene: ignored)"
                                                     : "");
            g_string_free (adj, TRUE);
        }
        if (strcmp (sub, "highlight") && !wpj_arr_len (aa))
            g_print ("  no layer of this wallpaper is adjusted\n");
    }
    if (pn)
        json_node_unref (pn);
    g_free (reply);
    return ok ? 0 : 3;
}
