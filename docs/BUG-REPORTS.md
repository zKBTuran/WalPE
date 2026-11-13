# Reporting a bug

Bugs are reported with the forms of the
[issue tracker](https://github.com/Fired-Cloud/WalPE/issues/new/choose). Two things save a
round of questions: the output of `walpe-ctl diag`, and for a problem with sound, with the
wallpaper window on Wayland or with the tray the archive that `walpe-collect` makes. A
security problem is not reported in public:
[SECURITY.md](SECURITY.md#reporting-a-security-problem).

`walpe-ctl diag` prints the daemon's state in a dozen lines: version, session, window
system, OpenGL driver, modules, sound, and per monitor what it shows and which rule pauses
it. Wallpapers are named by the last part of their path and the home directory is written
`~`, so it can be pasted as it is. WalPE 0.14.0 does not have it yet; there
`walpe-ctl status` is what to send: one line of JSON, with the full paths.

Its second line, `source:`, is the commit of the source the daemon was built from - test
builds answer the version of the release they follow, the commit tells them apart.
`walpe-ctl status` has it as `source_id`, and `walpe-daemon --version` prints it without
a daemon running. A build before this line existed has neither.

The collector, from the source directory (the one that holds `build/`), or `walpe-collect`
where WalPE is installed:

```sh
sh scripts/walpe-collect.sh --redact
```

It needs no root and takes about ten minutes. It stops the running daemon and tray, starts
the daemon with its debug output going to a file, asks you to do the few things only you
can do (show a wallpaper with sound; on a Wayland session put a video wallpaper on, click
the empty desktop, open the tray menu and choose Quit) and a few yes / no questions, and
at the end starts the daemon and the tray again if they ran before. It changes no setting
without asking and puts the sound settings back. `--sound-only` and `--wayland-only` run
one part; `WALPE_BUILD=<dir>` names the directory of `walpe-daemon` and `walpe-ctl` when
they are neither in `build/` nor next to the script. The result is the directory
`~/walpe-report` (`report.txt` in it lists what was run, what failed and your answers; a
report of an earlier run is replaced) and the same content packed as
`~/walpe-report.tar.gz` and, where a zip program is found, `~/walpe-report.zip`: attach
one of the two. Nothing is uploaded by the script.

**Look before you send.** The files hold the full paths of your wallpapers (so your user
name and folder names) and their titles, the texts of the tray menu, the names of the
programs that have a tray icon or an open window (class, size and position of each window,
not its title), your sound devices and every program that plays sound at that moment with
the title of what it plays, your user and host name, and GPU, driver, monitor models and
desktop versions. No passwords and no keys. `--redact` replaces your home path, user name
and host name by `<home>`, `<user>` and `<host>` in every file before packing (a name of
fewer than three letters is left alone); wallpaper names, window class names and device
names stay, so look into `~/walpe-report` all the same.

What else helps for one kind of problem: [WAYLAND.md](WAYLAND.md#for-a-bug-report) for the
Wayland backend, the last paragraph of [GPU.md](GPU.md#amd-intel-and-nvidia-in-short) for
AMD and Intel machines, and `walpe-gpu-report` for load, stutter or memory
([GPU-PROFILING.md](GPU-PROFILING.md)).
