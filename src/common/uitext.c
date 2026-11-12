/* uitext.c - see uitext.h */
#include "uitext.h"
#include <string.h>

static gboolean is_direction_control (gunichar c)
{
    return c == 0x061c || c == 0x200e || c == 0x200f || (c >= 0x202a && c <= 0x202e) ||
           (c >= 0x2066 && c <= 0x2069);
}

char *walpe_ui_text (const char *s, glong max_chars, WalpeUiTextMode mode)
{
    if (!s || max_chars < 1)
        return g_strdup ("");
    gboolean para = mode == WALPE_UITEXT_PARAGRAPHS;
    /* never walk more of the input than can end up in the result: every
     * character kept is at most 6 bytes, dropped ones are bounded too */
    gsize in_len = strlen (s);
    gsize budget = (gsize) max_chars * 8 + 64;
    char *valid = g_utf8_make_valid (s, (gssize) MIN (in_len, budget));
    GString *o = g_string_sized_new (MIN (in_len, (gsize) 256));
    glong n = 0, run = 0;
    gboolean cut = in_len > budget;
    for (const char *p = valid; *p; p = g_utf8_next_char (p)) {
        gunichar c = g_utf8_get_char (p);
        if (c == '\r')
            continue;
        if (c == 0x2028 || c == 0x2029)     /* line / paragraph separator */
            c = '\n';
        if (c == '\t' || (c == '\n' && !para))
            c = ' ';
        if ((c < 0x20 && c != '\n') || (c >= 0x7f && c < 0xa0) || c == 0xfeff)
            continue;
        if (!para && is_direction_control (c))
            continue;
        if (n >= max_chars) {
            cut = TRUE;
            break;
        }
        if (para) {
            if (g_unichar_isspace (c) || g_unichar_iswide (c) || c == 0x200b) {
                run = 0;
            } else if (++run > WALPE_UITEXT_RUN) {
                g_string_append (o, "\xe2\x80\x8b");
                run = 1;
            }
        }
        g_string_append_unichar (o, c);
        n++;
    }
    g_free (valid);
    if (cut) {
        /* no half-finished line or trailing blanks before the mark */
        while (o->len && (o->str[o->len - 1] == ' ' || o->str[o->len - 1] == '\n'))
            g_string_truncate (o, o->len - 1);
        g_string_append (o, "…");
    }
    return g_string_free (o, FALSE);
}

/* a combining diacritical mark: what turns a Latin, Greek or Cyrillic
 * letter into its accented form after decomposition */
static gboolean is_diacritic (gunichar c)
{
    return (c >= 0x0300 && c <= 0x036f) || (c >= 0x1ab0 && c <= 0x1aff) ||
           (c >= 0x1dc0 && c <= 0x1dff) || (c >= 0x20d0 && c <= 0x20ff) ||
           (c >= 0xfe20 && c <= 0xfe2f);
}

char *walpe_ui_search_fold (const char *s)
{
    if (!s)
        return g_strdup ("");
    char *valid = g_utf8_validate (s, -1, NULL) ? NULL : g_utf8_make_valid (s, -1);
    char *n = g_utf8_normalize (valid ? valid : s, -1, G_NORMALIZE_ALL);
    GString *o = g_string_sized_new (n ? strlen (n) : 0);
    for (const char *p = n ? n : ""; *p; p = g_utf8_next_char (p)) {
        gunichar c = g_utf8_get_char (p);
        if (!is_diacritic (c))
            g_string_append_unichar (o, c);
    }
    g_free (n);
    g_free (valid);
    char *f = g_utf8_casefold (o->str, (gssize) o->len);
    g_string_truncate (o, 0);
    for (const char *p = f; *p; p = g_utf8_next_char (p)) {
        gunichar c = g_utf8_get_char (p);
        g_string_append_unichar (o, c == 0x0131 ? 'i' : c);     /* dotless i */
    }
    g_free (f);
    return g_string_free (o, FALSE);
}
