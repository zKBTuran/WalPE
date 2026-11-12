/* scenevideo.h - the converted copy of a video inside a scene.
 *
 * A scene's video layer is a WE .tex with an MP4 in it. The player decodes
 * it at its own size whatever the monitor shows: a 3840 x 2160 clip on a
 * 1920 x 1080 monitor costs four times the decoding of a clip of the size
 * that is shown, and libavcodec's H.264 decoder cannot decode to a smaller
 * picture. With "video_scene_convert" (on by default) such a clip is
 * converted once, in the background, into a copy of the size it is needed
 * at, and the scene plays the copy from then on (engine/videotex.h: the
 * playing clip changes over where it starts again; until then, and
 * whenever there is no copy, the original plays as before).
 *
 * This file is the part without a display, a decoder or a child process:
 * the setting, the decision (is a copy worth making, how large), the name
 * of the copy in the cache, and the way a scene tells the daemon that it
 * wants one. The conversion itself is common/import.h
 * (walpe_import_start_scene), the daemon runs it (daemon/main.c). */
#pragma once
#include <glib.h>
#include "common/resolution.h"   /* WalpeVideoBox */

G_BEGIN_DECLS

/* ---- the setting ------------------------------------------------------
 * config "video_scene_convert", walpe-ctl set video_scene_convert=on|off.
 * WALPE_VIDEO_SCENE_CONVERT=0|1 in the daemon's environment overrides it.
 * Any thread. */
void walpe_scene_video_set_on (gboolean on);
gboolean walpe_scene_video_on (void);
/* A number that changes whenever a scene has to look at its videos again:
 * the setting was changed, a copy became ready, a conversion failed. A
 * scene compares it once per frame per video it has (nothing for a scene
 * without videos). Any thread. */
guint walpe_scene_video_gen (void);
void walpe_scene_video_touch (void);

/* ---- the decision -----------------------------------------------------
 * A copy is made only when the clip has at least this many times the
 * pixels of the copy: below that the saving does not pay for a second
 * file and a second generation of encoding. */
#define WALPE_SCENE_VIDEO_MIN_GAIN 1.5

/* Is a src_w x src_h clip to be played from a smaller copy, and how large
 * is the copy (*out_w x *out_h: the clip's shape, even sides)?
 *   box       the box of "video_resolution" for the monitors that show the
 *             scene (resolution.h walpe_video_box: auto = their largest
 *             render size). 0 x 0 (video_resolution none, or no monitor):
 *             no copy.
 *   view_w/h  the largest size the scene has drawn the clip at so far, in
 *             render pixels; 0 = not known yet. A layer drawn larger than
 *             the box (zoomed, or a scene that covers a monitor of another
 *             shape) gets a copy that covers what is drawn, in steps of an
 *             eighth of the clip's size, so that a copy is never shown
 *             enlarged by more than slack_pct per cent (the scene's
 *             texture slack, 2 by default).
 * The copy is the clip fitted into the box (never enlarged), or the larger
 * size the view asks for. FALSE (*out = 0): no copy - nothing to save, or
 * less than WALPE_SCENE_VIDEO_MIN_GAIN. */
gboolean walpe_scene_video_target (int src_w, int src_h, const WalpeVideoBox *box,
                                   int view_w, int view_h, double slack_pct,
                                   int *out_w, int *out_h);

/* ---- the copy in the cache --------------------------------------------
 * What the clip is: 32 hex digits from its place (path, offset, length)
 * and its content (the file's size and the first and last 64 KiB of the
 * clip, where an MP4 keeps its index). A scene package that is extracted
 * again keeps the name; a clip that was replaced gets another one. NULL
 * when the bytes cannot be read. g_free. */
char *walpe_scene_video_id (const char *path, guint64 off, guint64 len);
/* <user cache dir>/walpe/video/<id>-scene-<W>x<H>.mp4: next to the
 * converted copies of video wallpapers (import.h), under the same rules -
 * the daemon removes unfinished files at its start and copies nothing has
 * played for 90 days. Never in the wallpaper's own folder. g_free. */
char *walpe_scene_video_cache_file (const char *id, int w, int h);

/* ---- the copy's encoding ----------------------------------------------
 * WALPE_VIDEO_COPY_FAST=1 in the daemon's environment (read once; off
 * without it): copies made from then on are encoded for cheap decoding -
 * no B-frames, one reference picture, no CABAC, no deblocking filter, at a
 * rate factor that gives the same PSNR (import.h WalpeImportScene.fast):
 * about half the decoding work of every picture and two or three pictures
 * less in the decoder, for about twice the bytes on disk. Any thread.
 *
 * The two kinds are two files: a copy of this kind is
 * <id>-scene-<W>x<H>-fast.mp4, and walpe_scene_video_cache_file () gives
 * the name of the kind that is on - so a copy that was made the other way
 * is never taken for this one, both can be in the cache (each under the
 * rules above), and everything that is checked of a copy before it is
 * played is checked of either kind. */
gboolean walpe_scene_video_fast (void);
char *walpe_scene_video_cache_file_as (const char *id, int w, int h, gboolean fast);
/* is `file` (a name or a path) a copy of the kind encoded for cheap
 * decoding? By its name alone. */
gboolean walpe_scene_video_copy_is_fast (const char *file);

/* ---- a scene wants a copy ---------------------------------------------
 * The scene (a render thread) knows the clip, the daemon (main thread)
 * converts. walpe_scene_video_want (w, TRUE) says "this copy is wanted",
 * (w, FALSE) takes that back (the scene is gone, or wants something else
 * now); one FALSE for every TRUE. The daemon converts what is wanted, one
 * clip at a time, and stops a conversion nobody wants any more. Without a
 * hook (tests, tools) nothing happens. */
typedef struct {
    const char *src;        /* the file that holds the clip (absolute) */
    guint64 off, len;       /* the clip's bytes in it */
    const char *dst;        /* walpe_scene_video_cache_file (): its name says
                             * which encoding (..._copy_is_fast) */
    int w, h;               /* the copy's size */
    int matrix;             /* what the player converts the clip's colours with:
                             * 1 = BT.709, 6 = BT.601, 9 = BT.2020 (the H.264
                             * matrix_coefficients numbers); the copy is tagged
                             * with it, so that a small copy of an untagged HD
                             * clip is not taken for BT.601 */
} WalpeSceneVideoWant;
typedef void (*WalpeSceneVideoHook) (const WalpeSceneVideoWant *want, gboolean wanted);
void walpe_scene_video_set_hook (WalpeSceneVideoHook hook);
void walpe_scene_video_want (const WalpeSceneVideoWant *want, gboolean wanted);

G_END_DECLS
