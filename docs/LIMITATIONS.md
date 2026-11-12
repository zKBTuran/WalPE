# Known limitations and the state of testing

What WalPE does not do, what differs from Wallpaper Engine, and on which hardware it was
run. Limits of single parts are also in [WAYLAND.md](WAYLAND.md#limits), [GPU.md](GPU.md),
[RESOLUTION.md](RESOLUTION.md#limits) and [SOUND.md](SOUND.md#limits).

## State of testing

**What is new since 0.12.2, and how far it was tested.** A Wayland backend next to the
X11 one, with web wallpapers that need no X there; choice of the GPU on machines with
two, and support written for AMD and Intel
graphics; an OpenGL ES 3.1+ path for GPUs without desktop OpenGL; a render resolution
per monitor; hardware video decoding chosen by what the machine offers; scene sound
(off by default); camera parallax, camera shake, bloom, sprite-sheet layers and script
camera in scenes; builds for 64-bit ARM. **All of it was built and tested only on Mesa's
software renderer, a headless compositor, a null sound sink and - the ARM build - under
emulation, with one exception: the maintainer ran the X11 build of 0.14.0 on his own
NVIDIA card before the release. Nothing else has been run on real AMD, Intel, NVIDIA or
ARM hardware or on a
real Wayland desktop.** The NVIDIA / X11 path is meant to be unchanged from 0.12.2,
except that scenes which ask for bloom, camera shake, sprite sheets or a script camera
now get them, that two fixes change the picture of some scenes (layers with a tint and
effects, rope trails of particle systems) and that a fullscreen window pauses the
wallpaper of its own monitor only ([Pausing](USAGE.md#pausing)); that too was checked on the
software renderer only ([Known limitations](#known-limitations)).

## Known limitations

- **Run on one graphics card, under X11.** The maintainer runs WalPE on one
  NVIDIA laptop card (RTX 4050, proprietary driver, X11). A figure in the documents comes
  from that machine only where the text says so; all other sizes, counts and timings are
  counts or software-rendering numbers, not the performance of a particular card. Version
  0.12.1 was confirmed on that card as it stood before its last set of changes. The
  fixes of 0.12.2 were built and tested only on Mesa's software renderer (llvmpipe).
  The X11 build of 0.14.0 was run on that card by the maintainer before the release.
  **Apart from that, everything that is new since 0.12.2 was built and tested only on
  Mesa's software renderer (llvmpipe), a headless Wayland compositor, a null sound sink
  and - the build for 64-bit ARM - under emulation. Nothing else has been run on real
  AMD, Intel, NVIDIA
  or ARM hardware or on a real Wayland desktop**; the NVIDIA / X11 path is meant to be
  unchanged from 0.12.2 with default settings, except for the scenes of the next item
  and for the fullscreen pause rule with more than one monitor ([Pausing](USAGE.md#pausing): the
  monitor of the fullscreen window pauses, the others keep running; `fs_all=on` is the
  old rule), and that too is so far shown only on the software renderer, in two steps. The development tree as it
  stood before the fixes of 0.12.2 were merged in was compared with release 0.12.1: the
  same frames of the 91 test scenes at 960x540 (16 of them with the switch of their new
  feature off), the same threads,
  descriptors and idle cost, and for four scenes the same OpenGL calls in the same order
  over 30 frames (calls that only ask the driver something left aside). The frames of the
  final tree were then compared with those of that earlier tree. Measure on your own
  card with `walpe-ctl profile`.
- **Default pictures that changed.** Scenes that ask for bloom, camera shake, a sprite
  sheet or a script camera now get them: 16 of the 91 scenes of the test library draw
  another picture than in the 0.12 releases (bloom in 10 of them, camera shake in 5,
  sprite sheets in 2, the script camera in 1). `walpe-ctl set bloom=off camera_shake=off
  sprite_sheets=off script_camera=off` gives the earlier pictures back
  ([SCENE-FEATURES.md](SCENE-FEATURES.md)). Wallpaper Engine's HDR bloom is not
  implemented (such scenes get the standard bloom), the look of the camera shake is an
  estimate, and none of these was compared with Wallpaper Engine's own picture. Two
  fixes change pictures whatever the switches say: a layer with a tint and effects keeps
  its effects (two scenes of the test library), and a rope trail of a particle system is
  as long as its scene says at every frame rate (shorter than in 0.12 at 60 Hz and
  below); [CHANGELOG.md](../CHANGELOG.md) has both. Effects
  on colour-only layers are as in 0.12 at a monitor's own resolution; a render size
  chosen above 2048 pixels makes them sharper ([RESOLUTION.md](RESOLUTION.md)).
- **Wayland: tested headless only.** The Wayland backend needs a compositor with the
  layer shell (`zwlr_layer_shell_v1`); GNOME's Wayland session and weston's desktop shell
  have none and are not supported: the daemon ends there and says why (through Xwayland
  the wallpaper would lie above every window, so X11 is taken there only when asked for
  by name). Wallpapers were shown on a headless sway 1.9 with the
  software renderer and on nothing else: no real output, no GPU, no other compositor (a
  headless weston 13 served only as the compositor without layer shell that the daemon
  has to turn down). On Wayland
  the pointer is known only over the bare desktop, "Skip hidden areas" does not exist,
  the fullscreen and focus rules need the compositor's window list (on KDE Plasma a script
  in KWin, which the daemon loads while such a rule is on), and web wallpapers go
  through Xwayland where an X display answers and through the private page host (a
  Wayland display inside the daemon, no X needed) where none does. That page host ran
  real pages on the headless sway only; a page is expected to animate at 60 frames a
  second at most there ([WAYLAND.md](WAYLAND.md#web-wallpapers)). A compositor that
  accepts the connection and does not answer within 10 seconds counts as absent (X11 is
  taken when there is an X display). Known and left as they are: a wallpaper that is not shown keeps being
  drawn when "pause when covered" is off; a lock screen pauses through "pause when
  covered" only, unless the locker tells logind; with no monitor left `walpe-ctl status`
  still lists the monitors; while the daemon waits for a compositor to come back,
  `walpe-ctl` says it did not answer in time, and what it was sent is not carried out
  later; and a session started with a one-time socket (`WAYLAND_SOCKET`) cannot be
  restarted in place, so the daemon ends at the first change of a monitor there.
  [WAYLAND.md](WAYLAND.md#limits).
- **KDE Plasma on Wayland: desktop icons only with the add-on, and then only part of
  the pointer for the wallpaper.** Plasma's wallpaper, its desktop icons and its widgets are one window of
  Plasma's shell, and a wallpaper program of its own can only be in front of that window
  or behind it. WalPE is in front (the layer shell's bottom layer): the wallpaper stays
  whatever is clicked, Plasma's icons and widgets are behind it. The add-on in
  `contrib/plasma` (Plasma 6.5 or newer, installed by the user:
  `contrib/plasma/install.sh install`) shows them over the wallpaper; on a desktop that
  uses it the wallpaper gets the pointer's position, the left button and the wheel from
  the add-on, the last two over the bare desktop only, never the right or the middle
  button, and a drag also draws Plasma's selection frame; the Overview shows the desktop
  without the wallpaper. Seen on a headless Plasma 6.8.2 only
  (the bottom layer also on 5.27); no real session, no GPU.
  [WAYLAND.md](WAYLAND.md#kde-plasma).
- **AMD and Intel graphics, GPU choice, OpenGL ES: written, not run on hardware.** The
  list of graphics devices, render offload to another GPU (`gpu=integrated|discrete`, and
  the default `gpu=auto` on a laptop whose integrated GPU drives the screen while a
  discrete one sits beside it), the kernel's GPU statistics and the Mesa counterparts of
  the driver hints were tested against made-up machines only. On such a laptop the default
  now renders on the discrete GPU if that GPU passes a test at start, which costs a copy
  of every frame between the GPUs and keeps both awake; `walpe-ctl set gpu=display` is
  the earlier behaviour ([GPU.md](GPU.md)). The OpenGL ES path (OpenGL ES 3.1 at least) rendered
  every scene of the test library on Mesa's llvmpipe, and all 1934 effect programs of
  those scenes compile there; no ES-only driver and no vendor shader compiler has seen
  it. Pictures differ slightly from desktop OpenGL: OpenGL ES has no texture LOD bias,
  so minified textures are a little softer (65 of the 91 scenes; in 52 of them at most
  1 % of the pixels differ by more than 2 of 255; each is exactly the desktop picture
  without the bias).
  [GPU.md](GPU.md).
- **64-bit ARM: cross-compiled and tested under emulation, never run on ARM hardware**
  (65 of the 68 tests that need neither a display nor an OpenGL context pass there; no
  ARM GPU, no board):
  [64-bit ARM](BUILDING.md#64-bit-arm-aarch64).
- **Sound was measured, not listened to.** Scene sound layers (on by default) were measured
  against a PulseAudio null sink and against PipeWire's `pipewire-pulse` by recording the
  sink; no loudspeaker, no sound card. Quitting or restarting the daemon cuts a playing sound without a
  fade. A video wallpaper's sound track and a web page's own sound follow the same
  settings (measured against the PulseAudio null sink; web pages with WebKitGTK 2.52,
  their level and mute also against PipeWire 1.0.5 with WirePlumber 0.4.17, not with
  WirePlumber 0.5; a build without GStreamer's headers leaves both to the sound server,
  where a muted page can stay muted and other programs' streams are changed);
  videos imported with 0.14.0 or earlier have no sound track and must be imported again,
  and a video layer inside a scene is silent. Audio-reactive wallpapers react to wallpaper
  sound too.
  [SOUND.md](SOUND.md).
- **Builtin effects need a Wallpaper Engine install**
  ([USAGE.md](USAGE.md#wallpaper-engine-assets)); WalPE has no shaders of its own to
  replace them.
- **SceneScript is a subset.** Layers cannot be created or destroyed by script
  (`thisScene.createLayer()` returns a stand-in that is not drawn); scripts on properties
  the renderer does not drive have no effect. Of a script camera only the zoom and the
  eye's x and y are used; an animation's `addEndedCallback` is accepted and never called.
  Workshop item 3397287008 logs `'thisProject' is not defined` from its media handler: a
  gap in the script interface; the wallpaper runs.
- **Scenes are composed in 2D.** The x / y tilt of a layer is ignored; camera parallax
  moves layers sideways and does not change perspective; the scene settings for camera
  fade and HDR bloom are not applied (an option bound to one of them gets a note in the
  options panel); 3D model scenes do not load. Not read at all: `perspectiveoverridefov`,
  rope `uvscale` / `uvscrolling`, `copybackground`. The Fluid Simulation effect runs on
  the grid its file declares at a fixed 60 steps a second; whether it moves as in
  Wallpaper Engine has not been compared, its lit variant is not supported, and no scene
  of the test library is known to use it. Of sprite-sheet layers, frames stored on further
  images of one file and blending between frames are not covered; smoke made from a video
  plays at the rate of its clip.
- **Particles.** How many particles a system shows follows Wallpaper Engine's documentation
  (the cap is the particle file's `maxcount`). Where that leaves room for another reading
  there is a switch for it (`WALPE_PT_CAP=count`, `WALPE_PT_FADE=span`,
  `WALPE_PT_SIZE=full`, `WALPE_PT_WORLD=scaled`, `WALPE_PT_CAMERA=fixed`,
  [ENVIRONMENT.md](ENVIRONMENT.md)), for comparing with Wallpaper Engine on a real
  screen.
- **Quality presets.** At `low`, Workshop item 1251123383 is about 18 of 255 darker than at
  `high` (the particle part of `low`), and six other scenes of the test library differ by 2
  to 5 of 255 in mean level. A mix no preset uses (`fx_rate=2 fx_rate_mode=slow` with
  `fx_quality=fast`) was only run for crashes. `preset=max` for one wallpaper only does not
  switch `tex_downscale` off (that switch belongs to the whole daemon).
- **Video is H.264 in MP4 only** without a conversion; other codecs and containers go
  through ffmpeg once, and imported or converted videos are limited to `video_resolution`
  (1080p for a configuration that comes from an older version). A video wallpaper on two
  monitors is uploaded once per monitor.
- **Off by default and not confirmed on a graphics card:** `hwdec` (hardware video decoding,
  every API), `video_zerocopy`, `web_accel`, `skip_hidden`, `pointer_share`, `video_limit`,
  `frame_align`, the `video_upload` alternatives, `gpu` other than `auto` / `display`,
  `gl_api=gles`, `preset_follow_gpu`, `parallax_offset` and a render resolution other than
  `native`.
  Hardware decoding and web acceleration fall back by themselves.
- **"Skip hidden areas" under a compositing manager** (`skip_hidden=always`) cannot know
  which windows the compositor itself makes see-through: behind those the wallpaper
  stands still; see [SETTINGS.md](SETTINGS.md).
- **Two monitors.** The leading monitor of a shared scene does not apply its own
  "application focused" limit to its own window; a scene that only follows the pointer
  still ticks (drawing nothing) while the pointer rests; a pointer moved by a program while
  it rests is noticed at the next real movement.
- **Library window.** It still waits for the daemon in a few places ("Set as wallpaper",
  Workshop downloads).
- **Album art** is fetched from any address a music player names, including local ones.
- `application` wallpapers are Windows programs and are not supported.
