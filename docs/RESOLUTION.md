# Render resolution per monitor

Every monitor has its own render resolution: by default its own mode (`native`), or 720p /
1080p / 1440p / 4K, or any size you type. The wallpaper is rendered at that size and scaled
to the monitor. Smaller than the monitor means less GPU work and video memory and a softer
picture; larger means smoother edges and more of both. Layer textures, effects, imported
and converted videos and web pages follow the chosen size.

> **State of testing.** Built and tested only on Mesa's software renderer, under Xvfb and a
> headless sway. The cost figures below are pixel-count arithmetic, not measurements;
> nothing here was measured on a GPU or seen on a real monitor.

## Settings

Per monitor (`config.json`: `"monitors": { "<output>": { ... } }`; the output name is the
one `walpe-ctl resolution` shows):

| Key | Values | Meaning |
|---|---|---|
| `resolution` | `native` (default), `720p`, `1080p`, `1440p`, `4k`, `<width>x<height>` | A preset is a number of lines on the monitor's shorter side and keeps the monitor's shape: `1080p` is 1920x1080 on a 16:9 monitor, 2580x1080 on a 3440x1440 one, 1080x1920 on an upright one. A typed size is used as written: 320x200 at least, 16384 a side and 67 megapixels at most, never more than the GPU's largest texture; if its shape is not the monitor's, the picture is stretched. Also read: `2k`, `qhd` (= 1440p), `2160p`, `uhd` (= 4k), `fhd`, `hd` |
| `texture_limit` | `auto` (default), `none`, `<pixels>` | `auto`: every layer texture of a scene is kept at the size it is shown at in the chosen resolution. `<pixels>` (64-16384): the same, and no texture is kept longer than that on its longer side. `none`: textures stay at file size. One wallpaper on several monitors keeps one set of textures: the least strict limit holds |

Global:

| Key | Values | Meaning |
|---|---|---|
| `video_resolution` | `auto`, `none`, `720p` ... `4k`, `<width>x<height>` | the largest size videos are imported at and converted at ([VIDEO.md](VIDEO.md)). `auto`: the largest render resolution among the monitors that show the video (for an import: among all monitors); the default of a new installation. `none`: the video's own size. A `config.json` from an older version is set to `1080p`, what those versions did, so nothing changes size after an upgrade. Nothing is ever enlarged |
| `scale_filter` | `bilinear` (default), `nearest` | how a scene rendered at another size than the monitor's is scaled to it: smooth, or whole pixels (exact at whole multiples, blocky otherwise) |
| `resolution_prompt` | on (default), off | may the WalPE window ask about monitors it has not asked about |

## How to set it

The window asks once: on the first start of a new installation and the first time a new
monitor appears, with every connected monitor and its own resolution filled in ("Monitor
resolution": Apply, Keep native for all, or Not now). The same rows are in **Engine
settings > Resolution**, where a choice applies at once, and the dialog can be opened from
the window's menu, from the tray menu, with `walpe --resolution` and with `walpe-tray
--action resolution`. A monitor connected later is rendered at its own resolution and
announced once by a desktop notification (from the tray icon, or from the window when it
is open); nothing is announced while a fullscreen application has the focus. The daemon
never waits for an answer.

```sh
walpe-ctl resolution                      # monitor, output, mode, Hz, resolution, rendered at,
                                          # pixels in per cent of the mode, texture limit, asked or not
walpe-ctl resolution --json
walpe-ctl resolution HDMI-0 1080p         # one monitor, by output name or index
walpe-ctl resolution all native
walpe-ctl resolution DP-2 2560x1440 texture_limit=4096
walpe-ctl resolution --keep               # keep what every monitor has, do not ask about it
walpe-ctl resolution DP-2 --ask           # ask about it again in the window
walpe-ctl resolution DP-2 --forget        # drop the stored choice (native, not asked)
walpe-ctl resolution --prompt off         # never ask in the window
walpe-ctl set resolution=1080p            # every monitor, through `set`
walpe-ctl set resolution=HDMI-0:1080p,DP-2:native
walpe-ctl set texture_limit=2048 scale_filter=nearest video_resolution=none
```

A value that is not one (a typo, a size outside the limits, a monitor index that does not
exist) is refused with a sentence that names it, and nothing of the request is changed.
An output name that is not connected is not refused: the choice is stored for that output
and a note says so, which also happens to a mistyped name. A change
applies at once without restarting anything: scenes on that monitor load again, videos, web
pages and pictures follow with their next frame. `WALPE_RESOLUTION=1080p` (or
`HDMI-0:1080p,DP-2:native`) in the daemon's environment decides the sizes for that run and
saves nothing. `walpe-ctl status` shows per monitor `mode`, `resolution`, `render_size`,
`resolution_asked` and `texture_limit`, and `resolution_unasked`.

## What a choice costs or saves

Work per frame and video memory of a scene go with the number of pixels rendered: a 4K
monitor at 1080p renders a quarter of the pixels of native 4K, a 1080p monitor at 720p
about 44 %, a 1080p monitor at 4K four times as many (supersampling). One more picture of
the chosen size is kept per monitor whenever the size is not native (8 MB at 1080p, 33 MB
at 4K), and the frame is copied to the monitor once.

- The quality presets and `render_scale` work on top: `render_scale` is a percentage of
  the chosen resolution. Presets do not change the resolution.
- Videos: decoding costs what the clip costs; the choice decides how large the frames are
  kept (a clip at least twice the chosen size is halved while it is decoded) and how large
  imports and converted copies are (`video_resolution`).
- Web pages get the chosen size as their page size; pictures are decoded and stored at it;
  text in scenes is drawn at it and scaled with the frame, so it is pixel-exact only at
  native.
- The pointer is not affected: positions are relative to the monitor.
- Two same-size monitors showing one scene share one rendering only while they are set to
  the same render resolution.

## New monitors, upgrades

- A monitor nobody was asked about is rendered at its own resolution - what WalPE always
  did - and marked "not asked yet".
- When a monitor is plugged in, removed or switched to another mode the daemon restarts
  itself in place, as before; every monitor comes back with its stored choice. `native`
  follows the new mode, a preset keeps its lines, a typed size stays as typed.
- The choice is stored by output name: two screens used on the same connector share it.
- Upgrading: the monitors connected at the first start are entered as "native, asked";
  nothing changes size, and videos stay limited to 1080p until you change
  `video_resolution`. `config.json` moves to `"config_version": 4`.

## Limits

- Effects on colour-only layers of a scene (bars, gradients without a picture) are computed
  at most 2048 pixels wide, or as wide as the longer side of the render size if that is
  more. On a monitor wider than 2048 pixels such a layer is therefore sharper than in
  0.12.1, also at native resolution.
- A monitor with a 10-bit-per-colour window: a scene rendered at a non-native size goes
  through an 8-bit picture (`render_scale` has the same property).
- A typed size with another shape than the monitor's is stretched, not letterboxed.
- `texture_limit` as a number is applied to scene layer pictures after the first frames
  and to picture wallpapers; particle sprites and effect masks are not capped.
- `video_resolution=auto` fits a video inside the largest render size: a clip wider than
  the monitor's shape is kept a little smaller than covering it would need.
- On sway the "Monitor resolution" dialog is tiled beside the main window instead of
  floating over it.
