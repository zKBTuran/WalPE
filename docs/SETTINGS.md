# Settings

Where WalPE keeps what you set, every key, and how to change it.

| File | Holds | Written by |
|---|---|---|
| `~/.config/walpe/config.json` | all engine settings, the wallpaper of every monitor, wallpaper options, per-wallpaper quality, layer adjustments | the daemon alone (mode 0600), as a whole, with every change |
| `~/.config/walpe/gpu.conf` | presentation and driver hints, read once at start | you ([GPU-PRESENT.md](GPU-PRESENT.md)) |
| `~/.config/walpe/workshop.json` | Workshop tab settings and the optional Steam Web API key (mode 0600) | the GUI |
| `~/.config/walpe/tray.ini` | `walpe-tray`: `start_with_gui`, `confirm_quit`, `backend=auto\|sni\|xembed` | the GUI / the tray |

Change settings with the settings window of the GUI, the tray menu or `walpe-ctl set
key=value ...`. Do not edit `config.json` while the daemon runs: it writes the whole file
with every change. Members it does not know (a newer version's, a note written by hand) are
written back as they were, at the top of the file and inside an entry of `monitors`. A file
that is not a JSON object is not used and not overwritten:
it is kept as `config.json.broken-<time>`, the defaults apply and `walpe-ctl status` names
the copy (`config_broken_copy`). The file is written next to itself and renamed, so a save
that fails (a full disk, a home that cannot be written to) leaves the old file whole; the
change is in effect in the running daemon all the same, the reply to the request carries
the reason as `save_error` (`walpe-ctl` prints it) and `walpe-ctl status` shows
`config_save_error` until a save succeeds.

`walpe-ctl set` checks every pair before anything changes: one unknown key or one value of
the wrong kind and the whole request is refused. A switch takes `on` / `off` (also `true` /
`false` / `yes` / `no`).

## config.json

"set name" is the short name `walpe-ctl set` also accepts; the key itself works too. Where
that column is in brackets `walpe-ctl set` does not take the key: it is changed by the
command named there, or only in the file while the daemon is not running ("file only").
`walpe-ctl status` shows every key `set` takes under the key's name (`video_backend` as
`backend` and the window system setting `backend` as `display_backend_setting`, the
quality knobs in `walpe-ctl quality`, the scene feature switches in
`feature_switches`; `software_threads` there is the number in use,
`software_threads_saved` the setting).

### Wallpapers and playback

| Key | Default | set name | Meaning |
|---|---|---|---|
| `wallpapers_dir` | `~/.local/share/walpe/workshop` | (file only) | the library: project folders or loose media files |
| `current` | `""` | (`walpe-ctl open`) | the wallpaper of every monitor that has no entry in `monitor_current` |
| `monitor_current` | `{}` | (`walpe-ctl open --monitor N`) | output name -> wallpaper path |
| `paused` | false | (`walpe-ctl pause` / `resume`) | |
| `fps` | 60 | | frame rate limit of scenes and web pages, 5-200; a monitor never shows more than its refresh rate; a video or GIF keeps its own lower rate |
| `volume` | 0 | | volume of wallpaper sound, 0-1 ([SOUND.md](SOUND.md)); nothing is heard at 0 |
| `scene_sound` | false | | scenes play their sound layers; one wallpaper's own switch: `walpe-ctl setprop walpe_sound=on\|off` |
| `mute` | false | | wallpaper sound silent, the volume value stays |
| `parallax` | true | | scenes follow the pointer where they are made to |
| `video_backend` | `ffmpeg` | | stored only |
| `vsync`, `audio` | true, false | (file only) | stored only (presentation is set in `gpu.conf`) |
| `we_assets` | unset = auto-detect | | Wallpaper Engine install or its `assets/` folder; `we_assets=` (empty) goes back to auto-detection |
| `steam_user` | unset | | Steam account name for steamcmd downloads (never a password) |
| `props` | `{}` | (`walpe-ctl setprop` / `resetprops`) | wallpaper option overrides, `"<wallpaper path>\u001f<option>": "value"` |
| `layers` | `{}` | (`walpe-ctl layers ...`) | "Adjust layers", per wallpaper (below) |
| `config_version` | 4 | (written by the daemon) | format of the file; older files are migrated once (a frame rate that was an older version's default becomes 60; the monitors connected then count as asked about their render resolution, and `video_resolution` becomes `1080p`) |

### Pausing and limits by desktop state

| Key | Default | set name | Meaning |
|---|---|---|---|
| `pause_fullscreen` | true | `fs` | pause a monitor while a focused fullscreen window is on it; the other monitors keep running (a window across several monitors pauses each one it covers at least half of) |
| `pause_fullscreen_all` | false | `fs_all` | a focused fullscreen window pauses every monitor, not only the one it is on (what `pause_fullscreen` did up to 0.12) |
| `pause_dpms` | true | `dpms` | pause while the display is in power save or the screensaver runs |
| `pause_covered` | true | `covered` | pause a monitor while visible normal windows cover 85 % of it or more (X11); on Wayland: while the compositor does not show the wallpaper, or the focused window on that monitor is fullscreen or maximized |
| `transparent_windows` | `st` | | comma-separated list of see-through windows: WM_CLASS on X11, application ids on Wayland |
| `cover_transparent` | true | `transparent` | see-through windows (that list, or `_NET_WM_WINDOW_OPACITY` below 100 %) count as covering; off = they never cover |
| `focus_action` | `run` | `focus` | while an application's window on a monitor has the focus: `run`, `limit` (to `focus_fps`) or `pause` |
| `focus_fps` | 15 | | the limit of `focus_action=limit`, 1-200 |
| `pause_battery` | false | `battery` | pause every wallpaper on battery power |
| `battery_fps` | 0 | | frame rate limit on battery (0 = none) |
| `pause_idle` | 0 | `idle` | pause every wallpaper after this many seconds without input, 0-86400; 0 = off. Needs a session that reports idle time (a Wayland compositor with `ext-idle-notify-v1`; nothing on X11 in this version) |
| `preview_anim` | `always` | `previews` | animated previews in the GUI: `always`, `hover`, `off` |

Whether a pause setting can work in the running session, and from where its information
comes, is in `walpe-ctl status` (`pause_rules`) and `walpe-ctl backend`
([WAYLAND.md](WAYLAND.md)).

### Display, GPU and OpenGL API

All of these are written to the file only while they are not at their default.

| Key | Default | set name | Meaning |
|---|---|---|---|
| `backend` | `auto` | | the window system the daemon draws on: `auto`, `x11`, `wayland`. Read when the daemon starts; `walpe-ctl backend` shows the one in use and sets it too ([WAYLAND.md](WAYLAND.md)) |
| `wayland_pointer` | `auto` | | Wayland: the wallpaper takes pointer input while it uses the pointer (`auto`), or never (`off`) |
| `gpu` | `auto` | | which GPU renders: `auto`, `integrated`, `discrete`, or one device by PCI address, DRM node, ids or name; the daemon restarts itself ([GPU.md](GPU.md)) |
| `preset_follow_gpu` | false | | set the quality preset once when the kind of GPU that renders changes (discrete: high, integrated: balanced, CPU: low) |
| `preset_gpu_class` | unset | (written by the daemon) | the kind of renderer the preset was last chosen for |
| `gl_api` | `auto` | | `auto` (desktop OpenGL 3.3 where the driver has it, else OpenGL ES 3.2 / 3.1), `gl`, `gles`; the daemon restarts itself ([GPU.md](GPU.md)) |

### Render resolution

[RESOLUTION.md](RESOLUTION.md) has the details and `walpe-ctl resolution`.

| Key | Default | set name | Meaning |
|---|---|---|---|
| `monitors` | `{}` | (`walpe-ctl set resolution=...`, `texture_limit=...`) | output name -> `{ "resolution": native \| 720p \| 1080p \| 1440p \| 4k \| WxH, "texture_limit": auto \| none \| pixels }` and whether the monitor was asked about. `walpe-ctl set resolution=1080p` sets every connected monitor, `resolution=HDMI-0:1080p,DP-2:native` named ones |
| `video_resolution` | `auto` (new installation), `1080p` (a file from an older version) | | the largest size videos are imported and converted at: `auto`, `none`, a preset or `WxH`. Always written |
| `scale_filter` | `bilinear` | | how a scene rendered at another size than the monitor's is scaled to it: `bilinear`, `nearest` |
| `resolution_prompt` | true | | may the WalPE window ask about monitors it has not asked about |

### Rendering

| Key | Default | set name | Meaning |
|---|---|---|---|
| `render_shared` | true | `shared` | same-size monitors showing one scene: render once, present on each |
| `pointer_share` | false | | also for a scene that follows the pointer: it follows the pointer on the monitor the pointer is on, the others show that picture |
| `skip_hidden` | `off` | | "Skip hidden areas": what other windows hide is not rendered. `auto`: only while no compositing manager runs (then "hidden" is exact). `always`: also under one; windows without an alpha channel then count as opaque, which is wrong behind windows the compositor itself makes see-through (list their classes in `transparent_windows`) |
| `skip_hidden_margin`, `skip_hidden_inset` | 32, 0 | | with `always`: pixels drawn around what is visible / pixels taken off every side of a hiding window (rounded corners) |
| `fx_cache` | true | | reuse the output of effect chains that do not change |
| `fx_specialize` | false | | effect constants compiled into the shaders; each such shader is compared with the plain one before it is used (`walpe-ctl status`: `fx_verified`, `fx_rejected`). Reloads the scenes |
| `fx_quality` | `exact` | | `fast`: cheaper shader variants, stacked wave / shake effects in one pass (small pixel differences). Reloads the scenes |
| `tex_downscale` | true | | oversized layer pictures are stored at the size they are shown at (next load) |
| `particle_lowres` | `auto` | | large soft particles at reduced resolution: `off`, `auto` (only where the picture stays nearly the same), `all` |
| `particle_density` | 100 | | per cent of its particles every system emits, 5-100 |
| `particle_instancing` | true | | off = sprites as six vertices each (a way around a driver that mishandles instancing; same picture) |
| `software_render` | false | `software` | "Render on CPU": Mesa llvmpipe instead of the GPU driver; the daemon restarts itself. `walpe-ctl status`: `software_active`, `gl_renderer`, `software_note` |
| `software_threads` | 0 | | llvmpipe threads in that mode; 0 or `auto` = half of the CPUs, at most 8 |
| `web_accel` | false | | web pages on the GPU; restarts the daemon, is tested in a child process at start and falls back by itself (`web_render`, `web_webgl` in status) |
| `web_unload` | true | | when the wallpaper change that leaves no monitor with a web wallpaper comes, the daemon restarts in place instead of loading the next wallpaper itself, so WebKitGTK and GTK (which cannot be unloaded) leave its memory: every monitor loads its wallpaper again as at a start. At most once a minute; not while an import or a download runs or the copy of a scene's video is being made (`video_scene_convert`), and not when the settings cannot be saved (the restart reads the wallpapers from them). A restart that was put off for one of the first two reasons, or because the setting was off, is carried out at the next change of wallpapers - or before that, at the first moment at which the displays sleep or the session is locked and `pause_dpms` is on: nobody sees a wallpaper then. No other pause is such a moment (`walpe-ctl pause`, a fullscreen window, covering windows, battery, idle and focus rules). The wallpapers then start from their beginning when they are seen again. Off: the web module stays loaded until the daemon stops (`modules` in status) |
| `video_hwdec` | false | `hwdec` | decode videos with a hardware decoder: `hwdec=off\|auto\|<api>` (`on` = `auto`; [VIDEO.md](VIDEO.md)) |
| `video_hwdec_api` | unset | (`hwdec=<api>`) | the one API to use (`vaapi`, `cuda`, `vdpau`, `vulkan`, `drm`, `v4l2m2m`); written while one is chosen |
| `video_zerocopy` | false | | hardware-decoded pictures stay in GPU memory where the driver can do it and the samples are exactly the decoder's |
| `video_limit` | false | | videos follow the frame rate limit and decode only the pictures that are shown |
| `video_scene_convert` | true | | a video inside a scene that is larger than the scene shows it (a 4K clip on a 1080p monitor) is converted once, in the background, into a copy of the size shown (`video_resolution`) and played from it; the original plays until the copy is ready. `walpe-ctl status`: `scene_videos` ([VIDEO.md](VIDEO.md)) |
| `video_upload` | `legacy` | | how video frames reach their textures: `legacy`, `pbo`, `pbodata`, `persist`, `direct`, each also `+pingpong` (same picture) |

| `features` | `{}` | (`walpe-ctl set <switch>=off`, `=on`) | the scene feature switches that were ever set (`camera_parallax`, `parallax_offset`, `script_camera`, `sprite_sheets`, `script_texanim`, `bloom`, `camera_shake`), `{ "<name>": true\|false }`. All on by default except `parallax_offset` ([SCENE-FEATURES.md](SCENE-FEATURES.md)) |
| `fluid_sim` | true | | Wallpaper Engine's Fluid Simulation effect runs; off = its layer is shown without it |
| `fluid_quality` | `full` | | `full`, `reduced` (the smoke at half size), `low` (also 30 instead of 60 steps a second) |

Not saved: `walpe-ctl set present=...` and `frame_align=on|off` change the running daemon
only; `gpu.conf` keeps them.

## Quality presets

One setting that trades picture quality for GPU work: `walpe-ctl set preset=max|high|balanced|low`
(settings window: "Quality"). `high` is the default. `max` is the exact picture.

| Knob (config key) | max | high | balanced | low | Values |
|---|---|---|---|---|---|
| `fx_rate` | 1 | 1 | 2 | 2 | animated effects are redrawn every Nth frame, 1-3 |
| `fx_rate_mode` | slow | slow | slow | all | which effects `fx_rate` applies to: only those known to move slowly, or all |
| `fps_idle` | 0 | 0 | 0 | 30 | the wallpaper's own frame rate limit (0 = the `fps` setting), 5-200 |
| `fx_res` | full | full | screen | screen | effect passes at the next 1/8 step of the texture above the on-screen size (`full`), at the on-screen size (`screen`), at 3/4 (`reduced`) or 1/2 (`half`) of it |
| `post_processing` (`post`) | full | full | reduced | off | `reduced`: blur and bloom on smaller buffers; `off`: also no cosmetic full-screen passes (bloom, film grain, vignette, chromatic aberration). Scene bloom follows it too |
| `render_scale` (`scale`) | 100 | 100 | 100 | 75 | scene resolution in per cent of the monitor's render resolution, 25-100 |
| `tex_res` (`tex`) | full | full | full | half | layer textures stored at 1/1, 1/2 (`half`) or 1/4 (`quarter`); reloads the wallpaper |
| `texture_slack` (`slack`) | 0 | 2 | 2 | 2 | per cent a reduced texture or effect may be drawn larger than it is stored; 0 = exact sizes |

A global preset also sets these engine settings (a wallpaper's own preset does not):

| Key | max | high | balanced | low |
|---|---|---|---|---|
| `particle_density` | 100 | 100 | 100 | 50 |
| `particle_lowres` | off | auto | all | all |
| `particle_instancing` | off | on | on | on |
| `tex_downscale` | off | on | on | on |
| `fx_specialize` | off | off | on | on |
| `fx_quality` | exact | exact | exact | fast |
| `fluid_quality` | full | full | reduced | low |

Any knob can be set on its own (`walpe-ctl set fx_rate=2 post=reduced`); `quality_preset`
then reads `custom`. `walpe-ctl quality` prints the knobs, what the wallpapers on screen cost
(pixels shaded and effect passes per frame) and what each preset would save; `--effects`
lists every effect, `--json` gives the data.

Per wallpaper: `walpe-ctl quality --monitor N | --path P key=value ...` sets knobs for one
wallpaper only (`key=default` = as global, `reset` drops them all); the GUI has the same
below a wallpaper's options. Stored as `wallpaper_quality` in `config.json`: `{ "<path>": {
only the overridden keys } }`.

## Adjust layers

For a scene wallpaper: pick one layer or several and move, resize or hide them. The choice
is saved per wallpaper (`layers` in `config.json`) and the running scene applies it at its
next frame, without a reload. Scenes only, and the wallpaper has to be on a monitor: the
list comes from the running scene.

GUI: below a scene's options, **Adjust layers**. The list shows the layers front-most first
with their kind; selected layers are outlined on the wallpaper. Move by an offset, resize by
a factor, set visibility ("As the wallpaper sets it", "Hidden", "Always shown"); "Reset
selected" and "Reset all" undo.

```sh
walpe-ctl layers                       # the layers: key, type, position, size, text
walpe-ctl layers move 280 -10 10503    # 280 px right, 10 px down; adds to what is saved
walpe-ctl layers scale 1.4 3619        # resize (several layers: as one group)
walpe-ctl layers hide 2451 937         # hide / show
walpe-ctl layers set 147 dx=100 dy=0 scale=1 visible=default
walpe-ctl layers highlight 10503 --children   # outline them for a few seconds
walpe-ctl layers get                   # what is saved
walpe-ctl layers reset [KEY...]        # back to the wallpaper's own placement
```

A layer is named by its key as `layers` prints it (`10503:Name`), by its object id or by its
name. `--children` adds every layer below the named ones, `--monitor N` / `--path P` choose
the wallpaper, `--json` prints the daemon's reply. Units are scene pixels (the wallpaper's
own resolution); +x is right, +y is up.
