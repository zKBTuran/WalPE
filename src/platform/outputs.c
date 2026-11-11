/* outputs.c - output lists (platform.h): copying, comparing, finding by
 * name. No window system: the daemon's layout check and the tests use
 * these on lists a backend filled in or a test made up. */
#include "platform/impl.h"
#include <math.h>
#include <string.h>

/* A text the window system handed over (a connector name, what a monitor
 * says about itself) as the daemon keeps it. An X atom name or a string of
 * a Wayland compositor can hold any bytes, and these texts become keys of
 * config.json (a file with a key that is not UTF-8 cannot be read back),
 * parts of `status` replies and of log lines. So: valid UTF-8 (bytes that
 * are none become U+FFFD), no control characters ('?' instead: a line
 * break in a name would start a log line of its own), at most max bytes,
 * cut between two characters. A connector name as the drivers spell them
 * ("DP-1", "HDMI-A-1") passes unchanged. NULL or "": NULL. */
#define OUTPUT_NAME_MAX 255
#define OUTPUT_TEXT_MAX 1023

static char *output_text (const char *s, gsize max)
{
    if (!s || !*s)
        return NULL;
    char *t = g_utf8_validate (s, -1, NULL) ? g_strdup (s) : g_utf8_make_valid (s, -1);
    gsize len = strlen (t);
    if (len > max) {
        len = max;
        while (len > 0 && ((guchar) t[len] & 0xC0) == 0x80)
            len--;              /* not inside a character */
        t[len] = 0;
    }
    for (char *p = t; *p; p++)
        if ((guchar) *p < 0x20 || *p == 0x7f)
            *p = '?';
    return t;
}

void walpe_output_set_names (WalpeOutput *o, const char *name, const char *make, const char *model,
                             const char *serial, const char *description)
{
    /* (copied before anything is freed: an argument may be one of the record's
     * own strings) */
    char *n = output_text (name, OUTPUT_NAME_MAX);
    char *mk = output_text (make, OUTPUT_TEXT_MAX), *md = output_text (model, OUTPUT_TEXT_MAX);
    char *sn = output_text (serial, OUTPUT_TEXT_MAX);
    char *ds = output_text (description, OUTPUT_TEXT_MAX);
    g_free (o->name);
    g_free (o->canon);
    g_free (o->make);
    g_free (o->model);
    g_free (o->serial);
    g_free (o->description);
    o->name = n ? n : g_strdup ("");
    o->canon = walpe_output_name_canon (o->name);
    o->make = mk;
    o->model = md;
    o->serial = sn;
    o->description = ds;
}

void walpe_output_list_free (WalpeOutputList *l)
{
    for (int i = 0; l->o && i < l->n; i++) {
        g_free (l->o[i].name);
        g_free (l->o[i].canon);
        g_free (l->o[i].make);
        g_free (l->o[i].model);
        g_free (l->o[i].serial);
        g_free (l->o[i].description);
    }
    g_free (l->o);
    memset (l, 0, sizeof (*l));
}

void walpe_output_list_copy (WalpeOutputList *dst, const WalpeOutputList *src)
{
    memset (dst, 0, sizeof (*dst));
    dst->n = src->n;
    dst->o = g_new0 (WalpeOutput, src->n + 1);
    for (int i = 0; i < src->n; i++) {
        dst->o[i] = src->o[i];
        dst->o[i].name = dst->o[i].canon = dst->o[i].make = dst->o[i].model = NULL;
        dst->o[i].serial = dst->o[i].description = NULL;
        walpe_output_set_names (&dst->o[i], src->o[i].name, src->o[i].make, src->o[i].model,
                                src->o[i].serial, src->o[i].description);
    }
}

gboolean walpe_output_list_equal (const WalpeOutputList *a, const WalpeOutputList *b)
{
    if (a->n != b->n)
        return FALSE;
    for (int i = 0; i < a->n; i++) {
        const WalpeOutput *x = &a->o[i], *y = &b->o[i];
        if (x->x != y->x || x->y != y->y || x->width != y->width || x->height != y->height ||
            g_strcmp0 (x->name, y->name) || fabs (x->refresh_hz - y->refresh_hz) > 0.5 ||
            fabs (x->scale - y->scale) > 0.001 || x->transform != y->transform)
            return FALSE;
    }
    return TRUE;
}

guint32 walpe_output_list_only_id (const WalpeOutputList *l)
{
    return l && l->n == 1 && l->o ? l->o[0].id : 0;
}

const WalpeOutput *walpe_output_list_find (const WalpeOutputList *l, const char *name)
{
    if (!name)
        return NULL;
    for (int i = 0; i < l->n; i++)
        if (!g_strcmp0 (l->o[i].name, name))
            return &l->o[i];
    char *canon = walpe_output_name_canon (name);
    const WalpeOutput *hit = NULL;
    for (int i = 0; i < l->n && !hit; i++)
        if (!g_strcmp0 (l->o[i].canon, canon))
            hit = &l->o[i];
    g_free (canon);
    return hit;
}
