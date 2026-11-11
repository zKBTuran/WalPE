/* import.h - video import: normalize any ffmpeg-readable video into the one
 * format the renderer plays (H.264 High, yuv420p, <=1920x1080, MP4 with
 * faststart, no audio) by running the ffmpeg CLI as a child process.
 *
 * Everything is asynchronous on the thread-default main context: encoder
 * probe -> ffprobe -> ffmpeg transcode (or remux) -> thumbnail. ffmpeg is
 * spawned with an argv array (no shell), inputs/outputs carry a "file:"
 * prefix so names never parse as options or protocols. */
#pragma once
#include <glib.h>
#include <gio/gio.h>

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
 * 1920x1080, never upscaled, both dimensions even (>= 2) */
void     walpe_import_target_size (const WalpeMediaInfo *mi, int *out_w, int *out_h);
/* TRUE when the stream can be stream-copied into MP4 unchanged (H.264,
 * yuv420p 8-bit, even dims within 1920x1080, no rotation, square pixels) */
gboolean walpe_import_can_remux (const WalpeMediaInfo *mi);

/* argv for the ffmpeg run (NULL-terminated, g_strfreev). encoder is
 * ignored when remux is TRUE. */
char   **walpe_import_build_argv (const char *src, const char *dst_part,
                                  const WalpeMediaInfo *mi, const char *encoder,
                                  gboolean remux);

typedef struct _WalpeImport WalpeImport;
/* called once on the main context; ok=FALSE carries a human-readable error */
typedef void (*WalpeImportDone) (WalpeImport *imp, gboolean ok, const char *dst,
                                 const char *error, gpointer user);

/* start importing src (absolute path) into dest_dir (created if missing).
 * encoder: NULL = $WALPE_IMPORT_ENCODER or "libx264"; "h264_nvenc" uses the
 * NVIDIA encoder and falls back to libx264 if it fails. Returns NULL with
 * err set when nothing could start (e.g. ffmpeg/ffprobe not installed). */
WalpeImport *walpe_import_start (const char *src, const char *dest_dir,
                                 const char *encoder, WalpeImportDone done,
                                 gpointer user, GError **err);
/* the same pipeline writing exactly dst_file (absolute; its folder is
 * created, mode 0700), no thumbnail: the daemon's cache of converted
 * copies of video wallpapers the player cannot read itself. The source is
 * a Workshop file and nobody started this run in person, so it is bounded:
 * stopped (error passed to done) after WALPE_CONVERT_MAX_MIN minutes
 * (default 60), when the output passes WALPE_CONVERT_MAX_MB (default
 * 8192) or when the disk is nearly full; 0 = no limit. Needs a running
 * main loop on the default context, like the rest of the job. */
WalpeImport *walpe_import_start_to (const char *src, const char *dst_file,
                                    const char *encoder, WalpeImportDone done,
                                    gpointer user, GError **err);
/* where the converted copy of video src is cached:
 * <user cache dir>/walpe/video/<hash of path, size, mtime>.mp4 - a changed
 * or replaced source gets a new name. NULL when src cannot be stat'ed.
 * g_free. */
char        *walpe_video_cache_file (const char *src);
int          walpe_import_percent (WalpeImport *imp); /* -1 = unknown */
const char  *walpe_import_label   (WalpeImport *imp); /* source file stem */
const char  *walpe_import_mode    (WalpeImport *imp); /* "probe" "remux" "transcode:<enc>" */
/* kill ffmpeg + remove partial output; the done callback is NOT called */
void         walpe_import_cancel  (WalpeImport *imp);
/* free after done fired (or after cancel) */
void         walpe_import_free    (WalpeImport *imp);
/* remove the unfinished outputs ("<name>.mp4.part", regular files only) an
 * import that was killed with its daemon left in dir; returns how many.
 * Only for a moment when no import can be running (daemon start-up). */
guint        walpe_import_tidy_parts (const char *dir);

G_END_DECLS
