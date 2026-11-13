/* diagpath.h - no directory of the user's in the text of `walpe-ctl diag`.
 *
 * Not a public header: daemon/diag.h includes it, and tests/diagpath_test.c
 * runs it on made-up sentences. The summary is made to be posted. It names
 * a wallpaper by the last part of its path and writes the home directory as
 * "~" - but a monitor's line also quotes the reason a load failed, and that
 * sentence comes from the loaders, with the paths they were given: a
 * wallpaper opened from /mnt/..., /run/media/<name>/... or /tmp was printed
 * with every folder above it. Three steps, in this order, on the finished
 * text (pure text work; called when the summary is asked for, nothing
 * else):
 *
 *   walpe_diag_no_folders ()   the folders the daemon knows - the one each
 *                              wallpaper lies in, the library, the Wallpaper
 *                              Engine assets - are taken out wherever they
 *                              stand, so what lies below is named from
 *                              there on: "noscene/scene.json". By the exact
 *                              text: spaces in a folder's name do no harm.
 *   walpe_diag_no_home ()      the home directory becomes "~" (as before;
 *                              what lies below it stays as it is)
 *   walpe_diag_short_paths ()  whatever is still an absolute path of more
 *                              than one part becomes ".../<last part>"
 *
 * The last step finds a path by its shape: from a "/" at the start of a
 * word to the next blank (inside quotes: to the closing quote). A path with
 * a blank in it that is not below a known folder and not quoted is cut at
 * the blank, and what follows the blank stays. */
#pragma once
#include <glib.h>
#include <string.h>

static inline gboolean walpe_diag_name_char (char c)
{
    return g_ascii_isalnum (c) || c == '_' || c == '-' || c == '.' || (guchar) c >= 0x80;
}

/* does a path begin at s[at], or is that the middle of a longer one
 * ("/home/al" inside "/mnt/backup/home/al")? */
static inline gboolean walpe_diag_path_start (const GString *s, gsize at)
{
    return at == 0 || (!walpe_diag_name_char (s->str[at - 1]) && s->str[at - 1] != '/');
}

/* every "<home>" that is a whole path or the start of one becomes "~"
 * ("/home/al" is not the start of "/home/alice") */
static inline void walpe_diag_no_home (GString *s, const char *home)
{
    const gsize hl = home ? strlen (home) : 0;
    if (hl < 2)
        return;
    for (gsize at = 0; at + hl <= s->len;) {
        const char *p = strstr (s->str + at, home);
        if (!p)
            break;
        at = (gsize) (p - s->str);
        if (walpe_diag_name_char (s->str[at + hl]) || !walpe_diag_path_start (s, at)) {
            at++;
            continue;
        }
        g_string_erase (s, (gssize) at, (gssize) hl);
        g_string_insert_c (s, (gssize) at, '~');
        at++;
    }
}

static inline int walpe_diag_longer_first (gconstpointer a, gconstpointer b)
{
    const gsize la = strlen (*(const char *const *) a), lb = strlen (*(const char *const *) b);
    return la > lb ? -1 : la < lb;
}

/* folders: absolute paths of directories (char *, without a "/" at the
 * end; the array is sorted here). "<folder>/" goes wherever it stands; a
 * folder named on its own keeps its last part. A folder at or below
 * `home` is left to walpe_diag_no_home (), "/" and "" are skipped. */
static inline void walpe_diag_no_folders (GString *s, GPtrArray *folders, const char *home)
{
    const gsize hl = home ? strlen (home) : 0;
    g_ptr_array_sort (folders, walpe_diag_longer_first);
    for (guint i = 0; i < folders->len; i++) {
        const char *f = g_ptr_array_index (folders, i);
        const gsize fl = f ? strlen (f) : 0;
        if (fl < 2 || f[0] != '/')
            continue;
        if (hl >= 2 && !strncmp (f, home, hl) && (f[hl] == '/' || !f[hl]))
            continue;
        const char *slash = strrchr (f, '/');
        const gsize keep = fl - (gsize) (slash + 1 - f);    /* the length of its last part */
        for (gsize at = 0; at + fl <= s->len;) {
            const char *p = strstr (s->str + at, f);
            if (!p)
                break;
            at = (gsize) (p - s->str);
            const char next = s->str[at + fl];
            if (!walpe_diag_path_start (s, at)) {
                at++;       /* "/data/mnt/walls": the middle of another path */
            } else if (next == '/') {
                g_string_erase (s, (gssize) at, (gssize) fl + 1);
            } else if (!walpe_diag_name_char (next)) {
                g_string_erase (s, (gssize) at, (gssize) (fl - keep));
                at += keep;
            } else {
                at++;       /* "/mnt/walls2": another folder */
            }
        }
    }
}

/* an absolute path of more than one part becomes ".../<last part>" (one
 * part - "/tmp" - says nothing more than its last part and stays). A path
 * begins at a "/" that follows the start of the text, a blank, a quote,
 * "(", "[", "<", "=", "," or ":" and ends before the next blank or one of
 * "'<>|,;)] - right after a quote at the closing quote instead, so a
 * quoted name may hold blanks. "scheme://host/..." is left alone,
 * "file:///a/b" is the path /a/b. */
static inline void walpe_diag_short_paths (GString *s)
{
    for (gsize i = 0; i < s->len; i++) {
        if (s->str[i] != '/')
            continue;
        const char before = i ? s->str[i - 1] : ' ';
        if (!strchr (" \t\n\"'([<=,:", before))
            continue;
        if (before == ':' && i + 1 < s->len && s->str[i + 1] == '/') {
            if (i + 2 < s->len && s->str[i + 2] == '/') {
                i += 2;                 /* file:///a/b */
            } else {
                while (i < s->len && !strchr (" \t\n", s->str[i]))
                    i++;                /* a network address: not a path of this machine */
                continue;
            }
        }
        gsize e = i;
        if (before == '"' || before == '\'') {
            while (e < s->len && s->str[e] != before && s->str[e] != '\n')
                e++;
        } else {
            while (e < s->len && !strchr (" \t\n\"'<>|,;)]", s->str[e]))
                e++;
            while (e > i + 1 && strchr (":.", s->str[e - 1]))
                e--;                    /* the end of a sentence, not of a name */
        }
        gsize last = e;
        while (last > i && s->str[last - 1] != '/')
            last--;                     /* one past the last "/" of the path */
        if (last == e) {
            /* "/a/b/": the last part is in front of the last separator */
            gsize t = e - 1;
            while (t > i && s->str[t - 1] != '/')
                t--;
            last = t;
        }
        if (last <= i + 1) {            /* one part (or "/" alone) */
            i = e ? e - 1 : 0;
            continue;
        }
        g_string_erase (s, (gssize) i, (gssize) (last - 1 - i));
        g_string_insert (s, (gssize) i, "...");
        i += 3 + (e - last);            /* at the end of what is left of it */
    }
}
