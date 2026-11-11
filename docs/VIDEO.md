# Video

How WalPE plays video wallpapers and the video layers of scenes, what that costs and the
switches for it. No number in this file was measured on a graphics card: they are counts or
come from Mesa's software renderer (llvmpipe) and say what is done, not how fast your card
does it. Measure on your own machine with `walpe-ctl profile`.

## What is played

An in-house MP4 demuxer feeds libavcodec's **H.264** decoder (`walpe-video.so`); one decoder
is shared by all monitors and all objects that show the same clip. Frames are shown by
presentation time from a short queue. Colours: BT.709 for untagged HD, the stream's
`colorspace` tag otherwise; YUV to RGB on the GPU. A clip shown at half its size or less is
decoded at half size. Anything else ffmpeg can read (WebM, HEVC, MKV, ...) is converted once:
`walpe-ctl open` does it by itself into `~/.cache/walpe/video` and changes the wallpaper when
the result is ready; `walpe-ctl add` imports a file into the library for good.

## Import (`walpe-ctl add`, **Import** in the GUI)

* H.264 **High** profile, **yuv420p** 8-bit, MP4 with `+faststart`
* scaled to fit **within 1920×1080**, aspect preserved (sample aspect ratio
  and rotation metadata applied), **never upscaled**, even dimensions
* **source frame rate** kept
* **audio dropped** (`-an`) — the renderer has no audio output
* already-conforming H.264 files are **remuxed** (stream copy, instant)

Encoder: `libx264 -preset medium -crf 19` by default. To use the NVIDIA
encoder set `WALPE_IMPORT_ENCODER=h264_nvenc` in the daemon's environment;
if NVENC fails the import retries with libx264 automatically. Output lands in
`~/.local/share/walpe/workshop/imported/<name>.mp4` with a `<name>.jpg`
library thumbnail; `walpe-ctl status` shows progress (`import_percent`,
`import_mode`, `import_error`). ffmpeg runs as a child process with an
argument vector (no shell), so file names are never interpreted.

**ffmpeg and ffprobe must be installed at runtime** (Gentoo:
`media-video/ffmpeg` with `USE=x264`; without libx264 the importer falls
back to `h264_nvenc` or `libopenh264` if your ffmpeg has them).

## Build

## Decoder threads

A clip is decoded with as many threads as its pixels and bits per second
need, at most the count by picture size (1 up to 720p, 2 up to 1080p, 3 up
to 1440p, 4 above, never more than the CPUs minus one). When frames of a clip keep arriving
after their time (three of four over two seconds, also when the decoder is
only a few percent too slow), it gets what its measured rate asks for in one
step, from the next keyframe or the loop point on, without a pause, and keeps
that count while the engine runs (log: `video: the decoder fell behind with 1
thread(s): 3 from the next keyframe on`). `WALPE_VIDEO_THREADS=n` fixes the
count; `WALPE_VIDEO_THREADS_START=n` (testing) sets only what a clip starts
with.

## What it costs and the switches for it

| what | how | default |
|---|---|---|
| Videos follow the frame rate limit | window: Engine settings, "Videos follow the frame rate cap"; `walpe-ctl set video_limit=on\|off`; `WALPE_VIDEO_LIMIT=0\|1` in front of `walpe-daemon` overrides | off: videos keep their own frame rate |
| Hardware decoding | `walpe-ctl set hwdec=on\|off` (see below) | off |
| Leave the deblocking filter out | `WALPE_VIDEO_SKIP_DEBLOCK=nonref` in front of `walpe-daemon`: only for pictures no other picture is predicted from (they alone get slightly blockier; nothing builds on them); `=1`: for all pictures (blocky between keyframes) | filter on |
| Box filter kernel of the half-size path | `WALPE_VIDEO_SIMD=c\|sse2` (comparisons only; the result is the same bytes) | AVX2 when the CPU has it, else SSE2 |
| How a frame's planes reach their textures | `walpe-ctl set video_upload=legacy\|pbo\|pbodata\|persist\|direct`, each also with `+pingpong` (saved; takes effect with the next frame); `WALPE_VIDEO_UPLOAD=...` in front of `walpe-daemon` overrides; status: `video_upload`, `video_upload_now` | legacy |

**`video_limit=on`** does two things. A video wallpaper is shown at most as often as the frame
rate limit of its monitor allows (the fps setting, the battery / focus limits and the
wallpaper's own `fps_idle`, exactly like a scene); and a video - a wallpaper or a scene's video
layer - whose monitors show fewer pictures a second than the clip has decodes only what they
show: the pictures in between are not decoded at all when no other picture is predicted from
them (most B pictures), and are decoded but neither reduced nor uploaded otherwise. What is
shown are the clip's own pictures, bit for bit, at even spacing. Examples: a scene with a
4K 60 fps video layer that runs at 30 (`walpe-ctl quality --path <wallpaper> fps_idle=30`, or
on battery with `battery_fps=30`) decodes about half as much; a 144 fps clip under the default
limit of 60 decodes 60 % of what it did. With the limit at or above the clip's rate nothing
changes. `WALPE_FPS_LOG=5` shows it: `frame limit: 30.00 pictures a second wanted, N not
decoded, M decoded and not shown`.

**`video_upload`** chooses what the graphics driver is asked to do for every video frame.
The textures get the same bytes on every path (the test reads them back), so the picture
cannot differ; what differs is the cost, and that shows only on a real card: in `walpe-ctl
profile 30` the row `(scene update, uploads)` of the monitor with the video (GPU ms and CPU
ms per frame). `legacy`: `glTexSubImage2D` from main memory, the driver copies each plane
inside the call. `pbo` / `pbodata`: the planes go into a pixel buffer object that gets a new
store every frame (mapped and written, or `glBufferData`), and the textures are filled from
it. `persist`: three buffers that stay mapped, used in turn, each guarded by a fence. `direct`:
the decode thread writes the frame straight into mapped buffers - for a 4K clip on a 1080p
monitor they are the output of the 2x reduction, so one pass over the frame disappears - and
the render thread copies nothing. `+pingpong`: two sets of plane textures filled in turn.
`persist` and `direct` need OpenGL 4.4 or `GL_ARB_buffer_storage` and become `pbo` without it
(`video_upload_now` says so). `direct` keeps five frame-sized buffers per video (a 4K clip on
a 1080p monitor: 16 MB, at full 4K size: 62 MB), also while the video is paused. To compare
on your card:

```sh
for m in legacy pbo pbodata persist direct pingpong direct+pingpong; do
  walpe-ctl set video_upload=$m; sleep 5
  walpe-ctl status | grep -o '"video_upload_now":"[^"]*"'
  walpe-ctl profile 30 > up-$m.txt
done
walpe-ctl set video_upload=legacy
grep -H -e '(scene update, uploads)' -e 'uploads textures' -e 'TOTAL mon' up-*.txt
```

## Hardware decoding

Off by default (`walpe-ctl set hwdec=on`) and not confirmed on a graphics card: its
development and tests ran on Mesa's software renderer. To check it on your machine, with
your wallpapers running and nothing covering them:

```sh
ffmpeg -hide_banner -hwaccels                  # "cuda" must be listed (Gentoo: media-video/ffmpeg USE=nvenc)
walpe-ctl set hwdec=off; sleep 10
walpe-ctl status | grep -o '"video_decoder":"[^"]*"'
scripts/walpe-gpu-report.sh 30 swdec > swdec.txt
walpe-ctl set hwdec=on; sleep 10
walpe-ctl status | grep -o '"video_decoder":"[^"]*"'
scripts/walpe-gpu-report.sh 30 hwdec > hwdec.txt
nvidia-smi dmon -s um -c 10 > dmon.txt         # column "dec": how busy the video decoder is
./build/video-test /path/to/any-h264.mp4       # the hardware decoder's pictures against the CPU's
```

`video_decoder` reads `...: hardware (cuda)` when the card decodes (`, no picture yet` until
its first picture came out), and otherwise `software, N threads (hardware decoding was asked
for and is not in use: <why>)` - no device and each device's error, a library that could not
be loaded, the device refusing the stream, or pictures that failed. With two videos playing
the line lists both. For a bug report: the two status lines, `swdec.txt`, `hwdec.txt`,
`dmon.txt`, the line `video-test` prints, and whether the picture looked right (colours, no
stutter, pause for 10 seconds and resume). What it does: the card decodes through FFmpeg
(NVDEC as `cuda`, else VA-API, else VDPAU), the picture is copied back to main memory and
uploaded through OpenGL like a software frame - so the decoder's CPU time goes away, the
copies stay (a 4K picture: 12 MB down, then the 2x reduction and 3 MB up); a CUDA context
also takes some hundred MB of main and video memory. Software decoding takes over by itself
at the same picture when anything fails.

