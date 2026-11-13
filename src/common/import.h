/* import.h - video import: normalize any ffmpeg-readable video into the one
 * format the renderer plays (H.264 High, yuv420p, MP4 with faststart, no
 * audio) by running the ffmpeg CLI as a child process. How large the
 * result may be is the caller's to say (a WalpeVideoBox, resolution.h:
 * the setting "video_resolution" and the monitors' render sizes decide);
 * nothing here knows a screen size.
 *
 * Everything is asynchronous on the thread-default main context: encoder
 * probe -> ffprobe -> ffmpeg transcode (or remux) -> thumbnail. ffmpeg is
 * spawned with an argv array (no shell), inputs/outputs carry a "file:"
 * prefix so names never parse as options or protocols. */
#pragma once
#include <glib.h>
#include <gio/gio.h>
#include "common/resolution.h"   /* WalpeVideoBox */

G_BEGIN_DECLS

/* what ffprobe reported about the first video stream */
typedef struct {
    gboolean has_video;
    char *codec;          /* "h264", "hevc", "vp9", ... */
    char *pix_fmt;        /* "yuv420p", "yuv420p10le", ... */
    char *format_name;    /* container, e.g. "mov,mp4,m4a,3gp,3g2,mj2" */
    int width, height;    /* coded size (before rotation / SAR) */
    int sar_num, sar_den; /* sample aspect ratio (1:1 when unknown) */
    int rotation;         /* display rotation in degrees (0/90/180/270) */
    double duration;      /* seconds, 0 = unknown */
} WalpeMediaInfo;

void     walpe_media_info_clear (WalpeMediaInfo *mi);
/* parse ffprobe -print_format json -show_format -show_streams output */
gboolean walpe_media_info_parse (const char *json, WalpeMediaInfo *mi);

/* output geometry: display size (SAR + rotation applied) fitted inside
 * box (NULL or 0 x 0: no limit), never upscaled, both dimensions even
 * (>= 2) */
void     walpe_import_target_size (const WalpeMediaInfo *mi, const WalpeVideoBox *box,
                                   int *out_w, int *out_h);
/* TRUE when the stream can be stream-copied into MP4 unchanged (H.264,
 * yuv420p 8-bit, even dims within box, no rotation, square pixels) */
gboolean walpe_import_can_remux (const WalpeMediaInfo *mi, const WalpeVideoBox *box);

/* argv for the ffmpeg run (NULL-terminated, g_strfreev). encoder is
 * ignored when remux is TRUE. */
char   **walpe_import_build_argv (const char *src, const char *dst_part,
                                  const WalpeMediaInfo *mi, const char *encoder,
                                  gboolean remux, const WalpeVideoBox *box);

/* the same run for an import into the library (walpe_import_start): the
 * video's first sound track is kept, as AAC (192 kbit/s, two channels) -
 * the imported file is all that is left of the video, and the player takes
 * a video wallpaper's sound from the wallpaper's own file. A video without
 * a sound track is imported as before. */
char   **walpe_import_library_argv (const char *src, const char *dst_part,
                                    const WalpeMediaInfo *mi, const char *encoder,
                                    gboolean remux, const WalpeVideoBox *box);

typedef struct _WalpeImport WalpeImport;
/* called once on the main context; ok=FALSE carries a human-readable error */
typedef void (*WalpeImportDone) (WalpeImport *imp, gboolean ok, const char *dst,
                                 const char *error, gpointer user);

/* start importing src (absolute path) into dest_dir (created if missing).
 * encoder: NULL = $WALPE_IMPORT_ENCODER or "libx264"; "h264_nvenc" uses the
 * NVIDIA encoder and falls back to libx264 if it fails. box: the largest
 * picture the result may have (copied; NULL or 0 x 0 = the source's size).
 * Returns NULL with err set when nothing could start (e.g. ffmpeg/ffprobe
 * not installed). */
WalpeImport *walpe_import_start (const char *src, const char *dest_dir,
                                 const char *encoder, const WalpeVideoBox *box,
                                 WalpeImportDone done, gpointer user, GError **err);
/* the same pipeline writing exactly dst_file (absolute; its folder is
 * created, mode 0700), no thumbnail: the daemon's cache of converted
 * copies of video wallpapers the player cannot read itself. The source is
 * a Workshop file and nobody started this run in person, so it is bounded:
 * stopped (error passed to done) after WALPE_CONVERT_MAX_MIN minutes
 * (default 60), when the output passes WALPE_CONVERT_MAX_MB (default
 * 8192) or when the disk is nearly full; 0 = no limit. Needs a running
 * main loop on the default context, like the rest of the job. */
WalpeImport *walpe_import_start_to (const char *src, const char *dst_file,
                                    const char *encoder, const WalpeVideoBox *box,
                                    WalpeImportDone done, gpointer user, GError **err);
/* The copy of a scene's video (common/scenevideo.h): the clip is the MP4 at
 * [off, off + len) of src - a WE .tex with a video in it - and the copy is
 * exactly w x h. Otherwise the pipeline and the format of
 * walpe_import_start_to, with its limits (WALPE_CONVERT_MAX_MIN, ..._MB,
 * the disk), and with these differences:
 *   - the clip is read through FFmpeg's "subfile" protocol (the byte range
 *     of a file; no temporary copy), as an MP4 with H.264 in it and as
 *     nothing else, its pictures as they are stored (no rotation, no pixel
 *     aspect ratio: a scene stretches them over its layer);
 *   - only 8-bit 4:2:0 H.264 in limited range is converted (what the copy
 *     is): anything else ends with an error that says so and no output;
 *   - it is never a stream copy;
 *   - the copy is tagged with the colour matrix `matrix` (1 BT.709, 5 / 6
 *     BT.601, 9 BT.2020; anything else: no tag) and limited range; its
 *     samples are scaled, not converted;
 *   - ffmpeg runs at the lowest CPU and disk priority (nice 19, SCHED_IDLE,
 *     idle I/O class) and ends with the daemon;
 *   - with `fast` the copy is encoded for cheap decoding (scenevideo.h
 *     WALPE_VIDEO_COPY_FAST): libx264 with -crf 17 -refs 1 -bf 0 -tune
 *     fastdecode in the place of -crf 19. Only libx264 can do that: with
 *     another encoder the job ends with an error that says so and no
 *     output. Without `fast` the arguments are what they always were.
 * Source frame rate, no audio, as every import. */
typedef struct {
    guint64 off, len;
    int w, h;
    int matrix;
    gboolean fast;
} WalpeImportScene;
WalpeImport *walpe_import_start_scene (const char *src, const WalpeImportScene *sc,
                                       const char *dst_file, WalpeImportDone done, gpointer user,
                                       GError **err);
/* argv of that run (tests; NULL-terminated, g_strfreev) */
char   **walpe_import_scene_argv (const char *src, const WalpeImportScene *sc, const char *dst_part,
                                  const char *encoder);
/* where the converted copy of video src is cached:
 * <user cache dir>/walpe/video/<hash of path, size, mtime>[-<W>x<H> |
 * -full].mp4 - a changed or replaced source gets a new name, and a copy
 * made for another box has another one (a copy fitted into 1920 x 1080 is
 * not what a monitor set to 4K is to show). The 1920 x 1080 box has the
 * plain name the copies had while that size was built in, so those stay
 * in use. NULL when src cannot be stat'ed. g_free. */
char        *walpe_video_cache_file (const char *src, const WalpeVideoBox *box);
/* The daemon's start (nothing is being converted then): tidies
 * <user cache dir>/walpe/video. Removed are
 *  - what a conversion or a sound track left unfinished ("*.part"),
 *  - converted copies and sound tracks that nothing played for max_days
 *    (their modification time: the player touches what it plays),
 *  - and, only when WALPE_VIDEO_CACHE_MB=<MiB> is set (default: no limit
 *    on what the files add up to), the ones unused for the longest time
 *    until the rest is within that size. The one used last is never
 *    removed for the size: a single copy larger than the limit would be
 *    made again at every start.
 * said (may be NULL) is told each file and why. Returns how many went. */
typedef void (*WalpeCacheSaid) (const char *path, const char *why, gpointer user);
int          walpe_video_cache_tidy (int max_days, WalpeCacheSaid said, gpointer user);
/* The sound track of a video wallpaper. The player (engine/video.c) does
 * not decode it out of the video: it is taken out once, with ffmpeg, into
 * <user cache dir>/walpe/video/<hash of path, size, mtime>-sound.flac
 * (lossless, 16 bit, two channels; the scene sound player reads it), and
 * that file is what sounds from then on. A file of length 0 under that
 * name means "this video has no sound track". NULL when src is not a
 * regular file. g_free. */
char        *walpe_video_sound_file (const char *src);
/* Makes that file: runs ffmpeg and waits for it - for a thread that may
 * wait (the sound player's own, when the track is about to be heard for
 * the first time), never the main or a render thread. The child runs at
 * the lowest CPU and disk priority, ends with this process, reads src
 * under the limits of every conversion (local file, container formats
 * only) and is stopped after WALPE_CONVERT_MAX_MIN minutes. 1 = dst is
 * there; 0 = the video has no sound track (dst is the empty file that says
 * so); -1 = failed, *err (g_free) says why and nothing is left behind. */
int          walpe_video_sound_extract (const char *src, const char *dst, char **err);
/* The same for a caller that may lose interest: stop (user) is asked ten or
 * twenty times a second while the call waits - for its turn (one track is
 * taken out at a time) and for ffmpeg. Once it says TRUE the call ends: ffmpeg
 * is killed, nothing is left behind, -2 is returned (*err stays NULL). */
int          walpe_video_sound_extract_stoppable (const char *src, const char *dst,
                                                  gboolean (*stop) (gpointer user), gpointer user, char **err);
/* The process is about to end (or to start itself again): every call of the
 * two above that runs or waits gives up as if its caller had said stop -
 * ffmpeg and what it started is killed, the ".part" removed, -2 - and none
 * begins from now on. Waits up to max_ms for the calls to have returned
 * (0: tells them only; a call notices within a tenth of a second). Without
 * this the process was gone before the waiting thread looked: its ffmpeg
 * was left to the kernel (ended with its parent - not the real program
 * behind a wrapper script) and the ".part" to the next start. Any thread. */
void         walpe_video_sound_extract_end (guint max_ms);
/* argv of that run (tests; NULL-terminated, g_strfreev) */
char       **walpe_video_sound_argv (const char *src, const char *dst_part);
int          walpe_import_percent (WalpeImport *imp); /* -1 = unknown */
const char  *walpe_import_label   (WalpeImport *imp); /* source file stem */
const char  *walpe_import_mode    (WalpeImport *imp); /* "probe" "remux" "transcode:<enc>" */
/* kill ffmpeg + remove partial output; the done callback is NOT called */
void         walpe_import_cancel  (WalpeImport *imp);
/* free after done fired (or after cancel) */
void         walpe_import_free    (WalpeImport *imp);
/* The encoder of a cancelled conversion ends a moment after the cancel and
 * may create its unfinished file in that moment. The file is removed again
 * when the child has ended: from the main loop, or here, for whoever ends
 * before the loop gets to it (the daemon when it exits). Waits at most
 * max_ms; returns how many such children were still alive then. */
guint        walpe_import_settle  (guint max_ms);
/* remove the unfinished outputs ("<name>.mp4.part", regular files only) an
 * import that was killed with its daemon left in dir; returns how many.
 * Only for a moment when no import can be running (daemon start-up). */
guint        walpe_import_tidy_parts (const char *dir);
/* whether this process is (still) the child of process `parent`. A child
 * of an import asks between fork and exec: a parent that died before the
 * child could ask the kernel to end it with its parent has been replaced
 * by another by then. (Here for the test.) */
gboolean     walpe_import_child_of (gint64 parent);

G_END_DECLS
