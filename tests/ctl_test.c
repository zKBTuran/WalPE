/* ctl_test - walpe-ctl's command line and the "set" request
 *
 *   ctl-test <walpe-ctl binary> <src/daemon/main.c> <docs/SETTINGS.md>
 *
 * No daemon, no display. Three parts:
 *  1. bad input: walpe-ctl says what is wrong and exits with 1 before it
 *     talks to anybody (it used to send 0 for "abc", skip arguments without
 *     '=', take "--monitor" without a number for nothing); good input
 *     without a daemon exits with 2;
 *  2. a stand-in daemon on a private socket records what walpe-ctl sends:
 *     value types by key, several setprop pairs as one request, the
 *     --monitor / --path target, the sentence for a daemon that is older;
 *     and what walpe-ctl writes to the terminal when the replies carry
 *     control characters (a Workshop title, layer names of a scene): none
 *     of them, whether it prints text or the reply itself;
 *  3. common/setkeys.h: an unknown member or a value of the wrong type
 *     refuses the whole request, and every member the daemon's ipc_set ()
 *     reads is in the table (read from the source file). */
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "common/config.h"
#include "common/ipc.h"
#include "common/setkeys.h"
#include "common/quality.h"
#include "common/util.h"

static int fails;
#define CHECK(c) do { if (!(c)) { g_printerr ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static const char *ctl;
static char **ctl_stdout;       /* set: the next run_ctl () stores walpe-ctl's stdout there */
static GPtrArray *doc_short;    /* short name, key, short name, key ... */
static GPtrArray *doc_usage_names;  /* per key `set` takes: its short name, else the key */
static GMainLoop *loop;
static GMutex lock;
static char *last_req;
static char *last_props_req;   /* the last "props" a setprop asked */
static int props_asked;

static char *last_out;     /* what the last walpe-ctl run wrote to stdout */

/* run walpe-ctl with the arguments (NULL-terminated); *err_out = its stderr */
static int run_ctl (char **err_out, ...)
{
    GPtrArray *av = g_ptr_array_new ();
    g_ptr_array_add (av, (gpointer) ctl);
    va_list ap;
    va_start (ap, err_out);
    for (const char *a; (a = va_arg (ap, const char *));)
        g_ptr_array_add (av, (gpointer) a);
    va_end (ap);
    g_ptr_array_add (av, NULL);
    char *out = NULL, *err = NULL;
    int status = -1;
    GError *e = NULL;
    if (!g_spawn_sync (NULL, (char **) av->pdata, NULL, G_SPAWN_DEFAULT, NULL, NULL, &out, &err, &status,
                       &e)) {
        g_printerr ("spawn: %s\n", e->message);
        g_clear_error (&e);
    }
    g_ptr_array_free (av, TRUE);
    if (ctl_stdout) {       /* the caller asked for this run's output: its own copy */
        *ctl_stdout = g_strdup (out);
        ctl_stdout = NULL;
    }
    g_free (last_out);
    last_out = out;
    if (err_out)
        *err_out = err;
    else
        g_free (err);
    return WIFEXITED (status) ? WEXITSTATUS (status) : -1;
}

#define BAD(text, ...) do { \
        char *e_ = NULL; \
        int rc_ = run_ctl (&e_, __VA_ARGS__, NULL); \
        if (rc_ != 1 || !e_ || !strstr (e_, text)) { \
            g_printerr ("FAIL %s:%d: exit %d, stderr: %s\n", __FILE__, __LINE__, rc_, e_ ? e_ : ""); \
            fails++; \
        } \
        g_free (e_); \
    } while (0)
#define NODAEMON(...) CHECK (run_ctl (NULL, __VA_ARGS__, NULL) == 2)

/* Replies with control characters in every text walpe-ctl prints: an
 * escape sequence (ESC [ 2 J, ESC ] 0 ; ... BEL), a C1 control (U+009B),
 * DEL, and in the reply that is printed as it is the three JSON lets
 * through unescaped. */
static gint hostile_text;
#define E "\\u001b"
static const char hostile_status[] =
    "{\"ok\":true,\"state\":{\"install_phase\":\"downloading 1: " E "[2Jphase\",\"install_active\":false,"
    "\"install_dir\":\"/lib/" E "]0;dir\\u0007\",\"install_title\":\"title" E "[31m\\u009b1m\\u007f\"}}";
static const char hostile_quality[] =
    "{\"ok\":true,\"quality\":{\"quality_preset\":\"high" E "[2J\",\"fx_res\":\"full" E "c\","
    "\"post_processing\":\"on" E "c\",\"tex_res\":\"full" E "c\","
    "\"wallpapers\":{\"/w" E "[1m\":{\"fx_res" E "\":\"half" E "[0m\",\"n" E "\":2}},"
    "\"monitors\":[{\"index\":0,\"name\":\"DP" E "]0;x\\u0007\",\"path\":\"/p\\u009b1m\",\"type\":\"scene" E "\","
    "\"quality_preset\":\"low" E "\",\"fx_res\":\"q" E "\",\"post_processing\":\"p" E "\",\"tex_res\":\"t" E "\","
    "\"stats\":{\"measured\":true},\"effects\":\"layer" E "[2J | fx\\u0007 | static\\n\"}],"
    "\"presets\":{\"max\":{\"cost\":\"cost" E "[5m\",\"also\":{\"k" E "\":\"v" E "\",\"b" E "\":true,\"i" E "\":3}}},"
    "\"picture_note\":\"note" E "[7m\"}}";
static const char hostile_profile[] =
    "{\"state\":\"ready\",\"text\":\"layer " E "[2J\\tcolumn\\nnext line\\u009b\\n\",\"text_file\":\"/r/" E "c.txt\"}";
static const char hostile_layers[] =
    "{\"ok\":true,\"scene\":true,\"running\":true,\"fresh\":true,\"path\":\"/s" E "[2J\",\"size\":[1920,1080],"
    "\"layers\":[{\"key\":\"back" E "[1m\",\"type\":\"image" E "\",\"shown\":true,\"pos\":[1,2],\"size\":[3,4],"
    "\"parent\":-1,\"file\":\"models/a" E "]0;f\\u0007.json\"},"
    "{\"key\":\"words\\u009b2J\",\"type\":\"text\",\"shown\":true,\"parent\":0,\"text\":\"hello" E "[2J\\nworld\\u007f\","
    "\"adjust\":{\"dx\":1,\"visible\":\"hidden" E "\"}}],"
    "\"adjustments\":[{\"key\":\"gone" E "[5m\",\"missing\":true,\"scale\":2}]}";
/* (DEL, U+009B and 0x1F as they are: what the daemon's JSON writer leaves alone) */
static const char hostile_raw[] = "{\"ok\":true,\"title\":\"a\x7f" "b\xc2\x9b" "1m\x1f" "c" E "[2J\"}";
#undef E

/* the stand-in daemon: remembers the request; "quality" and "setprops"
 * are commands it does not have */
static char *handler (const char *req, gpointer user)
{
    (void) user;
    /* "props": the options of the wallpaper asked about (setprop asks when
     * a word would not be stored as typed). Not the request sent () hands
     * out: kept apart. (Not while the replies with control characters are
     * asked for: their "props" reply is the one further down.) */
    if (strstr (req, "\"cmd\":\"props\"") && !strstr (req, "/lib/123") && !g_atomic_int_get (&hostile_text)) {
        g_mutex_lock (&lock);
        props_asked++;
        g_free (last_props_req);
        last_props_req = g_strdup (req);
        g_mutex_unlock (&lock);
        return g_strdup ("{\"ok\":true,\"props\":{"
                         "\"title\":{\"type\":\"textinput\",\"text\":\"Title\",\"default\":\"abc\",\"value\":null},"
                         "\"tint\":{\"type\":\"color\",\"text\":\"Tint\",\"default\":\"1 1 1\",\"value\":null},"
                         "\"flag\":{\"type\":\"bool\",\"text\":\"Flag\",\"default\":false,\"value\":null},"
                         "\"speed\":{\"type\":\"slider\",\"text\":\"Speed\",\"default\":1,\"value\":null},"
                         "\"kind\":{\"type\":\"combo\",\"text\":\"Kind\",\"default\":\"off\",\"value\":null},"
                         "\"mode\":{\"type\":\"combo\",\"text\":\"Mode\",\"default\":2,\"value\":null}}}");
    }
    g_mutex_lock (&lock);
    g_free (last_req);
    last_req = g_strdup (req);
    g_mutex_unlock (&lock);
    if (g_atomic_int_get (&hostile_text)) {
        if (strstr (req, "\"cmd\":\"status\""))
            return g_strdup (hostile_status);
        if (strstr (req, "\"cmd\":\"quality\""))
            return g_strdup (hostile_quality);
        if (strstr (req, "\"cmd\":\"profile_result\""))
            return g_strdup (hostile_profile);
        if (strstr (req, "\"cmd\":\"profile\""))
            return g_strdup ("{\"ok\":true,\"seconds\":0}");
        if (strstr (req, "\"cmd\":\"props\""))
            return g_strdup (hostile_raw);
        if (strstr (req, "\"cmd\":\"layers\""))
            return g_strdup (hostile_layers);
        if (strstr (req, "\"cmd\":\"layeradj\""))
            return g_strdup ("{\"ok\":false,\"error\":\"no layer " "\\u001b" "[2J\"}");
        if (strstr (req, "\"cmd\":\"install\"") && strstr (req, "\"2\""))
            return g_strdup ("{\"ok\":false,\"error\":\"no" "\\u001b" "[2J\"}");
    }
    if (strstr (req, "\"cmd\":\"quality\"") || strstr (req, "\"cmd\":\"cancel\""))
        return g_strdup ("{\"ok\":false,\"error\":\"unknown cmd\"}");
    if (strstr (req, "\"refuse\""))
        return g_strdup ("{\"ok\":false,\"error\":\"no\"}");
    /* a saved window system is not the running one: the daemon says so */
    if (strstr (req, "\"backend\":\"wayland\""))
        return g_strdup ("{\"ok\":true,\"restart_needed\":true}");
    return g_strdup ("{\"ok\":true}");
}

/* the request the stand-in got, as an object (unref) */
static JsonNode *sent (void)
{
    g_mutex_lock (&lock);
    JsonNode *n = last_req ? wpj_parse_data (last_req, -1) : NULL;
    g_clear_pointer (&last_req, g_free);
    g_mutex_unlock (&lock);
    return n;
}

/* nothing a terminal would act on: no C0 control but line feed and tab,
 * no DEL, no C1 control (as UTF-8: 0xC2 0x80..0x9F) */
static gboolean terminal_safe (const char *t)
{
    for (const guchar *p = (const guchar *) (t ? t : ""); *p; p++)
        if ((*p < 0x20 && *p != '\n' && *p != '\t') || *p == 0x7f ||
            (*p == 0xc2 && p[1] >= 0x80 && p[1] < 0xa0))
            return FALSE;
    return TRUE;
}

static gboolean is_type (JsonObject *o, const char *k, GType t)
{
    JsonNode *n = o ? json_object_get_member (o, k) : NULL;
    return n && JSON_NODE_HOLDS_VALUE (n) && json_node_get_value_type (n) == t;
}

static gpointer client_thread (gpointer u)
{
    (void) u;
    JsonNode *n;
    JsonObject *o;
    char *err = NULL;

    /* set: several keys in one request, each with the type its key has */
    CHECK (run_ctl (NULL, "set", "fps=48", "covered=off", "steam_user=on", "focus=limit",
                    "transparent_windows=st,foot", "previews=off", "particle_density=60",
                    "software_threads=auto", "volume=0.25", "shared=TRUE", "preset=low", NULL) == 0);
    n = sent ();
    o = n ? json_node_get_object (n) : NULL;
    CHECK (o && !g_strcmp0 (wpj_str (o, "cmd", ""), "set"));
    CHECK (is_type (o, "fps", G_TYPE_INT64) && wpj_num (o, "fps", 0) == 48);
    CHECK (is_type (o, "pause_covered", G_TYPE_BOOLEAN) && !wpj_bool (o, "pause_covered", TRUE));
    CHECK (is_type (o, "steam_user", G_TYPE_STRING) && !g_strcmp0 (wpj_str (o, "steam_user", ""), "on"));
    CHECK (!g_strcmp0 (wpj_str (o, "focus_action", ""), "limit"));
    CHECK (!g_strcmp0 (wpj_str (o, "transparent_windows", ""), "st,foot"));
    CHECK (is_type (o, "preview_anim", G_TYPE_BOOLEAN));
    CHECK (wpj_num (o, "particle_density", 0) == 60);
    CHECK (!g_strcmp0 (wpj_str (o, "software_threads", ""), "auto"));
    CHECK (wpj_num (o, "volume", 0) == 0.25);
    CHECK (is_type (o, "render_shared", G_TYPE_BOOLEAN) && wpj_bool (o, "render_shared", FALSE));
    CHECK (!g_strcmp0 (wpj_str (o, "quality_preset", ""), "low"));
    CHECK (o && !walpe_set_check (o));      /* and the daemon would take all of it */
    if (n)
        json_node_unref (n);

    /* the fullscreen rule for every monitor: its short name and its full
     * one send the same switch */
    CHECK (run_ctl (NULL, "set", "fs=on", "fs_all=on", NULL) == 0);
    n = sent ();
    o = n ? json_node_get_object (n) : NULL;
    CHECK (is_type (o, "pause_fullscreen", G_TYPE_BOOLEAN) && wpj_bool (o, "pause_fullscreen", FALSE));
    CHECK (is_type (o, "pause_fullscreen_all", G_TYPE_BOOLEAN) && wpj_bool (o, "pause_fullscreen_all", FALSE));
    CHECK (o && !json_object_has_member (o, "fs_all") && !walpe_set_check (o));
    if (n)
        json_node_unref (n);
    CHECK (run_ctl (NULL, "set", "pause_fullscreen_all=off", NULL) == 0);
    n = sent ();
    o = n ? json_node_get_object (n) : NULL;
    CHECK (is_type (o, "pause_fullscreen_all", G_TYPE_BOOLEAN) && !wpj_bool (o, "pause_fullscreen_all", TRUE));
    if (n)
        json_node_unref (n);

    /* the window system: `backend NAME` and `set backend=NAME` send the
     * same member, as text ("on" / "off" stay text too: wayland_pointer) */
    /* ... and walpe-ctl passes the daemon's "restart_needed" on as a note
     * (it looked for it in the reply after freeing the reply: the note came
     * or not by what the allocator had left there) */
    CHECK (run_ctl (&err, "backend", "Wayland", NULL) == 0);
    CHECK (err && strstr (err, "the setting is read when walpe-daemon starts"));
    g_clear_pointer (&err, g_free);
    n = sent ();
    o = n ? json_node_get_object (n) : NULL;
    CHECK (o && !g_strcmp0 (wpj_str (o, "cmd", ""), "set") && json_object_get_size (o) == 2);
    CHECK (is_type (o, "backend", G_TYPE_STRING) && !g_strcmp0 (wpj_str (o, "backend", ""), "wayland"));
    CHECK (o && !walpe_set_check (o));
    if (n)
        json_node_unref (n);
    CHECK (run_ctl (&err, "set", "backend=x11", "wayland_pointer=off", NULL) == 0);
    CHECK (err && !strstr (err, "walpe-daemon starts"));    /* no note without "restart_needed" */
    g_clear_pointer (&err, g_free);
    n = sent ();
    o = n ? json_node_get_object (n) : NULL;
    CHECK (is_type (o, "backend", G_TYPE_STRING) && !g_strcmp0 (wpj_str (o, "backend", ""), "x11"));
    CHECK (is_type (o, "wayland_pointer", G_TYPE_STRING) &&
           !g_strcmp0 (wpj_str (o, "wayland_pointer", ""), "off"));
    CHECK (o && !walpe_set_check (o));
    if (n)
        json_node_unref (n);
    /* a name that is none of the three is answered by walpe-ctl itself */
    CHECK (run_ctl (&err, "backend", "mir", NULL) == 1);
    CHECK (err && strstr (err, "auto, x11 or wayland"));
    g_clear_pointer (&err, g_free);

    /* a key with a quote in it stays one member of a "set" (it used to
     * end the member and bring its own "cmd" along) */
    CHECK (run_ctl (NULL, "set", "zz\":1,\"cmd\":\"quit\",\"y=on", NULL) == 0);
    n = sent ();
    o = n ? json_node_get_object (n) : NULL;
    CHECK (o && !g_strcmp0 (wpj_str (o, "cmd", ""), "set") && json_object_get_size (o) == 2 &&
           json_object_has_member (o, "zz\":1,\"cmd\":\"quit\",\"y"));
    if (o) {
        char *e = walpe_set_check (o);      /* and the daemon names it as unknown */
        CHECK (e && strstr (e, "unknown setting"));
        g_free (e);
    }
    if (n)
        json_node_unref (n);

    /* setprop: one pair as "setprop", several as one "setprops"; the target */
    CHECK (run_ctl (NULL, "setprop", "speed=2", NULL) == 0);
    n = sent ();
    o = n ? json_node_get_object (n) : NULL;
    CHECK (o && !g_strcmp0 (wpj_str (o, "cmd", ""), "setprop") &&
           !g_strcmp0 (wpj_str (o, "key", ""), "speed") && wpj_num (o, "value", 0) == 2 &&
           !json_object_has_member (o, "monitor") && !json_object_has_member (o, "path"));
    if (n)
        json_node_unref (n);
    CHECK (run_ctl (NULL, "setprop", "--monitor", "1", "speed=2", "title=a b", "clock=off", NULL) == 0);
    n = sent ();
    o = n ? json_node_get_object (n) : NULL;
    JsonObject *pr = o ? wpj_obj (o, "props") : NULL;
    CHECK (o && !g_strcmp0 (wpj_str (o, "cmd", ""), "setprops") && wpj_num (o, "monitor", -1) == 1);
    CHECK (pr && json_object_get_size (pr) == 3 && wpj_num (pr, "speed", 0) == 2 &&
           !g_strcmp0 (wpj_str (pr, "title", ""), "a b") && is_type (pr, "clock", G_TYPE_BOOLEAN));
    if (n)
        json_node_unref (n);
    /* setprop: a word goes out as the option's type has it. Text options
     * (text, colour, a combo of words) keep "on", "TRUE" and "1.50" as
     * typed; a checkbox, a slider, a combo of numbers and an option the
     * wallpaper does not declare take the word as a boolean / number. The
     * daemon is asked for the options only when a word would not be stored
     * as typed, and about the same wallpaper. */
    g_mutex_lock (&lock);
    props_asked = 0;
    g_mutex_unlock (&lock);
    CHECK (run_ctl (NULL, "setprop", "title=a b", "speed=3", "flag=true", "title2=12", NULL) == 0);
    n = sent ();
    o = n ? json_node_get_object (n) : NULL;
    pr = o ? wpj_obj (o, "props") : NULL;
    CHECK (pr && props_asked == 0 && is_type (pr, "speed", G_TYPE_INT64) && is_type (pr, "flag", G_TYPE_BOOLEAN) &&
           is_type (pr, "title2", G_TYPE_INT64));
    if (n)
        json_node_unref (n);
    CHECK (run_ctl (NULL, "setprop", "title=on", NULL) == 0);
    n = sent ();
    o = n ? json_node_get_object (n) : NULL;
    CHECK (o && props_asked == 1 && !g_strcmp0 (wpj_str (o, "cmd", ""), "setprop") &&
           is_type (o, "value", G_TYPE_STRING) && !g_strcmp0 (wpj_str (o, "value", ""), "on"));
    if (n)
        json_node_unref (n);
    CHECK (run_ctl (NULL, "setprop", "--monitor", "2", "title=1.50", "tint=TRUE", "flag=on", "speed=1.50",
                    "kind=On", "mode=3.0", "other=off", "zip=007", NULL) == 0);
    n = sent ();
    o = n ? json_node_get_object (n) : NULL;
    pr = o ? wpj_obj (o, "props") : NULL;
    CHECK (pr && props_asked == 2 && json_object_get_size (pr) == 8 && wpj_num (o, "monitor", -1) == 2);
    CHECK (is_type (pr, "title", G_TYPE_STRING) && !g_strcmp0 (wpj_str (pr, "title", ""), "1.50"));
    CHECK (is_type (pr, "tint", G_TYPE_STRING) && !g_strcmp0 (wpj_str (pr, "tint", ""), "TRUE"));
    CHECK (is_type (pr, "flag", G_TYPE_BOOLEAN) && wpj_bool (pr, "flag", FALSE));
    CHECK (is_type (pr, "speed", G_TYPE_DOUBLE) && wpj_num (pr, "speed", 0) == 1.5);
    CHECK (is_type (pr, "kind", G_TYPE_STRING) && !g_strcmp0 (wpj_str (pr, "kind", ""), "On"));
    CHECK (is_type (pr, "mode", G_TYPE_DOUBLE) && wpj_num (pr, "mode", 0) == 3);
    CHECK (is_type (pr, "other", G_TYPE_BOOLEAN) && !wpj_bool (pr, "other", TRUE));
    CHECK (is_type (pr, "zip", G_TYPE_STRING) && !g_strcmp0 (wpj_str (pr, "zip", ""), "007"));
    if (n)
        json_node_unref (n);
    g_mutex_lock (&lock);
    {
        JsonNode *pn = last_props_req ? wpj_parse_data (last_props_req, -1) : NULL;
        CHECK (pn && wpj_num (json_node_get_object (pn), "monitor", -1) == 2);   /* the same wallpaper */
        if (pn)
            json_node_unref (pn);
    }
    g_mutex_unlock (&lock);

    CHECK (run_ctl (NULL, "props", "--path=/lib/123", NULL) == 0);
    n = sent ();
    o = n ? json_node_get_object (n) : NULL;
    CHECK (o && !g_strcmp0 (wpj_str (o, "cmd", ""), "props") &&
           !g_strcmp0 (wpj_str (o, "path", ""), "/lib/123"));
    if (n)
        json_node_unref (n);
    CHECK (run_ctl (NULL, "resetprops", "--monitor=0", NULL) == 0);
    n = sent ();
    o = n ? json_node_get_object (n) : NULL;
    CHECK (o && !g_strcmp0 (wpj_str (o, "cmd", ""), "resetprops") && wpj_num (o, "monitor", -1) == 0);
    if (n)
        json_node_unref (n);

    /* click / scroll: the numbers as given */
    CHECK (run_ctl (NULL, "click", "500", "400", "3", "2", NULL) == 0);
    n = sent ();
    o = n ? json_node_get_object (n) : NULL;
    CHECK (o && wpj_num (o, "x", 0) == 500 && wpj_num (o, "y", 0) == 400 &&
           wpj_num (o, "button", 0) == 3 && wpj_num (o, "count", 0) == 2);
    if (n)
        json_node_unref (n);
    CHECK (run_ctl (NULL, "scroll", "500", "400", "-3", NULL) == 0);
    n = sent ();
    o = n ? json_node_get_object (n) : NULL;
    CHECK (o && !g_strcmp0 (wpj_str (o, "action", ""), "scroll") && wpj_num (o, "dy", 0) == -3);
    if (n)
        json_node_unref (n);

    /* resolution: the request per form; values go out canonical */
    CHECK (run_ctl (NULL, "resolution", NULL) == 0);
    n = sent ();
    o = n ? json_node_get_object (n) : NULL;
    CHECK (o && !g_strcmp0 (wpj_str (o, "cmd", ""), "resolution") && json_object_get_size (o) == 1);
    if (n)
        json_node_unref (n);
    CHECK (run_ctl (NULL, "resolution", "HDMI-0", "2K", "texture_limit=4k", NULL) == 0);
    n = sent ();
    o = n ? json_node_get_object (n) : NULL;
    CHECK (o && !g_strcmp0 (wpj_str (o, "cmd", ""), "resolution") && is_type (o, "monitor", G_TYPE_STRING) &&
           !g_strcmp0 (wpj_str (o, "monitor", ""), "HDMI-0") && !g_strcmp0 (wpj_str (o, "value", ""), "1440p") &&
           !g_strcmp0 (wpj_str (o, "texture_limit", ""), "4096") && !json_object_has_member (o, "asked"));
    if (n)
        json_node_unref (n);
    CHECK (run_ctl (NULL, "resolution", "1", "1920X1200", NULL) == 0);     /* an index */
    n = sent ();
    o = n ? json_node_get_object (n) : NULL;
    CHECK (o && is_type (o, "monitor", G_TYPE_INT64) && wpj_num (o, "monitor", -1) == 1 &&
           !g_strcmp0 (wpj_str (o, "value", ""), "1920x1200"));
    if (n)
        json_node_unref (n);
    /* an index typed with zeros in front goes out as that number ("08" is
     * not JSON, and "010" is eight to a reader that takes it at all) */
    CHECK (run_ctl (NULL, "resolution", "08", "native", NULL) == 0);
    n = sent ();
    o = n ? json_node_get_object (n) : NULL;
    CHECK (o && is_type (o, "monitor", G_TYPE_INT64) && wpj_num (o, "monitor", -1) == 8);
    if (n)
        json_node_unref (n);
    CHECK (run_ctl (NULL, "resolution", "010", "native", NULL) == 0);
    n = sent ();
    o = n ? json_node_get_object (n) : NULL;
    CHECK (o && is_type (o, "monitor", G_TYPE_INT64) && wpj_num (o, "monitor", -1) == 10);
    if (n)
        json_node_unref (n);
    CHECK (run_ctl (NULL, "resolution", "all", "native", NULL) == 0);       /* every connected one */
    n = sent ();
    o = n ? json_node_get_object (n) : NULL;
    CHECK (o && !json_object_has_member (o, "monitor") && !g_strcmp0 (wpj_str (o, "value", ""), "native"));
    if (n)
        json_node_unref (n);
    CHECK (run_ctl (NULL, "resolution", "--keep", NULL) == 0);
    n = sent ();
    o = n ? json_node_get_object (n) : NULL;
    CHECK (o && is_type (o, "asked", G_TYPE_BOOLEAN) && wpj_bool (o, "asked", FALSE) &&
           !json_object_has_member (o, "monitor") && !json_object_has_member (o, "value"));
    if (n)
        json_node_unref (n);
    CHECK (run_ctl (NULL, "resolution", "DP-2", "--ask", NULL) == 0);
    n = sent ();
    o = n ? json_node_get_object (n) : NULL;
    CHECK (o && !g_strcmp0 (wpj_str (o, "monitor", ""), "DP-2") && is_type (o, "asked", G_TYPE_BOOLEAN) &&
           !wpj_bool (o, "asked", TRUE));
    if (n)
        json_node_unref (n);
    CHECK (run_ctl (NULL, "resolution", "DP-2", "--forget", NULL) == 0);
    n = sent ();
    o = n ? json_node_get_object (n) : NULL;
    CHECK (o && wpj_bool (o, "forget", FALSE) && !json_object_has_member (o, "value"));
    if (n)
        json_node_unref (n);
    CHECK (run_ctl (NULL, "resolution", "--prompt", "off", NULL) == 0);
    n = sent ();
    o = n ? json_node_get_object (n) : NULL;
    CHECK (o && is_type (o, "prompt", G_TYPE_BOOLEAN) && !wpj_bool (o, "prompt", TRUE) &&
           !json_object_has_member (o, "monitor"));
    if (n)
        json_node_unref (n);
    /* ... and through set: text stays text ("none", "off" as a resolution
     * word), the switch is a switch, and the daemon's check takes it */
    CHECK (run_ctl (NULL, "set", "resolution=HDMI-0:1080p,DP-2:native", "texture_limit=2048",
                    "scale_filter=nearest", "video_resolution=none", "resolution_prompt=off", NULL) == 0);
    n = sent ();
    o = n ? json_node_get_object (n) : NULL;
    CHECK (o && is_type (o, "resolution", G_TYPE_STRING) &&
           !g_strcmp0 (wpj_str (o, "resolution", ""), "HDMI-0:1080p,DP-2:native") &&
           !g_strcmp0 (wpj_str (o, "texture_limit", ""), "2048") &&
           !g_strcmp0 (wpj_str (o, "scale_filter", ""), "nearest") &&
           is_type (o, "video_resolution", G_TYPE_STRING) &&
           !g_strcmp0 (wpj_str (o, "video_resolution", ""), "none") &&
           is_type (o, "resolution_prompt", G_TYPE_BOOLEAN) && !wpj_bool (o, "resolution_prompt", TRUE));
    CHECK (o && !walpe_set_check (o));
    if (n)
        json_node_unref (n);

    /* open --monitor N */
    CHECK (run_ctl (NULL, "open", "/lib/123", "--monitor", "1", NULL) == 0);
    n = sent ();
    o = n ? json_node_get_object (n) : NULL;
    CHECK (o && !g_strcmp0 (wpj_str (o, "path", ""), "/lib/123") && wpj_num (o, "monitor", -1) == 1);
    if (n)
        json_node_unref (n);

    /* every short name of docs/SETTINGS.md goes out as the key it stands for */
    for (guint i = 0; doc_short && i + 1 < doc_short->len; i += 2) {
        char *arg = g_strdup_printf ("%s=on", (char *) doc_short->pdata[i]);
        CHECK (run_ctl (NULL, "set", arg, NULL) == 0);
        n = sent ();
        o = n ? json_node_get_object (n) : NULL;
        if (!o || !json_object_has_member (o, doc_short->pdata[i + 1])) {
            g_printerr ("FAIL: `walpe-ctl set %s` does not send \"%s\" (docs/SETTINGS.md)\n", arg,
                        (char *) doc_short->pdata[i + 1]);
            fails++;
        }
        if (n)
            json_node_unref (n);
        g_free (arg);
    }

    /* the daemon refused: 3; a command an older daemon does not have: 3 and a sentence */
    CHECK (run_ctl (NULL, "setprop", "refuse=1", NULL) == 3);
    CHECK (run_ctl (&err, "quality", NULL) == 3);
    CHECK (err && strstr (err, "older than this walpe-ctl"));
    g_free (err);

    /* Replies whose texts carry control characters: the title of a
     * Workshop item (install), names from a scene (quality --effects,
     * profile). What walpe-ctl prints has none of them left, and says
     * where they were. */
    g_atomic_int_set (&hostile_text, 1);
    err = NULL;
    CHECK (run_ctl (&err, "install", "1", NULL) == 0);
    CHECK (last_out && strstr (last_out, "installed: /lib/") && strstr (last_out, "title"));
    CHECK (last_out && strstr (last_out, "\\x1b[2Jphase") && strstr (last_out, "\\x1b]0;dir\\x07") &&
           strstr (last_out, "\\x1b[31m\\x9b1m\\x7f"));
    CHECK (terminal_safe (last_out) && terminal_safe (err));
    g_clear_pointer (&err, g_free);
    CHECK (run_ctl (&err, "install", "2", NULL) == 3);
    CHECK (err && strstr (err, "no\\x1b[2J") && terminal_safe (err));
    g_clear_pointer (&err, g_free);
    CHECK (run_ctl (&err, "quality", "--effects", NULL) == 0);
    CHECK (last_out && strstr (last_out, "preset: high\\x1b[2J") && strstr (last_out, "layer\\x1b[2J | fx\\x07 | static\n") &&
           strstr (last_out, "own settings of /w\\x1b[1m:") && strstr (last_out, "note\\x1b[7m"));
    CHECK (terminal_safe (last_out) && terminal_safe (err));
    g_clear_pointer (&err, g_free);
    CHECK (run_ctl (&err, "profile", "1", NULL) == 0);
    CHECK (last_out && strstr (last_out, "layer \\x1b[2J\tcolumn\nnext line\\x9b\n"));
    CHECK (terminal_safe (last_out) && terminal_safe (err));
    g_clear_pointer (&err, g_free);
    CHECK (run_ctl (&err, "layers", NULL) == 0);
    CHECK (last_out && strstr (last_out, "/s\\x1b[2J\n") && strstr (last_out, "back\\x1b[1m") &&
           strstr (last_out, "file models/a\\x1b]0;f\\x07.json") && strstr (last_out, "words\\x9b2J") &&
           strstr (last_out, "text \"hello\\x1b[2J world\\x7f\"") && strstr (last_out, "hidden\\x1b") &&
           strstr (last_out, "\"gone\\x1b[5m\""));
    CHECK (terminal_safe (last_out) && terminal_safe (err));
    g_clear_pointer (&err, g_free);
    CHECK (run_ctl (&err, "layers", "hide", "k", NULL) != 0);
    CHECK (err && strstr (err, "no layer \\x1b[2J") && terminal_safe (err));
    g_clear_pointer (&err, g_free);
    CHECK (run_ctl (&err, "layers", "--json", NULL) == 0);
    CHECK (last_out && strstr (last_out, "\\u009b2J") && terminal_safe (last_out) && terminal_safe (err));
    g_clear_pointer (&err, g_free);
    /* a reply printed as it is stays the same JSON */
    CHECK (run_ctl (&err, "props", NULL) == 0);
    CHECK (terminal_safe (last_out) && terminal_safe (err));
    {
        JsonNode *pn = last_out ? wpj_parse_data (last_out, -1) : NULL;
        JsonObject *po = pn && JSON_NODE_HOLDS_OBJECT (pn) ? json_node_get_object (pn) : NULL;
        CHECK (po && !g_strcmp0 (wpj_str (po, "title", ""), "a\x7f" "b\xc2\x9b" "1m\x1f" "c\x1b[2J"));
        if (pn)
            json_node_unref (pn);
    }
    g_clear_pointer (&err, g_free);
    g_atomic_int_set (&hostile_text, 0);

    g_main_loop_quit (loop);
    return NULL;
}

/* part 3: every member the "set" request is asked for is in the table */
static void scan_daemon (const char *path)
{
    char *txt = NULL;
    if (!g_file_get_contents (path, &txt, NULL, NULL)) {
        CHECK (!"daemon source readable");
        return;
    }
    /* the merged tree: the body of "set" is ipc_set_keys () (ipc_set () became
     * a wrapper that flushes the reloads once) and the effect shader keys
     * are read by ipc_fx_keys () */
    static const char *const heads[] = {
        "\nstatic void ipc_set_keys (JsonBuilder *b, JsonObject *o)\n{",
        "\nstatic gboolean ipc_fx_keys (JsonObject *o, gboolean *given, const char **err)\n{",
    };
    int found = 0;
    for (guint hi = 0; hi < G_N_ELEMENTS (heads); hi++) {
    const char *a = strstr (txt, heads[hi]);
    const char *z = a ? strstr (a, "\n}\n") : NULL;
    CHECK (a && z);
    if (a && z) {
        char *body = g_strndup (a, (gsize) (z - a));
        GRegex *re = g_regex_new ("\\(o, \"([a-z_0-9]+)\"", 0, 0, NULL);
        GMatchInfo *mi = NULL;
        g_regex_match (re, body, 0, &mi);
        while (g_match_info_matches (mi)) {
            char *k = g_match_info_fetch (mi, 1);
            found++;
            if (!walpe_set_key_find (k)) {
                g_printerr ("FAIL: ipc_set reads \"%s\", which common/setkeys.h does not list\n", k);
                fails++;
            }
            g_free (k);
            g_match_info_next (mi, NULL);
        }
        g_match_info_free (mi);
        g_regex_unref (re);
        g_free (body);
    }
    }
    CHECK (found > 40);     /* the scan saw the functions */

    /* and status shows every setting "set" takes, under the name it is set
     * by: a change could be made and saved that no `walpe-ctl status`
     * showed (pause_covered, parallax, focus_fps ...) */
    static const char *const elsewhere[] = {
        "cmd",
        /* short names of keys that are listed themselves */
        "preset", "post", "scale", "tex", "slack", "fx_fast",
        /* the quality knobs: `walpe-ctl quality` (status has quality_preset) */
        "fx_rate", "fx_rate_mode", "fps_idle", "post_processing", "tex_res", "fx_res", "texture_slack",
        /* in status as "backend" */
        "video_backend",
        /* written by the functions ipc_status calls: gpu_status_json
         * (daemon/gpu_glue.h), status_backend */
        "gpu", "preset_follow_gpu", "wayland_pointer",
        /* the scene feature switches: members of "feature_switches", one
         * per row of engine/features.h */
        "camera_parallax", "parallax_offset", "script_camera", "sprite_sheets", "script_texanim",
        "bloom", "camera_shake",
    };
    const char *sa = strstr (txt, "\nstatic void ipc_status (JsonBuilder *b)\n{");
    const char *sz = sa ? strstr (sa, "\n}\n") : NULL;
    CHECK (sa && sz);
    if (sa && sz) {
        char *body = g_strndup (sa, (gsize) (sz - sa));
        for (guint i = 0; i < G_N_ELEMENTS (walpe_set_keys); i++) {
            const char *k = walpe_set_keys[i].key;
            gboolean skip = FALSE;
            for (guint e = 0; e < G_N_ELEMENTS (elsewhere); e++)
                skip |= !strcmp (k, elsewhere[e]);
            if (skip)
                continue;
            char *member = g_strdup_printf ("(b, \"%s\"", k);
            if (!strstr (body, member)) {
                g_printerr ("FAIL: \"set\" takes \"%s\" but status does not show it\n", k);
                fails++;
            }
            g_free (member);
        }
        CHECK (strstr (body, "(b, \"software_threads_saved\""));   /* the setting next to the count in use */
        g_free (body);
    }
    g_free (txt);
}

/* part 4: docs/SETTINGS.md and the table of "set" keys agree. A row of the
 * config.json tables names its keys in the first column; the third has the
 * short name `walpe-ctl set` also takes or, in brackets, how the key is
 * changed instead. ("The key itself always works" stood there while
 * wallpapers_dir, vsync and audio were refused as unknown settings.) */
static void scan_doc (const char *path)
{
    char *txt = NULL;
    doc_short = g_ptr_array_new_with_free_func (g_free);
    if (!g_file_get_contents (path, &txt, NULL, NULL)) {
        CHECK (!"docs/SETTINGS.md readable");
        return;
    }
    const char *a = strstr (txt, "\n## config.json");
    const char *z = a ? strstr (a, "\n## Quality presets") : NULL;
    CHECK (a && z);
    int rows = 0;
    GPtrArray *names = g_ptr_array_new_with_free_func (g_free);    /* what the usage text has to name */
    if (a && z) {
        char *sect = g_strndup (a, (gsize) (z - a));
        char **lines = g_strsplit (sect, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            if (!g_str_has_prefix (lines[i], "| `"))
                continue;
            char **col = g_strsplit (lines[i], "|", 5);     /* "", keys, default, set name, meaning */
            if (g_strv_length (col) == 5) {
                rows++;
                char *how = g_strstrip (g_strdup (col[3]));
                gboolean other = how[0] == '(';
                char **keys = g_strsplit (col[1], "`", -1);     /* every second piece is a name */
                for (int k = 1; keys[k]; k += 2) {
                    gboolean taken = walpe_set_key_find (keys[k]) != NULL;
                    if (other && taken) {
                        g_printerr ("FAIL: SETTINGS.md says `set` does not take \"%s\", but it does\n", keys[k]);
                        fails++;
                    } else if (!other && !taken) {
                        g_printerr ("FAIL: SETTINGS.md lists \"%s\" as a key `walpe-ctl set` takes; "
                                    "it is refused as an unknown setting\n", keys[k]);
                        fails++;
                    }
                    if (!other && how[0] == '`' && strchr (how + 1, '`')) {
                        g_ptr_array_add (doc_short, g_strndup (how + 1, (gsize) (strchr (how + 1, '`') - how - 1)));
                        g_ptr_array_add (doc_short, g_strdup (keys[k]));
                        g_ptr_array_add (names, g_strndup (how + 1, (gsize) (strchr (how + 1, '`') - how - 1)));
                    } else if (!other) {
                        g_ptr_array_add (names, g_strdup (keys[k]));
                    }
                }
                g_strfreev (keys);
                g_free (how);
            }
            g_strfreev (col);
        }
        g_strfreev (lines);
        g_free (sect);
    }
    CHECK (rows > 30 && doc_short->len >= 16);
    doc_usage_names = names;
    /* the other way round: every key `set` takes is named in the document
     * (fx_fast is the earlier name of fx_quality=fast, kept for old clients) */
    for (guint i = 0; i < G_N_ELEMENTS (walpe_set_keys); i++) {
        const char *k = walpe_set_keys[i].key;
        if (!strcmp (k, "cmd") || !strcmp (k, "fx_fast"))
            continue;
        char *re = g_strdup_printf ("(?<![a-z_])%s(?![a-z_])", k);
        if (!g_regex_match_simple (re, txt, 0, 0)) {
            g_printerr ("FAIL: `walpe-ctl set` takes \"%s\", which docs/SETTINGS.md does not mention\n", k);
            fails++;
        }
        g_free (re);
    }
    g_free (txt);
}

static char *check_json (const char *req)
{
    JsonNode *n = wpj_parse_data (req, -1);
    char *e = n && JSON_NODE_HOLDS_OBJECT (n) ? walpe_set_check (json_node_get_object (n)) : g_strdup ("?");
    if (n)
        json_node_unref (n);
    return e;
}

#define REFUSED(req, text) do { \
        char *e_ = check_json (req); \
        if (!e_ || !strstr (e_, text)) { \
            g_printerr ("FAIL %s:%d: %s -> %s\n", __FILE__, __LINE__, req, e_ ? e_ : "(accepted)"); \
            fails++; \
        } \
        g_free (e_); \
    } while (0)
#define TAKEN(req) do { \
        char *e_ = check_json (req); \
        if (e_) { \
            g_printerr ("FAIL %s:%d: %s -> %s\n", __FILE__, __LINE__, req, e_); \
            fails++; \
        } \
        g_free (e_); \
    } while (0)

int main (int argc, char **argv)
{
    if (argc < 4) {
        g_printerr ("usage: ctl-test <walpe-ctl> <src/daemon/main.c> <docs/SETTINGS.md>\n");
        return 2;
    }
    ctl = argv[1];
    scan_doc (argv[3]);     /* before the walpe-ctl runs: they try its short names */
    /* everything private, before GLib reads the directories */
    char *dir = g_dir_make_tmp ("walpe-ctl-test-XXXXXX", NULL);
    g_setenv ("XDG_RUNTIME_DIR", dir, TRUE);
    g_setenv ("XDG_CONFIG_HOME", dir, TRUE);
    g_setenv ("HOME", dir, TRUE);
    char *sock = walpe_socket_path ();
    /* a temp dir too long for a socket address makes every WalPE program
     * fall back to a socket in a directory shared by all sessions of the
     * user: never talk to (or stand in for) whatever listens there */
    gboolean private_sock = g_str_has_prefix (sock, dir);
    if (!private_sock) {
        g_print ("ctl-test: %s is too long for a socket path (TMPDIR): the walpe-ctl runs are skipped\n", dir);
        goto table;
    }

    /* ---- 1. bad input, no daemon */
    {
        /* the usage text is the list the answer to an unknown setting
         * points to ("walpe-ctl without arguments lists the settings"): it
         * names every key of docs/SETTINGS.md that `set` takes, by its
         * short name where it has one (dpms was not in it), and the two
         * that are not saved (present was not). video_backend is left
         * out: stored only, for files of earlier versions. */
        char *usage = NULL;
        ctl_stdout = &usage;
        CHECK (run_ctl (NULL, NULL) == 1);
        CHECK (usage != NULL);
        for (guint i = 0; usage && doc_usage_names && i <= doc_usage_names->len + 1; i++) {
            const char *nm = i < doc_usage_names->len ? doc_usage_names->pdata[i]
                           : i == doc_usage_names->len ? "present" : "frame_align";
            if (!strcmp (nm, "video_backend"))
                continue;
            char *re = g_strdup_printf ("(?<![a-z_])%s(?![a-z_])", nm);
            if (!g_regex_match_simple (re, usage, 0, 0)) {
                g_printerr ("FAIL: the usage text of walpe-ctl does not name \"%s\"\n", nm);
                fails++;
            }
            g_free (re);
        }
        g_free (usage);
    }
    CHECK (run_ctl (NULL, NULL) == 1);
    CHECK (run_ctl (NULL, "help", NULL) == 0);
    CHECK (run_ctl (NULL, "--help", NULL) == 0);
    CHECK (run_ctl (NULL, "frobnicate", NULL) == 1);
    BAD ("at least one", "set");
    BAD ("is not <key>=<value>", "set", "fps", "30");
    BAD ("is not <key>=<value>", "set", "fps=30", "covered");
    BAD ("is not <key>=<value>", "set", "=3");
    BAD ("needs a number", "set", "fps=abc");
    BAD ("needs a number", "set", "fps=");
    BAD ("needs a number", "set", "volume=loud");
    BAD ("threads or auto", "set", "software_threads=many");
    BAD ("max, high, balanced or low", "set", "preset=best");
    BAD ("fx_rate", "set", "fps=30", "fx_rate=7");
    BAD ("whole number", "click", "abc", "def");
    BAD ("whole number", "click", "10", "10x");
    BAD ("BUTTON", "click", "10", "10", "9");
    BAD ("COUNT", "click", "10", "10", "1", "0");
    BAD ("DY", "scroll", "10", "10", "down");
    BAD ("--monitor needs", "open", "/lib/123", "--monitor");
    BAD ("--monitor needs", "open", "/lib/123", "--monitor", "one");
    BAD ("one wallpaper", "open", "/lib/123", "/lib/456");
    BAD ("is not <key>=<value>", "setprop", "speed");
    BAD ("needs a value", "setprop", "speed=2", "--monitor");
    BAD ("setprop needs", "setprop", "--monitor", "1");
    BAD ("--monitor N or --path P", "props", "--all");
    BAD ("1..600", "profile", "0.5");
    BAD ("say which wallpaper", "quality", "fx_rate=2");
    CHECK (run_ctl (NULL, "click", "10", NULL) == 1);
    CHECK (run_ctl (NULL, "open", NULL) == 1);
    CHECK (run_ctl (NULL, "setprop", NULL) == 1);
    NODAEMON ("status");
    NODAEMON ("set", "fps=30", "covered=off");
    NODAEMON ("click", "10", "10");
    NODAEMON ("setprop", "a=1", "b=2");
    NODAEMON ("props", "--monitor", "1");
    NODAEMON ("quality");
    NODAEMON ("cancel-import");
    /* resolution: a value that is not one never leaves walpe-ctl */
    BAD ("is not one of", "resolution", "HDMI-0", "1081q");
    BAD ("too small", "resolution", "HDMI-0", "10x10");
    BAD ("too large", "resolution", "HDMI-0", "20000x1200");   /* (20000x100 is too small first: 100 < 200 lines) */
    BAD ("which monitor", "resolution", "1080p");
    BAD ("texture_limit", "resolution", "HDMI-0", "texture_limit=huge");
    BAD ("out of range", "resolution", "HDMI-0", "texture_limit=7");
    BAD ("one argument too many", "resolution", "HDMI-0", "1080p", "720p");
    BAD ("unknown option", "resolution", "--frobnicate");
    BAD ("--prompt needs", "resolution", "--prompt", "maybe");
    BAD ("goes alone", "resolution", "HDMI-0", "1080p", "--forget");
    BAD ("nothing to set", "resolution", "HDMI-0");
    NODAEMON ("resolution");
    NODAEMON ("resolution", "all", "native");
    NODAEMON ("set", "resolution=1080p");

    /* ---- 2. what goes over the socket */
    loop = g_main_loop_new (NULL, FALSE);
    CHECK (walpe_ipc_serve (sock, handler, NULL));
    GThread *t = g_thread_new ("client", client_thread, NULL);
    g_main_loop_run (loop);
    g_thread_join (t);
    walpe_ipc_release (sock);

table:
    /* ---- 3. the daemon's check of a "set" request */
    TAKEN ("{\"cmd\":\"set\",\"fps\":30}");
    TAKEN ("{\"cmd\":\"set\",\"pause_covered\":false,\"pause_fullscreen\":1,\"volume\":0.5}");
    TAKEN ("{\"cmd\":\"set\",\"software_threads\":\"auto\",\"preview_anim\":false,\"particle_lowres\":\"all\"}");
    TAKEN ("{\"cmd\":\"set\",\"fx_rate\":\"3\",\"post\":\"off\",\"render_scale\":50,\"quality_preset\":\"low\"}");
    TAKEN ("{\"cmd\":\"set\",\"we_assets\":\"\",\"steam_user\":\"me\",\"present\":\"limiter\"}");
    REFUSED ("{\"cmd\":\"set\",\"coverd\":false,\"fps\":30}", "unknown setting \"coverd\"");
    REFUSED ("{\"cmd\":\"set\",\"fps\":true}", "fps needs a number");
    REFUSED ("{\"cmd\":\"set\",\"fps\":\"30\"}", "fps needs a number");
    REFUSED ("{\"cmd\":\"set\",\"pause_covered\":\"yes\"}", "pause_covered is a switch");
    TAKEN ("{\"cmd\":\"set\",\"pause_fullscreen_all\":true}");
    REFUSED ("{\"cmd\":\"set\",\"pause_fullscreen_all\":\"all\"}", "pause_fullscreen_all is a switch");
    REFUSED ("{\"cmd\":\"set\",\"focus_action\":false}", "focus_action needs a name");
    TAKEN ("{\"cmd\":\"set\",\"backend\":\"wayland\",\"wayland_pointer\":\"auto\"}");
    REFUSED ("{\"cmd\":\"set\",\"backend\":true}", "backend needs a name");
    REFUSED ("{\"cmd\":\"set\",\"wayland_pointer\":1}", "wayland_pointer needs a name");
    REFUSED ("{\"cmd\":\"set\",\"parallax\":[true]}", "parallax is a switch");
    REFUSED ("{\"cmd\":\"set\",\"particle_density\":{\"a\":1}}", "single value");
    /* the render resolution keys */
    TAKEN ("{\"cmd\":\"set\",\"resolution\":\"1080p\",\"texture_limit\":\"auto\",\"scale_filter\":\"nearest\"}");
    TAKEN ("{\"cmd\":\"set\",\"texture_limit\":2048,\"video_resolution\":\"none\",\"resolution_prompt\":false}");
    TAKEN ("{\"cmd\":\"set\",\"texture_limit\":false}");
    REFUSED ("{\"cmd\":\"set\",\"resolution\":1080}", "resolution needs a name or text");
    REFUSED ("{\"cmd\":\"set\",\"texture_limit\":[2048]}", "single value");
    REFUSED ("{\"cmd\":\"set\",\"resolution_prompt\":\"yes\"}", "resolution_prompt is a switch");
    REFUSED ("{\"cmd\":\"set\",\"video_resolution\":false}", "video_resolution needs a name");
    /* values of the right type that were answered with ok and changed nothing */
    REFUSED ("{\"cmd\":\"set\",\"skip_hidden\":\"alwys\"}", "skip_hidden: off, auto or always");
    REFUSED ("{\"cmd\":\"set\",\"skip_hidden\":\"7\"}", "skip_hidden: off, auto or always");
    REFUSED ("{\"cmd\":\"set\",\"fps\":50,\"skip_hidden_margin\":-5}", "skip_hidden_margin: a number of pixels");
    REFUSED ("{\"cmd\":\"set\",\"skip_hidden_inset\":-0.5}", "skip_hidden_inset: a number of pixels");
    REFUSED ("{\"cmd\":\"set\",\"video_backend\":\"vlc\",\"fps\":50}", "video_backend: ffmpeg");
    TAKEN ("{\"cmd\":\"set\",\"skip_hidden\":\"always\",\"skip_hidden_margin\":0,\"skip_hidden_inset\":12}");
    TAKEN ("{\"cmd\":\"set\",\"skip_hidden\":\"Auto\"}");
    TAKEN ("{\"cmd\":\"set\",\"skip_hidden\":true}");
    TAKEN ("{\"cmd\":\"set\",\"skip_hidden\":2}");
    TAKEN ("{\"cmd\":\"set\",\"skip_hidden\":\"2\"}");          /* as walpe-ctl sends skip_hidden=2 */
    TAKEN ("{\"cmd\":\"set\",\"video_backend\":\"ffmpeg\"}");
    TAKEN ("{\"cmd\":\"set\",\"video_backend\":\"mpv\"}");
    /* every quality key walpe-ctl knows is a member the daemon takes */
    static const char *const qk[] = { "preset", "quality_preset", "fx_rate", "fx_rate_mode", "fx_res",
                                      "fps_idle", "post", "post_processing", "render_scale", "scale",
                                      "tex_res", "tex", "texture_slack", "slack" };
    for (guint i = 0; i < G_N_ELEMENTS (qk); i++) {
        gboolean taken = FALSE;
        JsonBuilder *b = json_builder_new ();
        json_builder_begin_object (b);
        walpe_quality_kv_to_json (b, qk[i], "default", &taken, NULL);
        json_builder_end_object (b);
        g_object_unref (b);
        CHECK (taken && walpe_set_key_find (qk[i]));
    }
    scan_daemon (argv[2]);

    char *lk = g_strconcat (sock, ".lock", NULL);
    if (private_sock) {
        g_unlink (lk);
        g_unlink (sock);
    }
    g_rmdir (dir);
    g_free (lk);
    g_free (sock);
    g_free (dir);
    if (fails)
        g_printerr ("ctl-test: %d check(s) failed\n", fails);
    else
        g_print ("ctl-test: %s\n", private_sock ? "ok" : "ok (in part: see above)");
    return fails ? 1 : private_sock ? 0 : 77;
}
