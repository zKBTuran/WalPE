# Measuring WalPE's GPU use

Two tools. Both are off / not running unless you ask for them.

## 1. Comparing two builds or two settings (three commands)

`scripts/walpe-gpu-report.sh` (installed as `walpe-gpu-report`) works with any
build, also one that has no profiler. With your wallpapers showing:

    # 1) the first build (or setting) running, wallpapers on screen for a minute:
    sh scripts/walpe-gpu-report.sh 30 old > ~/walpe-gpu-old.txt
    # 2) quit it, start the other build's walpe-daemon, same wallpapers, wait a minute:
    sh scripts/walpe-gpu-report.sh 30 new > ~/walpe-gpu-new.txt
    # 3) compare the two files (or pack them for a bug report):
    tar czf ~/walpe-gpu-numbers.tgz -C ~ walpe-gpu-old.txt walpe-gpu-new.txt

Each run samples once a second for 30 s and prints min / avg / max of

- `nvidia-smi`: utilization.gpu, utilization.memory, memory.used, power.draw,
  clocks.gr, clocks.mem, temperature.gpu, pstate, and an estimate of the load
  at full clock (utilization x clocks.gr / clocks.max.gr);
- `nvidia-smi pmon`: walpe-daemon's own GPU memory and GPU share, and the
  same for every other program on the GPU (X, the compositor);
- `/proc`: walpe-daemon's CPU %, resident memory, threads, and its busiest
  threads by name (`av:h264:*` and `walpe-video` = video decode,
  `walpe-monN` = a monitor's render thread);

with what `walpe-ctl status` says about the monitors (refresh rate, wallpaper
id, paused / covered, the frame rate a scene is paced at) and whether a
compositor runs. Then, if the running daemon has the profiler (section 2),
it appends a 30 s `walpe-ctl profile` report. A value the GPU, the driver or
`nvidia-smi` does not have is printed as `n/a`; the rest still works. The
output has no file paths; of the wallpapers it holds workshop ids and, in
the profiler part, layer and effect names.

    sh scripts/walpe-gpu-report.sh 30 idle --baseline

with WalPE closed measures the desktop alone (X and the compositor) with the
same table; without `--baseline` the script does the same whenever no
walpe-daemon runs.

A build without the profiler does not tell how many frames it draws. Start
it as `WALPE_FPS_LOG=5 walpe-daemon 2>~/walpe.log` and the report shows the
daemon's own count per monitor.

Keep the runs comparable: same wallpapers, same windows open, same power
source (mains / battery), wallpapers fully loaded before starting.

"GPU share (sm)" is `nvidia-smi pmon`'s per-program figure. It prints `-` for
a second in which the driver has no figure for the program, so the script
gives two averages: over the seconds that had a value, and with the missing
ones counted as 0. Read the pair as a range.

### Reading utilization.gpu

NVIDIA defines it as the "Percent of time over the past sample period during
which one or more kernels was executing on the GPU" - the share of *time* the
GPU was busy at whatever clock it was running at, not the share of its
capacity. A laptop GPU in a low performance state (low `clocks.gr`, a P-state
far from P0) takes longer for the same work, so light work can read as a
high percentage. `power.draw` (watts) and the clocks printed next to the
percentage are what make two builds comparable: fewer watts at the same or
a lower clock is less GPU work.

## 2. The profiler: `walpe-ctl profile [SECONDS] [--json] [--sync]`

Measures every monitor's render thread for SECONDS (default 10), prints the
report and keeps it as `~/.cache/walpe/gpu-profile-<time>.txt` and `.json`
(the newest also as `gpu-profile.txt` / `.json`).

Per monitor:

- a table of the 15 most expensive rows, most GPU time first. A row is a
  layer's own draw (`(layer draw)`: its quad or mesh into the frame, its
  particles, a compose layer's capture) or one effect pass of a layer
  (`blur (e2 p1)` = effect `blur`, the layer's effect number 2, pass 1), with
  its render-target size. Columns: GPU ms per frame and its share, CPU ms
  per frame (render thread, issuing the commands), runs / draw calls /
  megapixels shaded per frame (target area x passes; for a layer draw the
  area of its quad on screen);
- per frame: effect passes drawn (and served from the static cache), draw
  calls, megapixels shaded, clears, blits, render-target switches, bytes
  uploaded to textures and buffers, readbacks (glReadPixels / glGetTexImage),
  glFinish / glFlush calls, mipmap builds;
- frames rendered, presented, skipped as unchanged;
- render-thread milliseconds per frame by stage, wall and CPU: `update`
  (scripts, audio / media, video frames), `layers` (passes and draws), `post`,
  `publish` (shared rendering), `swap` (eglSwapBuffers: waits for vsync),
  `present` (a monitor that shows another monitor's frame), `profiler`;
- a `TOTAL monN` line. "GPU busy" = GPU ms per frame x frames per second,
  as a share of wall time (what `utilization.gpu` would show if nothing
  else used the GPU).

What the report says about itself (added after the first reports from the maintainer's
card, an RTX 4050):

- `NOTE: monitor N was paused for S s of this run (reason)` right under the
  header, and `paused for S s of the T s (reason)` in the monitor's block: the
  wallpaper drew nothing for that part of the run (a fullscreen window had the
  focus, windows covered it, `walpe-ctl pause`, the display slept). Such a run
  reads as a low frame rate - 850 frames in 30 s are "28.3/s" when the
  wallpapers ran at 60 for 14 s and were paused for 16 - so the frames line
  gives both: `frames: 850 rendered (28.3/s over the run, 59.8/s while
  drawing)`. "GPU busy" on the TOTAL line is the share of the whole run; the
  figure in brackets is the share while drawing. `effect targets 0 / 0 B` in
  the memory line is the same sign: a paused scene gives its targets back.
  Without a pause, `nothing was drawn for S s` means a still picture, a scene
  in which nothing changed, or a wallpaper that was loading. Take a profile
  with the wallpapers visible for the whole run (no fullscreen window).
- A row marked `*`: `GL_TIME_ELAPSED` runs from "everything before the row
  has completed" to "everything in the row has completed", so the time the
  render thread spends inside the row is in it as time the GPU waited. A row
  is marked when its CPU ms are at least 0.05 and at least 30 % of its GPU
  ms; its GPU work is between GPU - CPU and GPU. The footnote sums what may
  be waiting, `TOTAL monN` adds `at least X ms of it GPU work` and the last
  line `(at least X ms GPU work ...)`. The usual case is an upload: the
  driver copies the pixels out of client memory inside `glTexSubImage2D`.
  Not marked on software GL and with `--sync` (their rows are wall time
  anyway).
- The update stage of a scene has two rows: `(scene update, uploads)` (video
  planes, text) and `(scene update: draws)` - what is drawn before the layers,
  i.e. a video frame's YUV -> RGBA conversion pass, with its target size and
  megapixels. The first shows what the upload costs, the second what the
  conversion costs.
- JSON: per monitor `paused_seconds`, `pause_reason`, `idle_seconds`,
  `draw_runs`, `fps_while_drawing`, `gpu_ms_per_frame_min`, `gpu_time_is_wall_time`; per
  row `gpu_ms_maybe_waiting`.

Then, for the whole daemon: texture and render-target memory by category
(compressed textures, other textures, effect targets, other targets,
buffers, window buffers), what the driver says about video memory
(NVIDIA: total, free, evictions during the run), the driver's KHR_debug
messages de-duplicated with counts (PERFORMANCE messages first: shader
recompiles, slow paths, buffer moves between video and host memory,
synchronous pixel transfers), and one `TOTAL:` line.

GPU time comes from `GL_TIME_ELAPSED` timer queries (desktop GL 3.3 or
ARB_timer_query; GLES 3 with EXT_disjoint_timer_query). Without them the
table shows CPU time only and says so. The `timer:` line of each monitor
tells which it was.

Options and switches:

- `--sync`: no timer queries; glFinish after every row and the wall time in
  between as its GPU time. Slows rendering, so frame rate and totals are not
  those of a normal run, but the split between rows is exact on any driver.
  Use it on software GL (llvmpipe), whose timer queries do not follow the
  draw order. Also `WALPE_GPU_PROFILE_SYNC=1`.
- `WALPE_GPU_PROFILE=<seconds>` in the daemon's environment: one profile
  shortly after start-up (`WALPE_GPU_PROFILE_DELAY=<seconds>`, default 5).
- `WALPE_GL_DEBUG=1` (also implied by `WALPE_GPU_PROFILE`): the monitors get
  debug GL contexts. A driver may keep some messages to itself otherwise;
  the `debug:` line of the report says which kind of context it was.
- `WALPE_GPU_PROFILE_EVERY=N`: timer queries on every Nth frame only.
- `WALPE_GPU_PROFILE_TIMER=0`: no timer queries (CPU time only).
- `WALPE_GPU_PROFILE_HOOKS=0`: do not count draw calls / uploads (see below).

### Cost

Off: each hook in the renderer is one load of a flag and a branch that is
not taken. No GL call is made or wrapped, nothing is allocated.

On: two timer-query calls per row and frame, results read back without
waiting a frame or two later; counters for draw calls, uploads, blits and
readbacks come from small wrappers the profiler puts into libepoxy's
dispatch pointers for the length of the run and takes out again.

### For developers

Hooks are the `WALPE_PROF_*` macros (`src/engine/gpuprof.h`), marked
`GPUPROF` where they are used: `WALPE_PROF_LAYER_BEGIN / _END` around a
layer in `scenenode_gl_render`, `WALPE_PROF_PASS_BEGIN / _END` around a pass
draw in `fx_chain_run` and the layer-colour pass, `WALPE_PROF_QUAD` in the
quad draws of `glutil.c`, `WALPE_PROF_STAGE`, and the frame begin / end in
the daemon. A new pass or layer draw site needs a begin / end pair; new
draw calls, uploads or blits inside them are counted without any hook. A
renderer that skips unchanged frames calls `WALPE_PROF_FRAME_SKIPPED`.
Layers and passes do not nest: a begin closes what was left open.

Tests: `gpuprof` (report formatting on hand-made reports; then real runs on
GL 3.3 and GLES 3 where an EGL device or surfaceless platform exists) and
`gpu-report` (the script against a stand-in `nvidia-smi`).
