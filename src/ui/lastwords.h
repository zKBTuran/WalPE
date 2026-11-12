/* lastwords.h - what the engine said last before it ended.
 *
 * An engine the window starts writes to ~/.cache/walpe/daemon.log, not to
 * a terminal. When it ends at once - no display it can draw on, no
 * OpenGL, a wallpaper that takes it down - the sentence that says why is
 * the last thing in that file, and the window showed "exit status 1" and
 * the file's name. These functions find the sentence in the text the
 * engine wrote since it was started. Pure; tests/ui_session_test.c. */
#pragma once
#include <glib.h>
#include <string.h>

#define WALPE_LAST_WORDS_MAX 300    /* characters quoted in the status line */

/* "12:34:56.789: " - the time GLib puts before a message */
static inline gboolean walpe_last_words_stamp (const char *p)
{
    static const char pat[] = "dd:dd:dd.ddd: ";
    for (int i = 0; pat[i]; i++)
        if (pat[i] == 'd' ? !g_ascii_isdigit (p[i]) : p[i] != pat[i])
            return FALSE;
    return TRUE;
}

/* One line of the engine's output as it can be quoted (g_free), or NULL
 * for a line that says nothing about an end: an empty one, and GLib's
 * lines of the levels message, info and debug, which the engine writes
 * all the time ("** Message: 12:00:00.000: ...",
 * "walpe-daemon-Message: ..."). What GLib puts before a warning, a
 * critical or an error ("** (process:123): WARNING **: 12:00:00.000: ",
 * "(walpe-daemon:123): Gtk-CRITICAL **: ...") and the engine's own
 * "walpe-daemon: " are taken away. */
static inline char *walpe_last_words_line (const char *line)
{
    const char *t = line;
    const char *mark = strstr (line, " **: ");
    if (mark) {
        t = mark + 5;
        if (walpe_last_words_stamp (t))
            t += 14;
    } else {
        static const char *const quiet[] = { "Message: ", "INFO: ", "DEBUG: " };
        for (guint i = 0; i < G_N_ELEMENTS (quiet); i++) {
            const char *q = strstr (line, quiet[i]);
            if (q && q - line < 80 && walpe_last_words_stamp (q + strlen (quiet[i])))
                return NULL;
        }
    }
    if (g_str_has_prefix (t, "walpe-daemon: "))
        t += 14;
    char *s = g_utf8_make_valid (t, -1);
    g_strstrip (s);
    if (!*s) {
        g_free (s);
        return NULL;
    }
    if (g_utf8_strlen (s, -1) > WALPE_LAST_WORDS_MAX) {
        char *cut = g_utf8_substring (s, 0, WALPE_LAST_WORDS_MAX);
        g_free (s);
        s = g_strconcat (cut, "…", NULL);
        g_free (cut);
    }
    return s;
}

/* the last line of text that walpe_last_words_line quotes (g_free), NULL
 * when there is none */
static inline char *walpe_last_words (const char *text)
{
    if (!text)
        return NULL;
    char **lines = g_strsplit (text, "\n", -1);
    char *words = NULL;
    for (int i = (int) g_strv_length (lines) - 1; i >= 0 && !words; i--)
        words = walpe_last_words_line (lines[i]);
    g_strfreev (lines);
    return words;
}
