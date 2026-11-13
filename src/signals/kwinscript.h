/* kwinscript.h - the script the provider "kwin" (kwin.c) loads into KWin,
 * and how what it reports is read. Kept apart from the D-Bus side so that
 * tests/sig_kwin_test.c can build the text and read reports without a bus.
 *
 * KWin offers no protocol that tells a client which windows are
 * fullscreen, where they are or which has the focus, and it goes on
 * asking a covered wallpaper surface for frames. What it does have is a
 * script interface: a script loaded at run time (org.kde.kwin.Scripting
 * on the session bus, no installation, no permission) sees every window
 * and can call a D-Bus method. The script below works the three answers
 * out inside KWin, per output, and calls the daemon only when one of them
 * changes: nothing crosses the bus while windows merely move or the
 * focus goes from one application to another on the same output.
 *
 * What counts, after the X11 backend (platform/x11.c) as far as the
 * script interface allows:
 *   covered     the windows of applications shown on the output hide at
 *               least 85 % of it: their rectangles on a 24 x 24 grid,
 *               as X11 does. Not minimized, on the current virtual
 *               desktop and activity, not in the "see-through windows"
 *               setting (matched against the window class, as there).
 *   focus_app   the active window is an application's and is on it
 *   fullscreen  the active window is fullscreen (and on it)
 * Not counted, unlike X11: windows that are in neither the task bar nor
 * the pager nor the window switcher (KWin lists panels of other programs
 * and its own helpers that way; a drop-down terminal is one too), and a
 * window's opacity is not looked at ("cover_transparent" has no effect).
 *
 * The report is one string: a line per output, "NAME<tab>FLAGS", FLAGS a
 * subset of "c" (covered), "a" (an application has the focus there), "f"
 * (that window is fullscreen). "unsupported" alone: a KWin whose script
 * interface is older than Plasma 6.
 *
 * Every script the daemon loads reports to an object path of its own
 * (WALPE_KWIN_OBJECT "/<number>"). When the script is replaced (the
 * see-through list changed), the one that is leaving can still have a
 * report on its way; it arrives at a path that is no longer there and
 * is nobody's - it must not be taken for the first word of the new
 * script (kwin.c). */
#pragma once
#include <glib.h>
#include <string.h>

#define WALPE_KWIN_SCRIPT_NAME "io.github.walpe.pause"
#define WALPE_KWIN_OBJECT "/io/github/walpe/KWin"
#define WALPE_KWIN_IFACE "io.github.walpe.KWin"
#define WALPE_KWIN_GRID 24
#define WALPE_KWIN_COVER 0.85

enum {
    WALPE_KWIN_COVERED = 1u << 0,
    WALPE_KWIN_FOCUS = 1u << 1,
    WALPE_KWIN_FULLSCREEN = 1u << 2,
};

/* the "see-through windows" setting as a JavaScript array of lower-case
 * strings; names with characters a window class does not have are left
 * out (nothing of the setting is ever run as code). g_free. */
static inline char *walpe_kwin_see_through_js (const char *list)
{
    GString *g = g_string_new ("[");
    char **v = g_strsplit (list ? list : "", ",", -1);
    for (guint i = 0; v[i]; i++) {
        const char *s = g_strstrip (v[i]);
        gboolean ok = *s != 0;
        for (const char *p = s; *p && ok; p++)
            ok = g_ascii_isalnum (*p) || *p == '.' || *p == '_' || *p == '-' || *p == ' ';
        if (!ok)
            continue;
        char *low = g_ascii_strdown (s, -1);
        g_string_append_printf (g, "%s\"%s\"", g->len > 1 ? "," : "", low);
        g_free (low);
    }
    g_strfreev (v);
    g_string_append_c (g, ']');
    return g_string_free (g, FALSE);
}

/* dest: the daemon's unique name on the session bus (":1.42"), where the
 * script sends its reports; path: the object there that is this script's;
 * pid: the daemon's, whose own windows are no application's; see_through:
 * the setting. g_free. */
static inline char *walpe_kwin_script_text (const char *dest, const char *path, guint pid,
                                            const char *see_through)
{
    static const char body[] =
        "    function send(state) { callDBus(DEST, PATH, \"" WALPE_KWIN_IFACE "\", \"Report\", state); }\n"
        "    if (typeof workspace.windowList !== \"function\") { send(\"unsupported\"); return; }\n"
        "    let sent = null;\n"
        "    const soon = new QTimer();\n"
        "    soon.singleShot = true;\n"
        "    soon.interval = 40;\n"
        "    // an application's window: not the desktop, a panel, a notification, the wallpaper\n"
        "    function app(w) {\n"
        "        return !!w && w.managed && !w.desktopWindow && !w.dock && !w.specialWindow && !w.popupWindow &&\n"
        "            w.pid !== PID && !(w.skipTaskbar && w.skipSwitcher && w.skipPager);\n"
        "    }\n"
        "    function shown(w) {\n"
        "        if (w.minimized) return false;\n"
        "        const now = workspace.currentDesktopForScreen ? workspace.currentDesktopForScreen(w.output) : workspace.currentDesktop;\n"
        "        if (!w.onAllDesktops && w.desktops.length && now && !w.desktops.some(d => d.id === now.id)) return false;\n"
        "        if (w.activities.length && workspace.currentActivity && w.activities.indexOf(workspace.currentActivity) < 0) return false;\n"
        "        return true;\n"
        "    }\n"
        "    function seeThrough(w) {\n"
        "        return [w.resourceClass, w.resourceName, w.desktopFileName].some(n => SEE.indexOf(String(n).toLowerCase()) >= 0);\n"
        "    }\n"
        "    function look() {\n"
        "        const apps = workspace.windowList().filter(w => app(w) && shown(w));\n"
        "        const rects = apps.filter(w => !seeThrough(w)).map(w => w.frameGeometry);\n"
        "        const active = workspace.activeWindow;\n"
        "        const focus = app(active) && shown(active) ? active : null;\n"
        "        const lines = [];\n"
        "        for (const o of workspace.screens) {\n"
        "            const g = o.geometry, cw = g.width / GRID, ch = g.height / GRID;\n"
        "            let marked = 0;\n"
        "            for (let cy = 0; cy < GRID && rects.length; cy++) {\n"
        "                for (let cx = 0; cx < GRID; cx++) {\n"
        "                    const px = g.x + (cx + 0.5) * cw, py = g.y + (cy + 0.5) * ch;\n"
        "                    if (rects.some(r => px >= r.x && px < r.x + r.width && py >= r.y && py < r.y + r.height)) marked++;\n"
        "                }\n"
        "            }\n"
        "            let flags = marked >= NEED ? \"c\" : \"\";\n"
        "            if (focus && focus.output === o) flags += focus.fullScreen ? \"af\" : \"a\";\n"
        "            lines.push(o.name + \"\\t\" + flags);\n"
        "        }\n"
        "        const state = lines.join(\"\\n\");\n"
        "        if (state !== sent) { sent = state; send(state); }\n"
        "    }\n"
        "    // while a window is dragged its geometry changes all the time: the look waits until it rests\n"
        "    function kick() { soon.start(); }\n"
        "    function watch(w) {\n"
        "        for (const s of [\"minimizedChanged\", \"fullScreenChanged\", \"desktopsChanged\", \"activitiesChanged\",\n"
        "                         \"outputChanged\", \"frameGeometryChanged\", \"skipTaskbarChanged\"])\n"
        "            if (w[s]) w[s].connect(kick);\n"
        "    }\n"
        "    soon.timeout.connect(look);\n"
        "    workspace.windowAdded.connect(function (w) { watch(w); kick(); });\n"
        "    for (const s of [\"windowRemoved\", \"windowActivated\", \"currentDesktopChanged\", \"currentActivityChanged\", \"screensChanged\"])\n"
        "        workspace[s].connect(kick);\n"
        "    workspace.windowList().forEach(watch);\n"
        "    look();\n"
        "})();\n";
    char *see = walpe_kwin_see_through_js (see_through);
    char *text = g_strdup_printf (
        "// The pause script of walpe-daemon (WalPE, live wallpapers). The daemon loads it into KWin\n"
        "// while one of its rules \"pause for fullscreen windows / on a covered monitor / when an\n"
        "// application has the focus\" is on, and unloads it when it exits. It tells the daemon,\n"
        "// per output and only when it changes, whether the windows cover the output and whether\n"
        "// the active window is an application's and fullscreen. src/signals/kwinscript.h in WalPE.\n"
        "(function () {\n"
        "    const DEST = \"%s\", PID = %u, SEE = %s, GRID = %d, NEED = %d;\n"
        "    const PATH = \"%s\";\n%s",
        dest, pid, see, WALPE_KWIN_GRID,
        (int) (WALPE_KWIN_COVER * WALPE_KWIN_GRID * WALPE_KWIN_GRID + 0.999), path, body);
    g_free (see);
    return text;
}

/* One line of a report: FALSE for a line that is none. *name points into
 * line (not terminated: *name_len bytes). */
static inline gboolean walpe_kwin_report_line (const char *line, gsize len, const char **name,
                                               gsize *name_len, guint *flags)
{
    const char *tab = memchr (line, '\t', len);
    if (!tab || tab == line)
        return FALSE;
    *name = line;
    *name_len = (gsize) (tab - line);
    *flags = 0;
    for (const char *p = tab + 1; p < line + len; p++) {
        if (*p == 'c')
            *flags |= WALPE_KWIN_COVERED;
        else if (*p == 'a')
            *flags |= WALPE_KWIN_FOCUS;
        else if (*p == 'f')
            *flags |= WALPE_KWIN_FULLSCREEN;
        else
            return FALSE;
    }
    return TRUE;
}

/* ---------------------------------------------------- the pointer script
 * The second script the daemon can have in KWin: where the pointer is
 * while it is over Plasma's desktop, for a wallpaper that lies under that
 * desktop (the add-on for Plasma, contrib/plasma) and uses the pointer.
 * The daemon loads it only then (kwin.c, "the pointer") and gives it the
 * outputs whose wallpaper wants the pointer. What it costs KWin: one small
 * function per movement of the pointer, which does nothing but note the
 * movement while a look is not yet due; a look (which window is under the
 * pointer) at most once in MS milliseconds; a call to the daemon only when
 * a look finds the pointer over Plasma's desktop window on one of those
 * outputs, or finds that it has left it. Over windows, panels and menus,
 * on other outputs and while the pointer rests nothing crosses the bus.
 *
 * It says one string at a time:
 *   "NAME<tab>X<tab>Y"  the pointer is on the output NAME, at X, Y from
 *                       its top left corner in logical pixels
 *   "NAME<tab>"         it has left Plasma's desktop on NAME
 *   "ready"             the script runs (once, when it starts)
 *   "unsupported"       this KWin's script interface cannot tell */
#define WALPE_KWIN_POINTER_SCRIPT_NAME "io.github.walpe.pointer"

enum {
    WALPE_KWIN_PTR_NONE = -1,
    WALPE_KWIN_PTR_MOTION = 0,      /* (the numbers of WalpeForeignPointer, platform.h) */
    WALPE_KWIN_PTR_LEAVE = 4,
};

/* output names, one per line, as a JavaScript array; a name with a
 * character no output name has is left out (nothing of it is ever run as
 * code). g_free. */
static inline char *walpe_kwin_outputs_js (const char *names)
{
    GString *g = g_string_new ("[");
    char **v = g_strsplit (names ? names : "", "\n", -1);
    for (guint i = 0; v[i]; i++) {
        const char *s = v[i];
        gboolean ok = *s != 0;
        for (const char *p = s; *p && ok; p++)
            ok = g_ascii_isalnum (*p) || *p == '.' || *p == '_' || *p == '-' || *p == ':' || *p == ' ';
        if (ok)
            g_string_append_printf (g, "%s\"%s\"", g->len > 1 ? "," : "", s);
    }
    g_strfreev (v);
    g_string_append_c (g, ']');
    return g_string_free (g, FALSE);
}

/* dest, path: as for the pause script; outputs: the names of the outputs
 * whose wallpaper wants the pointer, one per line; ms: the least time
 * between two looks. g_free. */
static inline char *walpe_kwin_pointer_script_text (const char *dest, const char *path, const char *outputs,
                                                    guint ms)
{
    static const char body[] =
        "    function send(what) { callDBus(DEST, PATH, \"" WALPE_KWIN_IFACE "\", \"Pointer\", what); }\n"
        "    if (typeof workspace.windowAt !== \"function\" || !workspace.cursorPosChanged) { send(\"unsupported\"); return; }\n"
        "    let on = null;              // the output the daemon was last given a position on\n"
        "    let x = 0, y = 0;           // and the position\n"
        "    let busy = false, again = false;\n"
        "    const pace = new QTimer();\n"
        "    pace.singleShot = true;\n"
        "    pace.interval = MS;\n"
        "    // Plasma's desktop window is the topmost thing under the pointer, on an output of the list\n"
        "    function look() {\n"
        "        const p = workspace.cursorPos;\n"
        "        const top = workspace.windowAt(p, 1)[0];\n"
        "        const o = top && top.desktopWindow && top.resourceClass === \"plasmashell\" && top.output &&\n"
        "            OUTS.indexOf(top.output.name) >= 0 ? top.output : null;\n"
        "        if (o) {\n"
        "            const nx = p.x - o.geometry.x, ny = p.y - o.geometry.y;\n"
        "            if (on === o.name && nx === x && ny === y) return;\n"
        "            on = o.name; x = nx; y = ny;\n"
        "            send(on + \"\\t\" + x + \"\\t\" + y);\n"
        "        } else if (on !== null) {\n"
        "            send(on + \"\\t\");\n"
        "            on = null;\n"
        "        }\n"
        "    }\n"
        "    // the pointer moves a thousand times a second: one look, then none before MS have passed\n"
        "    function moved() {\n"
        "        if (busy) { again = true; return; }\n"
        "        busy = true;\n"
        "        look();\n"
        "        pace.start();\n"
        "    }\n"
        "    pace.timeout.connect(function () { busy = false; if (again) { again = false; moved(); } });\n"
        "    workspace.cursorPosChanged.connect(moved);\n"
        "    // a window that comes or goes under a pointer that rests\n"
        "    for (const s of [\"windowAdded\", \"windowRemoved\", \"windowActivated\"])\n"
        "        workspace[s].connect(function () { again = true; if (!busy) { busy = true; pace.start(); } });\n"
        "    send(\"ready\");\n"
        "    moved();\n"
        "})();\n";
    char *outs = walpe_kwin_outputs_js (outputs);
    char *text = g_strdup_printf (
        "// The pointer script of walpe-daemon (WalPE, live wallpapers). The daemon loads it into KWin\n"
        "// while a wallpaper that lies under Plasma's desktop (the WalPE add-on for Plasma) uses the\n"
        "// pointer, and unloads it when none does. It tells the daemon where the pointer is while it\n"
        "// is over Plasma's desktop on the outputs named here, at most once in MS milliseconds, and\n"
        "// when it leaves. src/signals/kwinscript.h in WalPE.\n"
        "(function () {\n"
        "    const DEST = \"%s\", OUTS = %s, MS = %u;\n"
        "    const PATH = \"%s\";\n%s",
        dest, outs, ms, path, body);
    g_free (outs);
    return text;
}

/* One call of the pointer script: WALPE_KWIN_PTR_MOTION (*name, *x, *y),
 * WALPE_KWIN_PTR_LEAVE (*name) or WALPE_KWIN_PTR_NONE for anything else.
 * *name points into line (not terminated: *name_len bytes). */
static inline int walpe_kwin_pointer_line (const char *line, const char **name, gsize *name_len, double *x,
                                           double *y)
{
    const char *tab = strchr (line, '\t');
    char *end = NULL;
    if (!tab || tab == line)
        return WALPE_KWIN_PTR_NONE;
    *name = line;
    *name_len = (gsize) (tab - line);
    if (!tab[1])
        return WALPE_KWIN_PTR_LEAVE;
    *x = g_ascii_strtod (tab + 1, &end);
    if (end == tab + 1 || *end != '\t')
        return WALPE_KWIN_PTR_NONE;
    const char *second = end + 1;
    *y = g_ascii_strtod (second, &end);
    if (end == second || *end)
        return WALPE_KWIN_PTR_NONE;
    return WALPE_KWIN_PTR_MOTION;
}
