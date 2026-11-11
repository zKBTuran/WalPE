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
written back as they were. A file that is not a JSON object is not used and not overwritten:
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
`backend`, the quality knobs in `walpe-ctl quality`; `software_threads` there is the number
in use, `software_threads_saved` the setting).

### Wallpapers and playback

| Key | Default | set name | Meaning |
|---|---|---|---|
| `wallpapers_dir` | `~/.local/share/walpe/workshop` | (file only) | the library: project folders or loose media files |
| `current` | `""` | (`walpe-ctl open`) | the wallpaper of every monitor that has no entry in `monitor_current` |
| `monitor_current` | `{}` | (`walpe-ctl open --monitor N`) | output name -> wallpaper path |
| `paused` | false | (`walpe-ctl pause` / `resume`) | |
| `fps` | 60 | | frame rate limit of scenes and web pages, 5-200; a monitor never shows more than its refresh rate; a video or GIF keeps its own lower rate |
| `volume` | 0 | | stored only: the renderer is silent |
| `parallax` | true | | scenes follow the pointer where they are made to |
| `video_backend` | `ffmpeg` | | stored only |
| `vsync`, `audio` | true, false | (file only) | stored only (presentation is set in `gpu.conf`) |
| `we_assets` | unset = auto-detect | | Wallpaper Engine install or its `assets/` folder; `we_assets=` (empty) goes back to auto-detection |
| `steam_user` | unset | | Steam account name for steamcmd downloads (never a password) |
| `props` | `{}` | (`walpe-ctl setprop` / `resetprops`) | wallpaper option overrides, `"<wallpaper path>\u001f<option>": "value"` |
| `layers` | `{}` | (`walpe-ctl layers ...`) | "Adjust layers", per wallpaper (below) |
| `config_version` | 3 | (written by the daemon) | format of the file; older files are migrated once (a frame rate that was an older version's default becomes 60) |

### Pausing and limits by desktop state

| Key | Default | set name | Meaning |
|---|---|---|---|
| `pause_fullscreen` | true | `fs` | pause every monitor while a focused fullscreen window is on one of them |
| `pause_dpms` | true | `dpms` | pause while the display is in power save or the screensaver runs |
| `pause_covered` | true | `covered` | pause a monitor while visible normal windows cover 85 % of it or more |
| `transparent_windows` | `st` | | comma-separated WM_CLASS list of see-through windows |
| `cover_transparent` | true | `transparent` | see-through windows (that list, or `_NET_WM_WINDOW_OPACITY` below 100 %) count as covering; off = they never cover |
| `focus_action` | `run` | `focus` | while an application's window on a monitor has the focus: `run`, `limit` (to `focus_fps`) or `pause` |
| `focus_fps` | 15 | | the limit of `focus_action=limit`, 1-200 |
| `pause_battery` | false | `battery` | pause every wallpaper on battery power |
| `battery_fps` | 0 | | frame rate limit on battery (0 = none) |
| `preview_anim` | `always` | `previews` | animated previews in the GUI: `always`, `hover`, `off` |

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
| `video_hwdec` | false | `hwdec` | decode videos on the GPU when that works ([VIDEO.md](VIDEO.md)) |
| `video_limit` | false | | videos follow the frame rate limit and decode only the pictures that are shown |
| `video_upload` | `legacy` | | how video frames reach their textures: `legacy`, `pbo`, `pbodata`, `persist`, `direct`, each also `+pingpong` (same picture) |

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
| `post_processing` (`post`) | full | full | reduced | off | `reduced`: blur and bloom on smaller buffers; `off`: also no cosmetic full-screen passes (bloom, film grain, vignette, chromatic aberration) |
| `render_scale` (`scale`) | 100 | 100 | 100 | 75 | scene resolution in per cent of the monitor's, 25-100 |
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
