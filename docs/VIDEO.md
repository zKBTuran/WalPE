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
handed on at half size (decoding it costs the same; a video inside a scene is played from a
smaller copy instead, below). Anything else ffmpeg can read (WebM, HEVC, MKV, ...) is converted once:
`walpe-ctl open` does it by itself into `~/.cache/walpe/video` and changes the wallpaper when
the result is ready; `walpe-ctl add` imports a file into the library for good.

## Videos inside scenes: played from a smaller copy

A scene's video layer is decoded at the clip's own size. libavcodec's H.264 decoder cannot
decode to a smaller picture, so a 3840×2160 clip on a 1920×1080 monitor costs the decoding
of four times the pixels that are shown; the half-size path above only keeps the frames
smaller. With **`video_scene_convert`** (on by default) such a clip is converted once, in the
background, into a copy of the size it is needed at, and the scene plays the copy.

* **Which clips.** One that has at least 1.5 times the pixels of its copy. The copy is the
  clip fitted into `video_resolution` ([RESOLUTION.md](RESOLUTION.md): `auto` = the largest
  render resolution among the monitors that show the scene, 1920×1080 for a configuration
  that comes from an older version, `none` = no copies), in the clip's own shape, never
  enlarged. A layer the scene draws larger than that - zoomed, or a 16:9 scene that covers an
  upright or ultra-wide monitor - gets a copy that covers what is drawn, in steps of an
  eighth of the clip's size, or none when that would save too little: a copy is never shown
  enlarged by more than the texture slack (2 %). A scene whose textures stay at file size
  (`tex_downscale` off, `texture_limit` none) keeps its videos as they are too.
* **What the copy is.** The import's format: H.264 High, yuv420p, `libx264 -preset medium
  -crf 19`, bicubic scaling, MP4 with `+faststart`, no audio (video layers are silent). Every
  picture of the clip at the clip's own frame rate: the rate is never reduced, because what a
  monitor shows depends on settings that change while the copy stays (`fps`, the battery
  and focus limits), and `video_limit` already leaves out the pictures nobody sees. Only a
  clip that is 8-bit 4:2:0 H.264 in limited range - what the copy is - gets one; full range,
  10 bit, 4:2:2 and 4:4:4 clips play as they are. The copy says which colour matrix its
  samples are in (the one the player uses for the clip: an untagged clip's is decided by its
  size, and the copy is smaller), so the colours are the clip's. H.264 has no alpha channel:
  video layers are opaque with and without a copy.
* **Where it is kept.** `~/.cache/walpe/video/<id>-scene-<W>x<H>.mp4`, next to the converted
  video wallpapers and under their rules: a private folder, unfinished files removed when
  the daemon starts, copies nothing has played for 90 days removed too. `<id>` is made of
  where the clip is (file, offset, length) and of its content (its first and last 64 KiB),
  so a package that is unpacked again keeps its copy and a clip that was replaced gets a new
  one. Nothing is written into the wallpaper's folder or the Steam library. To clear them:
  `rm ~/.cache/walpe/video/*-scene-*.mp4`; they are made again when a scene needs them.
* **How it is made.** `ffmpeg` as a child process with an argument vector (no shell). It
  reads the clip's bytes out of the `.tex` through FFmpeg's `subfile` protocol - no
  temporary file - and for this run only that protocol and only the MP4 reader are allowed;
  the file name follows the protocol's options, behind `file:`. The limits of a conversion
  on open hold (`WALPE_CONVERT_MAX_MIN`, `WALPE_CONVERT_MAX_MB`, 256 MB of free disk). It
  runs at the lowest priority (nice 19, `SCHED_IDLE`, idle I/O class) and gets the time
  nothing else wants; one clip at a time, and an import or a conversion you start yourself
  comes first. It runs only while a scene that waits for the copy is shown: when that
  wallpaper is replaced, the conversion is stopped and its partial file removed. A scene
  without an oversized video starts nothing and keeps nothing.
* **The change-over.** Until the copy exists the clip plays as before. The copy takes over
  where the clip starts again - its last picture is followed by the copy's first one frame
  interval later, which is what a loop looks like anyway - so a clip that is minutes long
  plays to its end once more. From then on, and whenever the scene is loaded again, the copy
  plays from the first picture and the original's decoder is never opened. The layer keeps
  the clip's size in the scene (positions, effects). Monitors that show the same clip share
  one decoder as before; as soon as one of them needs the clip itself, all get it.
* **What it is doing.** `walpe-ctl status`, `scene_videos`: one entry per video of the scenes
  on screen,
  `CA2A….tex: source 3840x2160 60.00 fps, played 1920x1080 60.00 fps, copy: ready`. After
  `copy:` - `waiting`, `converting 42 %`, `ready (played from where the clip starts again)`,
  `ready`, `off`, `not needed`, `not made (why)`, `not usable (why)`, `failed (why)`.
  `video_decoder` is the decoder of what is played. The log says when a copy is started,
  made and taken into use.
* **The switch.** `walpe-ctl set video_scene_convert=off` (window: Engine settings, "Smaller
  copies of videos in scenes"; `WALPE_VIDEO_SCENE_CONVERT=0|1` in front of `walpe-daemon`
  overrides): the clip itself again, from where it starts next; nothing is converted. The
  copies stay in the cache for the next time it is on.

**Measured** (a scene whose background is a 3840x2160 60 fps H.264 clip of 12 s and 15.1 MB,
one 1920x1080 monitor, `video_resolution=1080p`, software decoding, on a machine without a
GPU; CPU time of the video threads per decoded picture, `WALPE_FPS_LOG`):

| | the clip itself | its 1920x1080 copy |
|---|---|---|
| decoding one picture | 14.6 - 15.2 ms | 4.8 ms |
| handing it on (the clip: reduced 2x2 first) | 1.4 ms | 0.0 ms |
| **video threads per picture** | **16.1 - 16.8 ms** | **4.9 ms** |
| the same at 60 pictures a second | 58 - 60 CPU seconds a minute | 17.5 CPU seconds a minute |
| daemon memory (anonymous, resident) | 196 - 201 MB | 88 MB |
| file | 15.1 MB | 6.9 MB |

That is 3.3 to 3.5 times less CPU time for the video and about 110 MB less memory. Making
the copy took 64 CPU seconds of `ffmpeg`, once (at the lowest priority it takes as long as
the rest of the machine leaves it; its memory, about 750 MB at the peak for a 3840x2160
clip, is the `ffmpeg` process's and is gone with it). How much it is on another machine
depends on its CPU: compare the daemon's CPU in a process monitor with the switch off and on.

## Import (`walpe-ctl add`, **Import** in the GUI)

* H.264 **High** profile, **yuv420p** 8-bit, MP4 with `+faststart`
* scaled to fit **within `video_resolution`** ([RESOLUTION.md](RESOLUTION.md): `auto` = the
  largest render resolution among the monitors, 1920×1080 for a configuration that comes from
  an older version), aspect preserved (sample aspect ratio and rotation metadata applied),
  **never upscaled**, even dimensions
* **source frame rate** kept
* **audio dropped** (`-an`): video wallpapers are silent
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
| A video inside a scene is played from a copy of the size it is shown at | window: Engine settings, "Smaller copies of videos in scenes"; `walpe-ctl set video_scene_convert=on\|off`; `WALPE_VIDEO_SCENE_CONVERT=0\|1` overrides (above) | on |
| Hardware decoding | `walpe-ctl set hwdec=off\|auto\|<api>`, `video_zerocopy=on\|off` (see below) | off |
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

Off by default, and **not confirmed on a graphics card**: it was written and tested on a
machine without a GPU and without any hardware decoder (the choice of the decoder against
made-up machines and made-up failures in the test `video-hwsel`; on the software renderer
the daemon answers "the renderer is a software renderer" and decodes on the CPU). Every
hardware path - VA-API, NVDEC, VDPAU, Vulkan Video, V4L2, zero-copy - is untested until
someone runs it on such hardware.

```sh
walpe-ctl set hwdec=off        # software decoding (the default)
walpe-ctl set hwdec=auto       # also hwdec=on and the switch in the settings window: try what
                               # the machine offers; anything that fails ends in software decoding
walpe-ctl set hwdec=vaapi      # only this API: vaapi, cuda (NVDEC), vdpau, vulkan, drm (V4L2
                               # request API), v4l2m2m, or another device type of the installed FFmpeg
walpe-ctl set video_zerocopy=on|off     # below; off by default
```

`config.json`: `"video_hwdec": true|false` and, when one API is chosen, `"video_hwdec_api"`.
`WALPE_VIDEO_HWDEC=off|auto|<api>` and `WALPE_VIDEO_ZEROCOPY=0|1` in the daemon's
environment override the settings. Both apply at once to videos that are playing (one short
stop: the decoder is reopened and continues after the last picture shown).

Which decoder is used with `auto` is decided by what the installed FFmpeg can do and by
which GPU the daemon renders on, not by who made the card - with one exception: where
NVIDIA's own kernel driver renders, `cuda` (NVDEC) is the first hardware decoder, as in
0.12.1. Otherwise the order is:

1. attempts on the GPU that renders the wallpaper (`vaapi` on its DRM render node);
2. attempts where the API picks the device itself (`vdpau`; `cuda` on a machine with one GPU);
3. attempts on another GPU than the one that renders: they work, but every picture is copied
   through main memory and that GPU is kept awake; the status line says so;
4. `vulkan` (Vulkan Video);
5. hardware decoders of their own (`v4l2m2m` and the like), not on x86-64: there `auto` does
   not open them, `hwdec=v4l2m2m` by name does.

An attempt that fails before it has shown a picture is followed by the next one, and when
none is left by software decoding. A decoder that fails after pictures were shown is
replaced by software decoding at once, from the keyframe before the last picture shown.
With software rendering ("Render on CPU") hardware decoding is not used.

**Zero-copy.** By default each decoded picture is downloaded to main memory and uploaded
like a software picture: the exact samples, on every driver (a 4K picture: 12 MB down, then
the 2x reduction and 3 MB up). `video_zerocopy=on`: where the decoder can export its
pictures as DMA-BUFs and the renderer's EGL can import them, the planes are used as
textures where the decoder left them (8-bit and 10..16-bit 4:2:0). The first two pictures
after every decoder start are also downloaded and compared with the imported planes, sample
for sample; one differing sample and the stream is downloaded from then on. `cuda` and
`vdpau` cannot export DMA-BUFs through FFmpeg and keep downloading. Not applied on this
path: the half-size reduction of a 4K clip on a 1080p monitor.

`walpe-ctl status`: `video_hwdec`, `video_hwdec_api` (`off`, `auto` or the API),
`video_zerocopy`, and `video_decoder` with one entry per open video, for example

```
1920x1080 60.00 fps: software, 2 threads
1920x1080 60.00 fps, H.264 High@L4.2, 8-bit 4:2:0: hardware (vaapi on /dev/dri/renderD128 [amdgpu], the GPU that renders; pictures are downloaded to main memory)
...: hardware (cuda device 0 on /dev/dri/renderD128 [nvidia], the GPU that renders; ...)
...: software, 2 threads (hardware decoding was asked for and is not in use: <attempt>: <kind of reason> (<detail>); ...)
```

(the codec profile is part of the line only while hardware decoding is asked for). Kinds of
reason: `codec unsupported`, `no device`, `permission` (the DRM node may not be opened by
this user: group `render` / `video`), `profile`, `resolution outside the decoder's limits`,
`stream refused`, `decoder could not be opened`, `decoding failed`, `the renderer is a
software renderer`.

Per API at run time, besides an FFmpeg built with it: `vaapi` needs libva and a VA driver
for the GPU; `cuda` NVIDIA's driver with `libcuda.so.1` and `libnvcuvid.so.1`; `vdpau`
libvdpau, the GPU's VDPAU driver and an X display; `vulkan` a driver with Vulkan Video
decode and FFmpeg 6.1 or newer. The build needs nothing new.

To check it on your machine, with a video wallpaper running and nothing covering it:

```sh
walpe-ctl set hwdec=off; sleep 10
walpe-ctl status | grep -o '"video_decoder":"[^"]*"'
scripts/walpe-gpu-report.sh 30 swdec > swdec.txt
walpe-ctl set hwdec=auto; sleep 10
walpe-ctl status | grep -o '"video_decoder":"[^"]*"'
scripts/walpe-gpu-report.sh 30 hwdec > hwdec.txt
nvidia-smi dmon -s um -c 10 > dmon.txt         # NVIDIA: column "dec", how busy the video decoder is
walpe-ctl gpu-stats --sample 10 > gpu.txt      # AMD / Intel: a row "daemon engine ..." for the video
                                               # decoder; no such row = it decodes on the CPU
./build/video-test /path/to/any-h264.mp4       # the hardware decoder's pictures against the CPU's
```

For a bug report: the two status lines, the three or four files, the line `video-test`
prints, and whether the picture looked right (colours, no stutter, pause for 10 seconds and
resume).
