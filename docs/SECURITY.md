# Security notes

A wallpaper from the Workshop is a program somebody else wrote: scene scripts, shaders, web
pages, and files in formats that are parsed by WalPE. This file says how to report a security
problem, what WalPE trusts, what it limits, and where the limits are.

## Reporting a security problem

Please report a security problem privately, not in a public issue: on the repository's
**Security** tab, use the **Report a vulnerability** button (GitHub's private vulnerability
reporting). Such a report is visible only to you and the maintainers.

What to include:

- the WalPE version or the commit: the line `walpe-daemon <version> "<name>" up` in the daemon's log,
  and `git rev-parse --short HEAD` for a build from git;
- what input triggers the problem (a wallpaper file, a script, a shader, a web page, a reply
  from a server, a request on the control socket) and what happens: a crash with its
  backtrace, a file read or written outside the wallpaper's folder, a limit of this document
  that does not hold;
- if one is needed to show it, the Workshop id of a wallpaper or a small file. Attach it to
  the private report, not to a public issue.

## Limits on wallpaper files

| File | Limit |
|---|---|
| texture / picture | 16384 px per side and 8192 x 8192 px; all textures of a scene 1024 MiB (`WALPE_TEX_BUDGET_MB=<MiB>`, 0 = none), the largest are stored smaller beyond that |
| picture inside a `.tex`, layer picture of the old scene format | PNG, JPEG, WebP (plain picture files also GIF, BMP, TIFF; old-format layers also TGA, AVIF, JXL), each read by its own decoder only, also when only its size is asked for; a JPEG above the texture size is decoded smaller, and one that its decoder has to hold whole (progressive) is not decoded beyond 256 MiB of that store; an arithmetic-coded JPEG is not decoded at all, under whatever name (a short one could keep the JPEG library decoding for ever), nor is one whose frame header is not within the file's first 32 MiB |
| picture or GIF wallpaper (a picture file chosen as the wallpaper) | shown only when gdk-pixbuf takes the file for a PNG, JPEG, GIF, BMP, TIFF, WebP, TGA, AVIF or JPEG XL picture, whatever its name says: a file it takes for anything else (an SVG document, XPM text, a Windows icon ...) is refused and not decoded as a picture; that loader has read the file's header by then, which is how the format is found out; a file that starts as a JPEG and is arithmetic-coded is refused before that |
| GIF (wallpaper, preview, cover art) | checked before any of it is decoded: the picture and every frame at least 1 x 1 pixel, every frame inside the picture; a GIF that fails is refused like a damaged picture |
| `.json` | 32 MiB, 512 levels of nesting |
| `scene.pkg` | a regular file inside the wallpaper's folder (no pipe, no link that leads out); entries stay inside the cache copy, at most 32 path components, 32768 files + directories, 4 x the package's size + 16 MiB written; the cache holds 1 GiB (`WALPE_PKG_CACHE_MB`) |
| puppet model `.mdl` | 256 MiB |
| video (`.mp4`, video inside a `.tex`) | 4194304 frames (19 hours at 60 fps), timescale below 2^31; a sample size table that is repeated in the file costs nothing more |
| font of a text layer | a TrueType / OpenType font file or a collection of them, at most 256 faces and named instances in all and 512 tables per face; anything else is not handed to fontconfig, and the layer is drawn with a system font |
| text layer | 16 KiB of text, 8192 px, a font size of at most 15000 points; the text layers of a scene together 1024 MiB of texture (the same `WALPE_TEX_BUDGET_MB`, counted apart from the pictures), a layer beyond that is not drawn |
| particles | 20000 per system, 128 systems per particle object, 4096 systems in all particle objects together (an object beyond that gets no further system, the log says so once), 400000 alive in all systems together; a system at a limit emits no more until particles end. How large a particle is drawn and how much of the screen all of them fill is not limited: a thousand particles the size of the screen are a thousand screens to fill per frame |
| names of files a wallpaper refers to | below the wallpaper's own folder (or the Wallpaper Engine `assets/`): no absolute path, no `..`, no symbolic link that leads out, regular files only; the preview picture the library window shows for a wallpaper is one of these |

A file beyond a limit is not loaded (the log says which); the rest of the wallpaper is.

Names and texts of a wallpaper that the engine prints in its log lines, and what a scene
script logs or throws, are printed with their control characters and bytes that are not
UTF-8 made visible (`\x0a`), at most 2048 bytes of a string: a name cannot start a log
line or send an escape sequence to the terminal the log is read in. This does not cover
shader compile logs, the error texts of libraries (a path in one can carry a control
character of a folder's name), the daemon's own lines and the video and web modules.

## Scripts and shaders

QuickJS version: wallpaper scripts are untrusted and run inside QuickJS, so its age matters.
`meson setup` warns ("QuickJS: quickjs.h has no JS_EVAL_FLAG_ASYNC ...") when the installed
library is older than the 2024-01-13 release, and the daemon says so once in its log. The
2021-03-27 release (still what Debian / Ubuntu package) lacks fixes for memory errors that plain
script can reach (CVE-2023-48183, CVE-2023-48184, CVE-2023-31922). Install a current
`dev-lang/quickjs` and rebuild, or build with `-Dscenescript=disabled`. (The check looks for
`JS_EVAL_FLAG_ASYNC` in `quickjs.h`; QuickJS has no version number in its header.)

Limits on what a wallpaper's scripts and shaders may cost (fixed, no switch):

| what | limit | beyond it |
|---|---|---|
| CPU time of all scripts per frame / per load step | 250 ms | the script (or timer) that was running is stopped for good; three times in a row with nobody to name (a promise reaction): every script of the wallpaper stops |
| one built-in call that does not return (regular expression, `join` of a huge array, also inside an argument conversion) | 1.5 s CPU | every script of the wallpaper stops, the wallpaper keeps running; its script memory (64 MiB at most) is not given back until the daemon restarts |
| script memory | 64 MiB | allocation fails inside the script |
| one log line of a script | 1024 bytes | cut, marked `[...]` |
| a number a script hands to a layer (origin, scale, angles, ...) / to an effect constant | +-1e9 / +-1e30 | brought into range; a vector with a component that is not a number is not applied |
| text a script hands to a layer in one call | 64 KiB | cut between two characters (a text layer shows 16 KiB of it) |
| "parent" chain of layers, also when a script re-parents at run time | 256 | the layer becomes a root |
| one shader file (`.frag`, `.vert`, included header) | 1 MiB | not read: the effect fails like one whose shader is missing |
| one shader with all its `#include`s | 4 MiB | further includes stay unexpanded (the effect then fails to compile) |
| one statement of a shader, macros expanded | 8192 tokens | the shader is not handed to the driver (a driver's compiler can run out of stack on a longer statement); the effect fails like one that does not compile |
| a shader the repairs look at | 256 KiB of source, 8192 bytes per line | a larger source, or a longer line, is compiled as written, without the repairs for Wallpaper Engine's dialect. On OpenGL ES a source of more than 256 KiB is not compiled at all (it is not translated to GLSL ES either): a warning names it and its effect is not drawn |
| a combo name from a wallpaper's JSON or a `[COMBO]` annotation | an identifier of at most 128 characters | anything else is left out of the shader source |
| a time zone name a script asks for | a zone of the system's zone directory | other names (files there that are no zones, `posix/`, `right/`) give no zone |

What a shader costs per pixel is not limited: a loop in it runs as often as it says, and
on the software renderer a frame of such a wallpaper can take minutes or never end. The
daemon goes on answering then (its main thread does not draw), and the wallpaper can be
left: a monitor that has not taken the wallpaper it was given 20 seconds after it was
asked for is restarted with the daemon, in place, and the log names the wallpaper that
held it; `walpe-ctl quit` and every restart in place wait 10 seconds for a render thread
and go on without it. With a graphics card the frame is the driver's to end (it resets a
context that keeps the card busy for seconds); that has not been run. Test:
`meson test -C build daemon-life` (case `held`).

A header is included once per file, whatever the `#include` line calls it (`"x.h"`, `"./x.h"`, a link).
Tests: `meson test -C build script-hostile fx-hostile scene-hostile` (the last two need EGL
surfaceless; `fx-hostile-test gl 1500` and `fx-hostile-test text 6000` run more seeds).

## Web wallpapers

A web wallpaper is a page somebody else wrote, shown by WebKitGTK in a window nobody sees.
What it gets and what it does not, as the code is today:

- **Its files** come from a file server inside the daemon: a TCP port on 127.0.0.1 that the
  system picks when the first page is opened. Every program on the machine can connect to
  that port, also one of another account or one in a sandbox with network access; a file is
  handed out only under the page's token (128 random bits, the first part of the address,
  new for every page; the log shows it as stars). The server answers GET and HEAD and only
  requests that name it as their host; the body of a request is read and dropped; one part
  of a file (`Range`) is answered straight from the file; at most 128 connections are open,
  and for one more the one that has waited longest for a request is closed. What it hands
  out: regular files inside the wallpaper's folder (and the folder of a preset of it; a
  link that leads out is not followed), the file chosen for a file option, and for a folder
  chosen for a directory option its pictures and videos down to one sub-folder, nothing
  hidden.
  `WALPE_WEB_SCHEME=1` serves the page through a scheme of the module's own instead (no
  port; media in pages does not play then). `WALPE_WEB_FILE_URLS=1` (diagnosis only) loads
  the page from `file://`, where WebKit lets it read every file you can read: its network
  access is switched off for that reason.
- **Network access is on** unless you switch it off: a page reaches the internet, your
  local network and services on this machine, as it does under Wallpaper Engine. Off for
  one wallpaper with its option "Allow network access" (`walpe-ctl setprop
  walpe_network=false`), off for all with `WALPE_WEB_OFFLINE=1`: every request of the page
  then goes to the file server above, which answers for the page's own files and refuses
  everything else, other ports of this machine included (WebRTC is switched off, so nothing
  goes around it). Certificate errors fail the request (WebKit's default).
- **Where it may go**: the page itself stays inside the wallpaper's folder (a navigation
  elsewhere is refused and the wallpaper stays); frames inside it may load other addresses;
  new windows are never opened; a download is cancelled as it starts. With
  `WALPE_WEB_OPEN_LINKS=1` an http(s) link you clicked that was refused is handed to
  `xdg-open`.
- **Nothing is kept**: every page has a session of its own that lives in memory (cookies,
  local storage, databases, cache) and ends with the page.
- **No permission is granted**: location, camera and microphone, notifications, clipboard
  reading, pointer lock and the rest are refused without a question. Script dialogs are
  answered at once, a file or colour chooser is cancelled, there is no context menu, no
  popup of a selection list, no print dialog, the hidden window does not go fullscreen,
  and a server that asks for a name and a password (401, a proxy's 407) is not asked about
  either: the request goes on without. The page gets pointer input that happens on the
  empty desktop; it never gets the keyboard, and what it selects or copies does not reach
  the selections of your session (X11: the daemon owns none unless `WALPE_WEB_SELECTIONS=1`
  is set; the private page host has no clipboard at all).
- **What it can ask of the daemon**: three things, by a message of at most 64 KiB - the
  audio spectrum (128 numbers 30 times a second, of what the default output plays and
  never of a microphone: [AUDIO-REACTIVE.md](AUDIO-REACTIVE.md#where-the-sound-comes-from)),
  the "now playing" state, and that a pointer event arrived. With network access a page
  can send both on: the spectrum tells what plays and may be enough to make out speech,
  "now playing" is what you listen to (the same page says how to switch each off). A page that
  sends megabytes, or more than 20000 messages in ten seconds, is stopped (status:
  `web_error`). A page whose process dies is loaded again, three times within a minute at
  most.
- **The window it lives in** (the page host) is one nobody sees. On an X11 session, and
  by default on a Wayland session, it is an X window (under Xwayland there), with the
  selection guard above. On a Wayland session without an X display that answers it is the
  private page host ([WAYLAND.md](WAYLAND.md)): a Wayland display inside the daemon to
  which only the page's toolkit is connected. Everything above limits a page there in
  the same way; what is its own:
  - Its socket is made in a directory of its own (mode 0700) below the runtime directory,
    takes one connection, which must come from the daemon's own process (the peer's
    credentials are checked), and is removed together with the directory as soon as the
    toolkit is connected. After that nothing listens: no second client can connect, and
    the display cannot be started a second time.
  - The display offers the seven interfaces the toolkit needs (`wl_compositor`,
    `wl_subcompositor`, `wl_shm`, `xdg_wm_base`, one `wl_output`, one `wl_seat`, and a
    `wl_data_device_manager` that offers nothing and cancels every copy and drag) and
    nothing else: no primary selection, no clipboard of anyone else, no screen capture,
    no input from anywhere but the daemon.
- **WebKit's sandbox stays on** for the processes that run pages (namespaces and a system
  call filter of WebKit's own making); WalPE adds no folder to it, does not configure it
  and does not switch it off. It keeps a page process that was taken over away from your
  files and from the control socket. It is WebKit's sandbox with WebKit's rules: it gives
  that process the socket of the display its toolkit is on, and the sound server's.
  - It also gives it, to write in, the directory of GStreamer's plugin list. Left alone
    that is the whole of `~/.cache` - every program's cache, and `~/.cache/walpe` with the
    converted videos and sound tracks the daemon reads - not `~/.cache/gstreamer-1.0`
    (WebKitGTK 2.52.6 and 2.54.2; so it was in WalPE up to 0.14.0). The daemon therefore
    names a file of its own for that list, `~/.cache/walpe/gstreamer/registry.bin`
    (`GST_REGISTRY`, for its page processes only), and the page process gets that one
    directory. A `GST_REGISTRY` or `GST_REGISTRY_1_0` you set yourself is left as it is.
  - With the X11 page host that is the X server's socket, on X11 and under Xwayland
    alike: such a process has what any X client has.
  - With the private page host WebKit would give it the session compositor's socket
    (`$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY`) - where a page process that was taken over could
    act as a program of your session, and on compositors that offer clipboard, screen
    capture and input protocols to every client could use those. The daemon therefore
    puts the full path of the same socket into its own `WAYLAND_DISPLAY` when it starts
    on a Wayland backend: every Wayland library takes that, and WebKit's sandbox set-up
    does not find a file to hand over. Seen with WebKitGTK 2.52: the page's process then
    has no display socket at all. This rests on how WebKit builds that file name; the
    test `web-wayland` looks into the page's process and fails when it changes. Not
    covered: a session started with a one-time socket (`WAYLAND_SOCKET`).
- Sound in a page plays, as under Wallpaper Engine.

Tests: `meson test -C build websec websec-private web-x11 web-wayland webhost-wl`.

## What is trusted on the local machine

- The control socket is for your own programs: mode 0600, a connection
  from another account is dropped (root is accepted). A request line may
  have 1 MiB, 64 connections may be open at once and 8 of them may
  subscribe to state changes; a reply the client does not take at once is
  queued and sent when the socket takes more, and a client that reads
  nothing for 3 seconds is dropped, so one that never reads cannot hold up
  the daemon's other clients. Whoever can reach it
  can do what you can do with WalPE: open any wallpaper folder (scene
  scripts, web pages), point `we_assets` at other shaders, run an import
  through ffmpeg or a download through steamcmd. Do not hand the socket to
  a sandboxed program.
- The socket, its lock file (`walpe.sock.lock`) and the crash-loop marker
  (`walpe.sock.loading`) live in `$XDG_RUNTIME_DIR` (without it: `~/.cache`)
  only when that directory is yours and nobody else can write to it. If it
  is not (`XDG_RUNTIME_DIR=/tmp`, a directory someone else created, one
  that is gone) they go to `<tmp>/walpe-<uid>/` (0700) and the log says so;
  if that name is taken by someone else too, no daemon starts and no client
  connects ("no safe place for the control socket"). The fix is a runtime
  directory of your own with mode 0700.
- `config.json` is written 0600 (it names your Steam account, every
  wallpaper path and what you typed into wallpaper options); a file from an
  older version is narrowed when the daemon reads it. Two more files name
  wallpaper paths and are written 0600 as well: the crash-loop marker next
  to the socket and the tray's list of the wallpapers last shown
  (`~/.local/state/walpe/tray-recent`; one from an older version is
  narrowed when it is next written). `workshop.json` (the
  Steam Web API key) and `daemon.log` were 0600 already. `~/.cache/walpe`
  (unpacked wallpapers, converted videos, reports) is made 0700 by the
  daemon at every start.
- `walpe-video.so` / `walpe-web.so` are code that runs inside the daemon:
  a file that belongs to another account, that everybody can write to, or
  that sits in such a directory is not loaded (log: "module: ... not
  loaded"), and `WALPE_MODULE_DIR` has to be an absolute path.
  `gst/libgstwalpesound.so` next to them is code too, of the page process
  (inside WebKit's sandbox) and of `gst-inspect-1.0`, which the daemon
  runs outside any sandbox when a web wallpaper has media files:
  GStreamer loads whatever is in that directory and below it, so the same
  rule decides whether the daemon names the directory to GStreamer at
  all - for the file, and for everything else in the directory; a
  directory with a `:` in its name is not named either (the plugin path
  is a list).
- A video the player cannot read is converted once by `ffmpeg` (found in
  `PATH`, like `ffprobe`). The file is a wallpaper's: both are given it as
  a local file and nothing else (`-protocol_whitelist file`, for a clip
  inside a scene's file a part of that file: no playlist, no script of
  file names, no address), may open container and video stream formats
  only, and no picture of more than 40 megapixels. A
  conversion nobody asked for in person (a wallpaper is opened, a scene
  has a video) is stopped after 60 minutes, at 8192 MB of output or when
  less than 256 MB are left on the disk (`WALPE_CONVERT_MAX_MIN`,
  `WALPE_CONVERT_MAX_MB`, 0 = no limit; `walpe-ctl add` has none), and ffmpeg
  ends with the daemon, however the daemon ends; what it had written so
  far is removed at the next start.
- The environment the daemon is started with is trusted like the daemon
  itself (`LD_*`, `GST_*`, `WEBKIT_*`, `LIBGL_*`, `__EGL_VENDOR_LIBRARY_*`,
  every `WALPE_*` switch): the restarts in place hand the start
  environment on unchanged, plus `WALPE_IPC_FD` / `WALPE_LAYOUT_RESTARTED` /
  `WALPE_WEB_UNLOADED`, and close every other descriptor.


## Cover art of media integration

A wallpaper with media integration is shown the cover picture the music player on the
session bus names (a file, a `data:` URL or an http(s) address; any address a player names
is fetched, also a local one, and an http redirect is followed wherever it leads). A local
file is read whole before it is decoded (a regular file of at most 64 MiB; not mapped, so a
player that writes its next cover over the same name cannot end the daemon), of an http
body at most 16 MiB are read, counted after unpacking for a compressed one, within 10
seconds; a picture of more than 8192 pixels per side is refused before any
pixel exists, a JPEG that its decoder has to hold whole is not decoded beyond 256 MiB of
that store, an arithmetic-coded JPEG is not decoded, and a GIF is checked as in the table above. The cover goes to a decoder only
when its first bytes are those of a JPEG, PNG, GIF, WebP or BMP file, and then to the
decoder of that one format: gdk-pixbuf is not left to pick one of its other loaders (SVG,
XPM, PNM, TGA, ICO, TIFF ...) for bytes a music player or a server chose. A cover in any
other format is not decoded, and the wallpaper is shown none (the log says "album art not
decodable"). Nothing of a cover is written to disk: two reduced copies (640 pixels at most)
are kept in memory. The texts a player sends (title, artist, album, genres) are kept up to
4096 bytes each, and a key it repeats counts once. Tests: `meson test -C build limits
media-player`.

## Parts added in 0.13

The same rule - a wallpaper's files are data from strangers - as applied in the new parts.
None of them has been attacked by anyone but its own tests.

- **Sound files** ([SOUND.md](SOUND.md)): looked up inside the wallpaper's folder or
  Wallpaper Engine's assets only (no absolute paths, no `..`, no links that lead out, no
  addresses), regular files of at most 2 GiB; the file's first bytes decide the format, and
  only four FFmpeg decoders can be reached (MP3, FLAC, Vorbis, Opus; WAV is read by WalPE
  itself); 8000-192000 Hz, 1-8 channels, at most 6 hours, 64 files per layer, 32 sound
  layers per wallpaper and 16 sounding layers in the whole daemon. A project cannot declare
  an option named `walpe_sound`. Tests: `scenesound`, `sounddec`.
- **Sprite sheets**: the frame table comes from the wallpaper and is checked before use
  (every frame on the picture and with an area, times that are numbers); a sheet that
  fails is refused. A table has at most 65536 frames, the animations of one scene
  1048576 together (every layer holds the table it plays from, 32 bytes a frame); a
  layer beyond that shows its first frame as a still. A script reaches the animations
  and the camera of its own scene only, and what it sets is clamped. A zoom in the
  scene file that no float holds (`1e39`) is no zoom: the scene is drawn at zoom 1.
  Tests: `sprite-sheet`, `scene-feat`.
- **Fluid simulation**: an effect file gets a grid of at most 1024 cells on its longer side
  and no buffer larger than the driver's largest texture. Test: `fx-sim`.
- **Effect shaders on OpenGL ES** go through one more rewriting stage ([GPU.md](GPU.md))
  with the limits of the table above. Tests: `fx-essl`, `fx-hostile`.
- **Web wallpapers on Wayland**: by default the page's hidden window is an X window under
  Xwayland, with the selection guard of X11 sessions; without an X display that answers
  it lives on the private page host ([WAYLAND.md](WAYLAND.md)). What a page gets on
  either, and what the private page host and WebKit's sandbox keep from it, is in
  [Web wallpapers](#web-wallpapers) above. The wallpaper surface itself never takes the
  keyboard.
- **GPU choice**: for a `gpu` value that names a device, and with `gpu=auto` on a machine
  whose integrated GPU drives the displays beside a discrete one, the daemon starts short
  child processes of itself (`--gpu-probe`) and sets driver variables in its own
  environment; a variable that is already set is never replaced. `walpe-ctl gpu-stats` and the kernel
  section of a profile read `/proc/<pid>/fdinfo` of your own processes and `/sys` of the
  GPU, nothing of another user's process, and start no program but `nvidia-smi` (the
  command and the report script only, never the daemon)
  ([GPU-PROFILING.md](GPU-PROFILING.md)).
- **A compositor that says nothing**: when the daemon starts it waits at most 10 seconds
  for the first answers of a Wayland compositor (5 seconds on the connection of the
  pause rules), and one second for every later one. A compositor that does not answer
  counts as absent; it cannot hold the daemon's start. Requests that queued on the
  control socket while the daemon waited for a compositor to come back are closed
  unanswered, not carried out later. Tests: `daemon-wayland-silent`, `sig-wl`, `ipc`.
- **Names from `/sys` and the PCI id list** (drivers, devices, outputs) are cut to a
  fixed length, made valid UTF-8 and cleared of control characters before they reach the
  log, the status reply or a driver variable. Tests: `gpusel`, `gpustat`.
- **logind** is asked over the system bus for three facts (suspend, locked hint, active
  session); `WALPE_LOGIND_BUS=session` (tests) looks for it on the session bus, where any
  program of yours can stand in for it.

## Workshop data

What Steam's servers answer and
what a wallpaper's `project.json` says is treated as data from strangers. Replies are read up
to a size limit and at most 120 s, redirects are followed only to Steam's https hosts (never
for calls that carry the Web API key), preview pictures go to a decoder only when their first
bytes are those of a JPEG, PNG, GIF or WebP file (library previews also BMP and AVIF) and then
to the decoder of that one format (gdk-pixbuf is never left to pick one of its other loaders -
SVG, XPM, PNM, TGA, ICO ... - for them), a JPEG that its decoder has to hold whole (progressive,
or a scan per component) is not decoded beyond 256 MiB of that store, an arithmetic-coded JPEG
is not decoded at all, and titles, tags, names
and descriptions are cut to sane lengths and cleaned of control and text-direction characters
before they are shown. `walpe-ctl` writes the texts it takes from a reply (a Workshop title, the
layer and effect names of a scene, a text layer's text, the file names in a memory report) with
their control characters made visible (`\x1b`), so an escape sequence in them never reaches the
terminal; a reply printed as JSON has them as `\u001b`. The optional Steam Web API key lives in `~/.config/walpe/workshop.json`
(mode 0600), is sent to api.steampowered.com only and is never logged; WalPE never handles a
Steam password. Tests: `meson test -C build wsnet uitext thumbs steamsub workshop`.

