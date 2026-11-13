/* setvalues.h - the values of a {"cmd":"set"} request, checked before anything
 * is changed.
 *
 * walpe_set_check () (setkeys.h) refuses a request with an unknown member
 * or a value of the wrong type "before anything is changed". A value of
 * the right type that its key does not allow was found later, by the
 * branch of the daemon's ipc_set () that applies the key - after the
 * branches in front of it had applied and saved theirs:
 *
 *   walpe-ctl set fps=31 focus=sideways
 *   {"ok":false,"error":"focus_action: run, limit or pause"}      (exit 3)
 *
 * and the frame rate limit was 31 from then on, in the running daemon and
 * in config.json, with nothing in the reply to say so. Which keys of a
 * refused request had been taken depended on the order of the branches.
 *
 * walpe_set_values_check () asks every such key with the function its
 * branch uses, in one pass in front of all branches: one value that is not
 * allowed and the request is refused as a whole, like an unknown key. What
 * only the running daemon can tell (the graphics devices, the Wallpaper
 * Engine assets folder, a restart that cannot be made now, the quality
 * knobs against the values in force) is checked there, in front of the
 * first change as well (ipc_set_keys).
 *
 * Header only: the daemon and tests/ctl_test.c include it. */
#pragma once
#include <glib.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include "common/backend.h"     /* walpe_backend_parse */
#include "common/config.h"      /* walpe_focus_action_parse, walpe_preview_anim_parse, ... */
#include "common/fluidsim.h"    /* walpe_fluid_quality_parse */
#include "common/glapi.h"       /* walpe_gl_api_parse */
#include "common/present.h"     /* walpe_present_spec_mode */
#include "common/workshop.h"    /* walpe_workshop_valid_user */
#include "engine/video.h"       /* walpe_video_upload_parse */
#include "engine/video_hwsel.h" /* walpe_hwsel_parse */

#define WALPE_SET_UNCHANGED "; nothing was changed"

/* the text of member k, NULL when it is not there or holds no text */
static inline const char *walpe_set_text (JsonObject *req, const char *k)
{
    JsonNode *n = json_object_get_member (req, k);
    return n && JSON_NODE_HOLDS_VALUE (n) && json_node_get_value_type (n) == G_TYPE_STRING
               ? json_node_get_string (n) : NULL;
}

/* NULL = every value of the request is one its key allows (asked after
 * walpe_set_check (): the members are known and of their types); else the
 * sentence for the reply (g_free). */
static inline char *walpe_set_values_check (JsonObject *req)
{
    const char *s;
    JsonNode *n = NULL;
    GType t = G_TYPE_INVALID;
#define WALPE_SET_NODE(k) \
    ((n = json_object_get_member (req, k)) != NULL && \
     ((t = JSON_NODE_HOLDS_VALUE (n) ? json_node_get_value_type (n) : G_TYPE_INVALID), TRUE))
    if ((s = walpe_set_text (req, "focus_action")) && walpe_focus_action_parse (s, -1) < 0)
        return g_strdup ("focus_action: run, limit or pause" WALPE_SET_UNCHANGED);
    /* always | hover | off, or a switch (off / always) */
    if (WALPE_SET_NODE ("preview_anim") && t != G_TYPE_BOOLEAN &&
        !(t == G_TYPE_STRING && walpe_preview_anim_parse (json_node_get_string (n), -1) >= 0))
        return g_strdup ("preview_anim: always, hover or off" WALPE_SET_UNCHANGED);
    /* off | auto | all, or a switch */
    if (WALPE_SET_NODE ("particle_lowres") && t != G_TYPE_BOOLEAN &&
        !(t == G_TYPE_STRING && walpe_particle_lowres_from_name (json_node_get_string (n), -1) >= 0))
        return g_strdup ("particle_lowres: off, auto or all" WALPE_SET_UNCHANGED);
    /* a number, also as text: percent */
    if (WALPE_SET_NODE ("particle_density")) {
        double pd = t == G_TYPE_INT64 || t == G_TYPE_DOUBLE ? json_node_get_double (n) : -1.0;
        const char *ps = t == G_TYPE_STRING ? json_node_get_string (n) : NULL;
        if (pd < 0 && ps && *ps)
            pd = g_ascii_strtod (ps, NULL);
        if (!(pd >= 5.0 && pd <= 100.0))
            return g_strdup ("particle_density: a percentage from 5 to 100" WALPE_SET_UNCHANGED);
    }
    /* a switch, or off | auto | the name of one decoding API */
    if (WALPE_SET_NODE ("video_hwdec") &&
        (t == G_TYPE_INVALID || (t == G_TYPE_STRING && !walpe_hwsel_parse (json_node_get_string (n), NULL))))
        return g_strdup ("video_hwdec: off, auto, or the name of one decoding API (vaapi, cuda, "
                         "vdpau, vulkan, drm, v4l2m2m, ...)" WALPE_SET_UNCHANGED);
    if ((s = walpe_set_text (req, "video_upload"))) {
        int mode = walpe_video_upload_parse (s);
        if (mode < 0 || (mode & WALPE_VUP_HOLD))    /* ("hold" is a measuring aid, no setting) */
            return g_strdup ("video_upload: legacy, pbo, pbodata, persist or direct, each also with "
                             "+pingpong" WALPE_SET_UNCHANGED);
    }
    /* (fx_fast is the GUI switch's spelling of fx_quality and wins over it) */
    if ((s = walpe_set_text (req, "fx_quality")) && !json_object_has_member (req, "fx_fast") &&
        strcmp (s, "exact") && strcmp (s, "fast"))
        return g_strdup ("fx_quality: exact or fast" WALPE_SET_UNCHANGED);
    if (json_object_has_member (req, "fluid_quality") &&
        !walpe_fluid_quality_parse (walpe_set_text (req, "fluid_quality"), NULL))
        return g_strdup ("fluid_quality: full, reduced or low" WALPE_SET_UNCHANGED);
    if ((s = walpe_set_text (req, "steam_user")) && *s && !walpe_workshop_valid_user (s))
        return g_strdup ("steam_user: Steam account names are letters, digits and _" WALPE_SET_UNCHANGED);
    /* (whether this build has the backend that is named: the daemon) */
    if ((s = walpe_set_text (req, "backend")) && walpe_backend_parse (s, -1) < 0)
        return g_strdup ("backend: auto, x11 or wayland" WALPE_SET_UNCHANGED);
    if ((s = walpe_set_text (req, "wayland_pointer")) && g_ascii_strcasecmp (s, "auto") &&
        g_ascii_strcasecmp (s, "off"))
        return g_strdup ("wayland_pointer: auto or off" WALPE_SET_UNCHANGED);
    if (json_object_has_member (req, "gl_api")) {
        WalpeGlApiPref ga;
        s = walpe_set_text (req, "gl_api");
        if (!walpe_gl_api_parse (s ? s : "", &ga))
            return g_strdup ("gl_api: auto, gl or gles" WALPE_SET_UNCHANGED);
    }
    if ((s = walpe_set_text (req, "present"))) {
        char *bad = NULL;
        walpe_present_spec_mode (s, NULL, -1, &bad);
        if (bad) {
            char *msg = g_strdup_printf ("present: \"%s\" is not understood (vsync, limiter, auto, "
                                         "NAME=mode)" WALPE_SET_UNCHANGED, bad);
            g_free (bad);
            return msg;
        }
    }
#undef WALPE_SET_NODE
    return NULL;
}
