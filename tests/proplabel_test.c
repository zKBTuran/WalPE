/* proplabel_test.c - option labels (src/ui/props.c row_label): WE property
 * "text" is HTML/BBCode written by wallpaper authors; the GUI must show
 * readable plain text and never raw markup.
 *
 *   proplabel-test                 fixed cases (meson test)
 *   proplabel-test <project.json>  print every option's label ("key<TAB>label",
 *                                  newlines as " / "); exit 1 if one still
 *                                  contains markup */
#include "ui/props.h"
#include "common/util.h"
#include "engine/propnotes.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static int fails;

static JsonObject *mkdef (const char *type, const char *text, gboolean with_value)
{
    JsonObject *d = json_object_new ();
    if (type)
        json_object_set_string_member (d, "type", type);
    if (text)
        json_object_set_string_member (d, "text", text);
    if (with_value)
        json_object_set_boolean_member (d, "value", TRUE);
    return d;
}

static void expect (const char *type, const char *key, const char *text, gboolean with_value,
                    const char *want)
{
    JsonObject *d = mkdef (type, text, with_value);
    char *got = walpe_props_label_preview (d, key, NULL);
    if (strcmp (got, want)) {
        fails++;
        fprintf (stderr, "FAIL %s: text [%s]\n  got  [%s]\n  want [%s]\n", key, text ? text : "(none)",
                 got, want);
    }
    g_free (got);
    json_object_unref (d);
}

/* raw markup left in a label? (a '<' that starts a tag, an http link) */
static gboolean has_markup (const char *s)
{
    for (const char *p = s; (p = strchr (p, '<')); p++)
        if (g_ascii_isalpha (p[1]) || p[1] == '/')
            return TRUE;
    return strstr (s, "src=") || strstr (s, "href=") || strstr (s, "://");
}

static int dump (const char *file)
{
    JsonNode *root = wpj_parse_file (file);
    JsonObject *gen = root && JSON_NODE_HOLDS_OBJECT (root)
        ? wpj_obj (json_node_get_object (root), "general") : NULL;
    JsonObject *props = wpj_obj (gen, "properties");
    JsonObject *loc = wpj_obj (gen, "localization");
    int bad = 0;
    GList *ks = props ? json_object_get_members (props) : NULL;
    for (GList *k = ks; k; k = k->next) {
        JsonObject *d = wpj_obj (props, k->data);
        if (!d)
            continue;
        char *l = walpe_props_label_preview (d, k->data, loc);
        gboolean m = has_markup (l);
        bad += m;
        char **parts = g_strsplit (l, "\n", -1);
        char *one = g_strjoinv (" / ", parts);
        printf ("%s%s\t%s\n", m ? "MARKUP " : "", (const char *) k->data, one);
        /* options of combos */
        JsonArray *oa = wpj_arr (d, "options");
        for (guint i = 0; i < wpj_arr_len (oa); i++) {
            JsonObject *oo = wpj_arr_obj (oa, i);
            const char *ol = oo ? wpj_str (oo, "label", NULL) : NULL;
            if (!ol)
                continue;
            JsonObject *od = mkdef ("bool", ol, TRUE);
            char *o2 = walpe_props_label_preview (od, "option", loc);
            if (has_markup (o2)) {
                bad++;
                printf ("MARKUP %s option\t%s\n", (const char *) k->data, o2);
            }
            g_free (o2);
            json_object_unref (od);
        }
        g_free (one);
        g_strfreev (parts);
        g_free (l);
    }
    g_list_free (ks);
    if (root)
        json_node_unref (root);
    return bad ? 1 : 0;
}

/* --cond <project.json>: "key<TAB>0|1" for every option with a condition,
 * evaluated with the default values */
static int dump_cond (const char *file)
{
    JsonNode *root = wpj_parse_file (file);
    JsonObject *gen = root && JSON_NODE_HOLDS_OBJECT (root)
        ? wpj_obj (json_node_get_object (root), "general") : NULL;
    JsonObject *props = wpj_obj (gen, "properties");
    JsonObject *vals = json_object_new ();
    GList *ks = props ? json_object_get_members (props) : NULL;
    for (GList *k = ks; k; k = k->next) {
        JsonObject *d = wpj_obj (props, k->data);
        JsonNode *v = d ? json_object_get_member (d, "value") : NULL;
        if (v && JSON_NODE_HOLDS_VALUE (v))
            json_object_set_member (vals, k->data, json_node_copy (v));
    }
    for (GList *k = ks; k; k = k->next) {
        JsonObject *d = wpj_obj (props, k->data);
        const char *c = d ? wpj_str (d, "condition", NULL) : NULL;
        if (c && *c)
            printf ("%s\t%d\n", (const char *) k->data, walpe_props_condition (c, vals));
    }
    g_list_free (ks);
    json_object_unref (vals);
    if (root)
        json_node_unref (root);
    return 0;
}

/* --notes <wallpaper dir>: "key<TAB>note" (engine/propnotes.h) */
static int dump_notes (const char *path)
{
    gint64 t0 = g_get_monotonic_time ();
    JsonObject *n = walpe_prop_notes (path);
    fprintf (stderr, "notes: %.1f ms\n", (g_get_monotonic_time () - t0) / 1000.0);
    GList *ks = n ? json_object_get_members (n) : NULL;
    for (GList *k = ks; k; k = k->next)
        printf ("%s\t%s\n", (const char *) k->data, json_object_get_string_member (n, k->data));
    g_list_free (ks);
    if (n)
        json_object_unref (n);
    return 0;
}

int main (int argc, char **argv)
{
    if (argc > 2 && !strcmp (argv[1], "--notes"))
        return dump_notes (argv[2]);
    if (argc > 2 && !strcmp (argv[1], "--cond"))
        return dump_cond (argv[2]);
    if (argc > 1)
        return dump (argv[1]);

    /* plain and simple markup */
    expect ("bool", "a", "Show clock", TRUE, "Show clock");
    expect ("bool", "a", "<big><b>Character display", TRUE, "Character display");
    expect ("bool", "a", "<font color=#ff6b6b><big><b>Prompt box", TRUE, "Prompt box");
    expect ("slider", "a", "<hr/><big>Fog", TRUE, "Fog");
    expect ("bool", "a", "Line one<br>Line two<br/>Line three", TRUE, "Line one\nLine two\nLine three");
    expect ("bool", "a", "A &amp; B &lt;3 &quot;x&quot;&nbsp;y", TRUE, "A & B <3 \"x\" y");
    expect ("bool", "a", "a < b and 2 > 1", TRUE, "a < b and 2 > 1");
    /* links show their text, never the URL */
    expect ("bool", "a", "<a href='https://example.org/x?y=1'>My page (click)</a>", TRUE, "My page (click)");
    expect ("bool", "a", "<big>Home<br/><a href='https://example.org'>Open it</a><big></p>", TRUE,
            "Home\nOpen it");
    expect ("bool", "a", "[url=https://example.org]Guide[/url]", TRUE, "Guide");
    /* picture-only labels: "(image)", not the tag and not the key WE
     * derived from it */
    expect ("bool", "imgsrchttpexampleorgpicv1xwidth200height200",
            "<img src=\"http://example.org/pic?/v1/x!/b&bo=AQF3&rf=viewer_4\" width=\"200\" height=\"200\">",
            TRUE, "(image)");
    expect ("bool", "brahrefhttpsxyimgsrc",
            "<br/><a href='https://example.org/u/1'><img src=\"http://example.org/p.png\" width=100></a>",
            TRUE, "(image)");
    expect ("color", "imgsrc", "<img src=http://example.org/a.png>", TRUE, "(image)");
    expect ("bool", "x", "<img src='a.png' alt='Night mode'>", TRUE, "Night mode");
    expect ("bool", "x", "[img]http://example.org/a.png[/img]", TRUE, "(image)");
    expect ("bool", "x", "<a href='https://example.org'></a>", TRUE, "(link)");
    expect ("bool", "x", "<hr><br>", TRUE, "(no label)");
    /* a picture-only header is decoration: no row text at all */
    expect (NULL, "x", "<br><p><center><img src=http://example.org/a.png>", FALSE, "# ");
    expect ("text", "x", "<img src=http://example.org/a.png>", TRUE, "# ");
    /* headers / notes */
    expect (NULL, "h", "<h3>Console</h3>", FALSE, "# Console");
    expect ("text", "h", "<br></br><u><h4>Colour Settings:</h4></u></h5>", TRUE, "# Colour Settings:");
    expect ("group", "g", "Main settings", TRUE, "# Main settings");
    expect ("text", "h", "<h4><u>Time:</u><sup><abbr title='1: 1 Hour'>[help]</abbr></sup></h4>", TRUE,
            "# Time:\n1: 1 Hour");
    /* cut-off tags, comments, scripts */
    expect ("bool", "a", "Rain<br", TRUE, "Rain");
    expect ("bool", "a", "Rain <img src=http://example.org/very/long", TRUE, "Rain");
    expect ("bool", "a", "Snow<!-- hidden -->fall", TRUE, "Snowfall");
    expect ("bool", "a", "A<script>alert(1)</script>B", TRUE, "AB");
    /* padding characters */
    expect ("bool", "a", "<big><b>\343\205\244\343\205\244Thanks\343\205\244", TRUE, "Thanks");
    /* no text: the key in words; WE's own keys in English */
    expect ("bool", "showSeconds", NULL, TRUE, "Show seconds");
    expect ("slider", "rain_amount", "", TRUE, "Rain amount");
    expect ("slider", "x", "ui_browse_properties_playback_rate", TRUE, "Playback rate");
    expect ("combo", "x", "ui_browse_properties_alignment", TRUE, "Alignment");
    expect ("bool", "x", "ui_my_own_key", TRUE, "My own key");
    {
        JsonObject *d = mkdef ("color", "ui_browse_properties_scheme_color", TRUE);
        char *got = walpe_props_label_preview (d, "schemecolor", NULL);
        if (!g_str_has_prefix (got, "Scheme color\n")) {
            fails++;
            fprintf (stderr, "FAIL scheme color: [%s]\n", got);
        }
        g_free (got);
        json_object_unref (d);
    }
    /* conditions (WE "condition": a small JS expression over other options) */
    {
        JsonObject *v = json_object_new ();
        json_object_set_boolean_member (v, "on", TRUE);
        json_object_set_boolean_member (v, "off", FALSE);
        json_object_set_string_member (v, "combo", "2");
        json_object_set_int_member (v, "num", 3);
        json_object_set_string_member (v, "empty", "");
        json_object_set_string_member (v, "name", "custom");
        static const struct { const char *e; gboolean want; } cs[] = {
            { "on.value", TRUE }, { "off.value", FALSE }, { "!off.value", TRUE }, { "", TRUE },
            { "on.value && off.value", FALSE }, { "on.value || off.value", TRUE },
            { "combo.value == 2", TRUE }, { "combo.value==1", FALSE }, { "combo.value != 17", TRUE },
            { "combo.value == \"2\"", TRUE }, { "combo.value === 2", FALSE }, { "num.value >= 3", TRUE },
            { "num.value < 3", FALSE }, { "on.value == true", TRUE }, { "on .value == true", TRUE },
            { "empty.value != ''", FALSE }, { "name.value == 'custom'", TRUE },
            { "(on.value && combo.value == 1 && name.value == 'custom') || empty.value != ''", FALSE },
            { "on.value && (off.value || num.value == 3)", TRUE },
            { "on.value && false", FALSE }, { "false && on.value && combo.value == 1", FALSE },
            { "missing.value", FALSE }, { "!missing.value", TRUE },
            /* ternary with a relabelling assignment (1509243786) */
            { "combo.value == 2 ? on.value ? on.text = 'ui_enable' : on.text = 'ui_disable' : false", TRUE },
            { "combo.value == 1 ? on.value ? on.text = 'ui_enable' : on.text = 'ui_disable' : false", FALSE },
            { "off.value ? 1 : 0", FALSE },
            /* what cannot be parsed never hides an option */
            { "on.value && ds+on.value", TRUE }, { "on.value &&& x", TRUE }, { "combo.value.length > 1", TRUE },
        };
        for (guint i = 0; i < G_N_ELEMENTS (cs); i++)
            if (walpe_props_condition (cs[i].e, v) != cs[i].want) {
                fails++;
                fprintf (stderr, "FAIL condition [%s]: want %d\n", cs[i].e, cs[i].want);
            }
        /* nesting is bounded (a condition of 20 000 parentheses overflowed
         * the stack of the GUI): 40 levels still evaluate, beyond the bound
         * the row is shown like any other condition that cannot be read */
        {
            GString *d = g_string_new (NULL);
            for (int k = 0; k < 40; k++)
                g_string_append_c (d, '(');
            g_string_append (d, "off.value");
            for (int k = 0; k < 40; k++)
                g_string_append_c (d, ')');
            if (walpe_props_condition (d->str, v) != FALSE) {
                fails++;
                fprintf (stderr, "FAIL condition: 40 nested parentheses\n");
            }
            g_string_truncate (d, 0);
            for (int k = 0; k < 2000; k++)
                g_string_append_c (d, k % 2 ? '!' : '(');
            g_string_append (d, "off.value");
            if (walpe_props_condition (d->str, v) != TRUE) {
                fails++;
                fprintf (stderr, "FAIL condition: 2000 nested levels must show the row\n");
            }
            g_string_truncate (d, 0);
            for (int k = 0; k < 200000; k++)
                g_string_append_c (d, '(');
            g_string_append (d, "off.value");
            if (walpe_props_condition (d->str, v) != TRUE) {
                fails++;
                fprintf (stderr, "FAIL condition: 200000 parentheses must show the row\n");
            }
            g_string_free (d, TRUE);
        }
        json_object_unref (v);
    }
    /* file options without a "value" are controls too */
    expect ("file", "image", "Custom picture", FALSE, "Custom picture");
    expect ("directory", "dir", "ui_img_dir", FALSE, "Img dir");

    /* The end of a tag, an entity or a BBCode block that never comes: the
     * search for it must not start over at every character. A megabyte of
     * each of these used to take minutes (the time grew with the square of
     * the length); the whole label is read in one pass now. The limit is
     * processor time of this process, far above what one pass needs (the
     * help texts of <abbr> become a line each: half the size for that one,
     * a sanitizer build is slow with tens of thousands of lines). */
    {
        static const char *const unit[] = { "&", "<x", "[b=", "<abbr title=x>", "<script>",
                                            "<style>", "[img]", "<", "[" };
        for (guint i = 0; i < G_N_ELEMENTS (unit) && !fails; i++) {
            gsize ul = strlen (unit[i]), n = ((i == 3 ? 1u << 19 : 1u << 20)) / ul;
            char *text = g_malloc (n * ul + 1);
            for (gsize k = 0; k < n; k++)
                memcpy (text + k * ul, unit[i], ul);
            text[n * ul] = 0;
            JsonObject *d = mkdef ("bool", text, TRUE);
            clock_t c0 = clock ();
            char *got = walpe_props_label_preview (d, "a", NULL);
            double cpu = (double) (clock () - c0) / CLOCKS_PER_SEC;
            if (cpu > 5.0) {
                fails++;
                fprintf (stderr, "FAIL a label of %zu x \"%s\" took %.1f s of processor time\n",
                         n, unit[i], cpu);
            }
            g_free (got);
            json_object_unref (d);
            g_free (text);
        }
        /* and the answers did not change with the way of searching */
        expect ("bool", "a", "A &amp B; C &#65; &#x42; &bogus; &", TRUE, "A &amp B; C A B &bogus; &");
        expect ("bool", "a", "[b]Bold[/b] [nope] [b" , TRUE, "Bold [nope] [b");
        expect ("bool", "a", "x <abbr title=\"help\">[i]</abbr> y <abbr title=\"more\">z", TRUE,
                "x y z\nhelp\nmore");
        expect ("bool", "a", "a<script>var x = '<b>';</script>b<style>p{}</style>c<script>d", TRUE, "abcd");
        expect ("bool", "a", "[img]http://example.org/a.png[/img]Label[img]http://example.org/b.png", TRUE,
                "Labelhttp://example.org/b.png");
        expect ("bool", "a", "a <unknown b> c <br d", TRUE, "a c");
    }

    if (fails) {
        fprintf (stderr, "proplabel: %d case(s) failed\n", fails);
        return 1;
    }
    printf ("proplabel: all cases pass\n");
    return 0;
}
