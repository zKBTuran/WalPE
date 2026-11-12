# Using WalPE

What WalPE plays, how to start it, the command line client, the most used settings, when
a wallpaper pauses, what is read from a Wallpaper Engine install, videos, the Steam
Workshop and the tray icon. Building and installing is in [BUILDING.md](BUILDING.md).

## What is played

| Content | Status | Notes |
|---|---|---|
| **Scene** (node graph, `objects[]`: the common format) | yes | `scene.pkg` extraction, `.tex` textures (LZ4 mips; DXT1/3/5 and R8/RG88 kept GPU-native; embedded PNG/JPEG; stored mip chains), parent transforms, camera, text layers |
| Scene, legacy format (`object` / `layers`) | yes | |
| Effects | yes | Wallpaper Engine's GLSL dialect compiled for GL 3.3 (`#include`, combos, HLSL helpers, implicit conversions repaired from the shader source before the first compile); multi-pass chains with named targets, `copy` passes, masks, compose / fullscreen layers. Builtin effects need your Wallpaper Engine install's `assets/` ([below](#wallpaper-engine-assets)) |
| Particle systems | yes | emitters, initializers, operators, children, instance overrides, control points; sprites, sprite trails and rope ribbons; perspective systems; animated sprite sheets; refracting materials |
| Puppet warp layers (`.mdl`) | yes | skinned 2D mesh, bones, keyframed bone animations |
| Video textures (MP4 inside a `.tex`) | yes | one shared decoder per unique texture; a clip larger than it is shown is played from a converted copy of the size shown ([Video](#video)) |
| SceneScript | subset | QuickJS: scripts on layer `text` / `visible` / `alpha` / `origin` / `scale` / `angles` / `color`, on effect visibility and material constants, media events, property animations, cursor events |
| Media integration | yes | the "now playing" session of any MPRIS player: title, artist, album art with its colours, playback state, timeline; for scene scripts and web pages |
| Audio-reactive wallpapers | yes | captures the default output monitor (PulseAudio or PipeWire's pipewire-pulse) while such a wallpaper is visible |
| **Video** | yes | in-house MP4 demuxer + libavcodec H.264; other codecs and containers are converted once with ffmpeg ([Video](#video)) |
| **Web** (`type: web`) | yes | WebKitGTK 6 off screen, pages served from a token-protected loopback server; `wallpaperPropertyListener`, media and audio listeners; real mouse input on the empty desktop |
| GIF / still images | yes | images render once (no idle cost) |
| User properties | yes | options panel in the GUI and `walpe-ctl props` / `setprop` / `resetprops`; for video, GIF and image wallpapers the built-in `walpe_fit` (cover, contain, stretch, center), `walpe_pos_x` / `walpe_pos_y`, `walpe_flip` |
| Presets (`project.json` with `dependency` + `preset`) | yes | the base item is found next to the preset, in the Steam libraries or the WalPE library |
| Steam Workshop | yes | browse, search and download through steamcmd; WalPE never handles a Steam password ([Steam Workshop](#steam-workshop)) |
| Camera parallax, camera shake, bloom, sprite-sheet layers, script camera | yes | each with a switch; they change the picture of scenes that ask for them ([SCENE-FEATURES.md](SCENE-FEATURES.md)). HDR bloom is not implemented; the look of the camera shake is an estimate |
| Sound | yes, on by default | scene sound layers (WAV, MP3, FLAC, Ogg) through PulseAudio / PipeWire: switched in Engine settings ("Wallpaper sound") or with `walpe-ctl set scene_sound=on\|off volume=0.5`, per wallpaper in its options ([SOUND.md](SOUND.md)). Video wallpapers are silent; web pages play their own media |
| 3D model scenes, `application` wallpapers | no | |

## Starting WalPE

```sh
walpe-daemon            # one desktop window (X11) or layer surface (Wayland) per monitor
walpe-daemon --backend x11|wayland   # this backend for this start, or an error
walpe-daemon --windowed # a 960x540 preview window instead
walpe                   # the library window (starts the daemon if needed)
walpe-ctl status
walpe-ctl open <project dir | media file>
```

| Path | What |
|---|---|
| `~/.local/share/walpe/workshop/` | the library: project folders or loose media files; imported videos in `imported/` |
| `~/.config/walpe/config.json` | settings ([SETTINGS.md](SETTINGS.md)) |
| `~/.config/walpe/gpu.conf` | presentation and driver hints ([GPU-PRESENT.md](GPU-PRESENT.md)) |
| `~/.cache/walpe/` | unpacked packages, converted videos, thumbnails, `daemon.log`, profiles |
| `$XDG_RUNTIME_DIR/walpe.sock` | the control socket (newline-delimited JSON) |

Scene objects are drawn in `objects[]` order with parent transforms, Y-up coordinates and
angles in radians, as in Wallpaper Engine.

### Starting with the session

Start the daemon from your session, for example in `~/.xinitrc` before `exec dwm`:

```sh
walpe-daemon &
walpe-tray &
```

For desktops that use them, `meson install` puts XDG autostart entries into
`<sysconfdir>/xdg/autostart` (`data/walpe-autostart.desktop`, and
`data/walpe-tray-autostart.desktop` where the tray is built). A systemd user unit is in
the source tree as `data/walpe.service`; `meson install` does not install it. To use it,
copy it to `~/.config/systemd/user/`; its `ExecStart` names `~/.local/bin/walpe-daemon`.
Wayland compositors such as sway do not run autostart entries: start both from the
compositor's configuration ([WAYLAND.md](WAYLAND.md#starting-with-the-session)).

## walpe-ctl

`walpe-ctl help` prints the full list. Exit status: 0 done, 1 bad input (or the output could not
be written: a full disk, a closed stdout), 2 no daemon runs
(or one runs and did not answer in time: the message says which), 3 the daemon refused (the
reason is in the reply). A wallpaper that cannot be loaded leaves
the one before it on screen; `walpe-ctl status` says why (`load_error`, `load_restored`).

| Command | Does |
|---|---|
| `open <path> [--monitor N]` | set the wallpaper on every monitor, or on monitor N (index from `status` "monitors"). A video the player cannot read is converted first; the wallpaper changes when it is ready |
| `add <video>` | import a video into the library ([Video](#video)) |
| `cancel-import` | stop a running import or conversion |
| `pause` / `resume` | |
| `status` | the daemon's state as JSON: monitors, what they show, why one is paused, import progress, renderer, counters |
| `set key=value ...` | change settings ([SETTINGS.md](SETTINGS.md)); refused as a whole when one key is unknown or one value has the wrong kind |
| `backend [auto\|x11\|wayland]` | show the window system the daemon draws on, how it was chosen, what the build holds and who reports each pause condition; or save the setting for its next start ([WAYLAND.md](WAYLAND.md)) |
| `gpu [--json]` | the graphics devices of this machine, the one that renders, and what `set gpu=auto\|integrated\|discrete\|<device>` would pick ([GPU.md](GPU.md)) |
| `gpu-stats [--sample SECONDS] [--json] ...` | GPU busy, memory, power and temperature as the kernel and the driver account them, for the whole GPU and for walpe-daemon; works without a running daemon ([GPU-PROFILING.md](GPU-PROFILING.md)) |
| `mem [--json] [--deep] [--wait S] [--events on\|off]`, `mem --diff A B` | where the daemon's memory is, computed when asked and at no other time: resident memory by kind of mapping and by name, the allocator, descriptors, threads by name, counters since the start, and per monitor the GL objects of its context (textures by size, buffers, programs) and what its wallpaper holds. Save two reports with `--json` and `--diff` shows what grew. `--events on` starts counting how often textures, framebuffers, buffers and programs are made and deleted and pixels are read back (nothing counts them before; `off` stops it) |
| `resolution [<monitor> <value>] ...` | the render resolution of every monitor, or set one: `native`, `720p` ... `4k`, `WxH` ([RESOLUTION.md](RESOLUTION.md)) |
| `quality [--json] [--effects]` | the quality knobs, what the wallpapers on screen cost and what each preset would save |
| `quality --monitor N \| --path P key=value ... \| reset` | quality knobs for one wallpaper only |
| `props [--monitor N \| --path P]` | the options (user properties) of a wallpaper |
| `setprop key=value ...` | set wallpaper options (saved; one reload) |
| `resetprops` | drop all option overrides of the wallpaper |
| `layers ...` | "Adjust layers": list, move, resize, hide the layers of the running scene (`walpe-ctl layers --help`) |
| `click X Y [BUTTON [COUNT]]` | click the wallpaper at a screen pixel, as a click on the empty desktop there does |
| `scroll X Y DY [DX]` | turn the wheel over a web wallpaper |
| `profile [SECONDS] [--json] [--sync]` | GPU profile of what every monitor shows ([GPU-PROFILING.md](GPU-PROFILING.md)) |
| `install <id \| url>` | download a Workshop item with steamcmd into the library |
| `steam-libraries` | list the Steam Wallpaper Engine workshop folders that were found |
| `quit` | stop the daemon |

```sh
walpe-ctl set fps=60 parallax=on fs=off covered=on transparent_windows=st,foot
walpe-ctl set preset=balanced
walpe-ctl setprop speed=2 clock=off title="a b"
walpe-ctl setprop walpe_fit=contain          # video / GIF / image: cover | contain | stretch | center
walpe-ctl open ~/wallpapers/123 --monitor 1
```

## Settings

All settings are in `~/.config/walpe/config.json`, written by the daemon; change them in
the settings window, the tray menu or with `walpe-ctl set`. The complete list with defaults
is [docs/SETTINGS.md](SETTINGS.md); the most used:

| `walpe-ctl set` | Default | |
|---|---|---|
| `fps=N` | 60 | frame rate limit of scenes and web pages (5-200); never above the monitor's refresh rate |
| `preset=max\|high\|balanced\|low` | high | quality for GPU load, all knobs at once (below) |
| `fs=on\|off`, `covered=on\|off`, `dpms=on\|off` | on | pause the monitor a focused fullscreen window is on / while covered / while the display sleeps |
| `fs_all=on\|off` | off | a focused fullscreen window pauses every monitor, not only the one it is on |
| `focus=run\|limit\|pause`, `focus_fps=N` | run, 15 | what a wallpaper does while an application has the focus on its monitor |
| `battery=on\|off`, `battery_fps=N` | off, 0 | pause / limit on battery power |
| `shared=on\|off` | on | render a scene once for same-size monitors |
| `idle=N` | 0 | pause after N seconds without input (needs a session that reports idle time: Wayland compositors with `ext-idle-notify-v1`) |
| `software=on\|off` | off | render on the CPU (Mesa llvmpipe) instead of the GPU |
| `backend=auto\|x11\|wayland` | auto | the window system the daemon draws on; read at its next start |
| `gpu=auto\|integrated\|discrete\|<device>` | auto | which GPU renders; `auto` sets nothing |
| `gl_api=auto\|gl\|gles` | auto | desktop OpenGL 3.3 where the driver has it, else OpenGL ES 3.2 / 3.1 |
| `resolution=native\|720p\|1080p\|1440p\|4k\|WxH` | native | render resolution, for every monitor or `NAME:value,...` |
| `scene_sound=on\|off`, `volume=0..1`, `mute=on\|off` | on, 0.5, off | wallpaper sound: the rows "Wallpaper sound", "Wallpaper volume" and "Mute" of the settings window |
| `bloom`, `camera_shake`, `camera_parallax`, `sprite_sheets`, `script_camera`, `script_texanim`, `fluid_sim` `=on\|off` | on | scene features; `parallax_offset` is off |
| `hwdec=off\|auto\|<api>`, `video_zerocopy=on\|off` | off | decode videos with a hardware decoder; keep its pictures in GPU memory |
| `video_scene_convert=on\|off` | on | a video inside a scene that is larger than it is shown is played from a converted copy of the size shown |
| `web_accel=on\|off` | off | web pages on the GPU |
| `web_unload=on\|off` | on | restart in place when the last web wallpaper is replaced, so WebKit leaves the daemon's memory |
| `we_assets=<dir>` | auto-detect | your Wallpaper Engine install |

**Quality presets.** `max` is the exact picture; `high` (the default) is the same except
that a reduced texture or effect may be drawn up to 2 % enlarged; `balanced` redraws slow
effects every second frame and computes effects at the size they have on screen; `low` also
halves texture sizes, renders at 75 % of the monitor's resolution, leaves cosmetic
full-screen passes out and limits the wallpaper to 30 frames a second. Every knob can be set
on its own, globally or per wallpaper; the table of what each preset sets is in
[SETTINGS.md](SETTINGS.md#quality-presets).

**Adjust layers.** For a scene: move, resize or hide single layers, saved per wallpaper
(GUI: below the wallpaper's options; command line: `walpe-ctl layers`). See
[SETTINGS.md](SETTINGS.md#adjust-layers).

**Presentation and driver hints** (swap interval, frame limiter, NVIDIA's `__GL_*` hints,
releasing GPU memory while paused, frame clocks on one grid) are read from
`~/.config/walpe/gpu.conf` at start: [GPU-PRESENT.md](GPU-PRESENT.md).

**Environment switches** for diagnosis and for comparing two behaviours on your hardware:
[ENVIRONMENT.md](ENVIRONMENT.md).

## Pausing

Per monitor, a wallpaper pauses while: you paused it; a focused fullscreen window is on
that monitor (`pause_fullscreen`); the display is in power save or the screensaver runs
(`pause_dpms`); or visible normal windows cover 85 % of the monitor or more
(`pause_covered`; docks, desktop windows, menus, minimized windows and the windows of other
dwm tags never count). See-through windows (WM_CLASSes in `transparent_windows`, or
`_NET_WM_WINDOW_OPACITY` below 100 %) count as covering while `cover_transparent` is on.

A fullscreen window on one monitor leaves the wallpapers of the other monitors running
(up to 0.12 it paused all of them; `walpe-ctl set fs_all=on`, `pause_fullscreen_all`,
brings that back, for example to leave the whole GPU to a game). The monitor is the one
that holds most of the window, so a border or gap a tiling window manager keeps around a
fullscreen window does not matter; a window across several monitors pauses each one it
covers at least half of. The rule follows the focus: while another window has the focus,
the monitor under the fullscreen window is paused by `pause_covered`, not by this rule.
`walpe-ctl status` says it per monitor (`monitors`: `fullscreen`, `fullscreen_pause`,
`paused`).

Those paragraphs describe X11. On Wayland each rule needs something in the session that
reports its condition (the compositor's frame events, its window list, its idle
notification, logind); `walpe-ctl backend` and the status member `pause_rules` say for the
running session which rule has a source and which cannot trigger, and why
([WAYLAND.md](WAYLAND.md#pause-rules)). New on both: `pause_idle` (`walpe-ctl set
idle=N`), pausing after N seconds without input where the session reports idle time.

A paused scene's clock stops (no jump on resume). A paused monitor does no decoding,
rendering or capturing; a video decoder sleeps once every monitor showing it is paused, a
web page is suspended once no monitor shows it, audio capture closes, and the daemon idles:
no periodic wake-up was measured while every monitor is paused or covered, and about one
every three seconds while a web page is loaded (measured on X11; not measured on
Wayland).

## Wallpaper Engine assets

The wallpapers are yours (your own files, or Workshop items downloaded with your own Steam
account), and Wallpaper Engine's own assets - its builtin effects, shader headers,
models, particle files and fonts - are read from your own Steam copy of Wallpaper
Engine. Without a Wallpaper Engine installation, video, web, GIF and picture wallpapers
work in full and a scene shows everything that is in the wallpaper's own files; what
the scene takes from Wallpaper Engine's `assets/` - for most scenes that includes their
effects - is missing.

Most scenes use files that belong to Wallpaper Engine itself, not to the wallpaper: its
**builtin** effects, the shader headers (`common*.h`) that effect shaders include, util
models, particle files and sprites, fonts. WalPE ships none of them: it reads them from
your own Wallpaper Engine install (the `assets/` folder next to `wallpaper64.exe`).

| | Without a Wallpaper Engine install |
|---|---|
| video, web, GIF and picture wallpapers | work in full: nothing of the install is read |
| a scene's layers, particle systems and effects whose files are all in the wallpaper | work |
| builtin effects (`effects/<name>/`: effect file, materials, shaders, textures) | not built; the log names each (`fx: ... not found (not in the project; WE assets not found)`). A layer with a picture, text or video is shown without the effect; a solid or compose layer, which is nothing but its effects, is not drawn |
| a wallpaper's own effects whose shaders include Wallpaper Engine's headers | fail to compile and are left out like builtin ones, unless the wallpaper carries the headers |
| layers on builtin models (`models/util/`) | cannot be built (`scenenode: model ... not found`) |
| builtin particle systems; builtin particle sprites | left out; drawn as a soft dot |
| builtin fonts of text layers | a system font is used |

The Workshop tab and `walpe-ctl install` read nothing of the install; downloads run through
`steamcmd` with your own Steam login ([Steam Workshop](#steam-workshop)).

WalPE looks, in this order (first hit wins):

1. `WALPE_WE_ASSETS=/path/to/wallpaper_engine/assets` (or the install root)
2. the config key `we_assets` (`walpe-ctl set we_assets=<dir>`; an empty value goes back to
   auto-detection)
3. the opened wallpaper's own path: `.../steamapps/workshop/content/431960/<id>` ->
   `.../steamapps/common/wallpaper_engine/assets`
4. every Steam library: `~/.local/share/Steam`, `~/.steam/steam`, `~/.steam/root`, Flatpak
   Steam, and each `"path"` in their `libraryfolders.vdf`

A directory counts when it has `shaders/`, `effects/` or `materials/` (or is an install
root with such an `assets/`). `walpe-ctl status` shows the result as `we_assets` and
`we_assets_source`. Proton / Wine installs work the same way.

## Video

The player reads H.264 in MP4. `walpe-ctl add <file>` (or **Import** in the GUI) turns
anything ffmpeg can read into that: H.264 High, yuv420p, scaled to fit within
`video_resolution` (by default the largest render resolution among your monitors; 1080p
for a configuration from an older version; never upscaled), source frame rate kept, audio
dropped; a file that already conforms is
remuxed. `walpe-ctl open` on another format converts it once into `~/.cache/walpe/video`.
ffmpeg runs as a child process with an argument vector (no shell).

A video inside a scene that is larger than the scene shows it - a 4K clip on a 1080p
monitor - is decoded at its own size, four times the work of what is seen. With
`video_scene_convert` (on by default) it is converted once, in the background at the lowest
priority, into a copy of the size shown (`video_resolution`) in the same cache, and the
scene plays the copy from where the clip next starts again; the original plays until then
and whenever the switch is off. `walpe-ctl status` says what each such video does
(`scene_videos`: source size, played size, `copy: converting 42 %` / `ready` / `off` / `not
needed`).

Hardware decoding (`walpe-ctl set hwdec=auto`, or one API by name: VA-API, NVDEC, VDPAU,
Vulkan Video, V4L2 through FFmpeg) is off by default and not confirmed on a graphics
card. With `auto` the decoder follows the GPU that renders (`cuda` first on NVIDIA's
driver, as before); software decoding takes over by itself when it fails, and `walpe-ctl
status` shows what the decoder really does (`video_decoder`). Details, the cost of each
step and the switches for it: [VIDEO.md](VIDEO.md).

## Steam Workshop

The Workshop tab of the GUI browses, searches and downloads; `walpe-ctl install <id|url>`
downloads from the command line. Downloads go through `steamcmd` with its cached login:
run `steamcmd +login <account> +quit` once in a terminal, then `walpe-ctl set
steam_user=<account>`. WalPE never sees or stores a Steam password. Existing Steam
Wallpaper Engine libraries are found and listed as they are.

Optional and off by default, with your own Steam Web API key (kept in
`~/.config/walpe/workshop.json`, mode 0600, sent to api.steampowered.com only): "Subscribe
on Steam when downloading" and "Also unsubscribe on Steam" when deleting. Deleting a
wallpaper in the GUI moves every copy of the item to the trash.

What Steam's servers answer and what a wallpaper's files say is treated as data from
strangers: see [SECURITY.md](SECURITY.md).

## Tray icon (walpe-tray)

Left click opens (or raises) the WalPE window, middle click pauses / resumes, right click
opens the menu (open, pause / resume all, wallpaper sound on / mute / unmute, next / random wallpaper, per-monitor
wallpaper and recent ones, pause switches, settings, restart engine, quit). The icon dims with a
pause badge while paused and turns grey with a red badge when the engine is not running.

It is a separate small process. It docks into classic XEmbed trays (dwm systray patch,
stalonetray, trayer, polybar, xfce4-panel) and uses StatusNotifierItem + dbusmenu when an
SNI host is running (KDE, waybar, GNOME AppIndicator extension); a Wayland session always
uses StatusNotifierItem. The tray also announces a newly connected monitor once, for its
render resolution ([RESOLUTION.md](RESOLUTION.md)). The GUI's "Tray icon
(start with WalPE)" switch starts and stops it and remembers the choice in
`~/.config/walpe/tray.ini` (`start_with_gui`, `confirm_quit`, `backend=auto|sni|xembed`).
`walpe-tray --quit` closes it, `walpe-tray --menu` pops its menu at the pointer (bind it to
a key), `--action=next|random|pause-toggle` runs a menu entry.
