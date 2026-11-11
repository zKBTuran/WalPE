# Environment switches

Everything WalPE reads from the environment. The settings most people need are in
`config.json` and reachable with `walpe-ctl set` or the settings window
([SETTINGS.md](SETTINGS.md)); the variables here are for diagnosis, for comparing two
behaviours on your own hardware (A/B), and for tests.

How to use them: put the variable in front of the program that reads it, almost always the
daemon (`WALPE_FPS_LOG=5 walpe-daemon`). Unless a row says otherwise a variable is read once,
when the daemon starts or when a wallpaper loads, and "set" means set to any value. The
daemon hands its start environment on unchanged when it restarts itself in place.

"Default" is what happens when the variable is not set.

## What is on by default, and what turns it off

For finding out whether one of the engine's shortcuts is behind something you see. Entries
written `key=value` are settings (`walpe-ctl set`), the others go in front of
`walpe-daemon`; each is described further down or in [SETTINGS.md](SETTINGS.md).

| Behaviour | Off |
|---|---|
| Unchanged frames are not drawn or presented | `WALPE_FRAME_SKIP=0` |
| Still layers at the back are composed once and reused | `WALPE_BAKE=0` |
| Textures and effects may be drawn up to 2 % larger than stored | `texture_slack=0`, or `WALPE_TEX_SLACK=0` |
| Effects that do not change are drawn once and kept | `fx_cache=off`, or `WALPE_FX_CACHE=0` |
| One scene is drawn once for monitors of the same size | `shared=off` |
| Drawing uses a display connection of its own | `WALPE_ONE_XCONN=1` |
| Effect targets are not cleared before a pass that writes every pixel | `WALPE_FX_CLEAR=1` |
| Smaller formats for effect targets and layer captures | `WALPE_FX_RT_FORMATS=0`, `WALPE_FX_CAPTURE_RGBA=1` |
| GL state and uniforms are sent only when they change | `WALPE_FX_STATE_CACHE=0`, `WALPE_GL_DRAW_CACHE=0` |
| The EGL window config is chosen and its swap behaviour set | `WALPE_EGL_CONFIG=first`, `WALPE_EGL_SWAP=keep` |
| Hidden layers load their texture when they are first shown | `WALPE_TEX_LOAD_HIDDEN=1` |
| A paused video frees its decoder after 5 s | `WALPE_VIDEO_PARK_S=0` |
| Video layers of scenes at half size, pictures stored at the size they are shown at | `WALPE_VIDEO_FULLRES=1`, `WALPE_IMAGE_FULLRES=1` |
| The mouse is followed by events instead of a query every frame | `WALPE_POINTER_POLL=1` |
| The log file is cut back at 16 MiB | `WALPE_LOG_MAX_MB=0` |

The other way round, `WALPE_MEM_STATS=1` switches memory numbers by kind in
`walpe-ctl status` on.

## Where things are found

| Variable | Default | Effect |
|---|---|---|
| `WALPE_WE_ASSETS=dir` | auto-detect | Wallpaper Engine install (`assets/` or the install root) for the builtin effects, shaders and sprites; wins over config `we_assets` and the search of the Steam libraries (README, "Wallpaper Engine assets") |
| `WALPE_MODULE_DIR=dir` | the daemon's own directory, then `<prefix>/<libdir>/walpe` | where `walpe-web.so` / `walpe-video.so` are looked for first; must be an absolute path |
| `WALPE_IMPORT_ENCODER=name` | `libx264` | ffmpeg encoder of the video importer (`h264_nvenc`, ...); falls back to libx264 when it fails |
| `WALPE_TRAY_BACKEND=sni\|xembed` | `tray.ini` `backend` (auto) | `walpe-tray`: force one tray protocol (`--backend=` on its command line wins) |

## Limits and budgets

| Variable | Default | Effect |
|---|---|---|
| `WALPE_TEX_BUDGET_MB=n` | 1024 | MiB all textures of one scene may take; beyond it the largest are stored smaller. `0` = no limit |
| `WALPE_PKG_CACHE_MB=n` | 1024 | MiB of unpacked `scene.pkg` files kept in `~/.cache/walpe` |
| `WALPE_LOG_MAX_MB=n` | 16 | MiB at which a `daemon.log` the daemon appends to is cut back to its last 1 MiB (0 = never, at most 4096) |
| `WALPE_CONVERT_MAX_MIN=n` / `WALPE_CONVERT_MAX_MB=n` | 60 / 8192 | a video wallpaper the player cannot read is converted once into `~/.cache/walpe/video`; the run is stopped after `n` minutes or when its output exceeds `n` MB (also when less than 256 MB of disk are left). `0` = no limit. `walpe-ctl add` is not limited |
| `WALPE_STEAMCMD_TIMEOUT=s` | 1800 | time limit of one steamcmd download |
| `WALPE_FIRST_FRAME_S=n` | 20 | seconds a video wallpaper may take for its first picture before the load counts as failed (the previous wallpaper is opened again; `walpe-ctl status` shows `load_error`). `0` = a video counts as loaded once its file was opened |
| `WALPE_MALLOC_MMAP_MB=n` | 4 | `walpe-daemon`: memory blocks from this size on get their own mapping and return to the system when freed (at most 32). `0` = glibc's adaptive threshold (more memory stays resident after a load) |

## Rendering on the CPU, monitors, pacing

| Variable | Default | Effect |
|---|---|---|
| `WALPE_SOFTWARE=1` / `=0` | config `software_render` (off) | render with Mesa llvmpipe / with the GPU driver, whatever the config says. `LP_NUM_THREADS=n` set by you wins over config `software_threads` |
| `WALPE_MONITORS=WxH+X+Y,...` or `NAME=WxH+X+Y@HZ,...` | Xinerama / XRandR | override the monitor layout (and refresh rates); for testing |
| `WALPE_FPS_LOG=s` | off | every `s` seconds one log line per monitor: frames really presented, refresh rate, role (leads / follows), fps setting, swap interval; a video adds frames decoded and shown |
| `WALPE_CADENCE=1` | off | print the interval of every swap |
| `WALPE_NO_REFRESH_CAP=1` | off | do not limit a monitor's scene to its refresh rate (fps setting only) |
| `WALPE_LEADER=first` | fastest monitor | which monitor of a shared group renders |
| `WALPE_NO_SHARED_RENDER=1` | config `render_shared` (on) | render a scene separately on every monitor |
| `WALPE_NO_SHARE=1` | off | no texture sharing between the monitors' GL contexts (one copy per monitor) |
| `WALPE_POINTER_SHARE=1` | config `pointer_share` (off) | a scene that follows the pointer is rendered once for same-size monitors |
| `WALPE_PTR_WAKE=1` | off | wake the render thread for every pointer sample instead of at the rate the wallpaper is drawn at (A/B) |
| `WALPE_POINTER_POLL=1` | off | keep polling the pointer position while it rests instead of waiting for motion events |
| `WALPE_FOLLOW_GATE=1` | off | a monitor that shows another monitor's frames always waits for its own refresh slot (A/B) |
| `WALPE_SKIP_HIDDEN=off\|auto\|always` | config `skip_hidden` (off) | "Skip hidden areas" for one run |
| `WALPE_SKIP_SETTLE_MS=n` | 500 | with `skip_hidden=always` under a compositing manager: how long after its last move or resize a window counts as settled and hides the wallpaper again |
| `WALPE_ONE_XCONN=1` | off | EGL shares the main loop's X connection instead of using its own |
| `WALPE_EGL_CONFIG=first\|<id>` | best match | take the first EGL config the driver lists, or the one with that config id |
| `WALPE_EGL_SWAP=keep` | ask for `EGL_BUFFER_DESTROYED` | leave the surface's swap behaviour at the driver's default (A/B) |
| `WALPE_PRESENT`, `WALPE_IDLE_RELEASE`, `WALPE_EGL_DISPLAY`, `WALPE_SWAP_DAMAGE`, `WALPE_FRAME_ALIGN`, `WALPE_YIELD`, `WALPE_THREADED`, `WALPE_MAX_FRAMES`, `WALPE_SYNC_DISPLAY`, `WALPE_SHADER_CACHE`, `WALPE_SHADER_CACHE_SIZE` | see [GPU-PRESENT.md](GPU-PRESENT.md) | every key of `~/.config/walpe/gpu.conf` can be given as `WALPE_<KEY>`; the environment wins over the file |

## Scenes: textures

| Variable | Default | Effect |
|---|---|---|
| `WALPE_TEX_DOWNSCALE=0` / `=1` | config `tex_downscale` (on) | oversized layer pictures stored at the size they are shown at |
| `WALPE_TEX_SLACK=n` | config `texture_slack` (2) | per cent (0-10) a reduced texture or an effect's output may be magnified when drawn; `0` = exact sizes |
| `WALPE_TEX_MIPMAPS=0` | on | no mip chains (level 0 only) |
| `WALPE_TEX_ANISO=n` | 4 on hardware GL, off on software GL | anisotropic filtering (0 = off) |
| `WALPE_TEX_LOD_BIAS=b` | -0.5 | trilinear LOD bias (0 = softer, -1 = sharper) |
| `WALPE_TEX_STORED_MIPS=0` | on | reduced textures are made by halving level 0 on the GPU instead of taking the file's own smaller levels |
| `WALPE_TEX_COMPACT=0` | on | RGBA8 data that only uses one or two channels is no longer stored as R8 / RG8 |
| `WALPE_TEX_DEDUPE=0` | on | identical pixels under different names are no longer shared |
| `WALPE_NO_DXT=1` | off | decode DXT textures to BGRA instead of uploading them compressed |
| `WALPE_TEX_DXT_FX=0` / `WALPE_TEX_DXT_SPRITES=0` | on | DXT data is decoded to RGBA8 for layers with effects and effect textures / for particle sprites |
| `WALPE_TEX_LAZY=0` | on | layer textures are loaded while the scene is parsed instead of when first drawn |
| `WALPE_TEX_LOAD_HIDDEN=1` | off | the textures of hidden layers are loaded with the scene instead of when the layer is first shown |
| `WALPE_TEX_ASYNC=0` | on | PNG / JPEG layer pictures are decoded on the render thread at full size instead of on worker threads at the size they are drawn at |
| `WALPE_TEX_DECODE_MB=n` / `WALPE_TEX_DECODE_THREADS=n` | 64 / half the CPUs, at most 4 | decoded pictures those workers may hold at a time (one picture is always allowed) / worker threads |
| `WALPE_TEX_CPU_REDUCE=0` / `=2` | pictures with even sizes are reduced before the upload | `0`: upload at full size and halve with GL blits; `2`: odd sizes are reduced before the upload too (those textures differ from the blit's by rounding) |
| `WALPE_TEX_OWN_FULL=0` | on | without a share group (`WALPE_NO_SHARE=1`): a reduced copy of a picture is made by decoding its file a second time |
| `WALPE_TEX_FX_SOFT=1` / `=keep` | decided per layer | `1`: every layer's picture is stored reduced to the window as the input of its effects (less GPU memory; layers whose first effect reads the picture at moved coordinates get softer). Default: such a layer keeps its picture at file size when the effect moves more than a quarter of the frame and the picture has fine detail. `keep`: every such layer stays at file size |
| `WALPE_TEX_FX_ODD=0` | on | a layer with effects and an odd texture size keeps its full-size input |
| `WALPE_IMAGE_FULLRES=1` | off | still images are stored at full size instead of at the size they are shown at |
| `WALPE_TEXT_PART=0` | on | a text layer whose string changes is rendered and uploaded whole instead of only where the old or the new string has ink |
| `WALPE_GPU_MEM=1` / `=2` | off | log texture count and bytes every 300 frames (`2` lists textures of 2 MiB and more) |

## Scenes: effects

| Variable | Default | Effect |
|---|---|---|
| `WALPE_NOFX=1` | off | no effect chains at all |
| `WALPE_FX_SKIP=a,b` / `WALPE_SKIP_IDS=12,34` | off | do not build effects whose file name contains `a` or `b` / do not draw these object ids |
| `WALPE_FX_TIME=s` | the clock | fixed effect time (for screenshots that can be compared) |
| `WALPE_FX_CACHE=0` / `=1` | config `fx_cache` (on) | cache of static effect chain output. `WALPE_FX_CACHE_CHECK=1` re-renders every 20th cached output and logs any difference |
| `WALPE_FX_STATIC_PASS=0` | on | every pass of a multi-pass effect is drawn every frame. Default: a pass that writes its own target and reads nothing that changes between frames is drawn once and kept until a constant of that effect changes |
| `WALPE_FX_REGION=0` | on | effect passes fill their whole targets. Default: a pass is clipped (scissor only) to the part of its target that is read later in the same run, and a pass whose target nothing reads is not drawn; same shaders, same bytes where the picture is read (`src/engine/fx_region.h`). Not used with `fx_specialize`, `fx_quality=fast`, effect rates above 1 or for cached results |
| `WALPE_FX_REGION_RECTS=n` / `WALPE_FX_REGION_MINPX=n` | 6 / 32768 | rectangles per restricted pass (1-6) / pixels a rectangle must save to be worth its GL calls (0 = any saving) |
| `WALPE_FX_REGION_LOG=1` / `WALPE_FX_REGION_CHECK=n` | off | log which passes are restricted and to what share / draw every nth restricted run again whole and compare where it is read (a difference is logged as `fx region check: ...`; costs two read-backs per compared run) |
| `WALPE_FX_SHARE=1` | off | one GL program per shader text instead of one per pass (fewer programs to compile at load, more uniform calls per frame; same picture). Always in use with `fx_specialize` / `fx_quality=fast` |
| `WALPE_FX_SPEC=0` / `=1` | config `fx_specialize` (off) | effect constants compiled into the shaders |
| `WALPE_FX_QUALITY=exact` / `fast` | config `fx_quality` (exact) | cheaper shader variants and fused displacement passes |
| `WALPE_FX_SPEC_VERIFY=n` | 6 | comparisons a specialised shader must pass before it is used (0 = unchecked) |
| `WALPE_FX_HOIST=0` | on (only with `fx_specialize`) | no moving of per-pixel constant work into the vertex shader |
| `WALPE_FX_ALPHA=after` | before | a layer with effects and an opacity below 100 %: `after` applies the opacity to the finished result instead of to the layer's own colour before its effects (the default is how Wallpaper Engine draws it); read when a wallpaper loads |
| `WALPE_FX_POST_LEVEL=0` | measured | with post-processing `off` (preset Low): leave bloom / film grain / vignette / chromatic aberration out by their name alone. Default: such an effect is only left out once it was measured, in its first 32 frames, to leave the picture's mean colour alone |
| `WALPE_FX_FULLRES=1` | off | effect render targets at the layer texture's size instead of its on-screen size |
| `WALPE_FX_MIN_SCALE=f` | 1/64 | effects never work below this fraction of the texture size (up to 1) |
| `WALPE_FX_RT16F=1` | off | effect render targets in RGBA16F instead of RGBA8 (banding diagnosis) |
| `WALPE_FX_RT_FORMATS=0` | on | ignore the target formats an effect declares (every target RGBA8) |
| `WALPE_FX_CAPTURE_RGBA=1` | off | the copy of the scene behind a layer is always RGBA8 |
| `WALPE_FX_CLEAR=1` / `=invalidate` | clear when needed | clear every effect target before each pass / invalidate it instead |
| `WALPE_FX_STATE_CACHE=0` / `WALPE_GL_DRAW_CACHE=0` | on | no filtering of redundant GL state changes in effect passes / in quad draws |
| `WALPE_FX_POOL_IDLE_MS=n` | 5000 | how long an unused effect target stays in the pool (at least 50) |
| `WALPE_FRAME_SKIP=0` | on | draw every frame tick, also when nothing changed |
| `WALPE_BAKE=0` | on | composite every layer every frame instead of reusing the still ones |
| `WALPE_RENDER_SCALE=n` | config `render_scale` (100) | render scale in per cent (25-100) for this run |

## Scenes: particles and scripts

| Variable | Default | Effect |
|---|---|---|
| `WALPE_PT_LOWRES=off\|auto\|all` | config `particle_lowres` (auto) | large soft particles through a 1/2 or 1/4 size buffer: never / only where the picture stays nearly the same / everything drawn large |
| `WALPE_PT_LOWRES_TOL=n` | 8 | the predicted resampling error of one sprite, in 1/255, that `auto` accepts (not a bound on the picture: overlapping sprites add up) |
| `WALPE_PT_DENSITY=n` | config `particle_density` (100) | every system emits n per cent of its particles (5-100) |
| `WALPE_PT_INSTANCED=0` / `=1` | config `particle_instancing` (on) | sprites as six full vertices each instead of one instanced record |
| `WALPE_PT_REFRACT_COPY=whole` / `=each` | what is missing | a refracting particle system copies the whole frame before it draws / the part it can read (the bounds of its sprites, each grown by how far its normal map can move a sample; nothing without a normal map). Default: of that part only what the copy made for the systems before it in the same frame does not hold, or what was drawn on since; the same picture in all three |
| `WALPE_PT_CAMERA=fixed` | follows the scene | perspective particle systems are seen from 1000 units in every scene instead of from the distance that follows the scene's height and field of view |
| `WALPE_PT_SIZE=full` | half | sprites as wide as their `size` instead of half of it |
| `WALPE_PT_WORLD=scaled` | own size | "world space" systems are drawn through their layer's scale and rotation instead of at their own size in scene units |
| `WALPE_PT_CAP=count` | the file's `maxcount` | the cap of live particles follows the instance's `count` (file maxcount x count) |
| `WALPE_PT_FADE=span` | start point | the `alphafade` operator reads `fadeouttime` as the length of the fade-out instead of the point of a particle's life where it starts |
| `WALPE_PT_STATS=1` / `=2` | off | every 150 frames one line with what the particles cost (live / drawn / off-screen, draw calls, KiB uploaded, areas, CPU ms); `2` adds one line per system at unload |
| `WALPE_NO_SCRIPTS=1` | off | no SceneScript |
| `WALPE_SS_PROFILE=1` | off | every 300 frames the log names the ten scripts that cost the most CPU |
| `WALPE_TRACE_ANIM=1` / `WALPE_TRACE_CURSOR=1` | off | log property animation events / where cursor events land |
| `WALPE_NO_MEDIA=1` | off | media integration off (scripts and pages see `enabled: false`) |
| `WALPE_NO_AUDIO=1` / `WALPE_AUDIO_SOURCE=name` / `WALPE_AUDIO_HZ=n` | on / default output monitor / 60 | audio capture off / another Pulse source / spectra per second |

## Video

| Variable | Default | Effect |
|---|---|---|
| `WALPE_VIDEO_HWDEC=0\|1\|cuda\|vaapi\|vdpau` | config `video_hwdec` (off) | hardware decoding off / on / one FFmpeg hwaccel only. Never used on software GL |
| `WALPE_VIDEO_LIMIT=0\|1` | config `video_limit` (off) | videos follow the frame rate limit |
| `WALPE_VIDEO_UPLOAD=mode` | config `video_upload` (legacy) | how frames reach their textures: `legacy`, `pbo`, `pbodata`, `persist`, `direct`, each also `+pingpong` |
| `WALPE_VIDEO_THREADS=n` | by need, at most 1 up to 720p, 2 up to 1080p, 3 up to 1440p, 4 above | fixed number of decoder threads |
| `WALPE_VIDEO_SKIP_DEBLOCK=1` / `=nonref` | filter on | leave the H.264 deblocking filter out for all pictures (blocky) / only for pictures nothing is predicted from |
| `WALPE_VIDEO_FULLRES=1` | off | never decode at half size |
| `WALPE_VIDEO_SIMD=c\|sse2` | AVX2 when the CPU has it, else SSE2 | kernel of the half-size reduction (same bytes; for comparisons) |
| `WALPE_CPU_YUV=1` / `WALPE_GPU_YUV=1` | GPU unless the GL renderer is software | where the colour conversion runs |
| `WALPE_VIDEO_NO_R16=1` | off | no 16-bit plane textures: 10-bit video is reduced to 8 bits on the CPU |
| `WALPE_VIDEO_PARK_S=n` | 5 | seconds after which a paused video's decoder is freed (0 = never) |

## Web wallpapers

| Variable | Default | Effect |
|---|---|---|
| `WALPE_WEB_ACCEL=0\|1\|dmabuf\|force` | config `web_accel` (off) | WebKit hardware acceleration off / on (frames through shared memory) / on with WebKit's DMA-BUF path / on without the software-renderer check |
| `WALPE_WEB_INPUT=js\|native\|off` | real input events, script events as fallback | mouse input: script-dispatched events only / never fall back to script events / none |
| `WALPE_WEB_OFFLINE=1` | off | no network access for any page (per wallpaper: its option "Allow network access") |
| `WALPE_WEB_OPEN_LINKS=1` | off | an external http(s) link you clicked is handed to `xdg-open` (default: refused) |
| `WALPE_WEB_SELECTIONS=1` | off | let pages set the X selections (default: the daemon never takes PRIMARY or CLIPBOARD) |
| `WALPE_WEB_SCHEME=1` | off | confined `walpe-web://` scheme instead of the loopback server (no media playback) |
| `WALPE_WEB_FILE_URLS=1` | off | diagnosis only: load the page from `file://`. WebKit then lets the page read every local file you can read, so such a page gets no network access |
| `WALPE_WEB_KEEP_RUNNING=1` | off | pages keep running while paused |
| `WALPE_WEB_AUDIO_SILENCE=off\|always` | one call a second once the picture stands still | audio listener in silence: always 30 calls a second / no calls after 1.5 s |
| `WALPE_WEB_NO_RAF_LIMIT=1` | off | no `requestAnimationFrame` limiter script |
| `WALPE_WEB_BIGWIN=1` | off | the page's hidden window at full size, painted by GTK |
| `WALPE_WEB_SNAPSHOT=1` | off | capture pages with WebKit snapshots instead of the frame path |
| `WALPE_WEB_MEMORY_MB=n` | off | experimental: page processes release memory from n/2 MB |
| `WALPE_WEB_CONSOLE=1` / `WALPE_WEB_TIMING=1` | off | the page's console output and input events go to the daemon log / every 2 s the CPU cost of the page's frame path |

## Shader diagnosis and profiling

| Variable | Default | Effect |
|---|---|---|
| `WALPE_PROBE=1` | off | log centre / quarter pixels after each frame (needs `G_MESSAGES_DEBUG=all`) |
| `WALPE_FX_PROBE=1` | off | log input / output pixels of effect chains |
| `WALPE_FX_STATS=1` | off | log effect passes drawn / served from the cache per frame |
| `WALPE_FX_POOL_LOG=1` | off | every 300 frames one line about the pool of effect targets |
| `WALPE_FX_DUMP=dir` / `WALPE_FX_DUMP_ALL=dir` | off | write effect shaders that fail to compile (all shaders), as compiled, with the log |
| `WALPE_FX_COST=frame` | off | at that frame, print texture reads per pixel of every effect pass |
| `WALPE_SCENE_PROFILE=frame` | off | at that frame, one log line per drawn layer: size on screen, pixels shaded, passes drawn / cached |
| `WALPE_PIX_STATS=frames` | off | log the fragments shaded per frame (desktop GL only) |
| `WALPE_MEM_STATS=1` | off | `walpe-ctl status` also carries allocator and process memory numbers |
| `WALPE_GPU_PROFILE=s`, `WALPE_GPU_PROFILE_DELAY=s`, `WALPE_GPU_PROFILE_EVERY=n`, `WALPE_GPU_PROFILE_SYNC=1`, `WALPE_GPU_PROFILE_TIMER=0`, `WALPE_GPU_PROFILE_HOOKS=0`, `WALPE_GL_DEBUG=1` | off | the GPU profiler: see [GPU-PROFILING.md](GPU-PROFILING.md) |

## For developers and tests

Not for daily use: test hooks, measuring aids and names the programs set for themselves.

| Variable | Read by | Purpose |
|---|---|---|
| `WALPE_FAKE_VSYNC=1` | daemon | every swap waits for an emulated vertical blank (X servers without vsync, such as Xvfb) |
| `WALPE_FAKE_DPMS=file` / `WALPE_FAKE_POWER=file` | daemon | the display's power state / the power source is read from a file instead of from X / the kernel |
| `WALPE_LOAD_DELAY_MS=n` | daemon | every wallpaper load takes at least `n` ms more (what must hold while a load runs) |
| `WALPE_WS_API_BASE=http://127.0.0.1:PORT/` | Workshop client | a loopback stand-in for the Steam Web API; nothing but that spelling is accepted, so the variable cannot send the key elsewhere |
| `WALPE_WS_TOTAL_TIMEOUT=n` | Workshop client | seconds one Workshop request may take in all (default 120) |
| `WALPE_STEAM_WAIT_START=s` | GUI | how long to wait for the Steam client to start a download (default 45) |
| `WALPE_UI_ANIM_MB=n` | GUI | frame budget of animated previews |
| `WALPE_UI_DEBUG=1` / `WALPE_UI_STATS=file` | GUI | timing of the options panel / counters of the GUI written to a file (SIGUSR1 resets, SIGUSR2 dumps) |
| `WALPE_VIDEO_MD5=n`, `WALPE_VIDEO_TEST_NV12=1`, `WALPE_VIDEO_TEST_TAIL=s`, `WALPE_VIDEO_THREADS_START=n` | video module | checksums of the first `n` frames, the NV12 layout path, start near the end of a clip, thread count a clip starts with |
| `WALPE_FX_NO_PREPASS=1`, `WALPE_FX_LOG_AS=nvidia\|glslang\|none`, `WALPE_FX_FAIL=a,b`, `WALPE_FX_DUMP_SRC=dir` | engine | shader repair tests: skip the source inspection, reword or withhold the driver's compile log, make effects fail to build, write every pass's sources before any repair (`build/fx-repair-test dir` checks that all of them build) |
| `WALPE_FX_PASS_DUMP=dir` / `WALPE_FX_SPEC_DUMP=dir` | engine | with `WALPE_FX_COST`: write what every pass of that frame drew / write the specialised shader sources as compiled |
| `WALPE_CPU_PHASES=1` / `=2` | engine | CPU time of a scene frame by phase, read by `build/scenecpu`; `2` adds hashes of the particle state |
| `WALPE_PROP_PROBE=dir` / `WALPE_PROP_PROBE_FRAME=n` | engine | scenes record which user-property values they consumed and write a digest of their state at frame `n` (default 6); used by the option-coverage tool |
| `WALPE_IPC_FD`, `WALPE_LAYOUT_RESTARTED`, `WALPE_GL_HINTS_SET`, `WALPE_WEB_ENV_OURS`, `WALPE_WEB_PROBED`, `WALPE_WEB_PROBE_ACCEL`, `WALPE_WEB_PROBE_PARENT` | daemon | set by the daemon for its own restart in place and for its WebKit start-up test and web acceleration probe; never set them by hand |
| `WALPE_UI_SET_GSK`, `WALPE_UI_SET_GDK` | GUI | set by the GUI to remember that a GTK renderer variable is its own choice, not yours |
| `WALPE_WS_FIXTURES=dir`, `WALPE_WS_LIVE=1` | `workshop-test` | another fixture directory / also run the calls against Steam's servers |
| `WALPE_FX_HOSTILE_CORPUS=dir`, `WALPE_FX_HOSTILE_VERBOSE=1`, `WALPE_SCENE_HOSTILE_VERBOSE=1`, `WALPE_TEST_VERBOSE=1` | test programs | extra shader corpus and verbose output |
| `WALPE_JOBS`, `WALPE_LOAD`, `WALPE_BUILDDIR` | `build.sh` | ninja's `-j` / `-l` and the build directory |
| `LIFECHECK_PATIENCE=N` (or `RESTORECHECK_PATIENCE=N`), `VIDEO_TEST_SLOW=n`, `CLIP_TEST_DEVICE=n`, `CLIP_TEST_LAYOUTS=a,b`, `CLIP_TEST_WINDOW=x,y,w,h`, `WEBSEC_KEEP=1` | single tests | knobs without the `WALPE_` prefix: every wait of `daemon-life` N times as long, the video test's time limits n times as long (valgrind), another EGL device / only some window layouts / one more window for the clip test, keep the web security test's temporary directory |
