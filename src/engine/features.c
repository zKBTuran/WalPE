/* features.c - run-time switches of renderer features, see features.h */
#include "engine/features.h"
#include <string.h>

static const struct {
    const char *name;
    gboolean def;
    gboolean reload;
    const char *about;
} feats[WALPE_FEAT_N] = {
    [WALPE_FEAT_CAMERA_PARALLAX] = { "camera_parallax", TRUE, TRUE,
        "scenes with camera parallax move their layers against the pointer" },
    /* (off by default: the term moves layers while the pointer rests, and
     * whether Wallpaper Engine has it is an estimate. Without it a scene
     * with camera parallax at rest - pointer in the middle or none - shows
     * exactly what its file says.) */
    [WALPE_FEAT_PARALLAX_OFFSET] = { "parallax_offset", FALSE, FALSE,
        "camera parallax also displaces layers that are off the camera centre (pointer at rest)" },
    [WALPE_FEAT_SCRIPT_CAMERA] = { "script_camera", TRUE, FALSE,
        "wallpaper scripts move and zoom the view" },
    [WALPE_FEAT_SPRITE_SHEETS] = { "sprite_sheets", TRUE, TRUE,
        "animated (sprite sheet) textures of image layers play" },
    /* (read at load: what a script did to an animation - paused it, set
     * its frame, its rate, switched its loop off - stays with the animation.
     * Switched off live, the layers kept the state the scripts had left
     * them in, which is not the picture of a scene loaded with the switch
     * off: its animations play by themselves.) */
    [WALPE_FEAT_SCRIPT_TEXANIM] = { "script_texanim", TRUE, TRUE,
        "wallpaper scripts play / pause / stop / set the frame of animated textures" },
    [WALPE_FEAT_BLOOM] = { "bloom", TRUE, FALSE, "bloom of scenes that switch it on" },
    [WALPE_FEAT_CAMERA_SHAKE] = { "camera_shake", TRUE, FALSE,
        "camera shake of scenes that switch it on" },
};

/* per feature: 0 = not set (the default holds), 1 = off, 2 = on; + 4 =
 * fixed by the environment */
static gint state[WALPE_FEAT_N];
static gint serial;
static gsize env_once;

static void env_init (void)
{
    if (!g_once_init_enter (&env_once))
        return;
    const char *e = g_getenv ("WALPE_FEATURES");
    char **kv = e && *e ? g_strsplit_set (e, ", ", -1) : NULL;
    for (int i = 0; kv && kv[i]; i++) {
        char *eq = strchr (kv[i], '=');
        if (!eq)
            continue;
        *eq++ = 0;
        gboolean on = !g_ascii_strcasecmp (eq, "on") || !g_ascii_strcasecmp (eq, "true") ||
                      !strcmp (eq, "1") || !g_ascii_strcasecmp (eq, "yes");
        gboolean off = !g_ascii_strcasecmp (eq, "off") || !g_ascii_strcasecmp (eq, "false") ||
                       !strcmp (eq, "0") || !g_ascii_strcasecmp (eq, "no");
        if (!on && !off)
            continue;   /* not a switch value: the name is not fixed */
        for (int f = 0; f < WALPE_FEAT_N; f++)
            if (!strcmp (kv[i], feats[f].name))
                g_atomic_int_set (&state[f], (on ? 2 : 1) | 4);
    }
    g_strfreev (kv);
    g_once_init_leave (&env_once, 1);
}

gboolean walpe_feature (WalpeFeature f)
{
    if ((int) f < 0 || (int) f >= WALPE_FEAT_N)
        return FALSE;
    env_init ();
    int v = g_atomic_int_get (&state[f]) & 3;
    return v ? v == 2 : feats[f].def;
}

int walpe_feature_find (const char *name)
{
    for (int f = 0; name && f < WALPE_FEAT_N; f++)
        if (!strcmp (name, feats[f].name))
            return f;
    return -1;
}

gboolean walpe_feature_set (const char *name, gboolean on)
{
    int f = walpe_feature_find (name);
    if (f < 0)
        return FALSE;
    env_init ();
    int old = g_atomic_int_get (&state[f]);
    if (old & 4)
        return TRUE;   /* the environment decides */
    int want = on ? 2 : 1;
    if (old != want) {
        gboolean was = walpe_feature ((WalpeFeature) f);
        g_atomic_int_set (&state[f], want);
        if (!was != !on)
            g_atomic_int_inc (&serial);
    }
    return TRUE;
}

const char *walpe_feature_name (int f) { return f >= 0 && f < WALPE_FEAT_N ? feats[f].name : NULL; }
gboolean walpe_feature_default (int f) { return f >= 0 && f < WALPE_FEAT_N ? feats[f].def : FALSE; }
const char *walpe_feature_about (int f) { return f >= 0 && f < WALPE_FEAT_N ? feats[f].about : NULL; }
gboolean walpe_feature_reloads (int f) { return f >= 0 && f < WALPE_FEAT_N ? feats[f].reload : FALSE; }

gboolean walpe_feature_fixed (int f)
{
    if (f < 0 || f >= WALPE_FEAT_N)
        return FALSE;
    env_init ();
    return (g_atomic_int_get (&state[f]) & 4) != 0;
}

guint walpe_feature_serial (void) { return (guint) g_atomic_int_get (&serial); }
