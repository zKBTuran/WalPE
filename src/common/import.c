/* import.c - video import through the ffmpeg CLI (see import.h).
 *
 * The renderer only plays H.264 (yuv420p, 8-bit) in MP4 through its own
 * demuxer, and it is silent (no audio path exists), so every import is
 * normalized to exactly that: video stream only (-an), H.264 High profile,
 * yuv420p, fitted inside the caller's box without upscaling (no box: the
 * source's size), source frame rate (the mp4 muxer keeps the input
 * timestamps), +faststart. Inputs that already match are remuxed (-c:v
 * copy: instant and lossless). */
#include "import.h"
#include "util.h"
#include <glib/gstdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <math.h>
#include <sched.h>
#include <signal.h>
#include <unistd.h>
#include <sys/statvfs.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

#define CRF "19"   /* visually lossless-ish for wallpapers, ~1/3 of crf 15 size */
/* WalpeImportScene.fast: no B-frames, one reference, CAVLC and no deblocking
 * filter cost about a decibel at the same rate factor; two steps lower
 * gives the PSNR of CRF again (measured on a 3840x2160 clip made 1920x1080:
 * 45.46 dB at 19, 45.50 dB this way at 17, for 2.1 times the bytes) */
#define CRF_FAST "17"
/* the box of the converted copies written before the box was a setting
 * (walpe_video_cache_file: their names stay valid for it) */
#define CACHE_V1_W 1920
#define CACHE_V1_H 1080

/* What ffmpeg / ffprobe may open while reading a source. The source is an
 * untrusted file (a Workshop download): a "video" that is really an HLS
 * playlist, a concat script or an image-sequence pattern makes ffmpeg read
 * other local files or fetch URLs and put their content into the output.
 * Only the local-file protocol and real container / elementary-stream
 * demuxers are allowed (no hls, concat, dash, image2, subtitles, lavfi). */
#define IN_PROTOCOLS "file"
#define IN_FORMATS "mov,matroska,avi,mpegts,mpeg,flv,asf,ogg,gif,ivf,nut,rm,wtv,mxf," \
                   "h264,hevc,m4v,mpegvideo,av1,obu,vc1,dv,bink,smk"
/* ... and the largest picture its decoders may allocate (libavcodec's
 * max_pixels): a file of a few kilobytes can declare 16384 x 16384 and
 * more, which costs the converter gigabytes. 40 megapixels is above the
 * top level of H.264, HEVC, VP9 and AV1 (8192 x 4352); the player's own
 * limit is the same (engine/video_custom.c). */
#define IN_MAX_PIXELS "40000000"

/* A clip inside another file (a scene's video texture: an MP4 somewhere in
 * a .tex) is read through FFmpeg's "subfile" protocol, which is the byte
 * range of a file and nothing else; the only container such a clip can be
 * is the one the player reads. */
#define IN_PROTOCOLS_RANGE "file,subfile"
#define IN_FORMATS_RANGE "mov"

static void push_input_limits (GPtrArray *a, gboolean range)
{
    g_ptr_array_add (a, g_strdup ("-max_pixels"));
    g_ptr_array_add (a, g_strdup (IN_MAX_PIXELS));
    g_ptr_array_add (a, g_strdup ("-protocol_whitelist"));
    g_ptr_array_add (a, g_strdup (range ? IN_PROTOCOLS_RANGE : IN_PROTOCOLS));
    g_ptr_array_add (a, g_strdup ("-format_whitelist"));
    g_ptr_array_add (a, g_strdup (range ? IN_FORMATS_RANGE : IN_FORMATS));
}

/* The input as ffmpeg and ffprobe are given it: "file:<src>", or for a
 * clip that is a byte range of src "subfile,,start,<n>,end,<m>,,:file:<src>".
 * The name comes after the last option of the protocol and behind "file:",
 * so nothing in it is read as an option or as another protocol. */
static char *input_url (const char *src, const WalpeImportScene *sc)
{
    if (!sc)
        return g_strconcat ("file:", src, NULL);
    return g_strdup_printf ("subfile,,start,%" G_GUINT64_FORMAT ",end,%" G_GUINT64_FORMAT
                            ",,:file:%s", sc->off, sc->off + sc->len, src);
}

/* FFmpeg's name of a colour matrix (H.264 matrix_coefficients), NULL =
 * not one a copy is tagged with */
static const char *matrix_name (int m)
{
    return m == 1 ? "bt709" : m == 5 ? "bt470bg" : m == 6 ? "smpte170m" : m == 9 ? "bt2020nc" : NULL;
}

/* ------------------------------------------------------------ probe info */

void walpe_media_info_clear (WalpeMediaInfo *mi)
{
    g_free (mi->codec);
    g_free (mi->pix_fmt);
    g_free (mi->format_name);
    memset (mi, 0, sizeof (*mi));
}

static gboolean parse_ratio (const char *s, int *num, int *den)
{
    int a = 0, b = 0;
    if (!s || sscanf (s, "%d:%d", &a, &b) != 2 || a <= 0 || b <= 0)
        return FALSE;
    *num = a;
    *den = b;
    return TRUE;
}

/* ffprobe numbers come as JSON numbers or strings depending on the field */
static double json_num_any (JsonObject *o, const char *k, double dflt)
{
    JsonNode *n = o ? json_object_get_member (o, k) : NULL;
    if (!n || json_node_get_node_type (n) != JSON_NODE_VALUE)
        return dflt;
    GType t = json_node_get_value_type (n);
    if (t == G_TYPE_STRING) {
        const char *s = json_node_get_string (n);
        char *end = NULL;
        double v = g_ascii_strtod (s, &end);
        return (end && end != s) ? v : dflt;
    }
    if (t == G_TYPE_INT64 || t == G_TYPE_DOUBLE)
        return json_node_get_double (n);
    return dflt;
}

gboolean walpe_media_info_parse (const char *json, WalpeMediaInfo *mi)
{
    memset (mi, 0, sizeof (*mi));
    mi->sar_num = mi->sar_den = 1;
    JsonNode *root = json ? wpj_parse_data (json, -1) : NULL;
    if (!root)
        return FALSE;
    if (json_node_get_node_type (root) != JSON_NODE_OBJECT) {
        json_node_unref (root);
        return FALSE;
    }
    JsonObject *ro = json_node_get_object (root);
    JsonArray *streams = wpj_arr (ro, "streams");
    for (guint i = 0; i < wpj_arr_len (streams); i++) {
        JsonObject *st = wpj_arr_obj (streams, i);
        if (!st || g_strcmp0 (wpj_str (st, "codec_type", "video"), "video"))
            continue;
        mi->has_video = TRUE;
        mi->codec = g_strdup (wpj_str (st, "codec_name", ""));
        mi->pix_fmt = g_strdup (wpj_str (st, "pix_fmt", ""));
        /* (a size that is no int - 1e30, "1e999" - is no size: walpe_d2i,
         * not the cast, which is undefined for it) */
        mi->width = walpe_d2i (json_num_any (st, "width", 0));
        mi->height = walpe_d2i (json_num_any (st, "height", 0));
        parse_ratio (wpj_str (st, "sample_aspect_ratio", NULL), &mi->sar_num, &mi->sar_den);
        /* rotation: display-matrix side data (ffprobe >= 5) or the legacy
         * "rotate" tag */
        double rot = 0;
        JsonArray *sd = wpj_arr (st, "side_data_list");
        for (guint j = 0; j < wpj_arr_len (sd); j++) {
            JsonObject *e = wpj_arr_obj (sd, j);
            if (e && json_object_has_member (e, "rotation"))
                rot = json_num_any (e, "rotation", 0);
        }
        if (rot == 0)
            rot = json_num_any (wpj_obj (st, "tags"), "rotate", 0);
        /* (an angle that is no number, or one lround () cannot hold: none) */
        rot = isfinite (rot) ? fmod (rot, 360.0) : 0;
        int r = ((int) lround (rot) % 360 + 360) % 360;
        mi->rotation = (r + 45) / 90 * 90 % 360;
        mi->duration = json_num_any (st, "duration", 0);
        break;
    }
    JsonObject *fmt = wpj_obj (ro, "format");
    if (fmt) {
        mi->format_name = g_strdup (wpj_str (fmt, "format_name", ""));
        double d = json_num_any (fmt, "duration", 0);
        if (d > 0)
            mi->duration = d;
    }
    json_node_unref (root);
    return TRUE;
}

/* ------------------------------------------------------------ decisions */

void walpe_import_target_size (const WalpeMediaInfo *mi, const WalpeVideoBox *box,
                               int *out_w, int *out_h)
{
    /* a stream that does not say its size: the box's, else something small
     * (ffmpeg scales to it; such a stream rarely decodes at all) */
    double w = mi->width > 0 ? mi->width : box && box->w > 0 ? box->w : 2;
    double h = mi->height > 0 ? mi->height : box && box->h > 0 ? box->h : 2;
    if (mi->sar_num > 0 && mi->sar_den > 0 && mi->sar_num != mi->sar_den)
        w = w * mi->sar_num / mi->sar_den;   /* anamorphic -> square pixels */
    if (mi->rotation == 90 || mi->rotation == 270) {
        double t = w;                        /* ffmpeg autorotates first */
        w = h;
        h = t;
    }
    /* fitted inside the box, never upscaled, even sides */
    walpe_video_box_fit (box, w, h, out_w, out_h);
}

gboolean walpe_import_can_remux (const WalpeMediaInfo *mi, const WalpeVideoBox *box)
{
    if (!mi->has_video || g_strcmp0 (mi->codec, "h264"))
        return FALSE;
    /* the renderer converts only 8-bit 4:2:0 (yuvj420p = full-range tag) */
    if (g_strcmp0 (mi->pix_fmt, "yuv420p") && g_strcmp0 (mi->pix_fmt, "yuvj420p"))
        return FALSE;
    if (mi->width < 2 || mi->height < 2 || !walpe_video_box_holds (box, mi->width, mi->height) ||
        (mi->width & 1) || (mi->height & 1))
        return FALSE;
    if (mi->rotation != 0)                   /* player ignores display matrices */
        return FALSE;
    if (mi->sar_num != mi->sar_den)          /* ...and sample aspect ratios */
        return FALSE;
    return TRUE;
}

static void push (GPtrArray *a, const char *s)
{
    g_ptr_array_add (a, g_strdup (s));
}

/* sc: the clip is a scene's (walpe_import_start_scene), else NULL */
static char **build_argv (const char *src, const char *dst_part, const WalpeMediaInfo *mi,
                          const char *encoder, gboolean remux, const WalpeVideoBox *box,
                          const WalpeImportScene *sc, gboolean sound)
{
    GPtrArray *a = g_ptr_array_new ();
    push (a, "ffmpeg");
    push (a, "-hide_banner");
    push (a, "-nostdin");
    push (a, "-y");
    push (a, "-v");
    push (a, "error");
    push_input_limits (a, sc != NULL);
    if (sc) {
        /* read the way the player reads it: an MP4 with H.264 in it, and
         * the pictures as they are stored (a scene stretches them over its
         * layer; the player applies no display matrix) */
        push (a, "-f");
        push (a, "mov");
        push (a, "-c:v");
        push (a, "h264");
        push (a, "-noautorotate");
    }
    push (a, "-i");
    g_ptr_array_add (a, input_url (src, sc));
    push (a, "-map");
    push (a, "0:v:0");
    if (sound) {
        push (a, "-map");
        push (a, "0:a:0?");         /* its first sound track, where it has one */
    }
    if (remux) {
        push (a, "-c:v");
        push (a, "copy");
    } else {
        int w = 0, h = 0;
        const char *mx = sc ? matrix_name (sc->matrix) : NULL;
        if (sc) {
            w = sc->w;
            h = sc->h;
        } else {
            walpe_import_target_size (mi, box, &w, &h);
        }
        push (a, "-vf");
        /* (a scene's copy says which matrix its samples are in - the one
         * the player took for the clip - since the player decides that of
         * an untagged stream by its size, and the copy is smaller; the
         * samples themselves are only scaled) */
        if (mx)
            g_ptr_array_add (a, g_strdup_printf ("scale=%d:%d:flags=bicubic,setsar=1,format=yuv420p,"
                                                 "setparams=colorspace=%s:range=tv", w, h, mx));
        else
            g_ptr_array_add (a, g_strdup_printf ("scale=%d:%d:flags=bicubic,setsar=1,format=yuv420p", w, h));
        if (mx) {
            push (a, "-colorspace");
            push (a, mx);
            push (a, "-color_range");
            push (a, "tv");
        }
        push (a, "-c:v");
        push (a, encoder);
        if (!g_strcmp0 (encoder, "h264_nvenc")) {
            push (a, "-preset");  push (a, "p5");
            push (a, "-tune");    push (a, "hq");
            push (a, "-rc");      push (a, "vbr");
            push (a, "-cq");      push (a, CRF);
            push (a, "-b:v");     push (a, "0");
        } else if (!g_strcmp0 (encoder, "libopenh264")) {
            push (a, "-b:v");     push (a, "12M");
        } else if (sc && sc->fast) { /* libx264, a copy that is cheap to decode */
            push (a, "-preset");  push (a, "medium");
            push (a, "-crf");     push (a, CRF_FAST);
            push (a, "-refs");    push (a, "1");
            push (a, "-bf");      push (a, "0");
            push (a, "-tune");    push (a, "fastdecode");
        } else { /* libx264 */
            push (a, "-preset");  push (a, "medium");
            push (a, "-crf");     push (a, CRF);
        }
        push (a, "-profile:v");
        push (a, "high");
        push (a, "-pix_fmt");
        push (a, "yuv420p");
    }
    if (sound) {
        /* An import into the library keeps the sound track: the imported
         * file is all that is left of the video, and the player takes a
         * video wallpaper's sound from the wallpaper's own file
         * (walpe_video_sound_extract). AAC, which every ffmpeg encodes and
         * MP4 holds, also in a stream copy of the pictures: a track of
         * another codec may be one MP4 does not take. */
        push (a, "-c:a");
        push (a, "aac");
        push (a, "-b:a");
        push (a, "192k");
        push (a, "-ac");
        push (a, "2");
    } else {
        push (a, "-an");             /* the pictures only: a copy made to be decoded */
    }
    push (a, "-sn");
    push (a, "-dn");
    push (a, "-map_metadata");
    push (a, "-1");
    push (a, "-movflags");
    push (a, "+faststart");
    push (a, "-progress");
    push (a, "pipe:1");
    push (a, "-nostats");
    push (a, "-f");
    push (a, "mp4");                 /* explicit: ".part" hides the extension */
    g_ptr_array_add (a, g_strconcat ("file:", dst_part, NULL));
    g_ptr_array_add (a, NULL);
    return (char **) g_ptr_array_free (a, FALSE);
}

char **walpe_import_build_argv (const char *src, const char *dst_part,
                                const WalpeMediaInfo *mi, const char *encoder,
                                gboolean remux, const WalpeVideoBox *box)
{
    return build_argv (src, dst_part, mi, encoder, remux, box, NULL, FALSE);
}

char **walpe_import_library_argv (const char *src, const char *dst_part,
                                  const WalpeMediaInfo *mi, const char *encoder,
                                  gboolean remux, const WalpeVideoBox *box)
{
    return build_argv (src, dst_part, mi, encoder, remux, box, NULL, TRUE);
}

char **walpe_import_scene_argv (const char *src, const WalpeImportScene *sc, const char *dst_part,
                                const char *encoder)
{
    return build_argv (src, dst_part, NULL, encoder, FALSE, NULL, sc, FALSE);
}

/* ------------------------------------------------------------ the job */

typedef enum { ST_ENCODERS, ST_PROBE, ST_ENCODE, ST_THUMB, ST_DONE } Stage;

struct _WalpeImport {
    int refs;
    Stage stage;
    gboolean cancelled;
    GCancellable *cancel;
    GSubprocess *proc;               /* current child */
    GDataInputStream *out;           /* ffmpeg stdout (progress + errors) */
    char *src, *dst, *dst_part, *thumb, *label;
    char *encoder;                   /* requested encoder */
    char *used_encoder;              /* encoder of the current run */
    char mode[48];
    gboolean remux, tried_fallback;
    WalpeVideoBox box;               /* the largest picture the output may have; 0 x 0 = any */
    WalpeMediaInfo mi;
    int percent;
    GString *errtail;                /* last non-progress output lines */
    WalpeImportDone done;
    gpointer user;
    /* conversions on open (walpe_import_start_to): the source is whatever
     * a Workshop item ships and nobody asked for this run in person, so
     * it is bounded (import_watch) */
    gint64 start_us;
    gint64 max_us, max_bytes;        /* 0 = no limit */
    guint watch_id;
    char *limit_msg;                 /* a limit was hit: why the child was stopped */
    /* the copy of a scene's video (walpe_import_start_scene): the clip is
     * a byte range of src, the output has exactly the size asked for, and
     * ffmpeg runs at the lowest priority */
    gboolean is_scene;
    gboolean keep_sound;             /* an import into the library: the sound track stays */
    WalpeImportScene scene;
};

static char *encoder_list;           /* cached `ffmpeg -encoders` output */

/* The encoder of a cancelled conversion.
 *
 * walpe_import_cancel () tells the child to end and removes the unfinished
 * file. The child is not gone at that moment: GLib sends the signal from
 * its worker thread, and a process that sits in the kernel - an ffmpeg at
 * idle CPU and disk priority on a busy machine is there for a long time,
 * opening its output - finishes that call first. A file it creates after
 * the removal stayed: "<copy>.mp4.part" in the cache after a clean exit of
 * the daemon, until the next start tidied it away.
 *
 * So the child is remembered until it has really ended, and its file is
 * removed once more then: from the main loop when that runs
 * (dying_cb), or by walpe_import_settle (), which the daemon calls when it
 * ends and waits a bounded time. Not when another job writes the same file
 * by then (the same copy asked for again at once): that file is its. */
typedef struct {
    GSubprocess *proc;
    char *part;
} Dying;
static GPtrArray *dying;             /* Dying *; main thread */
static GPtrArray *jobs;              /* every WalpeImport there is; main thread */

static void dying_free (gpointer p)
{
    Dying *d = p;
    g_object_unref (d->proc);
    g_free (d->part);
    g_free (d);
}

static WalpeImport *imp_ref (WalpeImport *imp)
{
    imp->refs++;
    return imp;
}

static void imp_unref (WalpeImport *imp)
{
    if (--imp->refs > 0)
        return;
    if (jobs)
        g_ptr_array_remove_fast (jobs, imp);
    g_clear_object (&imp->out);
    g_clear_object (&imp->proc);
    g_clear_object (&imp->cancel);
    walpe_media_info_clear (&imp->mi);
    g_free (imp->src);
    g_free (imp->dst);
    g_free (imp->dst_part);
    g_free (imp->thumb);
    g_free (imp->label);
    g_free (imp->encoder);
    g_free (imp->used_encoder);
    if (imp->errtail)
        g_string_free (imp->errtail, TRUE);
    g_free (imp->limit_msg);
    g_free (imp);
}

static void watch_stop (WalpeImport *imp)
{
    if (imp->watch_id) {
        g_source_remove (imp->watch_id);
        imp->watch_id = 0;
        imp_unref (imp);            /* the timer's reference */
    }
}

static void finish (WalpeImport *imp, gboolean ok, const char *error)
{
    watch_stop (imp);
    imp->stage = ST_DONE;
    imp->percent = ok ? 100 : -1;
    if (!ok)
        g_remove (imp->dst_part);
    if (imp->done && !imp->cancelled)
        imp->done (imp, ok, ok ? imp->dst : NULL, error, imp->user);
}

static gboolean have_encoder (const char *name)
{
    if (!encoder_list || !name)
        return FALSE;
    /* lines look like " V....D libx264              libx264 H.264 ..." */
    char *pat = g_strdup_printf (" %s ", name);
    gboolean hit = strstr (encoder_list, pat) != NULL;
    g_free (pat);
    return hit;
}

/* requested encoder if ffmpeg has it, else the first available H.264 one */
static const char *pick_encoder (const char *want)
{
    if (have_encoder (want))
        return want;
    static const char *chain[] = { "libx264", "h264_nvenc", "libopenh264", NULL };
    for (int i = 0; chain[i]; i++)
        if (have_encoder (chain[i]))
            return chain[i];
    return NULL;
}

static void start_encode (WalpeImport *imp);
static void start_thumb (WalpeImport *imp);

/* stdout = -progress key=value lines, stderr merged in (errors) */
static void read_line_cb (GObject *o, GAsyncResult *res, gpointer user)
{
    WalpeImport *imp = user;
    GDataInputStream *din = G_DATA_INPUT_STREAM (o);
    /* raw bytes, made valid here. ffmpeg's messages quote what the file
     * holds (codec and tag names, metadata); the _utf8 variant returned
     * an error for the first line that is not UTF-8 - taken for the end
     * of the output, so nothing read the pipe any more, ffmpeg blocked
     * writing to it once it was full, and the job never ended. */
    char *raw = g_data_input_stream_read_line_finish (din, res, NULL, NULL);
    char *line = raw ? g_utf8_make_valid (raw, -1) : NULL;
    g_free (raw);
    if (!line || imp->cancelled || din != imp->out) {
        g_free (line);
        imp_unref (imp);
        return;
    }
    const char *v = NULL;
    if (g_str_has_prefix (line, "out_time_us="))
        v = line + 12;
    else if (g_str_has_prefix (line, "out_time_ms="))
        v = line + 12; /* microseconds despite the name */
    if (v) {
        if (imp->mi.duration > 0) {
            /* (a value that is no number - "nan" in the progress text, or
             * infinity over an infinite duration - is 0 %: CLAMP lets a NaN
             * through and its cast to int is undefined, INT_MIN on x86-64
             * and 0 on aarch64) */
            double pc = g_ascii_strtod (v, NULL) / (imp->mi.duration * 1e6) * 100.0;
            imp->percent = pc == pc ? (int) CLAMP (pc, 0.0, 99.0) : 0;
        }
    } else if (g_str_has_prefix (line, "progress=end")) {
        imp->percent = 99; /* faststart rewrite, then exit */
    } else if (!strchr (line, '=') || strchr (line, ' ')) {
        /* not a progress key: an ffmpeg error/warning line */
        if (imp->errtail->len > 1024)
            g_string_erase (imp->errtail, 0, imp->errtail->len - 512);
        if (imp->errtail->len)
            g_string_append (imp->errtail, " | ");
        g_string_append (imp->errtail, g_strstrip (line));
    }
    g_free (line);
    g_data_input_stream_read_line_async (din, G_PRIORITY_DEFAULT, imp->cancel, read_line_cb, imp);
}

static void encode_wait_cb (GObject *o, GAsyncResult *res, gpointer user)
{
    WalpeImport *imp = user;
    GError *err = NULL;
    gboolean ok = g_subprocess_wait_check_finish (G_SUBPROCESS (o), res, &err);
    if (imp->cancelled) {
        g_clear_error (&err);
        imp_unref (imp);
        return;
    }
    g_clear_object (&imp->proc);
    g_clear_object (&imp->out); /* pending line read sees out change and stops */
    if (imp->limit_msg) {       /* stopped by import_watch: no retry, no output */
        g_warning ("import: %s: %s", imp->src, imp->limit_msg);
        finish (imp, FALSE, imp->limit_msg);
        g_clear_error (&err);
        imp_unref (imp);
        return;
    }
    if (ok && g_rename (imp->dst_part, imp->dst) != 0) {
        int e = errno;
        g_set_error (&err, G_IO_ERROR, g_io_error_from_errno (e), "rename %s: %s",
                     imp->dst_part, g_strerror (e));
        ok = FALSE;
    }
    if (ok) {
        g_message ("import: %s -> %s (%s)", imp->src, imp->dst, imp->mode);
        if (imp->thumb)
            start_thumb (imp);
        else
            finish (imp, TRUE, NULL);   /* cache copy: no library thumbnail */
    } else {
        g_remove (imp->dst_part);
        const char *enc = imp->remux ? pick_encoder (imp->encoder) : NULL;
        if (enc) {
            /* the stream could not be copied as it is (timestamps the MP4
             * muxer refuses, a damaged index): encode it instead of giving
             * up on a file ffmpeg can read */
            g_warning ("import: copying the stream of %s failed (%s); encoding it with %s", imp->src,
                       imp->errtail->len ? imp->errtail->str : (err ? err->message : "?"), enc);
            imp->remux = FALSE;
            g_free (imp->used_encoder);
            imp->used_encoder = g_strdup (enc);
            g_string_truncate (imp->errtail, 0);
            start_encode (imp);
        } else if (!imp->remux && !imp->tried_fallback &&
                   g_strcmp0 (imp->used_encoder, "libx264") && have_encoder ("libx264")) {
            /* NVENC (no GPU / driver mismatch) -> retry once with software x264 */
            g_warning ("import: %s failed (%s); retrying with libx264", imp->used_encoder,
                       imp->errtail->len ? imp->errtail->str : (err ? err->message : "?"));
            imp->tried_fallback = TRUE;
            g_free (imp->used_encoder);
            imp->used_encoder = g_strdup ("libx264");
            g_string_truncate (imp->errtail, 0);
            start_encode (imp);
        } else {
            char *msg = g_strdup_printf ("ffmpeg failed: %s",
                                         imp->errtail->len ? imp->errtail->str
                                                           : (err ? err->message : "unknown error"));
            g_warning ("import: %s: %s", imp->src, msg);
            finish (imp, FALSE, msg);
            g_free (msg);
        }
    }
    g_clear_error (&err);
    imp_unref (imp);
}

/* Between fork and exec of a conversion nobody asked for in person (only
 * calls that may be made there): the lowest CPU and disk priority - it gets
 * the time nothing else wants. What the system refuses stays as it was.
 * background 0 = the daemon's priority (a conversion somebody waits for; or
 * WALPE_CONVERT_BACKGROUND=0: for tests on a machine that is never idle,
 * where such a child would not get to run).
 *
 * For every child that reads the source: it does not outlive the daemon.
 * Only the daemon gives its output a name (the ".part" file is renamed
 * when the child is done) and only the daemon watches its limits
 * (import_watch). A child that went on after its daemon was killed wrote a
 * file nobody would ever use, for as long and as much as the source made
 * it, and the next daemon removed that file by name at its start while the
 * child still filled it.
 *
 * And it leads a process group of its own, which is what is killed when
 * the child is to end (child_kill, proc_kill): `ffmpeg` in PATH may be a
 * script that starts the real program without `exec` - distributions and
 * users have such wrappers - and a signal for the process that was started
 * here ended the script alone. The real ffmpeg then ran on as nobody's
 * child after a cancel, a limit or a replaced wallpaper: to the end of its
 * source, into a ".part" file that had been removed (or, when it had not
 * opened its output yet, into a new one that stayed). The group holds the
 * child and what it starts, and nothing of this process. (What the kernel
 * does for a parent that dies - the lines below - still reaches the
 * started process only: behind such a wrapper the real ffmpeg outlives a
 * daemon that is killed, until its source ends.) */
typedef struct {
    gboolean background;    /* the lowest priority (a scene's copy) */
    gint64 parent;          /* the process that starts the child */
} ChildSetup;

gboolean walpe_import_child_of (gint64 parent)
{
    return (gint64) getppid () == parent;
}

static void background_child_setup (gpointer user);

/* ends a child that was started with background_child_setup, and all it
 * started: its process group (which it leads; the spawn call returns after
 * the child's exec, so the group is there), then the child itself in case
 * it could not make one */
static void child_kill (pid_t pid)
{
    if (pid <= 1)
        return;
    if (kill (-pid, SIGKILL) != 0) {
        /* no such group */
    }
    kill (pid, SIGKILL);
}

/* the same for a GSubprocess (which knows its child until it is collected) */
static void proc_kill (GSubprocess *proc)
{
    const char *id = g_subprocess_get_identifier (proc);
    const gint64 pid = id ? g_ascii_strtoll (id, NULL, 10) : 0;
    if (pid > 1 && kill ((pid_t) -pid, SIGKILL) != 0) {
        /* no such group */
    }
    g_subprocess_force_exit (proc);
}

/* a child that reads or writes a video: started with background_child_setup */
static GSubprocess *proc_spawn (const char *const *argv, GSubprocessFlags flags, gboolean background, GError **err)
{
    GSubprocessLauncher *l = g_subprocess_launcher_new (flags);
    /* (read by the child between fork and exec, inside spawnv) */
    ChildSetup cs = { background, (gint64) getpid () };
    g_subprocess_launcher_set_child_setup (l, background_child_setup, &cs, NULL);
    GSubprocess *p = g_subprocess_launcher_spawnv (l, argv, err);
    g_object_unref (l);
    return p;
}

static void background_child_setup (gpointer user)
{
    const ChildSetup *cs = user;
    if (setpgid (0, 0) != 0) {
        /* stays in the daemon's group: only the child itself can be ended */
    }
    if (cs->background) {
        if (setpriority (PRIO_PROCESS, 0, 19) != 0) {
            /* stays at the daemon's */
        }
#ifdef SCHED_IDLE
        {
            struct sched_param sp = { 0 };
            if (sched_setscheduler (0, SCHED_IDLE, &sp) != 0) {
                /* nice 19 it is */
            }
        }
#endif
#if defined(__linux__) && defined(SYS_ioprio_set)
        /* IOPRIO_WHO_PROCESS, this process, IOPRIO_CLASS_IDLE << IOPRIO_CLASS_SHIFT */
        if (syscall (SYS_ioprio_set, 1, 0, 3 << 13) != 0) {
            /* the disk scheduler's default */
        }
#endif
    }
#if defined(__linux__) && defined(PR_SET_PDEATHSIG)
    prctl (PR_SET_PDEATHSIG, SIGKILL);
    /* ... unless the parent was gone before that line was reached (the
     * child had only just been made): the signal is sent when a parent
     * dies, not for one that is dead already. A different parent now means
     * it went away: no program is started. */
    if (!walpe_import_child_of (cs->parent))
        _exit (127);
#endif
}

static void start_encode (WalpeImport *imp)
{
    imp->stage = ST_ENCODE;
    imp->percent = -1;
    if (imp->remux)
        g_snprintf (imp->mode, sizeof (imp->mode), "remux");
    else
        g_snprintf (imp->mode, sizeof (imp->mode), "transcode:%s", imp->used_encoder);
    char **argv = build_argv (imp->src, imp->dst_part, &imp->mi, imp->used_encoder, imp->remux,
                              &imp->box, imp->is_scene ? &imp->scene : NULL, imp->keep_sound);
    GError *err = NULL;
    {
        const char *bg = g_getenv ("WALPE_CONVERT_BACKGROUND");
        imp->proc = proc_spawn ((const char *const *) argv,
                                G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE,
                                imp->is_scene && !(bg && *bg == '0'), &err);
    }
    g_strfreev (argv);
    if (!imp->proc) {
        finish (imp, FALSE, err->message);
        g_error_free (err);
        return;
    }
    imp->out = g_data_input_stream_new (g_subprocess_get_stdout_pipe (imp->proc));
    g_data_input_stream_read_line_async (imp->out, G_PRIORITY_DEFAULT, imp->cancel,
                                         read_line_cb, imp_ref (imp));
    g_subprocess_wait_check_async (imp->proc, NULL, encode_wait_cb, imp_ref (imp));
}

static void thumb_wait_cb (GObject *o, GAsyncResult *res, gpointer user)
{
    WalpeImport *imp = user;
    GError *err = NULL;
    gboolean ok = g_subprocess_wait_check_finish (G_SUBPROCESS (o), res, &err);
    g_clear_error (&err);
    g_clear_object (&imp->proc);
    if (!imp->cancelled) {
        /* best-effort: an empty/broken thumbnail is worse than none */
        GStatBuf st;
        if (!ok || g_stat (imp->thumb, &st) != 0 || st.st_size == 0)
            g_remove (imp->thumb);
        finish (imp, TRUE, NULL);
    }
    imp_unref (imp);
}

/* library grid thumbnail: <stem>.jpg next to the video (store.c pairs them) */
static void start_thumb (WalpeImport *imp)
{
    imp->stage = ST_THUMB;
    char ss[32];
    double d = imp->mi.duration;
    g_ascii_formatd (ss, sizeof (ss), "%.2f", d > 2.0 ? MIN (d * 0.1, 3.0) : 0.0);
    char *in = g_strconcat ("file:", imp->dst, NULL);
    char *out = g_strconcat ("file:", imp->thumb, NULL);
    /* -update 1: the name is the file to write, as it stands. Without it
     * the image2 muxer reads a "%d" in the name as the number of the frame:
     * the thumbnail of "take %03d.mp4" was written to "take 001.jpg" (over
     * a picture of that name, if there was one) and the video had none. */
    const char *argv[] = { "ffmpeg", "-hide_banner", "-nostdin", "-y", "-v", "error",
                           "-ss", ss, "-i", in, "-frames:v", "1",
                           "-vf", "scale=440:-2", "-q:v", "3", "-f", "image2", "-update", "1",
                           out, NULL };
    imp->proc = proc_spawn (argv, G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                            FALSE, NULL);
    g_free (in);
    g_free (out);
    if (!imp->proc) {
        finish (imp, TRUE, NULL);
        return;
    }
    g_subprocess_wait_check_async (imp->proc, NULL, thumb_wait_cb, imp_ref (imp));
}

static void probe_cb (GObject *o, GAsyncResult *res, gpointer user)
{
    WalpeImport *imp = user;
    char *out = NULL, *errs = NULL;
    GError *err = NULL;
    g_subprocess_communicate_utf8_finish (G_SUBPROCESS (o), res, &out, &errs, &err);
    gboolean exited_ok = !err && g_subprocess_get_if_exited (G_SUBPROCESS (o)) &&
                         g_subprocess_get_exit_status (G_SUBPROCESS (o)) == 0;
    g_clear_object (&imp->proc);
    if (imp->cancelled) {
        /* nothing */
    } else if (imp->limit_msg) {
        g_warning ("import: %s: %s", imp->src, imp->limit_msg);
        finish (imp, FALSE, imp->limit_msg);
    } else if (!exited_ok || !walpe_media_info_parse (out, &imp->mi) || !imp->mi.has_video) {
        char *msg = g_strdup_printf ("not a readable video (%s)",
                                     err ? err->message
                                         : (errs && *errs) ? g_strstrip (errs) : "no video stream");
        finish (imp, FALSE, msg);
        g_free (msg);
    } else if (imp->is_scene && (g_strcmp0 (imp->mi.codec, "h264") ||
                                 g_strcmp0 (imp->mi.pix_fmt, "yuv420p"))) {
        /* a copy is 8-bit 4:2:0 in limited range: only a clip that is the
         * same gets one, so that nothing but its size changes */
        char *msg = g_strdup_printf ("left as it is: %s %s is not 8-bit 4:2:0 H.264 in limited range",
                                     imp->mi.codec ? imp->mi.codec : "?",
                                     imp->mi.pix_fmt ? imp->mi.pix_fmt : "?");
        finish (imp, FALSE, msg);
        g_free (msg);
    } else if (imp->is_scene && (imp->scene.w > imp->mi.width || imp->scene.h > imp->mi.height)) {
        char *msg = g_strdup_printf ("left as it is: the clip is %dx%d, not larger than the "
                                     "%dx%d asked for", imp->mi.width, imp->mi.height,
                                     imp->scene.w, imp->scene.h);
        finish (imp, FALSE, msg);
        g_free (msg);
    } else if (imp->is_scene) {
        /* (a copy that is cheap to decode is libx264's whatever encoder is
         * set: the others have no such settings, and no copy is better
         * than one of the other kind under this kind's name) */
        const char *enc = !imp->scene.fast ? pick_encoder (imp->encoder)
                        : have_encoder ("libx264") ? "libx264" : NULL;
        if (!enc && imp->scene.fast) {
            finish (imp, FALSE, "left as it is: a copy that is cheap to decode "
                                "(WALPE_VIDEO_COPY_FAST) needs an ffmpeg with the libx264 "
                                "encoder");
        } else if (!enc) {
            finish (imp, FALSE, "this ffmpeg build has no H.264 encoder (libx264, h264_nvenc "
                                "or libopenh264)");
        } else {
            imp->remux = FALSE;     /* never: the point is another size */
            imp->used_encoder = g_strdup (enc);
            g_message ("import: %s (%" G_GUINT64_FORMAT " bytes at %" G_GUINT64_FORMAT "): h264 "
                       "%dx%d -> %s %dx%d%s", imp->src, imp->scene.len, imp->scene.off,
                       imp->mi.width, imp->mi.height, enc, imp->scene.w, imp->scene.h,
                       imp->scene.fast ? ", encoded for cheap decoding" : "");
            start_encode (imp);
        }
    } else {
        imp->remux = walpe_import_can_remux (&imp->mi, &imp->box);
        const char *enc = imp->remux ? "copy" : pick_encoder (imp->encoder);
        if (!enc) {
            finish (imp, FALSE, "this ffmpeg build has no H.264 encoder (libx264, h264_nvenc "
                                "or libopenh264): install an ffmpeg that has one (on Gentoo: "
                                "USE=x264 on media-video/ffmpeg)");
        } else {
            if (!imp->remux && g_strcmp0 (enc, imp->encoder))
                g_message ("import: encoder %s unavailable, using %s", imp->encoder, enc);
            imp->used_encoder = g_strdup (enc);
            int tw = 0, th = 0;
            walpe_import_target_size (&imp->mi, &imp->box, &tw, &th);
            g_message ("import: %s: %s %s %dx%d rot=%d -> %s %dx%d", imp->src,
                       imp->mi.codec, imp->mi.pix_fmt, imp->mi.width, imp->mi.height,
                       imp->mi.rotation, imp->remux ? "remux" : enc,
                       imp->remux ? imp->mi.width : tw, imp->remux ? imp->mi.height : th);
            start_encode (imp);
        }
    }
    g_free (out);
    g_free (errs);
    g_clear_error (&err);
    imp_unref (imp);
}

static void start_probe (WalpeImport *imp)
{
    imp->stage = ST_PROBE;
    g_snprintf (imp->mode, sizeof (imp->mode), "probe");
    char *in = input_url (imp->src, imp->is_scene ? &imp->scene : NULL);
    const char *argv[] = { "ffprobe", "-v", "error", "-max_pixels", IN_MAX_PIXELS,
                           "-protocol_whitelist", IN_PROTOCOLS, "-format_whitelist", IN_FORMATS,
                           "-print_format", "json",
                           "-show_format", "-show_streams", "-select_streams", "v:0",
                           in, NULL };
    /* (a scene's clip: the byte range, as an MP4 and nothing else) */
    const char *argv_scene[] = { "ffprobe", "-v", "error", "-max_pixels", IN_MAX_PIXELS,
                                 "-protocol_whitelist", IN_PROTOCOLS_RANGE,
                                 "-format_whitelist", IN_FORMATS_RANGE, "-f", "mov",
                                 "-print_format", "json",
                                 "-show_format", "-show_streams", "-select_streams", "v:0",
                                 in, NULL };
    GError *err = NULL;
    imp->proc = proc_spawn (imp->is_scene ? argv_scene : argv,
                            G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE, FALSE, &err);
    g_free (in);
    if (!imp->proc) {
        finish (imp, FALSE, err->message);
        g_error_free (err);
        return;
    }
    g_subprocess_communicate_utf8_async (imp->proc, NULL, imp->cancel, probe_cb, imp_ref (imp));
}

static void encoders_cb (GObject *o, GAsyncResult *res, gpointer user)
{
    WalpeImport *imp = user;
    char *out = NULL;
    g_subprocess_communicate_utf8_finish (G_SUBPROCESS (o), res, &out, NULL, NULL);
    g_clear_object (&imp->proc);
    if (!encoder_list && out)
        encoder_list = g_steal_pointer (&out);
    g_free (out);
    if (!imp->cancelled && imp->limit_msg)
        finish (imp, FALSE, imp->limit_msg);
    else if (!imp->cancelled)
        start_probe (imp);
    imp_unref (imp);
}

/* ------------------------------------------------- limits of a conversion
 * A conversion on open runs ffmpeg on a file from a Workshop item, with
 * nobody watching: it starts when the wallpaper is chosen (or with the
 * daemon, for a saved wallpaper) and wrote into ~/.cache for as long and
 * as much as the file made it. Every WATCH_S seconds the job is checked
 * and its child stopped (the partial output removed) when
 *   - it has run for more than WALPE_CONVERT_MAX_MIN minutes (default 60),
 *   - its output is larger than WALPE_CONVERT_MAX_MB (default 8192), or
 *   - the file system it writes to has less than LOW_DISK_MB left.
 * 0 switches a limit off. `walpe-ctl add` (walpe_import_start: a file the
 * user picked, with the progress in front of them) is not limited. */
#define WATCH_S 2
#define LOW_DISK_MB 256

static gint64 limit_env (const char *name, gint64 dflt)
{
    const char *e = g_getenv (name);
    char *end = NULL;
    gint64 v = e && *e ? g_ascii_strtoll (e, &end, 10) : dflt;
    if (e && *e && (end == e || *end || v < 0))
        return dflt;        /* not a number: the default */
    return MIN (v, 100000000);  /* (the callers multiply: no overflow) */
}

/* MiB that have to stay free on the file system a conversion or a sound
 * track is written to (WALPE_CONVERT_FREE_MB, 0 = no floor) */
static gint64 low_disk_mb (void)
{
    return limit_env ("WALPE_CONVERT_FREE_MB", LOW_DISK_MB);
}

/* MiB free for this user where dir is; G_MAXINT64 when nobody can tell */
static gint64 disk_free_mb (const char *dir)
{
    struct statvfs vfs;
    return statvfs (dir, &vfs) == 0 ? (gint64) ((guint64) vfs.f_bavail * vfs.f_frsize / (1024 * 1024))
                                    : G_MAXINT64;
}

static gboolean import_watch (gpointer user)
{
    WalpeImport *imp = user;
    char *why = NULL;
    if (!imp->cancelled && imp->stage != ST_DONE && !imp->limit_msg) {
        GStatBuf st;
        gint64 size = g_stat (imp->dst_part, &st) == 0 ? (gint64) st.st_size : 0;
        char *dir = g_path_get_dirname (imp->dst_part);
        gint64 free_mb = disk_free_mb (dir);
        const gint64 floor_mb = low_disk_mb ();
        g_free (dir);
        if (imp->max_us > 0 && g_get_monotonic_time () - imp->start_us > imp->max_us)
            why = g_strdup_printf ("the conversion was stopped: it ran for more than %d minutes "
                                   "(WALPE_CONVERT_MAX_MIN)",
                                   (int) (imp->max_us / (60 * G_USEC_PER_SEC)));
        else if (imp->max_bytes > 0 && size > imp->max_bytes)
            why = g_strdup_printf ("the conversion was stopped: its output grew beyond %d MB "
                                   "(WALPE_CONVERT_MAX_MB)",
                                   (int) (imp->max_bytes / (1024 * 1024)));
        else if (size > 0 && free_mb < floor_mb)
            why = g_strdup_printf ("the conversion was stopped: less than %d MB are left on "
                                   "the disk it writes to", (int) floor_mb);
        /* (between two children there is nothing to stop: look again) */
        if (!why || !imp->proc) {
            g_free (why);
            return G_SOURCE_CONTINUE;
        }
        imp->limit_msg = why;
        proc_kill (imp->proc);      /* its wait callback reports limit_msg */
    }
    imp->watch_id = 0;
    imp_unref (imp);
    return G_SOURCE_REMOVE;
}

/* <dir>/<stem>.mp4, or <stem>-2.mp4 ... when a different file already
 * holds the name (re-importing the same source replaces its output) */
static char *unique_dst (const char *dir, const char *stem, const char *src)
{
    for (int n = 1; n < 1000; n++) {
        char *name = n == 1 ? g_strconcat (stem, ".mp4", NULL)
                            : g_strdup_printf ("%s-%d.mp4", stem, n);
        char *p = g_build_filename (dir, name, NULL);
        g_free (name);
        if (!g_file_test (p, G_FILE_TEST_EXISTS) || !g_strcmp0 (p, src))
            return p;
        g_free (p);
    }
    return g_strdup_printf ("%s/%s-%" G_GINT64_FORMAT ".mp4", dir, stem, g_get_real_time ());
}

char *walpe_video_cache_file (const char *src, const WalpeVideoBox *box)
{
    GStatBuf st;
    if (!src || g_stat (src, &st) != 0)
        return NULL;
    char *key = g_strdup_printf ("%s\n%" G_GINT64_FORMAT "\n%" G_GINT64_FORMAT ".%ld", src,
                                 (gint64) st.st_size, (gint64) st.st_mtim.tv_sec,
                                 (long) st.st_mtim.tv_nsec);
    char *sum = g_compute_checksum_for_string (G_CHECKSUM_SHA256, key, -1);
    sum[32] = 0;
    /* the box is part of what the copy is: "<hash>-<W>x<H>.mp4", or
     * "<hash>-full.mp4" without a limit. Not for the box every copy was
     * fitted into before there was a choice (CACHE_V1_W x CACHE_V1_H):
     * those copies are plain "<hash>.mp4", and are still what that box
     * asks for. This is a file name rule only; it limits nothing. */
    const int bw = box ? MAX (box->w, 0) : 0, bh = box ? MAX (box->h, 0) : 0;
    char *name = bw == CACHE_V1_W && bh == CACHE_V1_H ? g_strconcat (sum, ".mp4", NULL)
               : bw > 0 && bh > 0 ? g_strdup_printf ("%s-%dx%d.mp4", sum, bw, bh)
               : g_strconcat (sum, "-full.mp4", NULL);
    char *p = g_build_filename (g_get_user_cache_dir (), "walpe", "video", name, NULL);
    g_free (name);
    g_free (sum);
    g_free (key);
    return p;
}

/* ------------------------------------------------- the cache's size */

typedef struct { char *path; gint64 size, mtime; } CacheFile;

static int cache_file_cmp (gconstpointer a, gconstpointer b)
{
    const CacheFile *x = a, *y = b;
    return x->mtime < y->mtime ? -1 : x->mtime > y->mtime ? 1 : strcmp (x->path, y->path);
}

int walpe_video_cache_tidy (int max_days, WalpeCacheSaid said, gpointer user)
{
    char *dir = g_build_filename (g_get_user_cache_dir (), "walpe", "video", NULL);
    GDir *d = g_dir_open (dir, 0, NULL);
    const gint64 now = g_get_real_time () / G_USEC_PER_SEC;
    const gint64 max_bytes = limit_env ("WALPE_VIDEO_CACHE_MB", 0) * 1024 * 1024;
    GArray *kept = g_array_new (FALSE, FALSE, sizeof (CacheFile));
    gint64 total = 0;
    int gone = 0;
    const char *n;
    while (d && (n = g_dir_read_name (d))) {
        const gboolean part = g_str_has_suffix (n, ".mp4.part") || g_str_has_suffix (n, "-sound.flac.part");
        const gboolean flac = g_str_has_suffix (n, "-sound.flac");
        if (!part && !flac && !g_str_has_suffix (n, ".mp4"))
            continue;
        char *p = g_build_filename (dir, n, NULL);
        GStatBuf st;
        if (g_lstat (p, &st) != 0 || !S_ISREG (st.st_mode)) {
            g_free (p);
            continue;
        }
        if (part || (max_days > 0 && now - (gint64) st.st_mtime > (gint64) max_days * 86400)) {
            if (said)
                said (p, part ? "unfinished conversion"
                           : flac ? "sound track not used for a long time"
                           : "converted copy not used for a long time", user);
            g_remove (p);
            g_free (p);
            gone++;
            continue;
        }
        CacheFile f = { p, (gint64) st.st_size, (gint64) st.st_mtime };
        g_array_append_val (kept, f);
        total += f.size;
    }
    if (d)
        g_dir_close (d);
    if (max_bytes > 0 && total > max_bytes) {
        g_array_sort (kept, cache_file_cmp);    /* the longest unused first */
        for (guint i = 0; i + 1 < kept->len && total > max_bytes; i++) {
            CacheFile *f = &g_array_index (kept, CacheFile, i);
            if (said)
                said (f->path, g_str_has_suffix (f->path, ".flac")
                               ? "sound track, the cache is over WALPE_VIDEO_CACHE_MB"
                               : "converted copy, the cache is over WALPE_VIDEO_CACHE_MB", user);
            if (g_remove (f->path) == 0) {
                total -= f->size;
                gone++;
            }
        }
    }
    for (guint i = 0; i < kept->len; i++)
        g_free (g_array_index (kept, CacheFile, i).path);
    g_array_free (kept, TRUE);
    g_free (dir);
    return gone;
}

/* ------------------------------------------- the sound track of a video */

char *walpe_video_sound_file (const char *src)
{
    GStatBuf st;
    if (!src || g_stat (src, &st) != 0 || !S_ISREG (st.st_mode))
        return NULL;
    char *key = g_strdup_printf ("%s\n%" G_GINT64_FORMAT "\n%" G_GINT64_FORMAT ".%ld", src,
                                 (gint64) st.st_size, (gint64) st.st_mtim.tv_sec,
                                 (long) st.st_mtim.tv_nsec);
    char *sum = g_compute_checksum_for_string (G_CHECKSUM_SHA256, key, -1);
    sum[32] = 0;
    char *name = g_strconcat (sum, "-sound.flac", NULL);
    char *p = g_build_filename (g_get_user_cache_dir (), "walpe", "video", name, NULL);
    g_free (name);
    g_free (sum);
    g_free (key);
    return p;
}

char **walpe_video_sound_argv (const char *src, const char *dst_part)
{
    GPtrArray *a = g_ptr_array_new ();
    push (a, "ffmpeg");
    push (a, "-hide_banner");
    push (a, "-nostdin");
    push (a, "-y");
    push (a, "-v");
    push (a, "error");
    push_input_limits (a, FALSE);
    push (a, "-i");
    g_ptr_array_add (a, input_url (src, NULL));
    push (a, "-map");
    push (a, "0:a:0");
    push (a, "-vn");
    push (a, "-sn");
    push (a, "-dn");
    push (a, "-map_metadata");
    push (a, "-1");
    push (a, "-ac");
    push (a, "2");
    /* one rate, whatever the track declares: the player takes 8000..192000
     * Hz (scenesound.h), and a track outside that would be written and then
     * never played; it also bounds what a second of a track costs on disk */
    push (a, "-ar");
    push (a, "48000");
    push (a, "-c:a");
    push (a, "flac");
    push (a, "-sample_fmt");
    push (a, "s16");
    /* what the player takes of a sound file at most (scenesound.h: six
     * hours, 2 GiB) */
    push (a, "-t");
    push (a, "21600");
    push (a, "-fs");
    push (a, "2000000000");
    push (a, "-f");
    push (a, "flac");                /* explicit: ".part" hides the extension */
    g_ptr_array_add (a, g_strconcat ("file:", dst_part, NULL));
    g_ptr_array_add (a, NULL);
    return (char **) g_ptr_array_free (a, FALSE);
}

int walpe_video_sound_extract (const char *src, const char *dst, char **err)
{
    return walpe_video_sound_extract_stoppable (src, dst, NULL, NULL, err);
}

int walpe_video_sound_extract_stoppable (const char *src, const char *dst, gboolean (*stop) (gpointer user),
                                         gpointer user, char **err)
{
    static GMutex one;      /* one at a time in this process: they share the ".part" of a video */
    int res = -1;
    if (err)
        *err = NULL;
    /* (a caller that can be stopped does not wait behind another video's
     * track for longer than it wants to) */
    while (!g_mutex_trylock (&one)) {
        if (stop && stop (user))
            return -2;
        g_usleep (50000);
    }
    if (g_file_test (dst, G_FILE_TEST_EXISTS)) {    /* made while this call waited */
        GStatBuf st;
        res = g_stat (dst, &st) == 0 && st.st_size > 0 ? 1 : 0;
        g_mutex_unlock (&one);
        return res;
    }
    char *dir = g_path_get_dirname (dst);
    char *part = g_strconcat (dst, ".part", NULL);
    char **argv = walpe_video_sound_argv (src, part);
    ChildSetup cs = { TRUE, (gint64) getpid () };
    GPid pid = 0;
    int efd = -1;
    GError *e = NULL;
    GString *said = g_string_new (NULL);
    gboolean timed_out = FALSE, low_disk = FALSE, stopped = FALSE;
    int status = -1;
    /* the floor every conversion has (import_watch): six hours of sound
     * are up to 2 GB of FLAC, from a video file that may be a hundredth of
     * that, and nothing looked at what the disk had left. Not begun below
     * the floor, stopped when the disk gets there. */
    const gint64 floor_mb = low_disk_mb ();
    if (g_mkdir_with_parents (dir, 0700) != 0) {
        g_string_printf (said, "%s: %s", dir, g_strerror (errno));
    } else if (floor_mb > 0 && disk_free_mb (dir) < floor_mb) {
        g_string_printf (said, "not taken out: less than %d MB are left on the disk of the cache",
                         (int) floor_mb);
    } else if (!g_spawn_async_with_pipes (NULL, argv, NULL,
                                          G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD |
                                          G_SPAWN_STDOUT_TO_DEV_NULL,
                                          background_child_setup, &cs, &pid, NULL, NULL, &efd, &e)) {
        g_string_assign (said, e->message);
        g_error_free (e);
    } else {
        /* its own child, waited for here: bounded like every conversion of
         * a file nobody chose in person (WALPE_CONVERT_MAX_MIN) */
        const gint64 max_us = limit_env ("WALPE_CONVERT_MAX_MIN", 60) * 60 * G_USEC_PER_SEC;
        const gint64 t0 = g_get_monotonic_time ();
        /* (what ffmpeg says is read while it runs: a full pipe would stop it) */
        fcntl (efd, F_SETFL, fcntl (efd, F_GETFL) | O_NONBLOCK);
        gint64 disk_at = t0;    /* when the free disk was looked at last */
        gboolean eof = FALSE;   /* ffmpeg has closed its end: it is about to be gone */
        for (;;) {
            char buf[512];
            ssize_t n;
            while ((n = read (efd, buf, sizeof buf)) > 0)
                if (said->len < 2000)
                    g_string_append_len (said, buf, n);
            eof |= n == 0;
            pid_t r = waitpid (pid, &status, WNOHANG);
            if (r == pid)
                break;
            if (r < 0 && errno != EINTR) {
                status = -1;
                break;
            }
            const gint64 now = g_get_monotonic_time ();
            timed_out = max_us > 0 && now - t0 > max_us;
            if (!timed_out && floor_mb > 0 && now - disk_at >= G_USEC_PER_SEC) {
                /* (looked at once a second, however often this loop wakes) */
                disk_at = now;
                low_disk = disk_free_mb (dir) < floor_mb;
            }
            stopped = !timed_out && !low_disk && stop && stop (user);
            if (timed_out || low_disk || stopped) {
                child_kill (pid);   /* (and the real ffmpeg behind a wrapper script) */
                while (waitpid (pid, &status, 0) < 0 && errno == EINTR)
                    ;
                break;
            }
            /* Until ffmpeg says something or ends (both wake the pipe), and
             * no longer than the caller is to be kept from being stopped.
             * (This was a look every 20 ms for as long as ffmpeg ran: 50
             * wake-ups a second of the waiting thread, for minutes with a
             * long video.) */
            if (eof) {
                g_usleep (20000);
            } else {
                struct pollfd pf = { efd, POLLIN, 0 };
                if (poll (&pf, 1, stop ? 100 : 1000) < 0 && errno != EINTR)
                    g_usleep (20000);
            }
        }
        {
            char buf[512];
            ssize_t n;
            while ((n = read (efd, buf, sizeof buf)) > 0)
                if (said->len < 2000)
                    g_string_append_len (said, buf, n);
        }
        close (efd);
        g_spawn_close_pid (pid);
        GStatBuf st;
        if (stopped) {
            /* (the caller's doing: neither a track nor "no track" was found out) */
        } else if (!timed_out && !low_disk && WIFEXITED (status) && WEXITSTATUS (status) == 0 &&
                   g_stat (part, &st) == 0 && st.st_size > 0 && g_rename (part, dst) == 0) {
            res = 1;
        } else if (!timed_out && !low_disk && strstr (said->str, "matches no streams")) {
            /* the video has no sound track: an empty file says so from now
             * on, and nobody asks ffmpeg again */
            g_remove (part);
            if (g_file_set_contents (dst, "", 0, NULL))
                res = 0;
        } else if (timed_out) {
            g_string_assign (said, "stopped: it took longer than WALPE_CONVERT_MAX_MIN minutes");
        } else if (low_disk) {
            g_string_printf (said, "stopped: less than %d MB are left on the disk of the cache",
                             (int) floor_mb);
        }
    }
    if (stopped) {
        g_remove (part);
        res = -2;
    } else if (res < 0) {
        g_remove (part);
        g_strchomp (said->str);
        if (err)
            *err = g_strdup (*said->str ? said->str : "ffmpeg failed");
    }
    g_string_free (said, TRUE);
    g_strfreev (argv);
    g_free (part);
    g_free (dir);
    g_mutex_unlock (&one);
    return res;
}

static WalpeImport *import_start (const char *src, const char *dest_dir, const char *dst_file,
                                  const char *encoder, const WalpeVideoBox *box,
                                  const WalpeImportScene *sc,
                                  WalpeImportDone done, gpointer user, GError **err);

WalpeImport *walpe_import_start (const char *src, const char *dest_dir,
                                 const char *encoder, const WalpeVideoBox *box,
                                 WalpeImportDone done, gpointer user, GError **err)
{
    return import_start (src, dest_dir, NULL, encoder, box, NULL, done, user, err);
}

/* a conversion into the cache: its folder, and the limits it runs under */
static WalpeImport *start_bounded (const char *src, const char *dst_file, const char *encoder,
                                   const WalpeVideoBox *box, const WalpeImportScene *sc,
                                   WalpeImportDone done, gpointer user, GError **err)
{
    char *dir = g_path_get_dirname (dst_file);
    g_mkdir_with_parents (dir, 0700);   /* created private; an existing dir keeps its mode */
    WalpeImport *imp = import_start (src, dir, dst_file, encoder, box, sc, done, user, err);
    g_free (dir);
    if (imp) {
        imp->max_us = limit_env ("WALPE_CONVERT_MAX_MIN", 60) * 60 * G_USEC_PER_SEC;
        imp->max_bytes = limit_env ("WALPE_CONVERT_MAX_MB", 8192) * 1024 * 1024;
        imp->watch_id = g_timeout_add_seconds (WATCH_S, import_watch, imp_ref (imp));
    }
    return imp;
}

WalpeImport *walpe_import_start_to (const char *src, const char *dst_file,
                                    const char *encoder, const WalpeVideoBox *box,
                                    WalpeImportDone done, gpointer user, GError **err)
{
    return start_bounded (src, dst_file, encoder, box, NULL, done, user, err);
}

WalpeImport *walpe_import_start_scene (const char *src, const WalpeImportScene *sc,
                                       const char *dst_file, WalpeImportDone done, gpointer user,
                                       GError **err)
{
    /* sizes an encoder takes, a range that is one, and an output in a
     * place of ours (the caller's cache name) */
    if (!sc || sc->len < 64 || sc->off > G_MAXINT64 - sc->len || sc->w < 2 || sc->h < 2 ||
        (sc->w & 1) || (sc->h & 1) || sc->w > WALPE_RES_MAX_SIDE || sc->h > WALPE_RES_MAX_SIDE ||
        !dst_file || !g_path_is_absolute (dst_file)) {
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "not a clip inside a file, or "
                     "not a size a copy can have");
        return NULL;
    }
    return start_bounded (src, dst_file, NULL, NULL, sc, done, user, err);
}

/* dst_file NULL: <dest_dir>/<stem>.mp4 + <stem>.jpg thumbnail (library
 * import); else exactly dst_file, no thumbnail. sc: the clip is a byte
 * range of src and the output has the size it names. */
static WalpeImport *import_start (const char *src, const char *dest_dir, const char *dst_file,
                                  const char *encoder, const WalpeVideoBox *box,
                                  const WalpeImportScene *sc,
                                  WalpeImportDone done, gpointer user, GError **err)
{
    if (!src || !g_path_is_absolute (src) || !g_file_test (src, G_FILE_TEST_IS_REGULAR)) {
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "no such file: %s", src ? src : "");
        return NULL;
    }
    for (int i = 0; i < 2; i++) {
        const char *tool = i ? "ffprobe" : "ffmpeg";
        char *p = g_find_program_in_path (tool);
        if (!p) {
            g_set_error (err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                         "%s not found in PATH: install ffmpeg (your distribution's package with "
                         "the programs ffmpeg and ffprobe) to import videos", tool);
            return NULL;
        }
        g_free (p);
    }
    if (g_mkdir_with_parents (dest_dir, 0755) != 0) {
        int e = errno;
        g_set_error (err, G_IO_ERROR, g_io_error_from_errno (e), "cannot create %s: %s",
                     dest_dir, g_strerror (e));
        return NULL;
    }

    WalpeImport *imp = g_new0 (WalpeImport, 1);
    imp->refs = 1;
    imp->cancel = g_cancellable_new ();
    imp->errtail = g_string_new (NULL);
    imp->start_us = g_get_monotonic_time ();
    imp->src = g_strdup (src);
    imp->percent = -1;
    imp->done = done;
    imp->user = user;
    if (box && box->w > 0 && box->h > 0)
        imp->box = *box;
    if (sc) {
        imp->is_scene = TRUE;
        imp->scene = *sc;
    }
    if (!encoder || !*encoder)
        encoder = g_getenv ("WALPE_IMPORT_ENCODER");
    imp->encoder = g_strdup (encoder && *encoder ? encoder : "libx264");

    char *base = g_path_get_basename (src);
    char *dot = strrchr (base, '.');
    if (dot && dot != base)
        *dot = 0;
    imp->label = base;
    imp->dst = dst_file ? g_strdup (dst_file) : unique_dst (dest_dir, base, src);
    imp->keep_sound = !dst_file;
    imp->dst_part = g_strconcat (imp->dst, ".part", NULL);
    if (!jobs)
        jobs = g_ptr_array_new ();
    g_ptr_array_add (jobs, imp);
    if (!dst_file) {
        gsize dl = strlen (imp->dst);
        imp->thumb = g_strdup (imp->dst);
        strcpy (imp->thumb + dl - 4, ".jpg");
    }

    if (encoder_list) {
        start_probe (imp);
    } else {
        imp->stage = ST_ENCODERS;
        const char *argv[] = { "ffmpeg", "-hide_banner", "-encoders", NULL };
        imp->proc = proc_spawn (argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                FALSE, NULL);
        if (imp->proc)
            g_subprocess_communicate_utf8_async (imp->proc, NULL, imp->cancel,
                                                 encoders_cb, imp_ref (imp));
        else
            start_probe (imp);
    }
    return imp;
}

int walpe_import_percent (WalpeImport *imp)
{
    return imp ? imp->percent : -1;
}

const char *walpe_import_label (WalpeImport *imp)
{
    return imp ? imp->label : NULL;
}

const char *walpe_import_mode (WalpeImport *imp)
{
    return imp ? imp->mode : NULL;
}

/* does a job that runs write this file? */
static gboolean part_in_use (const char *part)
{
    for (guint i = 0; jobs && i < jobs->len; i++) {
        const WalpeImport *imp = g_ptr_array_index (jobs, i);
        if (!imp->cancelled && imp->stage != ST_DONE && imp->dst_part && !strcmp (imp->dst_part, part))
            return TRUE;
    }
    return FALSE;
}

/* the child of a cancelled conversion has ended: what it made after the
 * cancel goes too */
static void dying_gone (Dying *d)
{
    if (!part_in_use (d->part))
        g_remove (d->part);
    g_ptr_array_remove_fast (dying, d);     /* (frees d) */
}

static void dying_cb (GObject *o, GAsyncResult *r, gpointer user)
{
    g_subprocess_wait_finish (G_SUBPROCESS (o), r, NULL);
    /* (walpe_import_settle () may have dealt with it: user is only compared) */
    for (guint i = 0; dying && i < dying->len; i++) {
        Dying *d = g_ptr_array_index (dying, i);
        if (d == user && d->proc == (GSubprocess *) o) {
            dying_gone (d);
            break;
        }
    }
}

guint walpe_import_settle (guint max_ms)
{
    const gint64 end = g_get_monotonic_time () + (gint64) max_ms * 1000;
    for (;;) {
        for (guint i = 0; dying && i < dying->len;) {
            Dying *d = g_ptr_array_index (dying, i);
            /* (GLib notes the end of a child on its worker thread: no main loop needed) */
            if (!g_subprocess_get_identifier (d->proc))
                dying_gone (d);
            else
                i++;
        }
        if (!dying || !dying->len || g_get_monotonic_time () >= end)
            break;
        g_usleep (2000);
    }
    return dying ? dying->len : 0;
}

void walpe_import_cancel (WalpeImport *imp)
{
    if (!imp || imp->cancelled || imp->stage == ST_DONE)
        return;
    imp->cancelled = TRUE;
    g_cancellable_cancel (imp->cancel);
    if (imp->proc)
        proc_kill (imp->proc);
    g_remove (imp->dst_part);
    if (imp->proc && imp->stage == ST_ENCODE && g_subprocess_get_identifier (imp->proc)) {
        /* the encoder may still make the file: once more when it has ended */
        Dying *d = g_new0 (Dying, 1);
        d->proc = g_object_ref (imp->proc);
        d->part = g_strdup (imp->dst_part);
        if (!dying)
            dying = g_ptr_array_new_with_free_func (dying_free);
        g_ptr_array_add (dying, d);
        g_subprocess_wait_async (d->proc, NULL, dying_cb, d);
    }
    /* stopped while the thumbnail was being written: the video itself is
     * complete and stays, the half-written picture goes (nobody removed
     * it: the library showed a broken preview) */
    if (imp->stage == ST_THUMB && imp->thumb)
        g_remove (imp->thumb);
    watch_stop (imp);
}

guint walpe_import_tidy_parts (const char *dir)
{
    guint n = 0;
    GDir *d = dir ? g_dir_open (dir, 0, NULL) : NULL;
    const char *name;
    while (d && (name = g_dir_read_name (d))) {
        if (!g_str_has_suffix (name, ".mp4.part"))
            continue;
        char *p = g_build_filename (dir, name, NULL);
        GStatBuf st;
        if (g_lstat (p, &st) == 0 && S_ISREG (st.st_mode) && g_remove (p) == 0)
            n++;
        g_free (p);
    }
    if (d)
        g_dir_close (d);
    return n;
}

void walpe_import_free (WalpeImport *imp)
{
    if (!imp)
        return;
    walpe_import_cancel (imp);
    imp_unref (imp);
}
