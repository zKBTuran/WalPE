/* diagpath_test - no directory of the user's in the text of `walpe-ctl diag`
 * (src/daemon/diagpath.h).
 *
 * The summary is made to be posted: wallpapers by the last part of their
 * path, the home directory as "~". The reason a load failed is quoted from
 * the loaders, and with a wallpaper outside the home directory - a library
 * on /mnt, a file opened from /tmp - its whole path stood in a monitor's
 * line. The three steps on sentences as the daemon makes them:
 *  1. the home directory, as a whole path or the start of one only;
 *  2. the folders the daemon knows, taken out by their exact text (blanks
 *     in a name included), longest first;
 *  3. whatever is still an absolute path: ".../<last part>" - and what is
 *     no path of this machine stays as it is ("a / b", a renderer's name,
 *     a network address, a relative path, "~/...", "/tmp").
 * No daemon, no display. */
#include <glib.h>
#include <stdio.h>
#include "daemon/diagpath.h"

static int fails;

static void same (const char *what, const GString *got, const char *want)
{
    if (strcmp (got->str, want)) {
        fails++;
        g_printerr ("FAIL %s:\n   got  \"%s\"\n   want \"%s\"\n", what, got->str, want);
    }
}

static void home_is (const char *home, const char *text, const char *want)
{
    GString *s = g_string_new (text);
    walpe_diag_no_home (s, home);
    same ("home", s, want);
    g_string_free (s, TRUE);
}

static void paths_are (const char *text, const char *want)
{
    GString *s = g_string_new (text);
    walpe_diag_short_paths (s);
    same ("short", s, want);
    g_string_free (s, TRUE);
}

/* the three steps as the daemon runs them */
static char *all (const char *home, const char *const *folders, const char *text)
{
    GString *s = g_string_new (text);
    GPtrArray *f = g_ptr_array_new_with_free_func (g_free);
    for (int i = 0; folders && folders[i]; i++)
        g_ptr_array_add (f, g_strdup (folders[i]));
    walpe_diag_no_folders (s, f, home);
    g_ptr_array_free (f, TRUE);
    walpe_diag_no_home (s, home);
    walpe_diag_short_paths (s);
    return g_string_free (s, FALSE);
}

static void all_is (const char *home, const char *const *folders, const char *text, const char *want)
{
    char *got = all (home, folders, text);
    if (strcmp (got, want)) {
        fails++;
        g_printerr ("FAIL all:\n   got  \"%s\"\n   want \"%s\"\n", got, want);
    }
    g_free (got);
}

int main (void)
{
    /* ---- 1. the home directory */
    home_is ("/home/al", "/home/al/walls/x: bad", "~/walls/x: bad");
    home_is ("/home/al", "in /home/al", "in ~");
    home_is ("/home/al", "(/home/al/a) and \"/home/al/b c\"", "(~/a) and \"~/b c\"");
    home_is ("/home/al", "/home/alice/walls", "/home/alice/walls");                 /* another user */
    home_is ("/home/al", "/mnt/backup/home/al/walls", "/mnt/backup/home/al/walls"); /* the middle of a path */
    home_is ("/home/al", "/home/al/a /home/al/b", "~/a ~/b");
    home_is ("/", "/usr/lib/x", "/usr/lib/x");                                      /* no home to speak of */
    home_is (NULL, "/usr/lib/x", "/usr/lib/x");
    /* GLib puts a file name between the quotation marks U+201C and U+201D
     * (here: a scene's package that cannot be copied into the cache), other
     * texts between guillemets and no-break spaces. A byte beyond ASCII in
     * front of the path counted as part of a name, and the home directory
     * stood there in full - in a summary that had "~" there before. */
    home_is ("/home/al", "Failed to create file \xe2\x80\x9c/home/al/.cache/walpe/pkg/7/scene.json.A1B2C3\xe2\x80\x9d: "
                         "No space left on device",
             "Failed to create file \xe2\x80\x9c~/.cache/walpe/pkg/7/scene.json.A1B2C3\xe2\x80\x9d: "
             "No space left on device");
    home_is ("/home/al", "\xe2\x80\x9c/home/al\xe2\x80\x9d and \xc2\xab\xc2\xa0/home/al/a\xc2\xa0\xc2\xbb",
             "\xe2\x80\x9c~\xe2\x80\x9d and \xc2\xab\xc2\xa0~/a\xc2\xa0\xc2\xbb");
    /* ... and a letter beyond ASCII still is part of a name */
    home_is ("/home/al", "/mnt/caf\xc3\xa9/home/al/x and /home/al\xc3\xa9/x",
             "/mnt/caf\xc3\xa9/home/al/x and /home/al\xc3\xa9/x");

    /* ---- 3. what is still a path */
    paths_are ("LOAD ERROR: /mnt/walls/Secret/noscene: scene.json not found in /mnt/walls/Secret/noscene",
               "LOAD ERROR: .../noscene: scene.json not found in .../noscene");
    paths_are ("/a/b/scene.json", ".../scene.json");
    paths_are ("cannot open (/dev/dri/renderD128), tried /dev/dri/card0.",
               "cannot open (.../renderD128), tried .../card0.");
    paths_are ("x=/a/b, y=[/c/d] 'e' </f/g>", "x=.../b, y=[.../d] 'e' <.../g>");
    paths_are ("a folder: /a/b/", "a folder: .../b/");
    paths_are ("quoted \"/mnt/My Walls/a b/scene.json\" and '/x/y z/w' end",
               "quoted \".../scene.json\" and '.../w' end");
    paths_are ("file:///mnt/x/index.html", "file://.../index.html");
    paths_are ("two\n/a/b\n/c/d e", "two\n.../b\n.../d e");
    /* between GLib's quotation marks (blanks in the name included), between guillemets */
    paths_are ("Failed to open file \xe2\x80\x9c/run/media/al/My Stick/a b.mp4\xe2\x80\x9d: Permission denied",
               "Failed to open file \xe2\x80\x9c.../a b.mp4\xe2\x80\x9d: Permission denied");
    paths_are ("in \xc2\xab\xc2\xa0/srv/x/y.png\xc2\xa0\xc2\xbb.", "in \xc2\xab\xc2\xa0.../y.png\xc2\xa0\xc2\xbb.");
    paths_are ("see /srv/x/y.png\xe2\x80\x9d: no", "see .../y.png\xe2\x80\x9d: no");
    paths_are ("/mnt/caf\xc3\xa9 \xe2\x80\x93 two/x.json", ".../caf\xc3\xa9 \xe2\x80\x93 two/x.json");    /* (cut at the blank, as before) */
    /* ... and what is none, or says nothing: unchanged */
    static const char *const kept[] = {
        "pause rules: fullscreen on; PAUSED by walpe-ctl pause / the tray",
        "modules: web=not loaded video=loaded (loaded when the first video / web wallpaper is)",
        "gl: NVIDIA Corporation | NVIDIA GeForce RTX 3060/PCIe/SSE2 | 4.6.0 NVIDIA 550.54.14",
        "in /tmp and in /",
        "~/walls/homescene: scene.json not found in ~/walls/homescene",
        "noscene/scene.json and shaders/a.frag",
        "http://example.com/a/b and https://host/x",
        "monitor 0 DP-1 2560x1440+0+0: scene \"1234\", running",
        /* the line "sound server:" of `walpe-ctl diag` (daemon/main.c sound_server_line,
         * engine/soundserver.c): commands, "/" between words and per cent signs, no path */
        "sound server: not asked: no web wallpaper is shown at a level above 0 (the sound of scenes and "
        "videos is WalPE's own stream \"walpe\", which the line above counts)",
        "sound server: no stream of WebKit is muted or at 0 % in the sound server (a page that is silent "
        "now plays nothing, or its stream is not there yet)",
        "sound server: could not be asked: `pactl -f json list sink-inputs` did not answer (no pactl, "
        "one older than 16, or no sound server)",
        "sound server: the sound server holds 1 of 2 stream(s) of WebKit silent (1 muted, 0 at 0 %): a web "
        "wallpaper's sound is such a stream, and it stays silent whatever the settings here say - WalPE "
        "never changes a stream in the server, and the server gives the stored value to every later one. "
        "Undo it once, while the page plays: in the desktop's mixer (Plasma: the volume applet, "
        "Applications; pavucontrol: Playback) unmute \"WebKit\" and turn it up, or: pactl "
        "set-sink-input-mute 57 0; pactl set-sink-input-volume 57 100%",
        "sound: on: nothing is playing (0 wallpaper(s) with sound shown) [scene_sound on, volume 0.50, mute off]",
        "n/a, 50/60 Hz, and/or",
        "",
        "/",
    };
    for (guint i = 0; i < G_N_ELEMENTS (kept); i++)
        paths_are (kept[i], kept[i]);

    /* ---- the three steps together, on lines as the daemon writes them */
    {
        static const char *const f[] = { "/mnt/My Walls", "/tmp/x/outside/Secret-Folder", "/mnt/steam/we", NULL };
        all_is ("/home/al", f,
                "monitor 0 A 640x360+0+0: none \"noscene\", running, LOAD ERROR: "
                "/tmp/x/outside/Secret-Folder/noscene: scene.json not found in /tmp/x/outside/Secret-Folder/noscene",
                "monitor 0 A 640x360+0+0: none \"noscene\", running, LOAD ERROR: "
                "noscene: scene.json not found in noscene");
        /* a blank in the folder's name; the file below the wallpaper keeps its place in it */
        all_is ("/home/al", f, "/mnt/My Walls/broken: bad scene json: /mnt/My Walls/broken/scene.json",
                "broken: bad scene json: broken/scene.json");
        /* the folder itself, and a folder that only begins like it */
        all_is ("/home/al", f, "cannot read /mnt/My Walls: denied; /mnt/My Walls2/a is another",
                "cannot read My Walls: denied; .../My Walls2/a is another");
        all_is ("/home/al", f, "/mnt/steam/we/assets/shaders/a.frag: not found", "assets/shaders/a.frag: not found");
        /* a folder in the home directory is the home step's: "~/..." as before */
        static const char *const h[] = { "/home/al/walls", NULL };
        all_is ("/home/al", h, "/home/al/walls/homescene: scene.json not found in /home/al/walls/homescene",
                "~/walls/homescene: scene.json not found in ~/walls/homescene");
        /* a longer folder goes before the one it lies in */
        static const char *const n[] = { "/mnt/w", "/mnt/w/sets/a", NULL };
        all_is ("/home/al", n, "/mnt/w/sets/a/one: x; /mnt/w/two: y", "one: x; two: y");
        /* a path nobody told the daemon about, next to one it knows */
        all_is ("/home/al", f, "/mnt/My Walls/a: the previous wallpaper could not be opened again "
                               "(/run/media/al/STICK/b: b.mp4 is no video)",
                "a: the previous wallpaper could not be opened again (.../b: b.mp4 is no video)");
        /* GLib's message about a file below a known folder, and below none */
        all_is ("/home/al", f, "/mnt/My Walls/a: Failed to open file \xe2\x80\x9c/mnt/My Walls/a/scene.pkg\xe2\x80\x9d: "
                               "Permission denied (\xe2\x80\x9c/srv/other/b c/d.pkg\xe2\x80\x9d too)",
                "a: Failed to open file \xe2\x80\x9c" "a/scene.pkg\xe2\x80\x9d: Permission denied "
                "(\xe2\x80\x9c.../d.pkg\xe2\x80\x9d too)");
        /* nothing of the folders is left in any of them */
        char *t = all ("/home/al", f, "/tmp/x/outside/Secret-Folder/clip.mp4 \"/tmp/x/outside/Secret-Folder\" "
                                      "/home/al/v.mp4 /mnt/My Walls/s/scene.json");
        if (strcmp (t, "clip.mp4 \"Secret-Folder\" ~/v.mp4 s/scene.json") || strstr (t, "/home/al") ||
            strstr (t, "/mnt") || strstr (t, "outside") || strstr (t, "/tmp")) {
            fails++;
            g_printerr ("FAIL: a folder is left in \"%s\"\n", t);
        }
        g_free (t);
    }
    if (fails)
        g_printerr ("diagpath-test: %d check(s) failed\n", fails);
    else
        g_print ("diagpath-test: ok\n");
    return fails ? 1 : 0;
}
