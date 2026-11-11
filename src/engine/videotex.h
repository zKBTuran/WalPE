/* videotex.h - video as a scene texture (WE .tex with an embedded MP4).
 *
 * Built on the shared decoder of video_custom.c: sources are keyed by
 * file + byte range, so every object and every monitor showing the same
 * video texture shares ONE demuxer + decoder thread; each user (one per
 * scene per monitor) owns a GL texture in its render thread's context and
 * uploads only when the shared frame advanced. The decoder loops the clip
 * and sleeps while every user is paused. HAVE_FFMPEG builds only; the
 * stubs in the !HAVE_FFMPEG case return NULL. */
#pragma once
#include <glib.h>
#include <epoxy/gl.h>

typedef struct WalpeVideoTex WalpeVideoTex;

/* any thread: open/attach the shared source (no GL). NULL on error. */
WalpeVideoTex *walpe_video_tex_new (const char *path, guint64 off, guint64 len, GError **err);
/* render thread: upload the newest decoded frame if it changed; returns
 * the GL texture (0 until the first frame arrived) and its size */
GLuint walpe_video_tex_update (WalpeVideoTex *v, int *w, int *h);
/* frame interval of the stream in µs (0 = not known yet) */
gint64 walpe_video_tex_interval_us (WalpeVideoTex *v);
/* on-screen size (pixels) the whole frame is drawn at this frame; 0 = full
 * resolution needed (effects sample it). <= half the video size on every
 * user lets the shared decoder produce half-size frames. */
void walpe_video_tex_set_view (WalpeVideoTex *v, int w, int h);
void walpe_video_tex_set_paused (WalpeVideoTex *v, gboolean paused);
/* render thread (deletes the GL texture), then detaches from the source */
void walpe_video_tex_free (WalpeVideoTex *v);
