# Presentation settings (swap, vsync, driver hints)

How a rendered frame gets from a monitor's render thread to the screen, and the
switches for it. Code: `src/common/present.h` (rules, tested by `tests/present_test.c`),
"presentation" section of `src/daemon/main.c`.

Defaults: without any of these settings the daemon presents with swap interval 1 on every
monitor and a frame clock at min(fps limit, refresh rate).

## Where settings come from

1. Environment `WALPE_<KEY>` (upper case), e.g. `WALPE_PRESENT=auto walpe-daemon`.
2. `~/.config/walpe/gpu.conf`, lines `key = value`, `#` comments. Use this when the daemon
   is started by the GUI, the tray or autostart.
3. `walpe-ctl set present=...` changes `present` on the running daemon (next swap, not saved).

Read once at start-up, before any thread exists and before EGL is loaded. Restart the
daemon after editing gpu.conf. The start-up log (`present: ...` lines) shows what is in effect;
`walpe-ctl status` has `monitors[].present`, `monitors[].swap_interval`, `compositor`.

## Keys

| key | values | expected effect |
|---|---|---|
| `present` | `vsync` (default), `limiter`, `auto`, plus `NAME=mode` per monitor (`auto,HDMI-0=limiter`) | `vsync`: swap interval 1, the driver shows each frame at a display refresh and may make the thread wait. `limiter`: swap interval 0, only the frame clock paces (sleeps to the deadline, swap returns at once); no wait inside the driver, tearing possible without a compositor. `auto`: `limiter` while a compositing manager (picom) runs or on a monitor other than the one named by `sync_display`, else `vsync`. |
| `yield` | `usleep`, `nothing`, `default` | sets `__GL_YIELD` (NVIDIA): how the driver waits when it has to. `usleep` = sleep instead of `sched_yield()` spinning: lower daemon CPU if the driver was spinning. |
| `max_frames` | `1`..`8` | sets `__GL_MaxFramesAllowed` (NVIDIA, not in its README): frames a vsync'd swap may queue before it blocks (driver default 2). `1` = less latency, swap blocks every frame. |
| `sync_display` | an output name (`DP-2`) | sets `__GL_SYNC_DISPLAY_DEVICE` (NVIDIA): the one display vsync'd swaps wait for. Name the fastest monitor, or it paces at the slower one's refresh. |
| `threaded` | `on`, `off` | sets `__GL_THREADED_OPTIMIZATIONS` (NVIDIA): driver CPU work moves to a worker thread. May lower or raise CPU; off in the driver by default. |
| `shader_cache` | `on`, `off`, `/absolute/dir` | sets `__GL_SHADER_DISK_CACHE` (and `_PATH`) (NVIDIA): the driver's disk cache of compiled shaders; on by default for a non-root user with `HOME` set. A directory moves it. |
| `shader_cache_size` | bytes | sets `__GL_SHADER_DISK_CACHE_SIZE` (NVIDIA). |
| `idle_release` | seconds (0 = off, default) | a wallpaper paused this long (covered, fullscreen app, display asleep, `walpe-ctl pause`) is unloaded: its textures and render targets leave the GPU; it loads again on resume (a video starts over). Like Wallpaper Engine's "Stop (free memory)". |
| `egl_display` | `own` | EGL uses an X connection of its own: the main loop's X requests never wait behind a render thread's swap. This is what the daemon does by default, whatever this key says (`WALPE_ONE_XCONN=1` in front of the daemon = one shared connection). |
| `swap_damage` | `on` | swaps go through `eglSwapBuffersWithDamage` where the EGL has it (test of the path: the damage is the whole surface, no content reports less yet). |
| `frame_align` | `on`, `off` (default) | every monitor's frame clock runs on one grid (multiples of 1 / fps on the monotonic clock) instead of starting when its wallpaper was loaded. Two monitors at 60 fps then present within a millisecond or two of each other, so a compositing manager gets their damage together: about 60 wake-ups a second instead of up to 120, each of which makes picom and the X server compose and present the screen once more. Frames, frame count and pictures are unchanged; a frame moves by less than one frame time. Scenes only (a plain video wallpaper follows its clip's own frame times). Live: `walpe-ctl set frame_align=on\|off`; `walpe-ctl status` has `frame_align`. Unmeasured on NVIDIA: compare the X and picom lines of `walpe-gpu-report.sh` ("programs on the GPU") and `power.draw` with it on and off; the daemon's own profile must not change. |

If the start-up log says "the EGL config has no swap interval 0", `limiter` cannot take effect through EGL;
on NVIDIA `__GL_SYNC_TO_VBLANK=0` in the daemon's environment is the driver-level equivalent.

A variable already set in the daemon's environment (`__GL_YIELD=... walpe-daemon`) is never replaced.
Other drivers (Mesa) ignore the `__GL_*` variables.

Existing switches that belong to the same picture: `walpe-ctl set fps=N` (frame limit),
`WALPE_NO_REFRESH_CAP=1` (do not limit a monitor to its refresh rate), `WALPE_LEADER=first`,
`walpe-ctl set shared=on|off`, `WALPE_FPS_LOG=s` (its line now ends with the swap interval of
every monitor and whether a compositing manager runs).
