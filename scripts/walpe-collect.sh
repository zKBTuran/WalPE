#!/bin/sh
# walpe-collect.sh (version 2) - collect the facts for a WalPE bug report (written for 0.14.0).
#
#   sh walpe-collect.sh                 everything that fits this session
#   sh walpe-collect.sh --sound-only    only the sound part (any session)
#   sh walpe-collect.sh --wayland-only  only the Wayland / KDE part (window, desktop, tray)
#   --redact   (with any of them) before packing, replace your home path, user name and host
#              name by <home>, <user>, <host> in every file of the report
#
# Put it into the WalPE folder (where build/ is) or run it from there. No root needed.
# It stops the running engine and tray, starts the engine with its debug output going to
# a file, asks you to do the few things only you can do (press Enter after each) and a few
# y/n questions, and at the end starts the engine and the tray again if they ran before.
# Result: ~/walpe-report/, ~/walpe-report.tar.gz and ~/walpe-report.zip. Nothing is uploaded.
# Settings: it changes none without asking, and puts the sound settings back.

COLLECT_VERSION=2
MODE=all
REDACT=0
for a in "$@"; do
    case "$a" in
        --sound-only) [ "$MODE" = all ] || { echo "walpe-collect: --sound-only or --wayland-only, not both" >&2; exit 1; }; MODE=sound ;;
        --wayland-only) [ "$MODE" = all ] || { echo "walpe-collect: --sound-only or --wayland-only, not both" >&2; exit 1; }; MODE=wayland ;;
        --redact) REDACT=1 ;;
        -h | --help) sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "walpe-collect: unknown option '$a' (--sound-only, --wayland-only, --redact, --help)" >&2; exit 1 ;;
    esac
done

# ---- where the programs are
here=$(dirname "$0")
B=
for d in "${WALPE_BUILD:-}" "$here/build" "./build" "$here" "."; do
    [ -n "$d" ] || continue
    if [ -x "$d/walpe-daemon" ] && [ -x "$d/walpe-ctl" ]; then
        B=$(cd "$d" && pwd)
        break
    fi
done
if [ -z "$B" ]; then
    echo "walpe-collect: build/walpe-daemon and build/walpe-ctl were not found." >&2
    echo "Run this script from the WalPE folder (the one that holds build/), after ./build.sh." >&2
    exit 1
fi
CTL=$B/walpe-ctl
DAEMON=$B/walpe-daemon
TRAY=$B/walpe-tray

# ---- the report directory
R=$HOME/walpe-report
if [ -e "$R" ]; then
    if [ -f "$R/.walpe-collect" ]; then
        rm -rf "$R"                     # an earlier report of this script
    else
        mv "$R" "$R.old.$$" || { echo "walpe-collect: cannot move $R away" >&2; exit 1; }
        echo "(an existing $R was moved to $R.old.$$)"
    fi
fi
mkdir -p "$R" || { echo "walpe-collect: cannot create $R" >&2; exit 1; }
: > "$R/.walpe-collect"
LOG=$R/report.txt

note () { echo "$*" >> "$LOG"; }
say () { echo "$*"; note "# $*"; }
# both clocks: the engine's own lines carry local time, the lines of WAYLAND_DEBUG
# ("[hh:mm:ss.micro]", or milliseconds on older libwayland) carry UTC
stamp () { echo "UTC $(LC_ALL=C date -u +%H:%M:%S), local $(LC_ALL=C date +%H:%M:%S)"; }

# the tools run with English messages: LC_ALL=C, or C.UTF-8 where the system has it (Qt's
# programs complain about plain C in every output)
CL=C
if locale -a 2> /dev/null | grep -qi '^C\.utf-\{0,1\}8$'; then CL=C.UTF-8; fi

# run FILE COMMAND [ARG...]: its output goes to (the end of) $R/FILE, with messages in
# English (LC_ALL=$CL); a program that is missing or fails is written down and nothing stops
run () {
    _f=$1
    shift
    if ! command -v "$1" > /dev/null 2>&1; then
        note "$_f: SKIPPED, $1 is not installed"
        echo "($1 is not installed)" >> "$R/$_f"
        return 127
    fi
    if command -v timeout > /dev/null 2>&1; then
        LC_ALL=$CL timeout 20 "$@" >> "$R/$_f" 2>&1
    else
        LC_ALL=$CL "$@" >> "$R/$_f" 2>&1
    fi
    _rc=$?
    if [ "$_rc" -eq 0 ]; then note "$_f: ok: $*"; else note "$_f: EXIT STATUS $_rc: $*"; fi
    return "$_rc"
}

# a line in the engine's (or tray's) own log file and in report.txt: what happens now
CUR=
mark () {
    _t="=== walpe-collect: $* ($(stamp)) ==="
    [ -z "$CUR" ] || echo "$_t" >> "$R/$CUR"
    note "$_t"
}

# the one thing a person has to do: said in one line, then Enter (end of input counts as Enter)
pause () {
    mark "ASKING THE USER NOW: $*"
    echo
    echo ">>> $*"
    printf '>>> then press Enter: '
    read -r _ans || echo
    mark "the user pressed Enter"
}

# ask QUESTION: yes = exit status 0; anything else, also end of input, is no
ask () {
    printf '%s [y/N] ' "$*"
    _ans=
    read -r _ans || echo
    note "QUESTION: $* -> ANSWER: ${_ans:-(none)} ($(stamp))"
    case "$_ans" in y | Y | yes | Yes | YES) return 0 ;; *) return 1 ;; esac
}

# choose KEY QUESTION: a one-letter answer, written down as "KEY: letter"; ANS holds it
choose () {
    _key=$1
    shift
    printf '%s ' "$*"
    ANS=
    read -r ANS || echo
    note "QUESTION: $* -> ANSWER: ${ANS:-(none)} ($(stamp))"
    note "$_key: ${ANS:-(none)}"
}

daemon_answers () { "$CTL" status 2> /dev/null | grep -q '"ok":true'; }
nap () { sleep "$1" 2> /dev/null || sleep 1; }

wait_daemon_gone () {       # seconds
    _n=$(($1 * 4))
    while [ "$_n" -gt 0 ]; do
        daemon_answers || return 0
        nap 0.25
        _n=$((_n - 1))
    done
    return 1
}

DPID=
TPID=
MPID=
stop_debug_daemon () {
    [ -n "$DPID" ] || return 0
    "$CTL" quit > /dev/null 2>&1
    _n=40
    while kill -0 "$DPID" 2> /dev/null && [ "$_n" -gt 0 ]; do
        nap 0.25
        _n=$((_n - 1))
    done
    if kill -0 "$DPID" 2> /dev/null; then
        note "the debug engine (pid $DPID) did not end after quit: sent SIGTERM"
        kill "$DPID" 2> /dev/null
        sleep 2
        kill -9 "$DPID" 2> /dev/null
    fi
    DPID=
}

stop_tray () {
    [ -n "$TPID" ] || return 0
    "$TRAY" --quit > /dev/null 2>&1
    nap 0.5
    kill "$TPID" 2> /dev/null
    TPID=
}

stop_monitor () {
    [ -n "$MPID" ] || return 0
    kill "$MPID" 2> /dev/null
    MPID=
}

# start_debug_daemon FILE [wayland]: the engine with all its messages in $R/FILE
# (appended; its own size limit for log files is switched off for this run)
start_debug_daemon () {
    _file=$1
    stop_debug_daemon
    CUR=$_file
    mark "engine started by walpe-collect"
    if [ "${2:-}" = wayland ]; then
        G_MESSAGES_DEBUG=all WALPE_LOG_MAX_MB=0 WAYLAND_DEBUG=1 "$DAEMON" >> "$R/$_file" 2>&1 &
    else
        G_MESSAGES_DEBUG=all WALPE_LOG_MAX_MB=0 "$DAEMON" >> "$R/$_file" 2>&1 &
    fi
    DPID=$!
    _n=120
    while [ "$_n" -gt 0 ]; do
        daemon_answers && break
        kill -0 "$DPID" 2> /dev/null || break
        nap 0.25
        _n=$((_n - 1))
    done
    if daemon_answers && kill -0 "$DPID" 2> /dev/null; then
        note "$_file: the engine runs (pid $DPID)"
        sleep 3                         # the last wallpaper comes back by itself
        return 0
    fi
    if daemon_answers; then
        note "$_file: ANOTHER ENGINE ANSWERS, not the one started here (something started one meanwhile)"
        echo "!!! another engine was started by something else meanwhile; close the WalPE window and run this again"
        DPID=
        return 1
    fi
    note "$_file: THE ENGINE DID NOT START (or does not answer): see the file"
    echo "!!! the engine did not start; what it said is in $R/$_file"
    return 1
}

# versions after 0.14.0 have `walpe-ctl diag` (a summary made for reports); 0.14.0 has not
try_diag () { LC_ALL=$CL "$CTL" diag > "$R/$1" 2> /dev/null || rm -f "$R/$1"; }

start_detached () {     # LOGFILE COMMAND...
    _log=$1
    shift
    if command -v setsid > /dev/null 2>&1; then
        setsid "$@" >> "$_log" 2>&1 < /dev/null &
    else
        nohup "$@" >> "$_log" 2>&1 < /dev/null &
    fi
}

# ---- values out of a status reply (one long line of JSON)
status_value () {       # FILE KEY: the value of a number / true / false member
    sed -n "s/.*\"$2\":\\([-0-9.eE+a-z]*\\).*/\\1/p" "$1" 2> /dev/null | head -n 1
}
status_first () {       # FILE KEY: the first string member of that name (the reply's own, not a monitor's)
    awk -v k="\"$2\":\"" '{ i = index($0, k); if (i) { s = substr($0, i + length(k)); j = index(s, "\""); print substr(s, 1, j - 1); exit } }' "$1" 2> /dev/null
}
onoff () { if [ "$1" = true ]; then echo on; else echo off; fi; }

# ---- who runs: process ids by name, and the program that started each (its parent)
procs_named () { LC_ALL=$CL ps -U "$(id -u)" -o pid= -o comm= 2> /dev/null | awk -v n="$1" '$2 == n { print $1 }'; }
describe_procs () {     # NAME: a line per process "pid P, started by PARENT (pid PP)"
    for _p in $(procs_named "$1"); do
        _pp=$(LC_ALL=$CL ps -o ppid= -p "$_p" 2> /dev/null | tr -d ' ')
        _pc=$(LC_ALL=$CL ps -o comm= -p "${_pp:-0}" 2> /dev/null)
        echo "$1: pid $_p, started by ${_pc:-?} (pid ${_pp:-?})"
    done
}

# ---- the engine's CPU use over 10 seconds (the kernel's clock ticks of the process)
cpu_ticks () { awk '{ sub(/^.*\) /, ""); print $12 + $13 }' "/proc/$1/stat" 2> /dev/null; }
cpu_sample () {         # LABEL
    [ -n "$DPID" ] && kill -0 "$DPID" 2> /dev/null || { note "cpu ($1): no engine to measure"; return 0; }
    echo "    (measuring the engine's CPU use for 10 seconds ...)"
    _a=$(cpu_ticks "$DPID")
    _pa=$(LC_ALL=$CL ps -o pcpu= -o cputime= -p "$DPID" 2> /dev/null)
    sleep 10
    _b=$(cpu_ticks "$DPID")
    _pb=$(LC_ALL=$CL ps -o pcpu= -o cputime= -p "$DPID" 2> /dev/null)
    _hz=$(getconf CLK_TCK 2> /dev/null)
    _pct=$(awk -v a="${_a:-0}" -v b="${_b:-0}" -v hz="${_hz:-100}" 'BEGIN { printf "%.1f", (b - a) * 100 / hz / 10 }' 2> /dev/null)
    note "ENGINE CPU ($1): ${_pct:-?} % of one core over 10 s (ps pcpu/cputime before:${_pa:- ?} after:${_pb:- ?})"
    echo "$1: ${_pct:-?} % of one core over 10 s; ps pcpu cputime before:${_pa:- ?} after:${_pb:- ?}" >> "$R/cpu.txt"
}

# ---- a video wallpaper: does its file have a sound track? (codec names only, no file name)
video_probe () {        # STATUSFILE LABEL
    [ "$(status_first "$1" type)" = video ] || return 0
    _vp=$(status_first "$1" path | sed 's#\\/#/#g')
    if [ -d "$_vp" ] && [ -f "$_vp/project.json" ]; then
        _vf=$(sed -n 's/.*"file"[ ]*:[ ]*"\([^"]*\)".*/\1/p' "$_vp/project.json" | head -n 1)
        _vp=$_vp/$_vf
    fi
    echo "=== $2: the streams of the video wallpaper shown ($(stamp)) ===" >> "$R/video-streams.txt"
    if [ -f "$_vp" ] && command -v ffprobe > /dev/null 2>&1; then
        LC_ALL=$CL ffprobe -v error -show_entries stream=index,codec_type,codec_name,channels,sample_rate,width,height,avg_frame_rate -of compact=p=0 "$_vp" >> "$R/video-streams.txt" 2>&1 < /dev/null
        note "video-streams.txt ($2): ffprobe exit status $? (a line with codec_type=audio = the file has a sound track)"
    elif [ -f "$_vp" ]; then
        note "video-streams.txt ($2): SKIPPED, ffprobe is not installed"
        echo "(ffprobe is not installed)" >> "$R/video-streams.txt"
    else
        note "video-streams.txt ($2): the video file was not found from the status reply"
        echo "(file not found from the status reply)" >> "$R/video-streams.txt"
    fi
}

# ---- KWin's windows, bottom to top, with their layer: a small KWin script is loaded over
# D-Bus, sends one line per window to a name on the session bus, and a bus monitor writes
# the lines down. Window titles are NOT read: class, layer, geometry, flags only.
# Best effort: without KWin, busctl or a monitor program nothing happens but a note.
STACKN=0
kwin_stack () {         # FILE LABEL
    _out=$1
    _label=$2
    echo "=== $_label ($(stamp)) ===" >> "$R/$_out"
    if ! command -v busctl > /dev/null 2>&1; then
        note "$_out: SKIPPED ($_label), busctl is not installed"
        return 0
    fi
    if ! LC_ALL=$CL busctl --user status org.kde.KWin > /dev/null 2>&1; then
        note "$_out: SKIPPED ($_label), no KWin on the session bus"
        echo "(no KWin on the session bus)" >> "$R/$_out"
        return 0
    fi
    STACKN=$((STACKN + 1))
    _tag=wc$$n$STACKN
    _js=$R/.stack-$_tag.js
    _raw=$R/.stack-$_tag.raw
    cat > "$_js" << EOF
(function () {
    var tag = "$_tag";
    function send(s) {
        try { callDBus("io.github.walpe.collect", "/", "io.github.walpe.collect", "w", tag + " " + s); } catch (e) {}
        try { print("WALPECOLLECT " + tag + " " + s); } catch (e) {}
    }
    var list = [], src = "stackingOrder";
    try { if (workspace.stackingOrder) list = workspace.stackingOrder; } catch (e) {}
    if (!list || !list.length) {
        src = "windowList";
        try { list = workspace.windowList ? workspace.windowList() : workspace.clientList(); }
        catch (e) { try { list = workspace.clientList(); src = "clientList"; } catch (e2) { list = []; } }
        list = Array.prototype.slice.call(list);
        list.sort(function (a, b) { return (a.stackingOrder || 0) - (b.stackingOrder || 0); });
    }
    send("BEGIN source=" + src + " count=" + list.length + " (bottom first; layer: 0 desktop, 1 below, 2 normal, 3 dock, higher = above)");
    for (var i = 0; i < list.length; i++) {
        var w = list[i];
        function p(n) { try { var v = w[n]; return v === undefined ? "?" : v; } catch (e) { return "!"; } }
        var g = null, o = "?";
        try { g = w.frameGeometry || w.geometry; } catch (e) {}
        try { o = w.output ? w.output.name : "?"; } catch (e) {}
        send(i + " stackingOrder=" + p("stackingOrder") + " layer=" + p("layer") + " class=" + p("resourceClass") +
             " name=" + p("resourceName") + " desktopWindow=" + p("desktopWindow") + " dock=" + p("dock") +
             " normal=" + p("normalWindow") + " keepBelow=" + p("keepBelow") + " keepAbove=" + p("keepAbove") +
             " active=" + p("active") + " minimized=" + p("minimized") + " hidden=" + p("hidden") +
             " opacity=" + p("opacity") + " wantsInput=" + p("wantsInput") + " pid=" + p("pid") + " output=" + o +
             " geom=" + (g ? g.x + "," + g.y + "+" + g.width + "x" + g.height : "?"));
    }
    send("END");
})();
EOF
    : > "$_raw"
    _hole=
    _mon=
    # (a receiver for the lines, where the tool for one exists: the calls then have an address)
    if command -v dbus-test-tool > /dev/null 2>&1; then
        LC_ALL=$CL dbus-test-tool black-hole --session --name=io.github.walpe.collect > /dev/null 2>&1 &
        _hole=$!
    fi
    if command -v dbus-monitor > /dev/null 2>&1; then
        LC_ALL=$CL dbus-monitor --session "type='method_call',interface='io.github.walpe.collect'" >> "$_raw" 2>&1 &
        _mon=$!
    else
        LC_ALL=$CL busctl --user monitor --match "type='method_call',interface='io.github.walpe.collect'" >> "$_raw" 2>&1 &
        _mon=$!
    fi
    nap 0.7
    _id=$(LC_ALL=$CL busctl --user call org.kde.KWin /Scripting org.kde.kwin.Scripting loadScript ss "$_js" "walpe-collect-$_tag" 2>> "$_raw" | awk '{ print $2 }')
    if [ -n "$_id" ]; then
        # KWin 6: /Scripting/Script<id>; KWin 5.27: /<id>
        LC_ALL=$CL busctl --user call org.kde.KWin "/Scripting/Script$_id" org.kde.kwin.Script run > /dev/null 2>&1 ||
            LC_ALL=$CL busctl --user call org.kde.KWin "/$_id" org.kde.kwin.Script run > /dev/null 2>&1
        _n=16
        while [ "$_n" -gt 0 ] && ! grep -q "$_tag END" "$_raw" 2> /dev/null; do
            nap 0.25
            _n=$((_n - 1))
        done
        LC_ALL=$CL busctl --user call org.kde.KWin /Scripting org.kde.kwin.Scripting unloadScript s "walpe-collect-$_tag" > /dev/null 2>&1
    fi
    [ -z "$_mon" ] || kill "$_mon" 2> /dev/null
    [ -z "$_hole" ] || kill "$_hole" 2> /dev/null
    # the lines, whichever monitor wrote them: ... "wcNnM <text>" ...
    _got=$(sed -n "s/.*\"$_tag \\(.*\\)\".*/\\1/p" "$_raw" | grep -v '^ *$')
    if [ -z "$_got" ] && command -v journalctl > /dev/null 2>&1; then
        # (the script also printed them: KWin's own log, where the journal has it)
        _got=$(LC_ALL=$CL journalctl --user -b -o cat --since "-30s" 2> /dev/null | sed -n "s/.*WALPECOLLECT $_tag //p")
    fi
    if [ -n "$_got" ]; then
        echo "$_got" >> "$R/$_out"
        note "$_out: ok ($_label): $(echo "$_got" | grep -c .) lines from KWin"
    else
        echo "(KWin gave no list: script id '${_id:-none}')" >> "$R/$_out"
        note "$_out: NO DATA ($_label): KWin script id '${_id:-none}', nothing came back"
    fi
    rm -f "$_js" "$_raw"
}

# ---- the tray's menu as it hands it to the panel, and the signals it sends about it
tray_menu () {          # FILE LABEL
    echo "=== $2 ($(stamp)) ===" >> "$R/$1"
    run "$1" busctl --user call io.github.walpe.tray /MenuBar com.canonical.dbusmenu GetLayout iias -- 0 -1 0
    echo >> "$R/$1"
}
start_tray_logged () {  # LABEL
    CUR=tray.txt
    mark "tray started by walpe-collect: $1"
    "$TRAY" >> "$R/tray.txt" 2>&1 &
    TPID=$!
    sleep 3
    stop_monitor
    echo "=== $1 ($(stamp)) ===" >> "$R/tray-signals.txt"
    if command -v dbus-monitor > /dev/null 2>&1; then
        LC_ALL=$CL dbus-monitor --session "type='signal',sender='io.github.walpe.tray'" >> "$R/tray-signals.txt" 2>&1 &
        MPID=$!
    elif command -v busctl > /dev/null 2>&1; then
        LC_ALL=$CL busctl --user monitor --match "type='signal',sender='io.github.walpe.tray'" >> "$R/tray-signals.txt" 2>&1 &
        MPID=$!
    else
        note "tray-signals.txt: SKIPPED, neither dbus-monitor nor busctl is installed"
    fi
    nap 0.5
}
# one round of the tray test; the engine and the tray run already. PREFIX names the files.
tray_round () {         # PREFIX LABEL
    _px=$1
    "$CTL" pause > /dev/null 2>&1
    sleep 1
    tray_menu "$_px-menu.txt" "$2: while paused (walpe-ctl pause)"
    "$CTL" resume > /dev/null 2>&1
    _cur=$(status_first "$R/$_px-status.json" path | sed 's#\\/#/#g')
    if [ -n "$_cur" ]; then
        if "$CTL" open "$_cur" > /dev/null 2>&1; then note "$_px: the same wallpaper was opened again (a wallpaper change for the menu)"; fi
    fi
    sleep 2
    tray_menu "$_px-menu.txt" "$2: running again, wallpaper opened again"
    run "$_px-items.txt" busctl --user get-property org.kde.StatusNotifierWatcher /StatusNotifierWatcher org.kde.StatusNotifierWatcher RegisteredStatusNotifierItems
    pause "Open the menu of the WalPE tray icon and LOOK at it. Do not choose anything yet"
    tray_menu "$_px-menu.txt" "$2: the user has looked at the menu (before Quit)"
    choose "MENU LOOK ($2)" "What does the menu look like?  c = correct   s = scrambled (texts in wrong rows)   n = no Quit entry   x = no tray icon at all   [c/s/n/x]"
    pause "Open that menu again and choose Quit (answer its question if one appears). Wait 5 seconds"
    if ask "Did Quit end WalPE (wallpaper gone, tray icon gone)?"; then note "QUIT WORKED, says the user ($2): yes"; else note "QUIT WORKED, says the user ($2): no (or not answered)"; fi
    if [ -n "$TPID" ] && kill -0 "$TPID" 2> /dev/null; then
        note "$_px after Quit: the tray process is STILL RUNNING"
        tray_menu "$_px-menu.txt" "$2: after Quit (the tray still runs)"
    else
        note "$_px after Quit: the tray process has ended"
        TPID=
    fi
    if daemon_answers; then
        note "$_px after Quit: the engine is STILL RUNNING"
        run "$_px-status-after-quit.json" "$CTL" status
    else
        note "$_px after Quit: the engine has ended"
        DPID=
    fi
    stop_monitor
    stop_tray
    stop_debug_daemon
}

# ---- sound settings: put back
SOUND_CHANGED=0
OLD_SS=
OLD_VOL=
OLD_MUTE=
restore_sound () {
    [ "$SOUND_CHANGED" = 1 ] || return 0
    SOUND_CHANGED=0
    if "$CTL" set "scene_sound=$OLD_SS" "volume=$OLD_VOL" "mute=$OLD_MUTE" > /dev/null 2>&1; then
        say "sound settings put back to what they were: scene_sound=$OLD_SS volume=$OLD_VOL mute=$OLD_MUTE"
    else
        say "!!! could not put the sound settings back; they were: scene_sound=$OLD_SS volume=$OLD_VOL mute=$OLD_MUTE"
        say "    (run: $CTL set scene_sound=$OLD_SS volume=$OLD_VOL mute=$OLD_MUTE)"
    fi
}

# ---- the end, also after Ctrl+C: settings back, our engine and tray stopped, the user's
# own started again as they were
DAEMON_WAS=0
TRAY_WAS=0
UNIT_WAS=0
FINISHED=0
finish () {
    [ "$FINISHED" = 0 ] || return 0
    FINISHED=1
    trap '' INT TERM HUP
    restore_sound
    stop_monitor
    stop_tray
    stop_debug_daemon
    CUR=
    if [ "$UNIT_WAS" = 1 ]; then
        systemctl --user start walpe.service > /dev/null 2>&1 && say "engine started again (systemd unit walpe.service)"
    elif [ "$DAEMON_WAS" = 1 ]; then
        _c=${XDG_CACHE_HOME:-$HOME/.cache}/walpe
        mkdir -p "$_c" && chmod 700 "$_c" 2> /dev/null
        ( umask 077; : >> "$_c/daemon.log" )
        start_detached "$_c/daemon.log" "$DAEMON"
        _n=40
        while ! daemon_answers && [ "$_n" -gt 0 ]; do
            nap 0.25
            _n=$((_n - 1))
        done
        if daemon_answers; then
            say "engine started again, as before (its log: $_c/daemon.log)"
        else
            say "!!! the engine did not come back: start it with $DAEMON (or the WalPE window)"
        fi
    fi
    if [ "$TRAY_WAS" = 1 ] && [ -x "$TRAY" ]; then
        start_detached /dev/null "$TRAY"
        say "tray started again, as before"
    fi
}
trap 'echo; echo "interrupted: putting things back"; finish; exit 130' INT TERM HUP

# ---- start
say "walpe-collect version $COLLECT_VERSION, mode $MODE, redact=$REDACT, $(LC_ALL=C date -u '+%Y-%m-%d %H:%M:%S UTC') = local $(LC_ALL=C date '+%H:%M:%S %z')"
note "times: the engine's and the tray's own lines carry LOCAL time; the lines of WAYLAND_DEBUG (in daemon-wayland.txt) carry UTC; every marker of this script has both"
note "programs: $B"
run version.txt "$TRAY" --version
SESSION=x11
if [ -n "${WAYLAND_DISPLAY:-}" ] || [ "${XDG_SESSION_TYPE:-}" = wayland ]; then SESSION=wayland; fi
note "session: $SESSION (XDG_SESSION_TYPE=${XDG_SESSION_TYPE:-not set}, XDG_CURRENT_DESKTOP=${XDG_CURRENT_DESKTOP:-not set})"
note "locale: LANG=${LANG:-not set} LC_ALL=${LC_ALL:-not set} LC_NUMERIC=${LC_NUMERIC:-not set} (the tools below run with LC_ALL=$CL, the engine and the tray with yours)"
DO_SOUND=0
DO_WL=0
case "$MODE" in
    sound) DO_SOUND=1 ;;
    wayland) DO_WL=1 ;;
    all)
        DO_SOUND=1
        if [ "$SESSION" = wayland ]; then DO_WL=1; else say "X11 session: the Wayland / KDE part is skipped"; fi
        ;;
esac

# what runs now and who started it, and the log of the runs where it went wrong
{
    echo "=== as found ($(stamp)) ==="
    describe_procs walpe-daemon
    describe_procs walpe-tray
    describe_procs walpe
} >> "$R/as-found.txt" 2>&1
if daemon_answers; then
    DAEMON_WAS=1
    run status-as-found.json "$CTL" status
fi
if command -v systemctl > /dev/null 2>&1 && systemctl --user is-active --quiet walpe.service 2> /dev/null; then
    UNIT_WAS=1
fi
for f in daemon.log daemon.log.old; do
    _src=${XDG_CACHE_HOME:-$HOME/.cache}/walpe/$f
    if [ -f "$_src" ]; then
        if tail -c 2097152 "$_src" > "$R/old-$f" 2> /dev/null; then note "old-$f: the last 2 MiB of the engine's log file before this run"; fi
    fi
done
if [ -x "$TRAY" ]; then
    "$TRAY" --quit > /dev/null 2>&1 && TRAY_WAS=1
fi
[ -z "$(procs_named walpe)" ] || note "as found: the WalPE window (walpe) is open"
note "as found: engine running=$DAEMON_WAS (systemd unit=$UNIT_WAS), tray running=$TRAY_WAS; who started them: as-found.txt"
sed 's/^/as found: /' "$R/as-found.txt" | grep 'pid' >> "$LOG"

# the machine: graphics driver, screens (colour / HDR / scale), GPUs as WalPE sees them
if [ -r /proc/driver/nvidia/version ]; then run hardware.txt cat /proc/driver/nvidia/version; else note "hardware.txt: no /proc/driver/nvidia/version (no NVIDIA kernel driver loaded)"; fi
if command -v nvidia-smi > /dev/null 2>&1; then
    run hardware.txt nvidia-smi --query-gpu=name,driver_version,memory.used,memory.total,utilization.gpu --format=csv
fi
run hardware.txt uname -sr
if [ "$SESSION" = wayland ] || [ "$DO_WL" = 1 ]; then
    if run kscreen.txt kscreen-doctor -o; then
        # (it colours its output also when that is a file)
        _esc=$(printf '\033')
        sed "s/$_esc\[[0-9;]*m//g" "$R/kscreen.txt" > "$R/kscreen.tmp" 2> /dev/null && mv "$R/kscreen.tmp" "$R/kscreen.txt"
    fi
fi

if [ "$DAEMON_WAS" = 1 ]; then
    say "stopping the running engine (it is started again at the end)"
    if [ "$UNIT_WAS" = 1 ]; then systemctl --user stop walpe.service > /dev/null 2>&1; fi
    "$CTL" quit > /dev/null 2>&1
    if ! wait_daemon_gone 10; then
        command -v pkill > /dev/null 2>&1 && pkill -u "$(id -u)" -x walpe-daemon
        if ! wait_daemon_gone 5; then
            say "!!! the running engine does not stop. End it (pkill -9 walpe-daemon) and run this script again."
            note "gave up: the engine found running did not stop"
            DAEMON_WAS=0
            finish
            exit 2
        fi
        note "the engine found running did not act on quit: it was ended with a signal"
    fi
fi

# ---- sound (any session)
if [ "$DO_SOUND" = 1 ]; then
    say "== sound"
    if start_debug_daemon daemon-sound.txt; then
        run status-start.json "$CTL" status
        run gpu.json "$CTL" gpu --json
        try_diag diag-sound.txt
        WVER=$(sed -n 's/.*walpe-daemon \([0-9][0-9.]*\) .* up (.*/\1/p' "$R/daemon-sound.txt" | head -n 1)
        note "engine version: ${WVER:-?}"
        pause "Show a wallpaper that should have sound (WalPE window, or: $CTL open <wallpaper>). Keep the desktop visible, no window on top of it. Wait 10 seconds"
        # a video has no sound in 0.14.0: the test needs a scene (or a web page)
        _try=0
        INSIST=0
        while :; do
            rm -f "$R/status-sound.json"
            run status-sound.json "$CTL" status
            WTYPE=$(status_first "$R/status-sound.json" type)
            note "the wallpaper shown for the sound test is of type: ${WTYPE:-?}"
            [ "$_try" -gt 0 ] || video_probe "$R/status-sound.json" "sound test"
            case "$WTYPE" in scene | web) break ;; esac
            _try=$((_try + 1))
            echo
            if [ "$WTYPE" = video ]; then
                if [ "${WVER:-0.14.0}" = 0.14.0 ]; then
                    echo "The wallpaper shown is a VIDEO. In WalPE 0.14.0 video wallpapers have no sound at all (their"
                    echo "sound track is not played: known, not a fault of your machine). The test needs a SCENE wallpaper."
                else
                    echo "The wallpaper shown is a VIDEO; this test is about SCENE wallpapers with sound."
                fi
            else
                echo "No scene or web wallpaper is shown (the engine says: type ${WTYPE:-unknown})."
            fi
            [ "$_try" -lt 3 ] || { note "sound test: still no scene / web wallpaper after $_try tries: going on with type ${WTYPE:-?}"; INSIST=1; break; }
            mark "ASKING THE USER NOW: put a scene wallpaper with sound on"
            printf '>>> Put a SCENE wallpaper with sound on and press Enter (or type  go  to test with this one): '
            _ans=
            read -r _ans || { echo; INSIST=1; note "sound test: end of input, going on with type ${WTYPE:-?}"; break; }
            mark "the user pressed Enter"
            case "$_ans" in go | GO | Go) INSIST=1; note "sound test: THE USER INSISTED on testing with type ${WTYPE:-?}"; break ;; esac
        done
        mark "the wallpaper for the sound test is shown (type ${WTYPE:-?})"
        run pactl.txt pactl info
        run pactl.txt pactl list sink-inputs
        run wpctl.txt wpctl status
        OLD_VOL=$(status_value "$R/status-sound.json" volume)     # as the engine wrote it: put back exactly
        _vol=$(awk -v v="$OLD_VOL" 'BEGIN { printf "%.2f", v }' 2> /dev/null)
        OLD_SS=$(onoff "$(status_value "$R/status-sound.json" scene_sound)")
        OLD_MUTE=$(onoff "$(status_value "$R/status-sound.json" mute)")
        _snd=$(status_first "$R/status-sound.json" sound)
        echo
        echo "The engine says about sound: ${_snd:-?}"
        echo "Your sound settings now: scene_sound=$OLD_SS volume=${_vol:-${OLD_VOL:-?}} mute=$OLD_MUTE"
        if [ -z "$OLD_VOL" ]; then
            note "the sound settings could not be read from the status: the test with sound switched on is skipped"
        elif ask "Switch wallpaper sound on for a test (scene_sound=on volume=0.5 mute=off)? Your settings are put back afterwards."; then
            SOUND_CHANGED=1
            mark "set scene_sound=on volume=0.5 mute=off"
            run set-sound.txt "$CTL" set scene_sound=on volume=0.5 mute=off
            sleep 5
            run status-sound2.json "$CTL" status
            run pactl2.txt pactl list sink-inputs
            note "the engine says about sound after the set: $(status_first "$R/status-sound2.json" sound)"
            if ask "Do you hear the wallpaper now?"; then
                note "HEARD after the set: yes (wallpaper type ${WTYPE:-?}, insisted=$INSIST)"
                if ask "Keep these sound settings (scene_sound=on volume=0.5 mute=off)? No = your old ones come back."; then
                    SOUND_CHANGED=0
                    note "the user keeps scene_sound=on volume=0.5 mute=off (before: scene_sound=$OLD_SS volume=$OLD_VOL mute=$OLD_MUTE)"
                else
                    restore_sound
                fi
            else
                note "HEARD after the set: no, or not answered (wallpaper type ${WTYPE:-?}, insisted=$INSIST)"
                restore_sound       # nothing was gained: back to what it was, and said
            fi
        fi
    fi
    stop_debug_daemon
    CUR=
fi

# ---- Wayland / KDE: the wallpaper window, the desktop, the tray
if [ "$DO_WL" = 1 ]; then
    say "== Wayland / KDE"
    {
        echo "XDG_SESSION_TYPE=${XDG_SESSION_TYPE:-not set} XDG_CURRENT_DESKTOP=${XDG_CURRENT_DESKTOP:-not set}"
        echo "WAYLAND_DISPLAY: $([ -n "${WAYLAND_DISPLAY:-}" ] && echo set || echo not set), DISPLAY: $([ -n "${DISPLAY:-}" ] && echo set || echo not set)"
    } >> "$R/session.txt"
    run session.txt plasmashell --version
    run session.txt kwin_wayland --version
    if command -v qdbus6 > /dev/null 2>&1; then
        run kwin.txt qdbus6 org.kde.KWin /KWin supportInformation
    elif command -v qdbus-qt6 > /dev/null 2>&1; then
        run kwin.txt qdbus-qt6 org.kde.KWin /KWin supportInformation
    elif command -v qdbus > /dev/null 2>&1; then
        run kwin.txt qdbus org.kde.KWin /KWin supportInformation
    else
        run kwin.txt busctl --user call org.kde.KWin /KWin org.kde.KWin supportInformation
    fi
    kwin_stack stacking.txt "1. before the engine is started"
    if start_debug_daemon daemon-wayland.txt wayland; then
        run backend.txt "$CTL" backend
        run status-wayland-start.json "$CTL" status
        [ -f "$R/gpu.json" ] || run gpu.json "$CTL" gpu --json
        try_diag diag-wayland-start.txt
        pause "Put a VIDEO wallpaper on (WalPE window, or: $CTL open <video file>) and wait until it should play"
        run status-before-click.json "$CTL" status
        video_probe "$R/status-before-click.json" "before the click"
        kwin_stack stacking.txt "2. the engine runs, before the click"
        if ask "Is the wallpaper VISIBLE on the desktop now?"; then note "VISIBLE before the click: yes"; else note "VISIBLE before the click: no (or not answered)"; fi
        choose "DESKTOP ICONS before the click" "Do you see your desktop icons over it?  y = yes   n = no, they are hidden   - = I have no desktop icons   [y/n/-]"
        cpu_sample "wallpaper as it is before the click"
        pause "Click ONCE on the empty desktop (where the wallpaper is, or should be). Wait 5 seconds"
        run status-after-click.json "$CTL" status
        kwin_stack stacking.txt "3. after the click on the desktop"
        try_diag diag-after-click.txt
        if ask "Did the wallpaper VANISH with the click (it was there before, it is gone now)?"; then note "VANISHED with the click: yes"; else note "VANISHED with the click: no (or not answered)"; fi
        if ask "Is the wallpaper visible NOW?"; then note "VISIBLE after the click: yes"; else note "VISIBLE after the click: no (or not answered)"; fi
        cpu_sample "after the click"
    fi
    stop_debug_daemon
    CUR=
    if [ -x "$TRAY" ]; then
        # the tray, both orders. A: the tray first, then the engine (the order that failed)
        say "== tray, order A: the tray is started first, then the engine"
        start_tray_logged "order A: tray first, no engine yet"
        tray_menu trayA-menu.txt "order A: tray alone, no engine"
        if start_debug_daemon daemon-tray.txt; then
            CUR=daemon-tray.txt
            run trayA-status.json "$CTL" status
            tray_menu trayA-menu.txt "order A: the engine has come up"
            kwin_stack stacking.txt "4. tray order A, engine up"
            tray_round trayA "order A"
        else
            stop_monitor
            stop_tray
        fi
        # B: the engine first, then the tray
        say "== tray, order B: the engine is started first, then the tray"
        if start_debug_daemon daemon-tray.txt; then
            run trayB-status.json "$CTL" status
            start_tray_logged "order B: engine first, then the tray"
            CUR=daemon-tray.txt
            tray_menu trayB-menu.txt "order B: tray started while the engine runs"
            tray_round trayB "order B"
        fi
    else
        note "tray: $TRAY is not there (built without the tray): skipped"
    fi
    CUR=
fi

# ---- put things back, pack
finish

HOSTN=$( (hostname 2> /dev/null || uname -n) | head -n 1)
USERN=${USER:-$(id -un 2> /dev/null)}
if [ "$REDACT" = 1 ]; then
    # home path, user name, host name: out of every file. A name of fewer than 3 letters is left
    # alone (it would match inside other words); sed's special characters are quoted.
    q () { printf '%s' "$1" | sed 's/[][\\.*^$#+?(){}|]/\\&/g'; }
    _h=$(q "$HOME")
    set -- -e "s#$_h#<home>#g"
    if [ "${#USERN}" -ge 3 ]; then
        _u=$(q "$USERN")     # (twice: two names with one character between them)
        set -- "$@" -e "s#/home/$_u#<home>#g" -e "s#(^|[^A-Za-z0-9_])$_u([^A-Za-z0-9_]|\$)#\\1<user>\\2#g" -e "s#(^|[^A-Za-z0-9_])$_u([^A-Za-z0-9_]|\$)#\\1<user>\\2#g"
    fi
    if [ "${#HOSTN}" -ge 3 ]; then
        _o=$(q "$HOSTN")
        set -- "$@" -e "s#(^|[^A-Za-z0-9_])$_o([^A-Za-z0-9_]|\$)#\\1<host>\\2#g" -e "s#(^|[^A-Za-z0-9_])$_o([^A-Za-z0-9_]|\$)#\\1<host>\\2#g"
    fi
    _nred=0
    for f in "$R"/*.txt "$R"/*.json "$R"/old-daemon.log*; do
        [ -f "$f" ] || continue
        if sed -E "$@" "$f" > "$f.red" 2> /dev/null && mv "$f.red" "$f"; then _nred=$((_nred + 1)); else rm -f "$f.red"; fi
    done
    note "REDACTED: home path -> <home>, user name -> <user>, host name -> <host> in $_nred files (wallpaper names and window classes are still there)"
fi

rm -f "$HOME/walpe-report.tar.gz" "$HOME/walpe-report.zip"
note "packing ($(stamp))"
tar czf "$HOME/walpe-report.tar.gz" -C "$HOME" walpe-report 2> /dev/null
TAR_RC=$?
ZIP_RC=none
if command -v zip > /dev/null 2>&1; then
    ( cd "$HOME" && zip -q -r walpe-report.zip walpe-report ) > /dev/null 2>&1
    ZIP_RC=$?
elif command -v bsdtar > /dev/null 2>&1; then
    bsdtar --format zip -cf "$HOME/walpe-report.zip" -C "$HOME" walpe-report 2> /dev/null
    ZIP_RC=$?
elif command -v python3 > /dev/null 2>&1; then
    ( cd "$HOME" && python3 -m zipfile -c walpe-report.zip walpe-report ) > /dev/null 2>&1
    ZIP_RC=$?
fi
sum_of () {
    if command -v sha256sum > /dev/null 2>&1; then sha256sum "$1" 2> /dev/null | awk '{ print $1 }'; else echo "(no sha256sum)"; fi
}
size_of () { wc -c < "$1" 2> /dev/null | tr -d ' '; }
# the last lines of report.txt (written after packing: the copy inside the archives ends above)
{
    echo "--- written after packing; the report.txt inside the archives ends above this line ---"
    echo "tar: exit status $TAR_RC, walpe-report.tar.gz, $(size_of "$HOME/walpe-report.tar.gz") bytes, sha256 $(sum_of "$HOME/walpe-report.tar.gz")"
    if [ "$ZIP_RC" = none ]; then
        echo "zip: not made (no zip, bsdtar or python3)"
    else
        echo "zip: exit status $ZIP_RC, walpe-report.zip, $(size_of "$HOME/walpe-report.zip") bytes, sha256 $(sum_of "$HOME/walpe-report.zip")"
    fi
} >> "$LOG"

echo
if [ "$TAR_RC" -eq 0 ]; then echo "Done. Send ONE of these (the same content):"; else echo "Done, but tar failed (exit status $TAR_RC):"; fi
[ "$TAR_RC" -ne 0 ] || echo "    $HOME/walpe-report.tar.gz"
[ "$ZIP_RC" != 0 ] || echo "    $HOME/walpe-report.zip     (if .tar.gz does not get through)"
tail -n 2 "$LOG" | sed 's/^/    /'
cat << EOF
(the single files are in $R; report.txt lists what was run, what failed, and your answers)

LOOK BEFORE SENDING - the files contain:
  daemon-*.txt, old-daemon.log*, status*.json,   the full paths of your wallpapers (so your user name
  tray*-status*.json, report.txt, as-found.txt   and folder names), their titles, the folder WalPE is in
  tray*-menu.txt, tray.txt, tray-signals.txt     the tray menu: wallpaper titles and monitor names
  tray*-items.txt                                which programs have a tray icon (their bus names)
  pactl*.txt, wpctl.txt                          your user and host name (wpctl: on every client line),
                                                 your sound devices, and every program that plays
                                                 sound right now with the title of what it plays
  stacking.txt                                   which programs have windows open (their class names,
                                                 sizes and positions - NOT the window titles)
  kwin.txt, kscreen.txt, hardware.txt, gpu.json, GPU, driver, monitors (model names), desktop versions
  session.txt, video-streams.txt, cpu.txt        the codecs of the video wallpaper, CPU use of the engine
No passwords or keys are in any of them. Nothing was uploaded: sending is up to you.
EOF
if [ "$REDACT" = 1 ]; then
    echo "--redact was used: your home path, user name and host name were replaced in the files. Wallpaper"
    echo "names, window class names and device names are still in them."
else
    echo "To have your home path, user name and host name replaced by <home> / <user> / <host> in all"
    echo "files, run the script again with --redact (the answers have to be given again)."
fi
exit 0
