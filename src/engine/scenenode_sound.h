/* scenenode_sound.h - the sound layers of a scene: all that scenenode.c
 * knows about scenesound.h. Included by scenenode.c once its types and its
 * property readers (prop_num, prop_str, node_visible, resolve_asset) are
 * defined; everything here runs on the scene's render thread.
 *
 * A scene object with a "sound" member is a sound layer. It draws nothing
 * and is handled by the renderer exactly as before; what is added is its
 * player (NodeObj.sound), which exists only in a program that allowed
 * sound output (the daemon) and for a layer with at least one file inside
 * the wallpaper. Whether the player is heard is decided in scenesound.c
 * (the setting, the wallpaper's own switch, volume, pause, one instance
 * per wallpaper): the scene reports two things, "shown and not paused"
 * (scene_sound_sync) and what the layer and its scripts ask for.
 *
 * Options: a layer's "volume" and "visible" may be bound to an option of
 * the wallpaper like any other layer property (also the combo form that
 * selects one of several layers); they are read when the scene loads, and
 * a changed option loads the scene again. */

/* the players of this scene, made with its first sound layer */
static WalpeSoundGroup *scene_sound_group (SceneNodeContent *s)
{
    if (!s->sound_group && !s->sound_tried) {
        s->sound_tried = TRUE;
        /* the wallpaper's own switch: its reserved option "walpe_sound"
         * (`walpe-ctl setprop walpe_sound=on|off`), which wins over the
         * setting "scene_sound" */
        char *sw = walpe_config_get_prop (s->origin, "walpe_sound");
        s->sound_group = walpe_sound_group_new (s->origin, walpe_sound_switch_parse (sw));
        g_free (sw);
    }
    return s->sound_group;
}

static void node_sound_load (SceneNodeContent *s, NodeObj *nv, JsonObject *o)
{
    s->sound_objects++;
    WalpeSoundGroup *g = scene_sound_group (s);
    if (!g)
        return;   /* this program plays no sound: the object is read no further */
    if (s->sound_full)
        return;   /* the scene has its WALPE_SOUND_MAX_LAYERS players (said once): a scene
                   * file with thousands of sound objects had up to 64 names of each
                   * looked up on disk for players that are never made */
    /* the layer's own "visible" (the renderer hides some sound objects for
     * its own reasons): an invisible sound layer is silent */
    nv->snd_visible = node_visible (o, s->user_props);
    double vol = prop_num (o, "volume", 1.0);
    nv->snd_volume = isfinite (vol) ? (float) CLAMP (vol, 0.0, 1.0) : 1.0f;
    JsonNode *sn = json_object_get_member (o, "sound");
    JsonArray *sa = sn && JSON_NODE_HOLDS_ARRAY (sn) ? json_node_get_array (sn) : NULL;
    guint n = sa ? json_array_get_length (sa) : 1;
    if (n > WALPE_SOUND_MAX_FILES) {
        g_message ("scenenode: sound layer %s names %u files: the first %d are used",
                   walpe_log_str (nv->name ? nv->name : "?"), n, WALPE_SOUND_MAX_FILES);
        n = WALPE_SOUND_MAX_FILES;
    }
    GPtrArray *files = g_ptr_array_new_with_free_func (g_free);
    for (guint i = 0; i < n; i++) {
        JsonNode *fn = sa ? json_array_get_element (sa, i) : sn;
        const char *f = fn && JSON_NODE_HOLDS_VALUE (fn) && json_node_get_value_type (fn) == G_TYPE_STRING
                        ? json_node_get_string (fn) : NULL;
        if (!f || !*f)
            continue;
        /* a name inside the wallpaper (or Wallpaper Engine's own assets),
         * never a path or an address of the scene's choosing: the name is
         * checked as text, then resolve_asset keeps what it finds below
         * the folder, links resolved, regular files only */
        char *p = NULL;
        if (walpe_sound_name_ok (f)) {
            p = resolve_asset (s->dir, f);
            if (!p && walpe_we_assets_dir ())
                p = resolve_asset (walpe_we_assets_dir (), f);
        }
        if (p) {
            g_ptr_array_add (files, p);
        } else {
            /* the name is the scene's text and was refused, perhaps for the
             * control characters in it: its first bytes, escaped (a line
             * break in it started a log line of the scene's own wording) */
            char *head = g_strndup (f, 120), *shown = g_strescape (head, NULL);
            g_message ("scenenode: sound layer %s: no file \"%s\" inside the wallpaper: left out",
                       walpe_log_str (nv->name ? nv->name : "?"), shown);
            g_free (shown);
            g_free (head);
        }
    }
    const char *mode = prop_str (o, "playbackmode");
    double mint = prop_num (o, "mintime", 0.0), maxt = prop_num (o, "maxtime", 0.0);
    WalpeSoundSpec spec = {
        .files = (const char *const *) files->pdata, .nfiles = (int) files->len, .volume = nv->snd_volume,
        .mode = !g_strcmp0 (mode, "random") ? WALPE_SOUND_RANDOM
              : !g_strcmp0 (mode, "single") ? WALPE_SOUND_SINGLE : WALPE_SOUND_LOOP,
        .mintime = isfinite (mint) ? (float) mint : 0.0f, .maxtime = isfinite (maxt) ? (float) maxt : 0.0f,
        .name = nv->name,
    };
    if (files->len) {
        nv->sound = walpe_sound_new (g, &spec);
        if (!nv->sound && !s->sound_full) {
            s->sound_full = TRUE;
            g_message ("scenenode: more than %d sound layers: layer %s and those after it stay silent",
                       WALPE_SOUND_MAX_LAYERS, walpe_log_str (nv->name ? nv->name : "?"));
        }
    }
    g_ptr_array_free (files, TRUE);
    if (!nv->sound)
        return;
    walpe_sound_hold (nv->sound, !nv->snd_visible);
    /* "startsilent": waits for a script's play (). Nothing is heard before
     * the scene's first frame (scene_sound_sync). */
    if (prop_num (o, "startsilent", 0.0) == 0)
        walpe_sound_play (nv->sound);
}

/* the scene's players may be heard while it is shown and not paused: the
 * daemon pauses a scene for a fullscreen window, a covered monitor, the
 * focus and battery rules, a sleeping display - sound follows all of them */
static void scene_sound_sync (SceneNodeContent *s)
{
    if (s->sound_group)
        walpe_sound_group_set_active (s->sound_group, s->sound_live && !s->paused);
}

/* a script reads a property of a layer that has a player. WSS_NONE = not
 * answered here: the caller goes on as for any layer, which is also what
 * happens for every property while sound is switched off for the
 * wallpaper (its scripts then see what they saw before there was sound) */
static WalpeSsType node_sound_get (SceneNodeContent *s, NodeObj *nv, const char *prop, double *v)
{
    gboolean vol = !strcmp (prop, "volume");
    if ((!vol && strcmp (prop, "playing")) || !walpe_sound_group_enabled (s->sound_group))
        return WSS_NONE;
    if (vol) {
        v[0] = nv->snd_volume;
        return WSS_NUM;
    }
    v[0] = walpe_sound_is_playing (nv->sound);
    return WSS_BOOL;
}

/* a script sets a property of a layer that has a player: play () / pause ()
 * / stop () arrive as "playing" 1 / 0 / -1 (as for particle systems), then
 * the volume and the layer's visibility. The caller goes on afterwards as
 * for any layer. */
static void node_sound_set (NodeObj *nv, const char *prop, WalpeSsType t, const double *v)
{
    if (!v || (t != WSS_NUM && t != WSS_BOOL))
        return;
    if (t == WSS_NUM && !strcmp (prop, "playing")) {
        if (v[0] > 0)
            walpe_sound_play (nv->sound);
        else if (v[0] < 0)
            walpe_sound_stop (nv->sound);
        else
            walpe_sound_pause (nv->sound);
    } else if (t == WSS_NUM && !strcmp (prop, "volume")) {
        if (isfinite (v[0])) {
            nv->snd_volume = (float) CLAMP (v[0], 0.0, 1.0);
            walpe_sound_set_volume (nv->sound, nv->snd_volume);
        }
    } else if (!strcmp (prop, "visible")) {
        nv->snd_visible = v[0] != 0;
        walpe_sound_hold (nv->sound, !nv->snd_visible);
    }
}

/* gl_init: one line about the scene's sound, so that a silent wallpaper
 * with sound layers explains itself in the log */
static void scene_sound_report (SceneNodeContent *s)
{
    if (!s->sound_objects)
        return;
    int players = 0;
    for (guint i = 0; i < s->nodes->len; i++)
        players += g_array_index (s->nodes, NodeObj, i).sound != NULL;
    g_message ("scenenode: %d sound layer(s), %d with files that can be played: %s", s->sound_objects, players,
               !s->sound_group ? "this program plays no sound"
               : walpe_sound_group_enabled (s->sound_group)
                   ? "sound is on for this wallpaper (heard at the volume setting, while it is shown)"
                   : "silent, sound is off (walpe-ctl set scene_sound=on, or for this wallpaper only: "
                     "walpe-ctl setprop walpe_sound=on)");
}

/* WALPE_PROP_PROBE, for a layer that has a player: what it is asked to do
 * and what its player did (a program that plays no sound has no players,
 * and its probe output is what it was before there was sound) */
static void node_sound_probe (NodeObj *nv, JsonBuilder *b)
{
    WalpeSoundStats st;
    walpe_sound_stats (nv->sound, &st);
    json_builder_set_member_name (b, "sound");
    json_builder_begin_object (b);
    json_builder_set_member_name (b, "audible");
    json_builder_add_boolean_value (b, nv->snd_visible);
    json_builder_set_member_name (b, "volume");
    json_builder_add_double_value (b, nv->snd_volume);
    json_builder_set_member_name (b, "playing");
    json_builder_add_boolean_value (b, walpe_sound_is_playing (nv->sound));
    json_builder_set_member_name (b, "running");
    json_builder_add_boolean_value (b, st.running);
    json_builder_set_member_name (b, "frames");
    json_builder_add_int_value (b, st.frames);
    json_builder_set_member_name (b, "rate");
    json_builder_add_int_value (b, st.rate);
    json_builder_set_member_name (b, "channels");
    json_builder_add_int_value (b, st.channels);
    json_builder_set_member_name (b, "plays");
    json_builder_add_int_value (b, st.plays);
    json_builder_set_member_name (b, "peak");
    json_builder_add_int_value (b, st.peak);
    json_builder_set_member_name (b, "null_output");
    json_builder_add_boolean_value (b, st.null_out);
    json_builder_set_member_name (b, "error");
    json_builder_add_string_value (b, st.error);
    json_builder_end_object (b);
}

/* the scene is freed: its players go with their group (their threads end
 * by themselves, nothing here waits for the sound server) */
static void scene_sound_free (SceneNodeContent *s)
{
    for (guint i = 0; s->nodes && i < s->nodes->len; i++)
        g_array_index (s->nodes, NodeObj, i).sound = NULL;
    g_clear_pointer (&s->sound_group, walpe_sound_group_free);
}
