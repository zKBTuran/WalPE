/* diag.h - what a bug report needs, said by the daemon itself.
 *
 * Not a public header: main.c includes it once, after its state (D, Bk, Sw,
 * Mon) and pause_reason_mon (). The first report from somebody else's
 * machine came with "the terminal showed nothing useful": the facts were
 * spread over twenty lines of start-up output, a status reply of 5 kB and
 * settings nobody had asked about.
 *
 * diag_text (): the state in a dozen lines - version, session, the window
 * system and why it was chosen, what the compositor offers and lacks, the
 * GL driver, the modules, sound and why it is silent, who reports the
 * pause signals, and per monitor what it shows and whether it is paused
 * and by which rule. The daemon logs it once, when its first GL context is
 * there (diag_say), and `walpe-ctl diag` asks for it again at any time
 * ({"cmd":"diag"} -> {"ok":true,"text":...}).
 *
 * diag_quit_ipc (), diag_quit_signal (): who asked the daemon to stop.
 *
 * diag_sound_line (): one line when the sound settings begin or stop to
 * keep every wallpaper silent.
 *
 * Everything is read from what the daemon holds anyway, on the main
 * thread, when it is asked or when the thing happens: no timer, no
 * thread, nothing on a frame's path. A wallpaper is named by the last part
 * of its path and the home directory is written as "~": the text is made
 * to be posted. */

/* the last component of a wallpaper's path ("" = none) */
static char *diag_base (const char *path)
{
    if (!path || !*path)
        return g_strdup ("");
    char *b = g_path_get_basename (path);
    char *r = g_strdup (walpe_log_str (b));
    g_free (b);
    return r;
}

/* a message from somewhere else (a load error, a note) may name a file
 * under the home directory */
static void diag_no_home (GString *s)
{
    const char *home = g_get_home_dir ();
    const gsize hl = home ? strlen (home) : 0;
    if (hl < 2)
        return;
    for (char *p = strstr (s->str, home); p; p = strstr (s->str + (p - s->str) + 1, home)) {
        const gssize at = p - s->str;
        g_string_erase (s, at, (gssize) hl);
        g_string_insert_c (s, at, '~');
        p = s->str + at;
    }
}

/* the sound settings as one fact: 0 = they let wallpapers be heard, else
 * which of them keeps them silent (the switch, mute, the volume: the order
 * of walpe_sound_status ()) */
static int diag_sound_gate (void)
{
    return !D.cfg->scene_sound ? 2 : D.cfg->mute ? 1 : D.cfg->volume <= 0.0 ? 3 : 0;
}

static const char *diag_sound_gate_text (int gate)
{
    static const char *const t[] = {
        "the settings let wallpapers be heard (scene_sound on, not muted)",
        "muted: every wallpaper is silent (walpe-ctl set mute=off)",
        "scene_sound is off: scenes do not play their sound layers, but for one with its own "
        "switch (walpe-ctl set scene_sound=on; one wallpaper: walpe-ctl setprop walpe_sound=on)",
        "the volume setting is 0: every wallpaper is silent (walpe-ctl set volume=0.5)",
    };
    return t[CLAMP (gate, 0, 3)];
}

/* after a request that changed something: one line when the sound settings
 * went from "heard" to one of the silent states or back (three compares
 * otherwise). The state at start is in the summary. */
static int diag_sound_said = -1;

static void diag_sound_line (void)
{
    int gate = diag_sound_gate ();
    if (gate == diag_sound_said)
        return;
    if (diag_sound_said >= 0 && gate)
        g_message ("sound: %s", diag_sound_gate_text (gate));
    else if (diag_sound_said >= 0)
        g_message ("sound: %s, volume %.2f", diag_sound_gate_text (gate), D.cfg->volume);
    diag_sound_said = gate;
}

static char *diag_text (void)
{
    GString *s = g_string_new (NULL);
    g_string_append_printf (s, "walpe-daemon %s \"%s\", pid %d, up for %.0f s\n", WALPE_VERSION,
                            WALPE_CODENAME, (int) getpid (),
                            (double) (g_get_monotonic_time () - daemon_start_us) / 1e6);
    {
        /* the session as the environment names it; of the two display
         * variables only whether they are set */
        const char *st = g_getenv ("XDG_SESSION_TYPE"), *cd = g_getenv ("XDG_CURRENT_DESKTOP");
        const char *wd = g_getenv ("WAYLAND_DISPLAY"), *xd = g_getenv ("DISPLAY");
        g_string_append_printf (s, "session: type %s, desktop %s, WAYLAND_DISPLAY %s, DISPLAY %s\n",
                                st && *st ? walpe_log_strn (st, 40) : "not told",
                                cd && *cd ? walpe_log_strn (cd, 80) : "not told",
                                wd && *wd ? "set" : "not set", xd && *xd ? "set" : "not set");
    }
    g_string_append_printf (s, "backend: %s%s [chosen by: %s; built in: %s]\n", Bk.why ? Bk.why : "?",
                            D.windowed ? " (--windowed)" : "", Bk.source ? Bk.source : "?",
                            walpe_backends_built (walpe_platform_built (WALPE_BACKEND_X11),
                                                  walpe_platform_built (WALPE_BACKEND_WAYLAND)));
    {
        char *desc = walpe_platform_describe ();
        g_string_append_printf (s, "window system: %s\n", desc ? desc : "?");
        g_free (desc);
    }
    g_mutex_lock (&Sw.lock);
    if (Sw.gl_renderer)
        g_string_append_printf (s, "gl: %s | %s | %s%s\n", Sw.gl_vendor, Sw.gl_renderer, Sw.gl_version,
                                walpe_sw_renderer_is_software (Sw.gl_renderer)
                                    ? " - rendering on the CPU" : "");
    else
        g_string_append (s, "gl: no GL context yet (no monitor has drawn)\n");
    g_mutex_unlock (&Sw.lock);
    {
        char *ms = walpe_modules_status ();
        g_string_append_printf (s, "modules: %s (loaded when the first video / web wallpaper is)\n", ms);
        g_free (ms);
    }
    {
        char *snd = walpe_sound_status (), *au = walpe_audio_status ();
        g_string_append_printf (s, "sound: %s [scene_sound %s, volume %.2f, mute %s]\n", snd,
                                D.cfg->scene_sound ? "on" : "off", D.cfg->volume,
                                D.cfg->mute ? "on" : "off");
        g_string_append_printf (s, "audio capture (for wallpapers that react to sound): %s\n", au);
        g_free (snd);
        g_free (au);
    }
    g_string_append_printf (s, "pause rules: fullscreen %s, covered %s, dpms %s, focus %s, battery %s, "
                            "idle %d s%s\n", D.cfg->pause_fullscreen ? "on" : "off",
                            D.cfg->pause_covered ? "on" : "off", D.cfg->pause_dpms ? "on" : "off",
                            D.cfg->focus_action == WALPE_FOCUS_PAUSE ? "pause"
                            : D.cfg->focus_action == WALPE_FOCUS_LIMIT ? "limit" : "run",
                            D.cfg->pause_battery ? "on" : "off", MAX (D.cfg->pause_idle, 0),
                            D.cfg->paused ? "; PAUSED by walpe-ctl pause / the tray" : "");
    g_string_append (s, "pause signals, who reports them:");
    for (int sig = 0; sig < WALPE_SIG_N; sig++) {
        const char *from = walpe_signal_sources (sig), *miss = walpe_signal_missing (sig);
        g_string_append_printf (s, " %s=%s", walpe_signal_name (sig), from && *from ? from : "nobody");
        if (miss && *miss)
            g_string_append_printf (s, " (%s)", miss);
    }
    g_string_append_c (s, '\n');
    if (!D.mons)
        g_string_append (s, "monitors: none\n");
    for (GList *l = D.mons; l; l = l->next) {
        Mon *m = l->data;
        const char *cur = m->want_path ? m->want_path : walpe_config_monitor_path (D.cfg, m->name);
        char *base = diag_base (cur);
        const char *why = m->own_paused ? pause_reason_mon (m) : "";
        g_string_append_printf (s, "monitor %d %s %dx%d+%d+%d: %s%s%s%s, %s%s%s", m->index,
                                m->name ? m->name : "?", m->geom.width, m->geom.height, m->geom.x,
                                m->geom.y, walpe_type_name (g_atomic_int_get (&m->content_type)),
                                *base ? " \"" : "", base, *base ? "\"" : "",
                                m->own_paused ? "paused: " : "running",
                                !m->own_paused ? "" : *why ? why : "nothing of it can be seen",
                                m->loading || (m->leader && m->leader->loading) ? ", loading" : "");
        if (m->leader)
            g_string_append_printf (s, ", shows the frames of monitor %d", m->leader->index);
        if (m->load_error)
            g_string_append_printf (s, ", LOAD ERROR: %s", walpe_log_str (m->load_error));
        if (g_atomic_int_get (&m->swap_lost))
            g_string_append (s, ", SWAP ERROR: its GL context or surface is gone");
        g_string_append_c (s, '\n');
        g_free (base);
    }
    diag_no_home (s);
    g_string_truncate (s, s->len ? s->len - 1 : 0);    /* (no newline at the end) */
    return g_string_free (s, FALSE);
}

/* The summary in the log, once in the life of the process, written by the
 * main thread at the first of: a wallpaper's load has reported (it is
 * shown, or the reason why not is known: load_note_cb); the first monitor
 * thread has its GL context and no wallpaper is being loaded (sw_note_gl
 * asks with first_gl set: a daemon that shows nothing); the daemon ends
 * (main: one that never drew). By the first two the window system, the
 * monitors and the driver are known. */
static gboolean diag_said;

static gboolean diag_say (gpointer first_gl)
{
    if (diag_said)
        return G_SOURCE_REMOVE;
    for (GList *l = D.mons; first_gl && l; l = l->next)
        if (((Mon *) l->data)->loading)
            return G_SOURCE_REMOVE;     /* (said when that load has reported) */
    diag_said = TRUE;
    char *t = diag_text ();
    g_message ("summary (walpe-ctl diag prints it again):\n%s", t);
    g_free (t);
    diag_sound_said = diag_sound_gate ();   /* (the summary has said it) */
    return G_SOURCE_REMOVE;
}

/* {"cmd":"diag"} */
static void diag_ipc (JsonBuilder *b)
{
    char *t = diag_text ();
    jb_bool (b, "ok", TRUE);
    jb_str (b, "text", t);
    g_free (t);
}

/* "quit" over the control socket: which program sent it (the socket tells
 * the process id, /proc its name: walpe-ctl, walpe-tray, walpe = the
 * window). Read once, when it happens. */
static char *diag_comm (int pid)
{
    char *path = g_strdup_printf ("/proc/%d/comm", pid), *comm = NULL;
    if (g_file_get_contents (path, &comm, NULL, NULL))
        g_strchomp (comm);
    g_free (path);
    return comm;
}

static void diag_quit_ipc (void)
{
    int pid = walpe_ipc_current_pid ();
    char *comm = pid > 0 ? diag_comm (pid) : NULL;
    if (pid > 0)
        g_message ("quit: asked for over the control socket by %s (pid %d): shutting down",
                   comm && *comm ? walpe_log_strn (comm, 32) : "a process that is gone already", pid);
    else
        g_message ("quit: asked for over the control socket: shutting down");
    g_free (comm);
}

/* A signal ended the daemon: which one, and the process that sent it where
 * the kernel tells (sender 0 = the kernel itself: Ctrl+C, a terminal that
 * was closed). sig 0 = not known (the fallback without a descriptor). */
static void diag_quit_signal (int sig, int sender)
{
    const char *name = sig == SIGTERM ? "SIGTERM" : sig == SIGINT ? "SIGINT (Ctrl+C)"
                       : sig == SIGHUP ? "SIGHUP (its terminal was closed)" : NULL;
    char *comm = sender > 0 ? diag_comm (sender) : NULL;
    if (!name)
        g_message ("signal: shutting down");
    else if (sender > 0)
        g_message ("signal %s from %s (pid %d): shutting down", name,
                   comm && *comm ? walpe_log_strn (comm, 32) : "a process that is gone already", sender);
    else
        g_message ("signal %s: shutting down", name);
    g_free (comm);
}
