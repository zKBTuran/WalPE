# Graphics drivers, GPU choice and OpenGL ES

Which graphics drivers WalPE can draw with, how to choose the GPU on a machine with two,
and what is different on OpenGL ES. Measuring is in [GPU-PROFILING.md](GPU-PROFILING.md),
presentation and driver hints are in [GPU-PRESENT.md](GPU-PRESENT.md).

> **State of testing.** Everything on this page that is new in 0.13 was built and tested
> only on Mesa's software renderer (llvmpipe: desktop OpenGL 4.5 and OpenGL ES 3.2, no
> GPU, no DRM device). The GPU list, the integrated / discrete classification and the
> kernel statistics were tested against made-up `/sys` and `/proc` trees. Nothing of it
> has been run on real AMD, Intel or NVIDIA hardware: no render offload between two GPUs,
> no driver that offers OpenGL ES only, no vendor OpenGL ES compiler. One log of 0.14.0
> from a real laptop (Intel graphics that drive the panel, an NVIDIA GPU beside them,
> KDE Plasma on Wayland) shows the device list, integrated / discrete and "the window
> system's GPU" as they are there; that machine rendered on the Intel GPU. **`gpu=auto`
> choosing the discrete GPU of such a laptop is new and has not run on one**: what was
> tested is the decision (on made-up machines) and what happens when the discrete GPU
> fails its test. The NVIDIA / X11 path with default settings is meant to be unchanged
> from 0.12.2: on a machine with one GPU, and wherever a card drives the displays,
> `gpu=auto` sets no variable and starts no child process, and the first context asked
> for is the same desktop OpenGL 3.3 context as before.

## What WalPE needs from a driver

OpenGL through EGL, one of:

| API | For |
|---|---|
| desktop OpenGL 3.3 or later | the default wherever the driver has it: NVIDIA's driver, Mesa on AMD / Intel / NVIDIA (nouveau), llvmpipe |
| OpenGL ES 3.1 or later | GPUs without desktop OpenGL 3.3: many ARM GPUs, a few old PC ones |

OpenGL ES 3.0 and OpenGL 2.1 are not enough; the daemon then ends with one sentence that
names what the driver offers.

Optional, each with a fallback that the log names once:

| Feature | Without it |
|---|---|
| S3TC texture compression (`GL_EXT_texture_compression_s3tc`) | DXT textures are decoded on the CPU at load and stored uncompressed: 4 to 8 times the video memory for those textures, and minified layers look slightly different (their mip levels are generated instead of read from the file) |
| anisotropic filtering | trilinear filtering |
| float render targets | effect targets declared `r16f` / `rg1616f` become RGBA8 (OpenGL ES 3.1 without the extension) |
| textures of 16384 texels a side | a picture above the driver's limit is halved until it fits, with a warning naming the file. Compressed (DXT) textures above the limit are not handled |
| persistent buffer mapping (GL 4.4 / `ARB_buffer_storage`) | `video_upload=persist\|direct` become `pbo` |
| timer queries, `KHR_debug`, pipeline statistics | profiling only |

The log line at start shows what the driver is and has (`gl: driver radeonsi 24.0 (AMD,
integrated), OpenGL 4.6, GLSL 4.60; has: ...; lacks: ...; largest texture 16384`), and
`walpe-ctl status` carries `gl_vendor`, `gl_renderer`, `gl_version` and `gl_driver`
(`nvidia`, `radeonsi`, `iris`, `llvmpipe`, ...). None of WalPE's memory rules depends on
how much video memory a driver reports, so they hold on integrated graphics, where
textures live in system memory.

## Which GPU renders: `gpu`

```sh
walpe-ctl gpu                  # the graphics devices, the one in use, what each value picks here
walpe-ctl gpu --json
walpe-ctl set gpu=auto         # default
walpe-ctl set gpu=display
walpe-ctl set gpu=integrated
walpe-ctl set gpu=discrete
walpe-ctl set gpu=<device>     # a PCI address (01:00.0), a DRM node (card1, renderD129),
                               # vendor:device ids (10de:25a0), or a part of the name or of the
                               # kernel driver's name (nvidia, radeon, arc, i915, amdgpu)
WALPE_GPU=<value> walpe-daemon # for this run; wins over the saved setting
```

| Value | Means |
|---|---|
| `auto` | A discrete GPU when the machine has one. See the table below: on most machines that is `display` |
| `display` | Nothing is set. The GPU the X server or the compositor itself uses renders, and frames reach the screen without a copy between GPUs. What `auto` meant up to 0.14.0 |
| `integrated` | the processor's own graphics: lowest power draw, shares the machine's memory, the slowest of the choices |
| `discrete` | the separate card or a laptop's second GPU: fastest, draws more power and keeps that GPU awake while a wallpaper is animated |
| `<device>` | exactly that device |

What `auto` does, by machine (`walpe-ctl gpu` says which line is yours, under "what the
values mean here"):

| The machine | `auto` |
|---|---|
| one GPU of any kind | that GPU. Nothing is set, no test process is started |
| a card drives the displays (with or without an unused integrated GPU, or a second card) | that card. Nothing is set, no test process is started |
| an integrated GPU drives the displays and a discrete GPU with a driver sits beside it (a laptop with two GPUs) | the discrete GPU, after the test described below. If the test fails, or takes longer than 5 seconds, the integrated GPU renders, and the log (`gpu = auto: ...`) and `gpu_note` say why |
| the same list of devices, but on X11 the X server itself is drawn by NVIDIA's driver (the server runs on the card; the kernel's driver for the unused processor graphics is loaded all the same). The server says so: a RandR provider named `NVIDIA-0` in `xrandr --listproviders` | that card. Nothing is set, no test process is started: the daemon asks the X server for its providers first, on one short connection |
| the same, and the X server or compositor turns out to render on the discrete GPU itself in a way the line above does not cover (another driver, Wayland) | that GPU. Nothing is set (a test process was started to find out) |
| the same, but the discrete GPU has no kernel driver, or NVIDIA's driver runs without its DRM module | the integrated GPU. Nothing is tested; `gpu_note` names the reason |
| several discrete GPUs beside the integrated one | the first that can be used: the firmware's primary one, else the lowest DRM card number. Name another one by its PCI address |
| `DRI_PRIME`, `__NV_PRIME_RENDER_OFFLOAD` or another of the variables below is set in the daemon's environment | whatever that variable says. Nothing is tested; `gpu_note` names the variable |
| "Render on CPU" is on | the CPU |

On a machine with two GPUs every value that does not name the GPU driving the displays
makes the driver render on one GPU and copy each finished frame to the other (PRIME render
offload): one full-screen copy per monitor and frame, and both GPUs stay awake while a
wallpaper is animated. That is what `auto` costs on a laptop with two GPUs, and it buys the
faster GPU for scenes with many effects.

**On a laptop, to save battery: `walpe-ctl set gpu=display`.** The discrete GPU is then not
touched by WalPE and can stay powered down. (`integrated` is the same device on a laptop
whose panel hangs on the integrated GPU; on a desktop with a card it is a different one.)
The other tools for battery are independent of the GPU: `battery=on` pauses every wallpaper
on battery power, `battery_fps=N` limits the frame rate there ([USAGE.md](USAGE.md)). A
pause stops drawing and decoding; it does not close the connection to the GPU and does not
free the wallpaper's textures (`idle_release` in `gpu.conf` frees them after a time,
[GPU-PRESENT.md](GPU-PRESENT.md)). Whether a paused wallpaper lets the discrete GPU power
down is the driver's decision and has not been measured. There is no setting yet that
renders on the discrete GPU on mains and on the display's GPU on battery.

How it is applied: the drivers pick their device from environment variables when EGL
starts. When another device than the display's is to render - a value that names one, or
`auto` on a laptop with two GPUs - the daemon reads the device list from sysfs, starts a
short child process of itself to learn which device renders when nothing is set and, if
another device is wanted, a second child with the variables set that must open a window
surface and draw one frame on that device. Only if that worked does the daemon set the
variables for itself (Mesa: `DRI_PRIME`; NVIDIA: `__NV_PRIME_RENDER_OFFLOAD`,
`__GLX_VENDOR_LIBRARY_NAME`; `__EGL_VENDOR_LIBRARY_FILENAMES` where needed). The daemon's
start waits for the two children, each for at most 10 seconds, 5 when `auto` made the
choice (a GPU that was powered down has to wake within that), and the log line says how
long the test took. The limit is the daemon's own clock: when it is up the test process is
sent SIGKILL and not waited for, so a driver that does not let the process end, or a
helper it started that lingers, cannot hold up the start. A variable that is already set in the daemon's environment is not
touched: the setting is then not applied and `gpu_note` says which variable decides. A
change restarts the daemon in place; a value that names no device of this machine is
refused with the list of devices. "Render on CPU" wins over the `gpu` setting.

Hardware video decoding (`hwdec`, off by default) asks the GPU that renders, whichever
that is, and web wallpapers with hardware acceleration (`web_accel`, off by default) run
their own start-up test with the same variables: neither is left on the other GPU.

`walpe-ctl status`: `gpu` (the setting), `gpu_env` (`WALPE_GPU` decides), `gpu_in_use`,
`gpu_candidates`, `gpu_offload`, `gpu_environment` (the variables set) and `gpu_note` (why
a setting was not honoured, or what it costs). The log lines start with `gpu:`.

Integrated or discrete is decided from the device's ids: Intel's device at PCI address
`00:02.0` is the processor's graphics, every other Intel device a card; NVIDIA is discrete;
AMD has a table of the ids of processors with graphics, and every other AMD id is treated
as a card and marked "(assumed)" - so a discrete Radeon reads as "discrete (assumed)", and
a processor newer than the table would be listed as discrete: name such a device by its PCI
address. Virtual machines' adapters are neither.

Expected limits: a display driven by the NVIDIA driver's X server usually cannot be drawn
to by another GPU (`integrated` then fails its test, the daemon stays on the NVIDIA GPU and
`gpu_note` says why); under Wayland the test process decides. The test draws one small
frame: a GPU that passes it and then draws wrongly, stutters or loses its context after a
suspend is not noticed by `auto`, and nothing takes the choice back by itself. If the
wallpaper misbehaves on a laptop with two GPUs, `walpe-ctl set gpu=display` is the first
thing to try (`WALPE_GPU=display walpe-daemon` when the daemon does not stay up), and
`walpe-ctl gpu --json` with the `gpu` lines of the log is what to send.

`preset_follow_gpu=on` (off by default): the quality preset is set once when the kind of
GPU that renders changes - discrete: `high`, integrated: `balanced`, CPU rendering or a
virtual GPU: `low`. A preset you choose afterwards stays until the kind changes again. It
goes by the GPU that really renders: where `auto` takes a laptop's discrete GPU that is
`high`, and `balanced` when the test failed and the integrated GPU renders.

When a Mesa driver renders, the `gpu.conf` hints that have a Mesa counterpart are handed
to Mesa as well ([GPU-PRESENT.md](GPU-PRESENT.md)).

## Desktop OpenGL or OpenGL ES: `gl_api`

Which API runs is decided when the daemon starts. With the default it asks for a desktop
OpenGL 3.3 context first - the context every earlier version asked for - and only when the
driver has none for OpenGL ES 3.2, then OpenGL ES 3.1.

```sh
walpe-ctl set gl_api=auto       # the rule above (default)
walpe-ctl set gl_api=gl         # desktop OpenGL 3.3
walpe-ctl set gl_api=gles       # OpenGL ES 3.2 / 3.1 even where desktop OpenGL exists
WALPE_GL_API=auto|gl|gles       # for one run of walpe-daemon; overrides the saved setting and
                                # is strict: gl / gles mean that API or nothing
```

A change restarts the daemon in place. A saved `gl` or `gles` that this machine cannot give
does not stop the daemon: it runs as `auto` would for that run, the log and `gl_api_note`
say so, and the saved value stays. Only `WALPE_GL_API` is strict. The setting is
independent of "Render on CPU": `walpe-ctl set software=on gl_api=gles` renders with
llvmpipe's OpenGL ES.

`walpe-ctl status`: `gl` (`gl` or `gles`), `gl_context` (`OpenGL 3.3`, `OpenGL ES 3.2`,
`OpenGL ES 3.1`), `gl_api` (the saved setting), `gl_api_env`, `gl_api_note`,
`gl_api_tried` (every version tried and what became of it), `glsl_es`, and `gl_features` /
`gl_features_off`: what the OpenGL ES path does another way or lacks. On desktop OpenGL the
two lists are empty.

### What is different on OpenGL ES

The renderer is the same code. The differences, as status names them:

| Item | On OpenGL ES |
|---|---|
| effect shaders | written as GLSL ES 3.10 / 3.20 and translated (below) |
| texture LOD bias | not available: minified mipmapped textures are sampled plain trilinear, slightly softer than on desktop OpenGL |
| BGRA upload, S3TC, anisotropic filtering, float effect targets, 16-bit video planes | by extension; without it: bytes reordered on the CPU, DXT decoded on the CPU, trilinear only, RGBA8 targets (values clamp; a simulation that stores signed values is left out), 10-bit video shown with 8 bits |
| GPU timer queries | by `GL_EXT_disjoint_timer_query`, else the profiler measures wall clock time around `glFinish` |
| exact sample counts, pipeline statistics | off: the profiler's pixel statistics and `WALPE_PIX_STATS` stay empty |
| texture read-back | `WALPE_PROBE` and the per-texture memory report are not available |
| persistent buffer mapping | off: `video_upload=persist\|direct` fall back to `pbo` |
| fused effect passes | off: `fx_quality=fast` runs stacked displacement effects one by one (same picture, more draws) |

GLSL ES has no implicit conversion from integers to floats and reserves more words, and
Wallpaper Engine's effect shaders rely on both. Before a shader is compiled WalPE types its
expressions and makes each conversion explicit; what it cannot type is left as written,
and if the driver then rejects a line, rewrites of that line are tried (the log says `fx:
shader ... compiled after N error-driven repair round(s)`). A shader that still does not
compile is reported (`compile failed`) and its effect is left out; the layer is drawn
without it, as on desktop OpenGL. Known limits: a statement of more than 24 lines, a
statement that continues behind a preprocessor line, more than 16 lines that need the
driver's error to be found, a function named like a GLSL ES built-in, `sampler1D` /
`sampler2DRect`. The repair reads three forms of compiler messages (Mesa's, NVIDIA's and
the Khronos reference compiler's); a driver that words its errors otherwise gets the typed
stage only.

What was measured, on llvmpipe only: all 91 scenes of the test library render on the
OpenGL ES path (3.2, and the 20 largest also with OpenGL ES limited to 3.1) with no effect
shader left out. The texture LOD bias is the one picture difference found: 65 of the 91
scenes differ from desktop OpenGL, each of them byte-identical to desktop OpenGL run with
`WALPE_TEX_LOD_BIAS=0`; in 52 of them at most 1 % of the pixels differ by more than 2 of
255, the largest is 15 % (minified text). The replacement paths for missing extensions
were run by hiding the extensions from Mesa. Between different GPUs exact equality is not
to be expected at all.

Trying it on a machine with desktop OpenGL: `WALPE_GL_API=gles walpe-daemon`, then
`walpe-ctl status | grep -E '"gl|glsl_es'`. A scene that looks wrong on OpenGL ES: look
for `compile failed` / `repair round` in the log, compare with `gl_api=gl` on the same
machine, and attach the `WALPE_FX_DUMP` directory to a report.

## AMD, Intel and NVIDIA in short

- **NVIDIA's driver**: as in 0.12.1. `cuda` stays the first hardware video decoder there
  ([VIDEO.md](VIDEO.md)); `walpe-ctl profile` samples no kernel statistics there unless
  asked.
- **AMD and Intel with Mesa**: desktop OpenGL 3.3, so `gl_api=auto` gives the same path as
  on NVIDIA. New for them: the `gpu` setting, `walpe-ctl gpu-stats` and the kernel's
  figures in a profile ([GPU-PROFILING.md](GPU-PROFILING.md)), VA-API video decoding on the
  GPU that renders ([VIDEO.md](VIDEO.md)), the Mesa counterparts of the `gpu.conf` hints.
  Web wallpapers run without hardware acceleration by default on every driver;
  `WALPE_WEB_ACCEL=dmabuf` (WebKit's own default mode) is worth trying on Mesa drivers and
  is untested.
- **ARM GPUs** (Mali with panfrost / panthor, Broadcom V3D, Adreno with freedreno): the
  OpenGL ES path is meant for them. Never run there.

Troubleshooting switches: `WALPE_GL_LACKS`, `WALPE_GL_MAX_TEXTURE`, `WALPE_GPU_QUERY`,
`WALPE_GPU_SYSFS_ROOT`, `WALPE_GPU_PCI_IDS` ([ENVIRONMENT.md](ENVIRONMENT.md)); older ones
that help when a driver misbehaves: `particle_instancing=off`, `WALPE_NO_DXT=1`,
`WALPE_TEX_ANISO=0`, `WALPE_NO_SHARE=1`, `WALPE_EGL_CONFIG=first`, `software=on`.

For a bug report from an AMD or Intel machine: `walpe-ctl gpu`, `walpe-ctl status | tr ','
'\n' | grep -E '"(gpu|gl)_'`, and the daemon's log lines starting with `gpu:`, `gl:`,
`present:` and `monitor context:`. They hold no file names and nothing of the wallpapers.
