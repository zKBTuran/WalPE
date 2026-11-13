/* outputs_test.c - the daemon's bookkeeping of monitors across changes
 * (platform/outputs.c and the layout decision of platform.h): comparing
 * and copying output lists, finding an output by its name or by the
 * canonical form of it, and what the daemon decides when the window
 * system announces a change - walked through plugging a monitor in,
 * pulling it, a passing state, a mode change, a surface taken away.
 * No display, no daemon: the lists are made up here. */
#include <glib.h>
#include <string.h>
#include "platform/impl.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; g_printerr ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

typedef struct { const char *name; int x, y, w, h; double hz; double scale; int transform; } Spec;

static void make (WalpeOutputList *l, const Spec *s, int n)
{
    memset (l, 0, sizeof (*l));
    l->n = n;
    l->o = g_new0 (WalpeOutput, n + 1);
    for (int i = 0; i < n; i++) {
        WalpeOutput *o = &l->o[i];
        o->id = (guint32) i + 1;
        walpe_output_set_names (o, s[i].name, NULL, NULL, NULL, NULL);
        o->x = o->logical_x = s[i].x;
        o->y = o->logical_y = s[i].y;
        o->width = o->logical_width = s[i].w;
        o->height = o->logical_height = s[i].h;
        o->refresh_hz = s[i].hz;
        o->scale = s[i].scale > 0 ? s[i].scale : 1.0;
        o->transform = s[i].transform;
    }
}

static const Spec one[] = { { "DP-1", 0, 0, 1920, 1080, 144.0, 1.0, 0 } };
static const Spec two[] = { { "DP-1", 0, 0, 1920, 1080, 144.0, 1.0, 0 },
                            { "HDMI-A-1", 1920, 0, 1920, 1080, 60.0, 1.0, 0 } };

static void test_equal_copy (void)
{
    WalpeOutputList a, b, c;
    make (&a, two, 2);
    make (&b, two, 2);
    CHECK (walpe_output_list_equal (&a, &b) && walpe_output_list_equal (&b, &a));
    walpe_output_list_copy (&c, &a);
    CHECK (walpe_output_list_equal (&a, &c));
    CHECK (c.o[0].name != a.o[0].name && !strcmp (c.o[1].name, "HDMI-A-1"));    /* its own strings */
    CHECK (!strcmp (c.o[1].canon, "HDMI-1") && c.o[1].id == 2);
    /* every field that makes a layout another layout */
    b.o[1].x = 1921;
    CHECK (!walpe_output_list_equal (&a, &b));
    b.o[1].x = 1920;
    b.o[1].height = 1200;
    CHECK (!walpe_output_list_equal (&a, &b));
    b.o[1].height = 1080;
    b.o[1].refresh_hz = 59.94;                  /* the same mode, read slightly differently */
    CHECK (walpe_output_list_equal (&a, &b));
    b.o[1].refresh_hz = 75.0;
    CHECK (!walpe_output_list_equal (&a, &b));
    b.o[1].refresh_hz = 60.0;
    b.o[1].scale = 1.5;
    CHECK (!walpe_output_list_equal (&a, &b));
    b.o[1].scale = 1.0;
    b.o[1].transform = WALPE_TRANSFORM_90;
    CHECK (!walpe_output_list_equal (&a, &b));
    b.o[1].transform = 0;
    walpe_output_set_names (&b.o[1], "HDMI-A-2", NULL, NULL, NULL, NULL);
    CHECK (!walpe_output_list_equal (&a, &b));  /* another connector, same rectangle */
    walpe_output_set_names (&b.o[1], "HDMI-A-1", "ACME", "Panel 27", NULL, "ACME Panel 27 (HDMI-A-1)");
    CHECK (walpe_output_list_equal (&a, &b));   /* what a monitor says about itself is no layout */
    /* the registry id is not part of a layout either: a restarted
     * compositor numbers its outputs anew */
    b.o[0].id = 77;
    CHECK (walpe_output_list_equal (&a, &b));
    /* order matters: the index names the monitor (--monitor N, "monN") */
    WalpeOutput t = b.o[0];
    b.o[0] = b.o[1];
    b.o[1] = t;
    CHECK (!walpe_output_list_equal (&a, &b));
    /* empty lists */
    WalpeOutputList e1 = { 0 }, e2 = { 0 };
    CHECK (walpe_output_list_equal (&e1, &e2) && !walpe_output_list_equal (&a, &e1));
    walpe_output_list_free (&a);
    walpe_output_list_free (&b);
    walpe_output_list_free (&c);
    CHECK (a.n == 0 && a.o == NULL);
    walpe_output_list_free (&a);                /* twice is fine */
}

static void test_find (void)
{
    WalpeOutputList a;
    make (&a, two, 2);
    CHECK (walpe_output_list_find (&a, "DP-1") == &a.o[0]);
    CHECK (walpe_output_list_find (&a, "HDMI-A-1") == &a.o[1]);
    /* a setting saved under X11's spelling finds the Wayland output */
    CHECK (walpe_output_list_find (&a, "HDMI-1") == &a.o[1]);
    CHECK (walpe_output_list_find (&a, "hdmi-a-1") == &a.o[1]);
    CHECK (walpe_output_list_find (&a, "DP-2") == NULL);
    CHECK (walpe_output_list_find (&a, "DP-0") == NULL);    /* another numbering is another name */
    CHECK (walpe_output_list_find (&a, NULL) == NULL);
    CHECK (walpe_output_list_find (&a, "") == NULL);
    /* "the output" without a name (a provider that was told none): the
     * only one of a list, nobody's when there are several or none */
    WalpeOutputList single, none = { 0 };
    make (&single, one, 1);
    single.o[0].id = 37;                    /* a Wayland registry name, not an index */
    CHECK (walpe_output_list_only_id (&single) == 37);
    CHECK (walpe_output_list_only_id (&a) == 0);        /* two outputs */
    CHECK (walpe_output_list_only_id (&none) == 0);     /* every output is gone */
    CHECK (walpe_output_list_only_id (NULL) == 0);
    walpe_output_list_free (&single);
    CHECK (walpe_output_list_only_id (&single) == 0);   /* freed: empty */
    /* the exact name wins over a canonical match further up the list */
    static const Spec odd[] = { { "HDMI-A-1", 0, 0, 800, 600, 60.0, 1.0, 0 },
                                { "HDMI-1", 800, 0, 800, 600, 60.0, 1.0, 0 } };
    WalpeOutputList b;
    make (&b, odd, 2);
    CHECK (walpe_output_list_find (&b, "HDMI-1") == &b.o[1]);
    CHECK (walpe_output_list_find (&b, "HDMI-A-1") == &b.o[0]);
    walpe_output_list_free (&a);
    walpe_output_list_free (&b);
}

/* names and descriptions are kept as text that can be a key of config.json
 * and a part of a status reply, whatever bytes the window system sent */
static void test_texts (void)
{
    WalpeOutput o;
    memset (&o, 0, sizeof o);
    walpe_output_set_names (&o, "HDMI-A-1", "ACME", "Panel 27", "0001", "ACME Panel 27");
    CHECK (!strcmp (o.name, "HDMI-A-1") && !strcmp (o.canon, "HDMI-1") && !strcmp (o.make, "ACME") &&
           !strcmp (o.model, "Panel 27") && !strcmp (o.serial, "0001") &&
           !strcmp (o.description, "ACME Panel 27"));
    /* bytes that are no UTF-8, a line break, an escape character */
    walpe_output_set_names (&o, "DP\xff-1\n\x1b[31m", "", NULL, NULL, "two\nlines");
    CHECK (g_utf8_validate (o.name, -1, NULL) && !strchr (o.name, '\n') && !strchr (o.name, 0x1b));
    CHECK (g_str_has_prefix (o.name, "DP") && strstr (o.name, "-1?"));
    CHECK (g_utf8_validate (o.canon, -1, NULL));
    CHECK (o.make == NULL && o.model == NULL && o.serial == NULL);
    CHECK (!strcmp (o.description, "two?lines"));
    /* the record's own strings as arguments */
    walpe_output_set_names (&o, o.name, o.make, o.model, o.serial, o.description);
    CHECK (g_str_has_prefix (o.name, "DP") && !strcmp (o.description, "two?lines"));
    /* a very long name is cut between two characters */
    char *big = g_malloc (3000 + 1);
    for (int i = 0; i < 3000; i += 3)
        memcpy (big + i, "\xe2\x82\xac", 3);     /* U+20AC, three bytes */
    big[3000] = 0;
    walpe_output_set_names (&o, big, NULL, NULL, NULL, big);
    CHECK (strlen (o.name) <= 255 && strlen (o.name) % 3 == 0 && g_utf8_validate (o.name, -1, NULL));
    CHECK (strlen (o.description) <= 1023 && g_utf8_validate (o.description, -1, NULL));
    g_free (big);
    walpe_output_set_names (&o, NULL, NULL, NULL, NULL, NULL);
    CHECK (!strcmp (o.name, "") && !strcmp (o.canon, "") && o.description == NULL);
    g_free (o.name);
    g_free (o.canon);
}

/* one look of the daemon at the outputs, as layout_check does it: returns
 * the verdict and keeps `pending` as the daemon would */
static WalpeLayoutVerdict look (const WalpeOutputList *running, WalpeOutputList *pending,
                                const Spec *now, int n, gboolean force, gboolean too_soon,
                                gboolean busy)
{
    WalpeOutputList seen;
    make (&seen, now, n);
    gboolean settled = FALSE;
    WalpeLayoutVerdict v = walpe_layout_verdict (running, &seen, pending, force, too_soon, busy,
                                                 &settled);
    if (v == WALPE_LAYOUT_SAME) {
        walpe_output_list_free (pending);
        walpe_output_list_free (&seen);
    } else if (v == WALPE_LAYOUT_WAIT) {
        walpe_output_list_free (pending);
        *pending = seen;
    } else {
        CHECK (settled);
        walpe_output_list_free (&seen);
    }
    return v;
}

static void test_hotplug (void)
{
    WalpeOutputList running, pending = { 0 };
    make (&running, one, 1);

    /* nothing changed (an event that meant nothing) */
    CHECK (look (&running, &pending, one, 1, FALSE, FALSE, FALSE) == WALPE_LAYOUT_SAME);
    CHECK (pending.n == 0);

    /* a second monitor is plugged in: seen once = wait, seen again = restart */
    CHECK (look (&running, &pending, two, 2, FALSE, FALSE, FALSE) == WALPE_LAYOUT_WAIT);
    CHECK (pending.n == 2);
    CHECK (look (&running, &pending, two, 2, FALSE, FALSE, FALSE) == WALPE_LAYOUT_RESTART);
    walpe_output_list_free (&pending);

    /* a passing state: the new layout shows once, then the old one is back */
    CHECK (look (&running, &pending, two, 2, FALSE, FALSE, FALSE) == WALPE_LAYOUT_WAIT);
    CHECK (look (&running, &pending, one, 1, FALSE, FALSE, FALSE) == WALPE_LAYOUT_SAME);
    CHECK (pending.n == 0);

    /* the layout keeps changing: each new state starts the wait again */
    static const Spec moved[] = { { "DP-1", 0, 0, 1920, 1080, 144.0, 1.0, 0 },
                                  { "HDMI-A-1", 0, 1080, 1920, 1080, 60.0, 1.0, 0 } };
    CHECK (look (&running, &pending, two, 2, FALSE, FALSE, FALSE) == WALPE_LAYOUT_WAIT);
    CHECK (look (&running, &pending, moved, 2, FALSE, FALSE, FALSE) == WALPE_LAYOUT_WAIT);
    CHECK (look (&running, &pending, moved, 2, FALSE, FALSE, FALSE) == WALPE_LAYOUT_RESTART);
    walpe_output_list_free (&pending);

    /* a restart was a moment ago, or an import runs: it waits, however
     * often the new layout is seen, and goes when that is over */
    CHECK (look (&running, &pending, two, 2, FALSE, TRUE, FALSE) == WALPE_LAYOUT_WAIT);
    CHECK (look (&running, &pending, two, 2, FALSE, TRUE, FALSE) == WALPE_LAYOUT_WAIT);
    CHECK (look (&running, &pending, two, 2, FALSE, FALSE, TRUE) == WALPE_LAYOUT_WAIT);
    CHECK (look (&running, &pending, two, 2, FALSE, FALSE, FALSE) == WALPE_LAYOUT_RESTART);
    walpe_output_list_free (&pending);
    walpe_output_list_free (&running);

    /* the other way round: running on two, one is pulled */
    make (&running, two, 2);
    CHECK (look (&running, &pending, one, 1, FALSE, FALSE, FALSE) == WALPE_LAYOUT_WAIT);
    CHECK (look (&running, &pending, one, 1, FALSE, FALSE, FALSE) == WALPE_LAYOUT_RESTART);
    walpe_output_list_free (&pending);

    /* every output is gone (a laptop lid on Wayland, all monitors off):
     * there is nothing to restart for - it waits until one is back */
    CHECK (look (&running, &pending, NULL, 0, FALSE, FALSE, FALSE) == WALPE_LAYOUT_WAIT);
    CHECK (look (&running, &pending, NULL, 0, FALSE, FALSE, FALSE) == WALPE_LAYOUT_WAIT);
    CHECK (look (&running, &pending, NULL, 0, TRUE, FALSE, FALSE) == WALPE_LAYOUT_WAIT);
    /* ... the same two come back: nothing to do */
    CHECK (look (&running, &pending, two, 2, FALSE, FALSE, FALSE) == WALPE_LAYOUT_SAME);

    /* a mode or scale change of one output is a layout change */
    static const Spec scaled[] = { { "DP-1", 0, 0, 1920, 1080, 144.0, 1.0, 0 },
                                   { "HDMI-A-1", 1920, 0, 1920, 1080, 60.0, 2.0, 0 } };
    CHECK (look (&running, &pending, scaled, 2, FALSE, FALSE, FALSE) == WALPE_LAYOUT_WAIT);
    CHECK (look (&running, &pending, scaled, 2, FALSE, FALSE, FALSE) == WALPE_LAYOUT_RESTART);
    walpe_output_list_free (&pending);

    /* a surface was taken away while the list reads the same: the layout
     * on screen is not the daemon's any more - restart once it holds */
    CHECK (look (&running, &pending, two, 2, TRUE, FALSE, FALSE) == WALPE_LAYOUT_WAIT);
    CHECK (look (&running, &pending, two, 2, TRUE, FALSE, FALSE) == WALPE_LAYOUT_RESTART);
    walpe_output_list_free (&pending);
    walpe_output_list_free (&running);
}

/* what became of each output between two lists (walpe_follow_plan): the
 * daemon keeps a monitor's surface while its output is the same one */
static void test_follow_plan (void)
{
    WalpeOutputList run, seen;
    WalpeFollowPlan p;
    make (&run, two, 2);

    /* nothing changed */
    make (&seen, two, 2);
    walpe_follow_plan (&run, &seen, &p);
    CHECK (p.n_running == 2 && p.n_seen == 2 && p.kept == 2 && !p.placed && !p.resized && !p.gone && !p.added);
    CHECK (p.running[0] == WALPE_FOLLOW_KEEP && p.running_to[1] == 1 && p.seen_from[1] == 1);
    walpe_follow_plan_clear (&p);

    /* the second output moves below the first, gets another scale, another
     * refresh rate, is turned upside down: the same pixels */
    seen.o[1].x = 0;
    seen.o[1].y = 1080;
    walpe_follow_plan (&run, &seen, &p);
    CHECK (p.kept == 1 && p.placed == 1 && p.running[1] == WALPE_FOLLOW_PLACE && p.seen[1] == WALPE_FOLLOW_PLACE);
    walpe_follow_plan_clear (&p);
    seen.o[1].x = 1920;
    seen.o[1].y = 0;
    seen.o[1].scale = 1.5;
    walpe_follow_plan (&run, &seen, &p);
    CHECK (p.placed == 1 && p.running[1] == WALPE_FOLLOW_PLACE);
    walpe_follow_plan_clear (&p);
    seen.o[1].scale = 1.0;
    seen.o[1].refresh_hz = 59.94;               /* the same mode, read slightly differently */
    walpe_follow_plan (&run, &seen, &p);
    CHECK (p.kept == 2);
    walpe_follow_plan_clear (&p);
    seen.o[1].refresh_hz = 75.0;
    walpe_follow_plan (&run, &seen, &p);
    CHECK (p.placed == 1);
    walpe_follow_plan_clear (&p);
    seen.o[1].refresh_hz = 60.0;
    seen.o[1].transform = WALPE_TRANSFORM_180;
    walpe_follow_plan (&run, &seen, &p);
    CHECK (p.placed == 1 && !p.resized);
    walpe_follow_plan_clear (&p);
    seen.o[1].transform = 0;

    /* another mode: another size in pixels (also when it moved besides) */
    seen.o[1].width = 2560;
    seen.o[1].height = 1440;
    seen.o[1].x = 0;
    walpe_follow_plan (&run, &seen, &p);
    CHECK (p.kept == 1 && p.resized == 1 && !p.placed && p.running[1] == WALPE_FOLLOW_RESIZE &&
           p.seen[1] == WALPE_FOLLOW_RESIZE && p.running_to[1] == 1);
    walpe_follow_plan_clear (&p);
    walpe_output_list_free (&seen);

    /* the lists are in the order of the layout: the outputs swap places */
    static const Spec swapped[] = { { "HDMI-A-1", 0, 0, 1920, 1080, 60.0, 1.0, 0 },
                                    { "DP-1", 1920, 0, 1920, 1080, 144.0, 1.0, 0 } };
    make (&seen, swapped, 2);
    seen.o[0].id = 2;
    seen.o[1].id = 1;
    walpe_follow_plan (&run, &seen, &p);
    CHECK (p.placed == 2 && p.running_to[0] == 1 && p.running_to[1] == 0 && p.seen_from[0] == 1);
    walpe_follow_plan_clear (&p);
    walpe_output_list_free (&seen);

    /* the second output is pulled */
    make (&seen, one, 1);
    walpe_follow_plan (&run, &seen, &p);
    CHECK (p.kept == 1 && p.gone == 1 && !p.added && p.running[1] == WALPE_FOLLOW_GONE && p.running_to[1] == -1);
    walpe_follow_plan_clear (&p);
    /* ... and every output */
    seen.n = 0;
    walpe_follow_plan (&run, &seen, &p);
    CHECK (p.gone == 2 && p.n_seen == 0);
    walpe_follow_plan_clear (&p);
    seen.n = 1;
    walpe_output_list_free (&seen);

    /* a third one is plugged in */
    static const Spec three[] = { { "DP-1", 0, 0, 1920, 1080, 144.0, 1.0, 0 },
                                  { "HDMI-A-1", 1920, 0, 1920, 1080, 60.0, 1.0, 0 },
                                  { "DP-2", 3840, 0, 1280, 720, 60.0, 1.0, 0 } };
    make (&seen, three, 3);
    walpe_follow_plan (&run, &seen, &p);
    CHECK (p.kept == 2 && p.added == 1 && p.seen[2] == WALPE_FOLLOW_NEW && p.seen_from[2] == -1);
    walpe_follow_plan_clear (&p);

    /* an output that was switched off and on again is another output with
     * the old name (the window system gave it a new id): one gone, one new,
     * not "the same one" - its surface went with the old one */
    walpe_output_list_free (&seen);
    make (&seen, two, 2);
    seen.o[1].id = 9;
    walpe_follow_plan (&run, &seen, &p);
    CHECK (p.kept == 1 && p.gone == 1 && p.added == 1 && p.running[1] == WALPE_FOLLOW_GONE &&
           p.seen[1] == WALPE_FOLLOW_NEW);
    walpe_follow_plan_clear (&p);
    /* ... and an id that is used again for an output of another name too */
    seen.o[1].id = 2;
    walpe_output_set_names (&seen.o[1], "HDMI-A-2", NULL, NULL, NULL, NULL);
    walpe_follow_plan (&run, &seen, &p);
    CHECK (p.gone == 1 && p.added == 1);
    walpe_follow_plan_clear (&p);
    walpe_output_list_free (&seen);
    walpe_output_list_free (&run);
}

int main (void)
{
    test_equal_copy ();
    test_find ();
    test_texts ();
    test_hotplug ();
    test_follow_plan ();
    if (fails)
        g_printerr ("outputs-test: %d check(s) failed\n", fails);
    else
        g_print ("outputs-test: ok\n");
    return fails ? 1 : 0;
}
