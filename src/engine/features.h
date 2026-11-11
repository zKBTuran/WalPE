/* features.h - run-time switches of renderer features.
 *
 * A renderer feature that changes what wallpapers show (camera parallax,
 * sprite-sheet layers, bloom ...) has a switch here, so that it can be
 * taken out while the daemon runs and the picture compared with the one
 * before it existed. Three places set a switch, the later one winning:
 *   - config.json "features": { "<name>": true|false } (written by the
 *     daemon; common/config.h),
 *   - `walpe-ctl set <name>=on|off` (the name must also be a row of
 *     common/setkeys.h, like every other setting),
 *   - WALPE_FEATURES="name=off,name=on" in the daemon's environment, which
 *     fixes the named switches for the run (tests and A/B runs).
 * Reads are lock-free and may happen on any thread. A scene compares
 * walpe_feature_serial () once per frame and draws a frame when it moved;
 * a feature that is decided when a scene loads (walpe_feature_reloads)
 * needs the wallpapers loaded again, which is the caller's job. */
#pragma once
#include <glib.h>

typedef enum {
    WALPE_FEAT_CAMERA_PARALLAX = 0, /* general.cameraparallax: layers move against the pointer
                                     * by their parallaxDepth */
    WALPE_FEAT_PARALLAX_OFFSET,     /* ... and layers away from the camera centre are displaced
                                     * by depth x amount even while the pointer rests (the
                                     * one switch that is off by default) */
    WALPE_FEAT_SCRIPT_CAMERA,       /* scripts move the view (setCameraTransforms: eye, zoom) */
    WALPE_FEAT_SPRITE_SHEETS,       /* animated (sprite sheet) textures of image layers play */
    WALPE_FEAT_SCRIPT_TEXANIM,      /* scripts drive them (getTextureAnimation) */
    WALPE_FEAT_BLOOM,               /* general.bloom */
    WALPE_FEAT_CAMERA_SHAKE,        /* general.camerashake */
    WALPE_FEAT_N
} WalpeFeature;

gboolean walpe_feature (WalpeFeature f);
/* by name; FALSE = no such feature. A name fixed by WALPE_FEATURES keeps
 * the environment's value (TRUE is returned: the name exists). */
gboolean walpe_feature_set (const char *name, gboolean on);
int walpe_feature_find (const char *name);     /* index, -1 = no such feature */
const char *walpe_feature_name (int f);        /* NULL past the end */
gboolean walpe_feature_default (int f);
const char *walpe_feature_about (int f);       /* one line for a help text / a settings row */
/* the switch is read when a scene loads: a change shows after a reload */
gboolean walpe_feature_reloads (int f);
/* WALPE_FEATURES names it: walpe_feature_set () does not change it */
gboolean walpe_feature_fixed (int f);
/* counts every change of a switch's value */
guint walpe_feature_serial (void);
