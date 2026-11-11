/* modules.h - optional heavy backends as dlopen'd modules.
 *
 * WebKitGTK (+ GTK 4, GStreamer, JavaScriptCore) and FFmpeg (libavcodec and
 * its codec libraries) were linked into walpe-daemon at load time: every
 * daemon mapped and relocated ~180 shared libraries even for a static image.
 * They now live in walpe-web.so / walpe-video.so, loaded on first use from
 * $WALPE_MODULE_DIR, the daemon's own directory (build tree) or
 * WALPE_MODULEDIR (install: <libdir>/walpe). The modules call back into the
 * daemon through the WALPE_API functions (src/common/api.h). Each module
 * exports one entry point returning its function table. A module, once
 * loaded, stays loaded (WebKit and FFmpeg do not survive dlclose). */
#pragma once
#include <glib.h>
#include "content.h"
#include "video.h"
#include "videotex.h"
#include "web.h"
#include "scenesound.h"   /* WalpeSoundDecoder */

/* 4 (version 0.12): the web module's web_pointer slot became web_input and
 * web_input_status was appended; WalpeVideo gained `fit` (video.h). */
/* 5: Mp4Demux / Mp4Sample changed (samples are read
 * with pread, rotation + pixel aspect), WalpeVideo gained `no_frame` and
 * WalpeContent `first_frame_pending`.
 * 6: 5 + web_info appended to the web module's table +
 * WalpeContent `frame_unchanged`, `surface_lost`, `set_render_scale` (later
 * also `surface_exposed`, `picture_serial`) and WalpeLoadOpts
 * `render_scale` / `no_tex_slack`. One number for all of it:
 * daemon, walpe-video.so and walpe-web.so come from the same build.
 * 7: WalpeContent gained `surface_exposed` and `picture_serial`.
 * 8: Mp4Sample gained `sync`, Mp4Demux `has_sync`
 * (the video module reads both through mp4_demux_*), and the video
 * module needs walpe_yuv_coeffs, walpe_video_hwdec,
 * walpe_video_note_decoder and walpe_video_note_frame from the daemon.
 * 9: WalpeContent gained `set_quality`,
 * `quality_stats`, `quality_describe` and `stats_wanted`, WalpeLoadOpts
 * `quality` (the quality presets).
 * 10 (version 0.12.1): the video module takes walpe_video_limit,
 * walpe_video_frame_limit, walpe_video_upload_mode and
 * walpe_video_note_upload from the daemon, and the daemon defines
 * XSetSelectionOwner for what walpe-web.so loads. A module with another
 * number than the daemon's is refused.
 * 12: WalpeWebInput has the keyboard kinds and their fields (key, pressed,
 * mods, keymap_fd, keymap_size) and the web module's table ends with
 * web_host; the video module takes walpe_video_hwdec_api and
 * walpe_video_zerocopy from the daemon; WalpeContent has render_w,
 * render_h, scale_filter and tex_limit, WalpeLoadOpts video_box_w /
 * video_box_h, WalpeVideo render_w / render_h; WalpeVideoModule ends with
 * `sound` (the decoders of compressed sound files, sounddec.c); the modules
 * take walpe_gl_tex_image_bgra and walpe_gl_tex_sub_image_bgra from the
 * daemon. (Comments elsewhere that say "module ABI 11" name one of these
 * changes.)
 *
 * NEVER UNLOADED. A module that was loaded stays loaded until the process
 * ends (modload.c closes a library only when it has no entry symbol).
 * Scene sound depends on that: its player threads are detached, nobody
 * waits for them, and a thread may still be inside `sound.read` or
 * `sound.close` after its wallpaper, its scene and its group are gone
 * (scenesound.c). Whoever adds a dlclose of walpe-video.so has to wait for
 * walpe_sound_threads () == 0 first - and those threads may be blocked in
 * the sound server without a time limit. */
#define WALPE_MODULE_ABI 12

typedef struct {
    int abi;
    WalpeContent *(*web_new) (const char *path, const WalpeLoadOpts *opts, GError **err);
    void (*web_input) (gpointer viewer, const WalpeWebInput *in);   /* ABI 4 (was web_pointer) */
    void (*web_stats) (guint *snapshots);
    char *(*web_media_missing) (void);
    char *(*web_error) (void);      /* ABI 2: WebKit start-up test result */
    int (*web_probe_run) (void);    /* ABI 2: `walpe-daemon --web-probe` */
    char *(*web_input_status) (void);   /* ABI 4 */
    char *(*web_info) (void);           /* ABI 5: what renders WebGL in pages */
    char *(*web_host) (void);           /* ABI 11: where the pages' windows live */
} WalpeWebModule;

typedef struct {
    int abi;
    WalpeVideo *(*custom_new) (const char *path, GError **err);
    WalpeVideoTex *(*tex_new) (const char *path, guint64 off, guint64 len, GError **err);
    GLuint (*tex_update) (WalpeVideoTex *v, int *w, int *h);
    gint64 (*tex_interval_us) (WalpeVideoTex *v);
    void (*tex_set_view) (WalpeVideoTex *v, int w, int h);
    void (*tex_set_paused) (WalpeVideoTex *v, gboolean paused);
    void (*tex_free) (WalpeVideoTex *v);
    WalpeSoundDecoder sound;        /* ABI 11: MP3 / FLAC / Ogg for scene sound */
} WalpeVideoModule;

/* module entry points (defined in web.c / video_custom.c) */
const WalpeWebModule *walpe_web_module_entry (void);
const WalpeVideoModule *walpe_video_module_entry (void);

/* core: the loaded module's table; load = dlopen it if not loaded yet.
 * NULL when not built, not found, or (load FALSE) not loaded yet. */
const WalpeWebModule *walpe_web_module (gboolean load);
const WalpeVideoModule *walpe_video_module (gboolean load);
/* "web=loaded video=not loaded" style summary for status (g_free) */
char *walpe_modules_status (void);
/* A module is code that runs inside the daemon. path is taken as one only
 * when nobody but this user or root can have put it there: the file and
 * its directory belong to this user or root, the file is not writable by
 * others, and neither is the directory unless it is sticky (there only
 * the owner can replace a file). Group permissions are not looked at
 * (a group of one's own with umask 002 is common). FALSE: *why (optional,
 * g_free) says what is wrong. */
gboolean walpe_module_file_trusted (const char *path, char **why);
