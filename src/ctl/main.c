/* walpe-ctl - control the running daemon from the shell */
#include <glib.h>
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/ipc.h"
#include "common/config.h"
#include "common/quality.h"
#include "common/setkeys.h"
#include "common/util.h"
#include "common/workshop.h"

/* text from a reply on its way to the terminal (below) */
static const char *tt (const char *s);
static void print_json (const char *reply);

#include "layers_ctl.h"   /* walpe-ctl layers ... */

static void usage (void)
{
    g_print ("usage:\n"
             "  walpe-ctl open <path> [--monitor N]  set wallpaper (project dir or media file)\n"
             "                                 on every monitor, or only on monitor N (status\n"
             "                                 \"monitors\": index, name, geometry, current)\n"
             "  walpe-ctl add <path>           import a video: normalize to h264 mp4\n"
             "                                 <=1080p into <wallpapers>/imported/\n"
             "                                 (open does the same on its own for a video the\n"
             "                                 player cannot read: WebM, HEVC, MKV ... - into a\n"
             "                                 cache, the wallpaper changes when it is ready)\n"
             "  walpe-ctl cancel-import        stop a running import / video conversion\n"
             "  walpe-ctl pause | resume       control playback\n"
             "  walpe-ctl status               show daemon state (+ import progress)\n"
             "  walpe-ctl set fps=N volume=X parallax=on|off fs=on|off\n"
             "  walpe-ctl set covered=on|off   pause a monitor while windows cover it\n"
             "  walpe-ctl set dpms=on|off      pause while the display is in power save or the\n"
             "                                 screensaver runs\n"
             "  walpe-ctl set transparent=on|off  see-through windows (transparent_windows list,\n"
             "                                 _NET_WM_WINDOW_OPACITY) count as covering\n"
             "  walpe-ctl set focus_action=run|limit|pause focus_fps=N\n"
             "                                 while an application's window on a monitor has the\n"
             "                                 focus: keep running (default), limit that monitor's\n"
             "                                 wallpaper to focus_fps, or pause it\n"
             "  walpe-ctl set battery=on|off battery_fps=N\n"
             "                                 on battery power: pause every wallpaper / limit the\n"
             "                                 frame rate (0 = no limit); both off by default\n"
             "  walpe-ctl set previews=always|hover|off  animated (GIF) previews in the GUI\n"
             "  walpe-ctl set shared=on|off fx_cache=on|off tex_downscale=on|off\n"
             "                                 render a scene once for same-size monitors /\n"
             "                                 static effect cache / downscale big textures\n"
             "  walpe-ctl set pointer_share=on|off   a scene that follows the pointer is rendered once for\n"
             "                         same-size monitors and follows the pointer on the monitor it is on (off)\n"
             "  walpe-ctl set skip_hidden=off|auto|always [skip_hidden_margin=PX skip_hidden_inset=PX]\n"
             "                         do not render what other windows hide (off; auto = only without a\n"
             "                         compositing manager, always = also under one: see README)\n"
             "  walpe-ctl set present=vsync|limiter|auto  how frames are presented (NAME=mode for one\n"
             "                                 monitor), for this run of the daemon: gpu.conf keeps it\n"
             "  walpe-ctl set frame_align=on|off  the frame clocks of all monitors run on one grid (off by\n"
             "                                 default; for this run of the daemon: `frame_align = on` in\n"
             "                                 ~/.config/walpe/gpu.conf keeps it)\n"
             "  walpe-ctl set fx_specialize=on|off fx_quality=exact|fast\n"
             "                                 effect shaders, both off / exact by default:\n"
             "                                 constants compiled in (each such shader is first\n"
             "                                 compared with the plain one; status: fx_verified,\n"
             "                                 fx_rejected) / cheaper variants, stacked wave and\n"
             "                                 shake effects in one pass (small pixel differences);\n"
             "                                 reloads the wallpapers shown\n"
             "  walpe-ctl set software=on|off  render on the CPU (Mesa llvmpipe) instead of the GPU:\n"
             "                                 no GPU load, much more CPU load; the daemon restarts\n"
             "                                 itself. status: software_active, gl_renderer,\n"
             "                                 software_note (why not, when it is not available)\n"
             "  walpe-ctl set software_threads=N  CPU threads llvmpipe draws with (0 or auto = half\n"
             "                                 of the CPUs, at most 8)\n"
             "  walpe-ctl set texture_slack=2|0  2: textures and effects may be drawn up to 2 %%\n"
             "                                 larger than stored (less GPU memory and load);\n"
             "                                 0: exact sizes. Applies to the next wallpaper load\n"
             "  walpe-ctl set render_scale=N   scenes render at N percent of the monitor's size\n"
             "                                 (25..100, default 100): less GPU load, softer picture\n"
             "  walpe-ctl set web_accel=on|off web pages on the GPU (off: WebKit paints on the CPU);\n"
             "                                 restarts the daemon; status \"web_render\" says what runs\n"
             "  walpe-ctl set hwdec=on|off     decode videos (video wallpapers, video layers of\n"
             "                                 scenes) on the GPU (NVDEC, VA-API or VDPAU through\n"
             "                                 FFmpeg) when that works; off (the default) = on the\n"
             "                                 CPU. Videos that are playing switch at once; status\n"
             "                                 shows what the decoder really does (video_decoder)\n"
             "  walpe-ctl set video_limit=on|off  videos follow the frame rate limit: a video wallpaper\n"
             "                                 is shown at most fps times a second (and by the battery /\n"
             "                                 focus / fps_idle limits), and pictures of a video that\n"
             "                                 no monitor shows are not decoded; off (the default):\n"
             "                                 videos keep their own frame rate\n"
             "  walpe-ctl set video_upload=legacy|pbo|pbodata|persist|direct[+pingpong]\n"
             "                                 how video frames reach their textures (same picture;\n"
             "                                 for comparing with `walpe-ctl profile`): legacy (the\n"
             "                                 default) from main memory, pbo / pbodata through a\n"
             "                                 buffer object, persist / direct through persistently\n"
             "                                 mapped buffers (direct: written by the decoder);\n"
             "                                 +pingpong: two texture sets in turn. Status shows what\n"
             "                                 is really used (video_upload_now)\n"
             "  walpe-ctl set particle_lowres=off|auto|all particle_density=N\n"
             "                                 big soft particles (fog, glows) at reduced resolution:\n"
             "                                 auto = only where the picture stays the same; emit\n"
             "                                 N %% of every system's particles (5-100)\n"
             "  walpe-ctl set particle_instancing=on|off\n"
             "                                 off = sprites as six vertices each, the submission\n"
             "                                 before the instanced one (if particles are missing\n"
             "                                 or misplaced on a driver)\n"
             "  walpe-ctl set preset=max|high|balanced|low\n"
             "                                 quality for GPU load, all knobs at once; single knobs:\n"
             "      fx_rate=1|2|3              animated effects are redrawn every Nth frame ...\n"
             "      fx_rate_mode=slow|all      ... only the ones known to move slowly / all of them\n"
             "      fx_res=full|screen|reduced|half\n"
             "                                 effect passes at the layer's on-screen size (instead of\n"
             "                                 the next 1/8 step of its texture) / at 3/4 / 1/2 of it\n"
             "      fps_idle=N                 the wallpaper's frame rate limit while the desktop is\n"
             "                                 idle (0 = the fps setting); never exceeded when focused\n"
             "      post=full|reduced|off      blur + bloom on smaller buffers / also no cosmetic post\n"
             "                                 passes (bloom, film grain, vignette, chromatic aberration)\n"
             "      render_scale=25..100       scene resolution in percent of the monitor's\n"
             "      tex_res=full|half|quarter  layer textures stored smaller (reloads the wallpaper)\n"
             "      texture_slack=2|0          2: a reduced texture or effect may be drawn up to 2 %% larger\n"
             "                                 than it is stored; 0: exact sizes (preset max)\n"
             "  walpe-ctl quality [--json] [--effects]\n"
             "                                 the knobs, what the wallpapers on screen cost and what\n"
             "                                 each preset would save; --effects lists every effect\n"
             "  walpe-ctl quality --monitor N | --path P  key=value ... | reset\n"
             "                                 knobs for one wallpaper only (value default = as global)\n"
             "  walpe-ctl set we_assets=<dir>  Wallpaper Engine install (or its assets/) for builtin\n"
             "                                 effects/shaders; we_assets= (empty) = auto-detect\n"
             "  walpe-ctl props                list user properties of the active wallpaper\n"
             "  walpe-ctl setprop <key>=<val> [<key>=<val> ...]\n"
             "                                 set wallpaper user properties (one reload)\n"
             "  walpe-ctl resetprops           drop all property overrides of the active wallpaper\n"
             "      props / setprop / resetprops [--monitor N | --path P]: the wallpaper monitor N\n"
             "                                 shows, or any wallpaper, instead of the default one\n"
             "  walpe-ctl layers               list the layers of the running scene; move, resize or\n"
             "                                 hide them: `walpe-ctl layers --help`\n"
             "  walpe-ctl install <id|url>     download a workshop item with steamcmd into the library\n"
             "                                 (set steam_user=<account> first; log in to steamcmd once)\n"
             "  walpe-ctl steam-libraries      list discovered Steam Wallpaper Engine workshop folders\n"
             "  walpe-ctl click X Y [BUTTON [COUNT]]\n"
             "                                 click the wallpaper at screen pixel X,Y, as a click on the\n"
             "                                 empty desktop there does (SceneScript cursor events: left\n"
             "                                 button; web pages: BUTTON 1 left, 2 middle, 3 right,\n"
             "                                 COUNT 2 = double click)\n"
             "  walpe-ctl scroll X Y DY [DX]   turn the wheel over a web wallpaper (DY > 0 = down)\n"
             "  walpe-ctl profile [SECONDS] [--json] [--sync]\n"
             "                                 GPU profile of what every monitor shows, for SECONDS\n"
             "                                 (default 10): GPU time per layer and effect pass, draw\n"
             "                                 calls, pixels shaded, uploads, memory, driver performance\n"
             "                                 messages. Prints the report; text and JSON are also kept\n"
             "                                 in ~/.cache/walpe/gpu-profile-<time>.txt / .json.\n"
             "                                 --sync: measure with glFinish instead of timer queries\n"
             "                                 (slows rendering; for software GL / odd drivers)\n"
             "  walpe-ctl quit                 stop the daemon\n"
             "exit status: 0 done, 1 usage, 2 no daemon, 3 the daemon refused the command\n");
}

/* is v a number as JSON spells it? g_ascii_strtod also takes nan, inf,
 * hex, a leading '+' or '.', a trailing '.': pasted into a request those
 * made the whole line invalid ("bad json") */
static gboolean json_number_ok (const char *v)
{
    const char *p = v;
    if (*p == '-')
        p++;
    if (*p == '0')
        p++;
    else if (g_ascii_isdigit (*p))
        while (g_ascii_isdigit (*p))
            p++;
    else
        return FALSE;
    if (*p == '.') {
        if (!g_ascii_isdigit (*++p))
            return FALSE;
        while (g_ascii_isdigit (*p))
            p++;
    }
    if (*p == 'e' || *p == 'E') {
        p++;
        if (*p == '+' || *p == '-')
            p++;
        if (!g_ascii_isdigit (*p))
            return FALSE;
        while (g_ascii_isdigit (*p))
            p++;
    }
    return *p == 0;
}

/* --monitor N: a monitor index, -1 when it is not a number (atoi made
 * "--monitor abc" monitor 0) */
static int parse_monitor (const char *v)
{
    char *end = NULL;
    long n = strtol (v, &end, 10);
    if (!*v || !end || *end || n < 0 || n > 1000) {
        g_printerr ("--monitor needs a monitor index (see walpe-ctl status \"monitors\"), not '%s'\n", v);
        exit (1);
    }
    return (int) n;
}

/* a whole number in lo..hi from the command line; anything else ends the
 * program with exit status 1 (atoi made "abc" 0 and "3x" 3: `walpe-ctl
 * click abc def` clicked the top left corner) */
static int parse_int (const char *what, const char *v, long lo, long hi)
{
    char *end = NULL;
    long n = strtol (v, &end, 10);
    if (!*v || !end || *end || n < lo || n > hi) {
        g_printerr ("%s needs a whole number from %ld to %ld, not '%s'\n", what, lo, hi, v);
        exit (1);
    }
    return (int) n;
}

/* [--monitor N | --path P] of props / setprop / resetprops as request
 * members (",\"monitor\":N" or ",\"path\":\"P\""; "" = the default
 * wallpaper); *i is advanced over what was taken. FALSE = argv[*i] is not
 * one of the two. */
static gboolean target_arg (int argc, char **argv, int *i, GString *out)
{
    const char *a = argv[*i], *v = NULL;
    gboolean mon = !g_strcmp0 (a, "--monitor") || g_str_has_prefix (a, "--monitor=");
    gboolean path = !g_strcmp0 (a, "--path") || g_str_has_prefix (a, "--path=");
    if (!mon && !path)
        return FALSE;
    const char *eq = strchr (a, '=');
    if (eq)
        v = eq + 1;
    else if (*i + 1 < argc)
        v = argv[++*i];
    if (!v || !*v) {
        g_printerr ("%s needs a value (%s)\n", mon ? "--monitor" : "--path",
                    mon ? "a monitor index, see walpe-ctl status \"monitors\"" : "a wallpaper");
        exit (1);
    }
    g_string_truncate (out, 0);
    if (mon) {
        g_string_append_printf (out, ",\"monitor\":%d", parse_monitor (v));
    } else {
        char *abs = walpe_expand_path (v), *esc = walpe_json_escape (abs);
        g_string_append_printf (out, ",\"path\":\"%s\"", esc);
        g_free (abs);
        g_free (esc);
    }
    return TRUE;
}

/* Text in a reply is in part a stranger's: the title of a Workshop item,
 * the layer and effect names of a scene, a line steamcmd printed. A
 * terminal obeys the control characters in what is written to it (an
 * escape sequence renames the window, clears or rewrites the screen, on
 * some terminals fills the clipboard). So what walpe-ctl prints as text
 * goes through tt (): the C0 controls except line feed and tab, DEL and the
 * C1 controls come out as visible text (backslash, x, two hex digits),
 * bytes that are not UTF-8 as '?'.
 * The copies live until the program ends (a handful per run). */
static gboolean term_control (gunichar c)
{
    return (c < 0x20 && c != '\n' && c != '\t') || (c >= 0x7f && c < 0xa0);
}

static const char *tt (const char *s)
{
    static GPtrArray *kept;
    if (!s)
        return NULL;
    GString *o = g_string_new (NULL);
    for (const char *p = s; *p;) {
        gunichar c = g_utf8_get_char_validated (p, -1);
        if (c == (gunichar) -1 || c == (gunichar) -2) {
            g_string_append_c (o, '?');
            p++;
            continue;
        }
        const char *n = g_utf8_next_char (p);
        if (term_control (c))
            g_string_append_printf (o, "\\x%02x", (guint) c);
        else
            g_string_append_len (o, p, n - p);
        p = n;
    }
    if (!kept)
        kept = g_ptr_array_new ();
    char *r = g_string_free (o, FALSE);
    g_ptr_array_add (kept, r);
    return r;
}

/* A reply printed as it came is JSON, and the daemon's JSON carries a few
 * control characters as they are inside its strings (DEL, the C1 range,
 * 0x1F). They are written as JSON's own escape (backslash, u, four hex
 * digits) here - the same text to whatever reads the output, nothing to a
 * terminal. */
static void print_json (const char *reply)
{
    GString *o = g_string_new (NULL);
    for (const char *p = reply; *p;) {
        gunichar c = g_utf8_get_char_validated (p, -1);
        if (c == (gunichar) -1 || c == (gunichar) -2) {
            g_string_append_c (o, *p++);
            continue;
        }
        const char *n = g_utf8_next_char (p);
        if (term_control (c) && c != '\r')
            g_string_append_printf (o, "\\u%04x", (guint) c);
        else
            g_string_append_len (o, p, n - p);
        p = n;
    }
    g_print ("%s\n", o->str);
    g_string_free (o, TRUE);
}

/* setprop: a bare word that is sent as a JSON boolean or number - what it
 * is sent as ("true" / "false" / the number as typed); NULL = it is text */
static const char *setprop_word (const char *v)
{
    if (!g_ascii_strcasecmp (v, "true") || !g_ascii_strcasecmp (v, "on"))
        return "true";
    if (!g_ascii_strcasecmp (v, "false") || !g_ascii_strcasecmp (v, "off"))
        return "false";
    return json_number_ok (v) ? v : NULL;
}

/* setprop: would the word, sent as that boolean or number, be stored as
 * other text than was typed? ("true", "false" and a plain whole number are
 * stored letter for letter; "on", "TRUE", "1.50", "1e3", "-0" are not.) */
static gboolean setprop_word_changes (const char *v)
{
    const char *typed = setprop_word (v);
    if (!typed)
        return FALSE;
    if (typed != v)
        return strcmp (typed, v) != 0;
    const char *p = v + (*v == '-');
    gsize digits = strspn (p, "0123456789");
    return p[digits] != 0 || digits > 18 || (*v == '-' && *p == '0');
}

/* setprop: the daemon's list of the target wallpaper's options (the reply
 * to "props"), or NULL: no daemon, an old one, no wallpaper */
static JsonNode *setprop_options (const char *target)
{
    char *sock = walpe_socket_path ();
    char *req = g_strdup_printf ("{\"cmd\":\"props\"%s}", target);
    char *reply = walpe_ipc_request (sock, req, 2000);
    JsonNode *n = reply ? wpj_parse_data (reply, -1) : NULL;
    g_free (reply);
    g_free (req);
    g_free (sock);
    if (n && !(JSON_NODE_HOLDS_OBJECT (n) && wpj_obj (json_node_get_object (n), "props")))
        g_clear_pointer (&n, json_node_unref);
    return n;
}

/* setprop: option `key` of the wallpaper holds text - every type but a
 * checkbox and a slider; a combo by its default value, which is how the
 * scene reads what is stored for it. An option the wallpaper does not
 * declare (or no list at all) is not text: the word decides, as before. */
static gboolean setprop_is_text (JsonObject *opts, const char *key)
{
    JsonObject *o = opts ? wpj_obj (opts, key) : NULL;
    if (!o)
        return FALSE;
    const char *type = wpj_str (o, "type", "");
    if (!g_ascii_strcasecmp (type, "bool") || !g_ascii_strcasecmp (type, "checkbox") ||
        !g_ascii_strcasecmp (type, "slider"))
        return FALSE;
    if (!g_ascii_strcasecmp (type, "combo")) {
        JsonNode *d = json_object_get_member (o, "default");
        return d && JSON_NODE_HOLDS_VALUE (d) && json_node_get_value_type (d) == G_TYPE_STRING;
    }
    return TRUE;
}

/* a daemon older than this walpe-ctl answers "unknown cmd" to a command
 * it does not have: say that instead of leaving the raw reply alone */
static void note_old_daemon (const char *reply, const char *cmd)
{
    JsonNode *rn = reply ? wpj_parse_data (reply, -1) : NULL;
    JsonObject *ro = rn && JSON_NODE_HOLDS_OBJECT (rn) ? json_node_get_object (rn) : NULL;
    if (ro && !wpj_bool (ro, "ok", FALSE) && !g_strcmp0 (wpj_str (ro, "error", ""), "unknown cmd"))
        g_printerr ("the running walpe-daemon is older than this walpe-ctl and has no `%s` "
                    "(restart it: walpe-ctl quit, then walpe-daemon)\n", cmd);
    if (rn)
        json_node_unref (rn);
}

/* install: send, then follow the daemon's status until the job ends */
static int install_and_wait (char *id)
{
    char *sock = walpe_socket_path ();
    char *req = g_strdup_printf ("{\"cmd\":\"install\",\"id\":\"%s\"}", id);
    char *reply = walpe_ipc_request (sock, req, 2000);
    g_free (req);
    int rc = 0;
    JsonNode *rn = reply ? wpj_parse_data (reply, -1) : NULL;
    JsonObject *ro = rn && JSON_NODE_HOLDS_OBJECT (rn) ? json_node_get_object (rn) : NULL;
    if (!ro || !wpj_bool (ro, "ok", FALSE)) {
        g_printerr ("%s\n", ro ? tt (wpj_str (ro, "error", "install failed"))
                                : "walpe-daemon is not running");
        rc = ro ? 3 : 2;
    } else {
        char *last = NULL;
        for (;;) {
            g_usleep (500000);
            char *st = walpe_ipc_request (sock, "{\"cmd\":\"status\"}", 2000);
            JsonNode *sn = st ? wpj_parse_data (st, -1) : NULL;
            JsonObject *so = sn && JSON_NODE_HOLDS_OBJECT (sn)
                ? wpj_obj (json_node_get_object (sn), "state") : NULL;
            g_free (st);
            if (!so) {
                g_printerr ("lost the daemon\n");
                rc = 2;
                if (sn) json_node_unref (sn);
                break;
            }
            const char *ph = wpj_str (so, "install_phase", "");
            if (g_strcmp0 (ph, last)) {
                g_print ("%s\n", tt (ph));
                g_free (last);
                last = g_strdup (ph);
            }
            gboolean active = wpj_bool (so, "install_active", FALSE);
            if (!active) {
                const char *e = wpj_str (so, "install_error", NULL);
                if (e) {
                    g_printerr ("install failed: %s\n", tt (e));
                    rc = 3;
                } else {
                    g_print ("installed: %s (%s)\n", tt (wpj_str (so, "install_dir", "?")),
                             tt (wpj_str (so, "install_title", "")));
                }
                json_node_unref (sn);
                break;
            }
            json_node_unref (sn);
        }
        g_free (last);
    }
    if (rn)
        json_node_unref (rn);
    g_free (reply);
    g_free (sock);
    g_free (id);
    return rc;
}

/* profile: start it, wait for the run to end, print the report. Exit
 * status as the other commands (3 = refused, e.g. a daemon older than this
 * command answers "unknown cmd") */
static int profile_and_wait (double seconds, gboolean json, gboolean sync)
{
    char *sock = walpe_socket_path ();
    char num[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd (num, sizeof (num), "%.3f", seconds);
    char *req = g_strdup_printf ("{\"cmd\":\"profile\",\"seconds\":%s,\"sync\":%s}", num,
                                 sync ? "true" : "false");
    char *reply = walpe_ipc_request (sock, req, 2000);
    g_free (req);
    JsonNode *rn = reply ? wpj_parse_data (reply, -1) : NULL;
    JsonObject *ro = rn && JSON_NODE_HOLDS_OBJECT (rn) ? json_node_get_object (rn) : NULL;
    int rc = 0;
    if (!ro || !wpj_bool (ro, "ok", FALSE)) {
        const char *e = ro ? wpj_str (ro, "error", "the profile could not be started") : NULL;
        if (e && !g_strcmp0 (e, "unknown cmd"))
            e = "the running walpe-daemon is older than this walpe-ctl and has no profiler";
        g_printerr ("%s\n", e ? tt (e) : "walpe-daemon is not running");
        rc = ro ? 3 : 2;
    } else {
        double left = wpj_num (ro, "seconds", seconds);
        g_printerr ("profiling for %.0f s ...\n", left);
        /* the daemon waits for render threads in the middle of a frame */
        gint64 deadline = g_get_monotonic_time () + (gint64) ((left + 150.0) * G_USEC_PER_SEC);
        g_usleep ((gulong) (left * G_USEC_PER_SEC));
        for (;;) {
            char *st = walpe_ipc_request (sock, "{\"cmd\":\"profile_result\"}", 5000);
            JsonNode *sn = st ? wpj_parse_data (st, -1) : NULL;
            JsonObject *so = sn && JSON_NODE_HOLDS_OBJECT (sn) ? json_node_get_object (sn) : NULL;
            g_free (st);
            if (!so) {
                g_printerr ("lost the daemon\n");
                rc = 2;
            } else if (!g_strcmp0 (wpj_str (so, "state", ""), "ready")) {
                const char *jf = wpj_str (so, "json_file", NULL);
                const char *tf = wpj_str (so, "text_file", NULL);
                char *jtxt = NULL;
                if (json && jf && g_file_get_contents (jf, &jtxt, NULL, NULL))
                    print_json (jtxt);
                else
                    g_print ("%s", tt (wpj_str (so, "text", "")));
                g_free (jtxt);
                if (tf || jf)
                    g_printerr ("report files: %s  %s\n", tf ? tt (tf) : "", jf ? tt (jf) : "");
            } else if (g_get_monotonic_time () > deadline) {
                g_printerr ("the daemon did not finish the profile\n");
                rc = 3;
            } else {
                json_node_unref (sn);
                g_usleep (200000);
                continue;
            }
            if (sn)
                json_node_unref (sn);
            break;
        }
    }
    if (rn)
        json_node_unref (rn);
    g_free (reply);
    g_free (sock);
    return rc;
}

/* one quality "key=value" as a JSON member ("key":value), NULL + error
 * when the value is not allowed; *taken = it is a quality key at all */
static char *quality_kv (const char *k, const char *v, gboolean *taken, char **err)
{
    JsonBuilder *b = json_builder_new ();
    json_builder_begin_object (b);
    gboolean ok = walpe_quality_kv_to_json (b, k, v, taken, err);
    json_builder_end_object (b);
    char *out = NULL;
    if (ok && *taken) {
        JsonNode *root = json_builder_get_root (b);
        JsonGenerator *g = json_generator_new ();
        json_generator_set_root (g, root);
        char *txt = json_generator_to_data (g, NULL);   /* {"key":value} */
        gsize n = strlen (txt);
        if (n >= 2)
            out = g_strndup (txt + 1, n - 2);
        g_free (txt);
        g_object_unref (g);
        json_node_unref (root);
    }
    g_object_unref (b);
    return out;
}

static const char *q_rate_text (JsonObject *q, char *buf, gsize n)
{
    int r = (int) wpj_num (q, "fx_rate", 1);
    if (r <= 1)
        g_snprintf (buf, n, "every frame");
    else
        g_snprintf (buf, n, "every %s frame, %s", r == 2 ? "2nd" : "3rd",
                    !g_strcmp0 (wpj_str (q, "fx_rate_mode", "slow"), "all") ? "all animated effects"
                                                                           : "slow effects only");
    return buf;
}

static void quality_print (JsonObject *q, gboolean effects)
{
    char buf[96];
    int idle = (int) wpj_num (q, "fps_idle", 0);
    char fi[48];
    if (idle > 0)
        g_snprintf (fi, sizeof fi, "%d (fps_idle)", idle);
    else
        g_snprintf (fi, sizeof fi, "the fps setting");
    g_print ("preset: %s    fps setting: %d\n", tt (wpj_str (q, "quality_preset", "?")),
             (int) wpj_num (q, "fps_cap", 0));
    g_print ("  effect update rate   %s\n  effect resolution    %s\n  frame rate limit     %s\n"
             "  post-processing      %s\n  render scale         %d %%\n"
             "  texture resolution   %s\n  texture slack        %d %%\n",
             q_rate_text (q, buf, sizeof buf), tt (wpj_str (q, "fx_res", "?")), fi,
             tt (wpj_str (q, "post_processing", "?")),
             (int) wpj_num (q, "render_scale", 100), tt (wpj_str (q, "tex_res", "?")),
             (int) wpj_num (q, "texture_slack", 2));
    JsonObject *wq = wpj_obj (q, "wallpapers");
    GList *ws = wq ? json_object_get_members (wq) : NULL;
    for (GList *w = ws; w; w = w->next) {
        JsonObject *ov = wpj_obj (wq, w->data);
        g_print ("own settings of %s:", tt (w->data));
        GList *ks = ov ? json_object_get_members (ov) : NULL;
        for (GList *k = ks; k; k = k->next) {
            JsonNode *vn = json_object_get_member (ov, k->data);
            if (JSON_NODE_HOLDS_VALUE (vn) && json_node_get_value_type (vn) == G_TYPE_STRING)
                g_print (" %s=%s", tt (k->data), tt (json_node_get_string (vn)));
            else
                g_print (" %s=%d", tt (k->data), (int) wpj_num (ov, k->data, 0));
        }
        g_list_free (ks);
        g_print ("\n");
    }
    g_list_free (ws);
    static const char *const names[] = { "max", "high", "balanced", "low", "current" };
    JsonArray *ms = wpj_arr (q, "monitors");
    for (guint i = 0; i < wpj_arr_len (ms); i++) {
        JsonObject *m = wpj_arr_obj (ms, i);
        if (!m)
            continue;
        g_print ("monitor %d (%s): %s [%s]", (int) wpj_num (m, "index", 0), tt (wpj_str (m, "name", "")),
                 tt (wpj_str (m, "path", "")), tt (wpj_str (m, "type", "?")));
        if (json_object_has_member (m, "follows")) {
            g_print (" - shows what monitor %d renders\n", (int) wpj_num (m, "follows", 0));
            continue;
        }
        g_print ("\n  runs with: preset %s%s, %s, effect resolution %s, post-processing %s, "
                 "render scale %d %%, textures %s, texture slack %d %%, %d fps now\n",
                 tt (wpj_str (m, "quality_preset", "?")),
                 wpj_bool (m, "own_settings", FALSE) ? " (its own settings)" : "",
                 q_rate_text (m, buf, sizeof buf), tt (wpj_str (m, "fx_res", "?")),
                 tt (wpj_str (m, "post_processing", "?")),
                 (int) wpj_num (m, "render_scale", 100), tt (wpj_str (m, "tex_res", "?")),
                 (int) wpj_num (m, "texture_slack", 2), (int) wpj_num (m, "fps_now", 0));
        JsonObject *st = wpj_obj (m, "stats"), *es = wpj_obj (m, "estimate");
        if (st && es) {
            double base = wpj_num (st, "px_base", 0), fx = wpj_num (st, "px_fx", 0);
            g_print ("  a frame at High: %.1f M pixels shaded (%.1f M in %.1f effect passes; the "
                     "rest %s)\n", (base + fx) / 1e6, fx / 1e6, wpj_num (st, "passes", 0),
                     wpj_bool (st, "measured", FALSE) ? "counted by the GPU"
                                                      : "estimated from layer sizes, particles not counted");
            g_print ("  estimate for this wallpaper, per preset:\n"
                     "    preset     pixels shaded   effect passes   frames   pixels per second\n"
                     "               per frame       per frame       per s    against High\n");
            for (guint k = 0; k < G_N_ELEMENTS (names); k++) {
                JsonObject *e = wpj_obj (es, names[k]);
                if (e)
                    g_print ("    %-9s  %7.1f M       %6.1f          %4d     %+5.0f %%\n", names[k],
                             wpj_num (e, "px_frame", 0) / 1e6, wpj_num (e, "passes_frame", 0),
                             (int) wpj_num (e, "fps", 0), -100 * wpj_num (e, "saving", 0));
            }
        } else if (!g_strcmp0 (wpj_str (m, "type", ""), "scene")) {
            g_print ("  (no numbers yet: the scene has not drawn enough frames)\n");
        } else {
            g_print ("  (only the frame rate limit applies to this kind of wallpaper)\n");
        }
        if (effects && json_object_has_member (m, "effects"))
            g_print ("  effects (layer | effect | kind, speed | update divisor it tolerates):\n%s",
                     tt (wpj_str (m, "effects", "")));
    }
    /* what each preset costs the viewer, and which knobs can change the
     * picture at all */
    JsonObject *ps = wpj_obj (q, "presets");
    g_print ("what the viewer gives up:\n");
    for (guint k = 0; ps && k + 1 < G_N_ELEMENTS (names); k++) {
        JsonObject *e = wpj_obj (ps, names[k]);
        if (!e)
            continue;
        g_print ("  %-9s %s\n", names[k], tt (wpj_str (e, "cost", "")));
        /* what the preset sets outside the quality knobs, in this build */
        JsonObject *al = wpj_obj (e, "also");
        GList *ks = al ? json_object_get_members (al) : NULL;
        if (ks)
            g_print ("            sets also:");
        for (GList *l = ks; l; l = l->next) {
            JsonNode *v = json_object_get_member (al, l->data);
            GType t = json_node_get_value_type (v);
            if (t == G_TYPE_BOOLEAN)
                g_print (" %s=%s", tt (l->data), json_node_get_boolean (v) ? "on" : "off");
            else if (t == G_TYPE_STRING)
                g_print (" %s=%s", tt (l->data), tt (json_node_get_string (v)));
            else
                g_print (" %s=%d", tt (l->data), (int) json_node_get_int (v));
        }
        if (ks)
            g_print ("\n");
        g_list_free (ks);
    }
    if (json_object_has_member (q, "picture_note"))
        g_print ("%s\n", tt (wpj_str (q, "picture_note", "")));
}

/* walpe-ctl quality [--json] [--effects] [--monitor N | --path P] [key=value ... | reset] */
static int quality_cmd (int argc, char **argv)
{
    gboolean json = FALSE, effects = FALSE, reset = FALSE;
    int mon = -1;
    const char *path = NULL;
    GString *vals = g_string_new (NULL);
    for (int i = 2; i < argc; i++) {
        if (!g_strcmp0 (argv[i], "--json"))
            json = TRUE;
        else if (!g_strcmp0 (argv[i], "--effects"))
            effects = TRUE;
        else if (!g_strcmp0 (argv[i], "reset"))
            reset = TRUE;
        else if (!g_strcmp0 (argv[i], "--monitor") && i + 1 < argc)
            mon = parse_monitor (argv[++i]);
        else if (g_str_has_prefix (argv[i], "--monitor="))
            mon = parse_monitor (argv[i] + 10);
        else if (!g_strcmp0 (argv[i], "--path") && i + 1 < argc)
            path = argv[++i];
        else if (g_str_has_prefix (argv[i], "--path="))
            path = argv[i] + 7;
        else {
            char **kv = g_strsplit (argv[i], "=", 2);
            gboolean taken = FALSE;
            char *qerr = NULL;
            char *frag = kv[0] && kv[1] ? quality_kv (kv[0], kv[1], &taken, &qerr) : NULL;
            if (!frag) {
                g_printerr ("%s\n", qerr ? qerr : "quality: expected key=value (preset, fx_rate, "
                            "fx_rate_mode, fx_res, fps_idle, post, render_scale, tex_res, "
                            "texture_slack), reset, --monitor N, --path P, --json or --effects");
                g_free (qerr);
                g_strfreev (kv);
                g_string_free (vals, TRUE);
                return 1;
            }
            g_string_append_printf (vals, "%s%s", vals->len ? "," : "", frag);
            g_free (frag);
            g_strfreev (kv);
        }
    }
    char *sock = walpe_socket_path ();
    char *req, *reply;
    int rc = 0;
    if (vals->len || reset) {
        if (mon < 0 && !path) {
            g_printerr ("quality: say which wallpaper (--monitor N or --path P); the global "
                        "setting is `walpe-ctl set key=value`\n");
            g_free (sock);
            g_string_free (vals, TRUE);
            return 1;
        }
        GString *r = g_string_new ("{\"cmd\":\"setquality\"");
        if (path) {
            char *abs = walpe_expand_path (path), *esc = walpe_json_escape (abs);
            g_string_append_printf (r, ",\"path\":\"%s\"", esc);
            g_free (abs);
            g_free (esc);
        } else {
            g_string_append_printf (r, ",\"monitor\":%d", mon);
        }
        if (reset)
            g_string_append (r, ",\"reset\":true");
        g_string_append_printf (r, ",\"values\":{%s}}", vals->str);
        req = g_string_free (r, FALSE);
        reply = walpe_ipc_request (sock, req, 2000);
        if (reply)
            print_json (reply);
    } else {
        req = g_strdup_printf ("{\"cmd\":\"quality\"%s}", effects ? ",\"describe\":true" : "");
        reply = walpe_ipc_request (sock, req, 2000);
        JsonNode *rn = reply ? wpj_parse_data (reply, -1) : NULL;
        JsonObject *q = rn && JSON_NODE_HOLDS_OBJECT (rn)
                            ? wpj_obj (json_node_get_object (rn), "quality") : NULL;
        /* the renderer starts counting with the first request: ask again
         * (up to three times) while a scene has no numbers yet, and once
         * more for the GPU's own pixel count and the effect list */
        for (int tries = 0; tries < 3; tries++) {
            gboolean none = FALSE, more = FALSE;
            JsonArray *ms = q ? wpj_arr (q, "monitors") : NULL;
            for (guint i = 0; i < wpj_arr_len (ms); i++) {
                JsonObject *m = wpj_arr_obj (ms, i);
                if (!m || g_strcmp0 (wpj_str (m, "type", ""), "scene") ||
                    json_object_has_member (m, "follows"))
                    continue;
                if (!json_object_has_member (m, "stats"))
                    none = TRUE;
                else if (!wpj_bool (wpj_obj (m, "stats"), "measured", FALSE) ||
                         (effects && !json_object_has_member (m, "effects")))
                    more = TRUE;
            }
            if (!none && !(more && tries == 0))
                break;
            if (rn)
                json_node_unref (rn);
            g_free (reply);
            g_usleep (1200000);
            reply = walpe_ipc_request (sock, req, 2000);
            rn = reply ? wpj_parse_data (reply, -1) : NULL;
            q = rn && JSON_NODE_HOLDS_OBJECT (rn) ? wpj_obj (json_node_get_object (rn), "quality")
                                                  : NULL;
        }
        if (reply && (json || !q))
            print_json (reply);
        else if (q)
            quality_print (q, effects);
        if (rn)
            json_node_unref (rn);
    }
    if (!reply) {
        g_printerr ("walpe-daemon is not running (no reply on %s)\n", sock);
        rc = 2;
    } else {
        JsonNode *rn = wpj_parse_data (reply, -1);
        if (!(rn && JSON_NODE_HOLDS_OBJECT (rn) && wpj_bool (json_node_get_object (rn), "ok", FALSE))) {
            rc = 3;
            note_old_daemon (reply, "quality");
        }
        if (rn)
            json_node_unref (rn);
    }
    g_free (reply);
    g_free (req);
    g_free (sock);
    g_string_free (vals, TRUE);
    return rc;
}

int main (int argc, char **argv)
{
    if (argc < 2) {
        usage ();
        return 1;
    }
    char *req = NULL;
    const char *cmd = argv[1];

    if (!g_strcmp0 (cmd, "help") || !g_strcmp0 (cmd, "--help") || !g_strcmp0 (cmd, "-h")) {
        usage ();
        return 0;
    }
    if (!g_strcmp0 (cmd, "layers"))
        return layers_main (argc, argv);
    if (!g_strcmp0 (cmd, "install") && argc >= 3) {
        char *id = walpe_workshop_parse_id (argv[2]);
        if (!id) {
            g_printerr ("not a workshop id or steamcommunity.com/sharedfiles/filedetails/?id=... URL\n");
            return 1;
        }
        return install_and_wait (id);
    } else if (!g_strcmp0 (cmd, "profile")) {
        /* walpe-ctl profile [SECONDS] [--json] [--sync] */
        double seconds = 10.0;
        gboolean json = FALSE, sync = FALSE;
        for (int i = 2; i < argc; i++) {
            if (!g_strcmp0 (argv[i], "--json")) {
                json = TRUE;
            } else if (!g_strcmp0 (argv[i], "--sync")) {
                sync = TRUE;
            } else if (json_number_ok (argv[i]) && g_ascii_strtod (argv[i], NULL) >= 1.0 &&
                       g_ascii_strtod (argv[i], NULL) <= 600.0) {
                seconds = g_ascii_strtod (argv[i], NULL);
            } else {
                g_printerr ("profile takes a number of seconds (1..600), --json and --sync, not "
                            "'%s'\n", argv[i]);
                return 1;
            }
        }
        return profile_and_wait (seconds, json, sync);
    } else if (!g_strcmp0 (cmd, "steam-libraries")) {
        req = g_strdup ("{\"cmd\":\"steam_libraries\"}");
    } else if (!g_strcmp0 (cmd, "open") && argc >= 3) {
        /* walpe-ctl open PATH [--monitor N] (N = index in status "monitors") */
        const char *path = NULL;
        int mon = -1;
        for (int i = 2; i < argc; i++) {
            if (!g_strcmp0 (argv[i], "--monitor") && i + 1 < argc) {
                mon = parse_monitor (argv[++i]);
            } else if (g_str_has_prefix (argv[i], "--monitor=")) {
                mon = parse_monitor (argv[i] + 10);
            } else if (!g_strcmp0 (argv[i], "--monitor")) {
                /* it was taken for nothing and the wallpaper went to
                 * every monitor */
                g_printerr ("--monitor needs a monitor index (see walpe-ctl status \"monitors\")\n");
                return 1;
            } else if (!path) {
                path = argv[i];
            } else {
                g_printerr ("open takes one wallpaper (and --monitor N), not also '%s'\n", argv[i]);
                return 1;
            }
        }
        if (!path) {
            usage ();
            return 1;
        }
        char *abs = walpe_expand_path (path);
        char *esc = walpe_json_escape (abs);
        req = mon >= 0
            ? g_strdup_printf ("{\"cmd\":\"open\",\"path\":\"%s\",\"monitor\":%d}", esc, mon)
            : g_strdup_printf ("{\"cmd\":\"open\",\"path\":\"%s\"}", esc);
        g_free (abs);
        g_free (esc);
    } else if (!g_strcmp0 (cmd, "add") && argc >= 3) {
        char *abs = walpe_expand_path (argv[2]);
        char *esc = walpe_json_escape (abs);
        req = g_strdup_printf ("{\"cmd\":\"add\",\"path\":\"%s\"}", esc);
        g_free (abs);
        g_free (esc);
    } else if (!g_strcmp0 (cmd, "cancel-import")) {
        req = g_strdup ("{\"cmd\":\"import_cancel\"}");
    } else if (!g_strcmp0 (cmd, "pause")) {
        req = g_strdup ("{\"cmd\":\"pause\"}");
    } else if (!g_strcmp0 (cmd, "resume")) {
        req = g_strdup ("{\"cmd\":\"resume\"}");
    } else if (!g_strcmp0 (cmd, "status")) {
        req = g_strdup ("{\"cmd\":\"status\"}");
    } else if (!g_strcmp0 (cmd, "click") && argc >= 4 && argc <= 6) {
        int x = parse_int ("click: X", argv[2], -100000, 100000);
        int y = parse_int ("click: Y", argv[3], -100000, 100000);
        int button = argc >= 5 ? parse_int ("click: BUTTON", argv[4], 1, 3) : 1;
        int count = argc >= 6 ? parse_int ("click: COUNT", argv[5], 1, 3) : 1;
        req = g_strdup_printf ("{\"cmd\":\"pointer\",\"action\":\"click\",\"x\":%d,\"y\":%d,"
                               "\"button\":%d,\"count\":%d}", x, y, button, count);
    } else if (!g_strcmp0 (cmd, "scroll") && argc >= 5 && argc <= 6) {
        int x = parse_int ("scroll: X", argv[2], -100000, 100000);
        int y = parse_int ("scroll: Y", argv[3], -100000, 100000);
        int dy = parse_int ("scroll: DY", argv[4], -32, 32);
        int dx = argc >= 6 ? parse_int ("scroll: DX", argv[5], -32, 32) : 0;
        req = g_strdup_printf ("{\"cmd\":\"pointer\",\"action\":\"scroll\",\"x\":%d,\"y\":%d,"
                               "\"dy\":%d,\"dx\":%d}", x, y, dy, dx);
    } else if (!g_strcmp0 (cmd, "quit")) {
        req = g_strdup ("{\"cmd\":\"quit\"}");
    } else if (!g_strcmp0 (cmd, "set")) {
        GString *s = g_string_new ("{\"cmd\":\"set\"");
        if (argc < 3) {
            g_printerr ("set needs at least one <key>=<value> (walpe-ctl without arguments lists "
                        "them)\n");
            g_string_free (s, TRUE);
            return 1;
        }
        for (int i = 2; i < argc; i++) {
            char **kv = g_strsplit (argv[i], "=", 2);
            if (!kv[0] || !*kv[0] || !kv[1]) {
                /* it was skipped without a word: `walpe-ctl set fps 30`
                 * set nothing, `set fps=30 covered` only the first */
                g_printerr ("set: '%s' is not <key>=<value>; nothing was changed\n", argv[i]);
                g_strfreev (kv);
                g_string_free (s, TRUE);
                return 1;
            }
            const char *k = kv[0], *v = kv[1];
            {
                /* quality knobs (preset, fx_rate, post, ...): checked
                 * here, so a typo is reported instead of sent */
                gboolean taken = FALSE;
                char *qerr = NULL;
                char *frag = quality_kv (k, v, &taken, &qerr);
                if (taken) {
                    if (!frag) {
                        g_printerr ("%s\n", qerr ? qerr : "not allowed");
                        g_free (qerr);
                        g_strfreev (kv);
                        g_string_free (s, TRUE);
                        return 1;
                    }
                    g_string_append_printf (s, ",%s", frag);
                    g_free (frag);
                    g_strfreev (kv);
                    continue;
                }
            }
            if (!g_strcmp0 (k, "we_assets") && *v && *v != '~' && !g_path_is_absolute (v)) {
                /* the daemon's cwd is not ours: send an absolute path */
                char *abs = g_canonicalize_filename (v, NULL);
                g_free (kv[1]);
                kv[1] = abs;
                v = abs;
            }
            {
                /* short names: fs, covered, dpms, transparent, shared, ... */
                static const char *alias[][2] = {
                    { "fs", "pause_fullscreen" }, { "covered", "pause_covered" },
                    { "dpms", "pause_dpms" }, { "transparent", "cover_transparent" },
                    { "shared", "render_shared" }, { "software", "software_render" },
                    { "battery", "pause_battery" }, { "previews", "preview_anim" },
                    { "focus", "focus_action" }, { "hwdec", "video_hwdec" },
                };
                for (guint a = 0; a < G_N_ELEMENTS (alias); a++)
                    if (!g_strcmp0 (k, alias[a][0]))
                        k = alias[a][1];
            }
            if (!g_strcmp0 (k, "software_threads") && g_ascii_strcasecmp (v, "auto") &&
                (!json_number_ok (v) || strchr (v, '.') || strchr (v, '-'))) {
                g_printerr ("software_threads needs a number of threads or auto, not '%s'\n", v);
                g_strfreev (kv);
                g_string_free (s, TRUE);
                return 1;
            }
            gboolean yes = !g_ascii_strcasecmp (v, "on") || !g_ascii_strcasecmp (v, "true") ||
                           !g_ascii_strcasecmp (v, "yes");
            gboolean no = !g_ascii_strcasecmp (v, "off") || !g_ascii_strcasecmp (v, "false") ||
                          !g_ascii_strcasecmp (v, "no");
            /* what the daemon expects for this key (common/setkeys.h): text
             * stays text (an account or a window class named "on"), a
             * number is checked here; a key it does not know goes out as
             * written and the daemon says so */
            const WalpeSetKey *sk = walpe_set_key_find (k);
            /* the key goes into the request as a JSON string like every
             * value: a quote in it must not end the member */
            char *ek = walpe_json_escape (k);
            if (sk && sk->kind == WALPE_SET_STR) {
                char *ev = walpe_json_escape (v);
                g_string_append_printf (s, ",\"%s\":\"%s\"", ek, ev);
                g_free (ev);
            } else if (yes || no) {
                g_string_append_printf (s, ",\"%s\":%s", ek, yes ? "true" : "false");
            } else if ((sk && sk->kind == WALPE_SET_NUM) || !g_strcmp0 (k, "particle_density")) {
                if (!json_number_ok (v)) {
                    g_printerr ("%s needs a number, not '%s'\n", k, v);
                    g_free (ek);
                    g_strfreev (kv);
                    g_string_free (s, TRUE);
                    return 1;
                }
                g_string_append_printf (s, ",\"%s\":%s", ek, v);
            } else {
                char *ev = walpe_json_escape (v);
                g_string_append_printf (s, ",\"%s\":\"%s\"", ek, ev);
                g_free (ev);
            }
            g_free (ek);
            g_strfreev (kv);
        }
        g_string_append_c (s, '}');
        req = g_string_free (s, FALSE);
    } else if (!g_strcmp0 (cmd, "quality")) {
        return quality_cmd (argc, argv);
    } else if (!g_strcmp0 (cmd, "props") || !g_strcmp0 (cmd, "resetprops")) {
        /* [--monitor N | --path P]: another wallpaper than the default one */
        GString *tgt = g_string_new (NULL);
        for (int i = 2; i < argc; i++)
            if (!target_arg (argc, argv, &i, tgt)) {
                g_printerr ("%s takes --monitor N or --path P, not '%s'\n", cmd, argv[i]);
                g_string_free (tgt, TRUE);
                return 1;
            }
        req = g_strdup_printf ("{\"cmd\":\"%s\"%s}", cmd, tgt->str);
        g_string_free (tgt, TRUE);
    } else if (!g_strcmp0 (cmd, "setprop") && argc >= 3) {
        /* walpe-ctl setprop [--monitor N | --path P] <key>=<value> ... -
         * value: bare word -> JSON bool when on/off/true/false, number
         * when numeric, else string. One pair goes as "setprop" (every
         * daemon has it), several as one "setprops" = one reload. (Only
         * the first pair was sent, the others were dropped without a
         * word.)
         * An option whose values are text - a text field, a colour, a file,
         * a combo of words - gets the word as it was typed: "on" stays
         * "on", "1.50" stays "1.50" (as a boolean and a number they were
         * stored as "true" and "1.5": a title read "true", a combo value
         * "on" matched no choice). Only the daemon knows the options of a
         * wallpaper, so it is asked (props) when a word would not be
         * stored as typed. */
        GString *tgt = g_string_new (NULL), *vals = g_string_new (NULL), *one = g_string_new (NULL);
        GPtrArray *pairs = g_ptr_array_new_with_free_func ((GDestroyNotify) g_strfreev);
        gboolean ask = FALSE;
        for (int i = 2; i < argc; i++) {
            if (target_arg (argc, argv, &i, tgt))
                continue;
            char **kv = g_strsplit (argv[i], "=", 2);
            if (!kv[0] || !*kv[0] || !kv[1]) {
                g_printerr ("setprop: '%s' is not <key>=<value>; nothing was changed\n", argv[i]);
                g_strfreev (kv);
                g_ptr_array_unref (pairs);
                g_string_free (tgt, TRUE);
                g_string_free (vals, TRUE);
                g_string_free (one, TRUE);
                return 1;
            }
            ask |= setprop_word_changes (kv[1]);
            g_ptr_array_add (pairs, kv);
        }
        if (!pairs->len) {
            g_printerr ("setprop needs <key>=<value>\n");
            g_ptr_array_unref (pairs);
            g_string_free (tgt, TRUE);
            g_string_free (vals, TRUE);
            g_string_free (one, TRUE);
            return 1;
        }
        JsonNode *defs = ask ? setprop_options (tgt->str) : NULL;
        JsonObject *opts = defs ? wpj_obj (json_node_get_object (defs), "props") : NULL;
        for (guint n = 0; n < pairs->len; n++) {
            char **kv = g_ptr_array_index (pairs, n);
            const char *v = kv[1], *typed = setprop_word (v);
            char *ek = walpe_json_escape (kv[0]), *jv;
            if (typed && !(setprop_word_changes (v) && setprop_is_text (opts, kv[0]))) {
                jv = g_strdup (typed);
            } else {
                char *ev = walpe_json_escape (v);
                jv = g_strdup_printf ("\"%s\"", ev);
                g_free (ev);
            }
            g_string_append_printf (vals, "%s\"%s\":%s", n ? "," : "", ek, jv);
            g_string_printf (one, "\"key\":\"%s\",\"value\":%s", ek, jv);
            g_free (ek);
            g_free (jv);
        }
        req = pairs->len == 1 ? g_strdup_printf ("{\"cmd\":\"setprop\",%s%s}", one->str, tgt->str)
                              : g_strdup_printf ("{\"cmd\":\"setprops\",\"props\":{%s}%s}", vals->str, tgt->str);
        if (defs)
            json_node_unref (defs);
        g_ptr_array_unref (pairs);
        g_string_free (tgt, TRUE);
        g_string_free (vals, TRUE);
        g_string_free (one, TRUE);
    } else {
        usage ();
        return 1;
    }

    char *sock = walpe_socket_path ();
    char *reply = walpe_ipc_request (sock, req, 2000);
    if (reply) {
        print_json (reply);
        /* exit status 3 when the daemon refused the command (as `install`
         * does), so scripts can tell */
        JsonNode *rn = wpj_parse_data (reply, -1);
        gboolean ok = rn && JSON_NODE_HOLDS_OBJECT (rn) &&
                      wpj_bool (json_node_get_object (rn), "ok", FALSE);
        /* done, but config.json could not be written: the reply has the reason */
        const char *se = rn && JSON_NODE_HOLDS_OBJECT (rn)
                             ? wpj_str (json_node_get_object (rn), "save_error", NULL) : NULL;
        if (se)
            g_printerr ("the engine could not save its settings: %s - what was changed is in "
                        "effect now and lost when the engine restarts\n", se);
        if (rn)
            json_node_unref (rn);
        if (!ok)
            note_old_daemon (reply, cmd);
        g_free (reply);
        if (!ok) {
            g_free (sock);
            g_free (req);
            return 3;
        }
        if (!g_strcmp0 (cmd, "set") && strstr (req, "\"video_hwdec\""))
            g_printerr ("hwdec: videos that are playing switch within a second (a paused one when it "
                        "resumes); `walpe-ctl status` shows the decoder in use as \"video_decoder\" - "
                        "\"software\" there after hwdec=on means it was not available or failed, "
                        "and the same line says why\n");
    } else {
        g_printerr ("walpe-daemon is not running (no reply on %s)\n", sock);
        g_free (sock);
        g_free (req);
        return 2;
    }
    g_free (sock);
    g_free (req);
    return 0;
}
