/* thumbs.h - asynchronous thumbnails for the library grid and preview.
 *
 * Decoding and scaling happen on a small worker pool, never on the GTK
 * main thread. Results are scaled to the exact pixel size asked for (no
 * per-frame downscale in the renderer), cached on disk under
 * $XDG_CACHE_HOME/walpe/thumbs (key: source path + mtime + size + target
 * size + mode) and in a bounded in-memory LRU.
 *
 * Animated previews: a GIF source shows its first frame like any other
 * picture and, when animated previews are on (thumbs_set_anim_mode), plays
 * while its picture can be seen - mapped, inside the scrolled viewport, in
 * the active window (and under the pointer in "hover" mode). Frames are
 * decoded once on the worker pool at the size asked for, at most
 * 20 per second and 48 per file, shared by every picture showing that file
 * at that size, and kept in their own byte budget (least recently shown
 * out first; a picture whose animation was dropped shows the still again).
 * Nothing ticks while no animated picture is visible, and after 20 s of
 * that the frames are freed (decoded again when they play next).
 *
 * Sources are untrusted (a preview named by a downloaded project.json):
 * files over 16384 px a side / 64 MP (still) or 4 MP per frame (animated)
 * are not decoded, and only JPEG, PNG, GIF, BMP, WebP and AVIF go to a
 * decoder at all (thumbs_format_allowed), whatever the file is called. */
#pragma once
#include <gtk/gtk.h>

typedef enum {
    THUMB_COVER,     /* fill w x h, centre-cropped (grid tiles) */
    THUMB_CONTAIN,   /* fit inside w x h, aspect kept (selection preview) */
} ThumbMode;

/* Show the thumbnail of src (NULL = none) on pic at w x h device pixels.
 * Synchronous on a memory-cache hit; otherwise pic is cleared and filled
 * when the worker finishes - unless pic was re-targeted in the meantime
 * (recycled list items) or destroyed. */
void thumbs_set (GtkPicture *pic, const char *src, int w, int h, ThumbMode mode);
/* forget the pending target of pic (list item unbind) and clear it */
void thumbs_clear (GtkPicture *pic);

/* The window that shows the pictures went off the screen, or is back
 * (winstate.h: another workspace, an icon, suspended - not: without the
 * focus). Off the screen the pictures kept in memory for a quick return
 * (the LRU above: every preview looked at, up to its budget) are given
 * back; what a picture shows stays, and anything else is read again from
 * the disk cache when it is asked for. */
void thumbs_window_hidden (gboolean hidden);
/* diagnostics: bytes of decoded stills the memory cache holds */
gsize thumbs_cache_bytes (void);

/* animated previews: WALPE_PREVIEW_ALWAYS / _HOVER / _OFF (common/config.h) */
void thumbs_set_anim_mode (int mode);
int  thumbs_get_anim_mode (void);
/* a picture whose still image the caller sets itself (Workshop tiles):
 * play `file` on it under the same rules when it is an animated GIF.
 * unwatch before the picture shows something else. */
void thumbs_anim_watch (GtkPicture *pic, const char *file, int w, int h, ThumbMode mode);
void thumbs_anim_unwatch (GtkPicture *pic);
/* something that decides visibility changed (the caller's own cases;
 * scrolling, window focus and mapping are followed by thumbs.c itself) */
void thumbs_anim_kick (void);
/* is a file gdk-pixbuf identifies as this format (gdk_pixbuf_format_get_name)
 * decoded for a thumbnail? */
gboolean thumbs_format_allowed (const char *gdk_pixbuf_format_name);
/* diagnostics: animations loaded / playing, bytes of frames held */
void thumbs_anim_stats (guint *loaded, guint *playing, gsize *bytes);
