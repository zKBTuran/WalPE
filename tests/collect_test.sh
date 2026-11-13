#!/bin/sh
# collect_test.sh <scripts/walpe-collect.sh>
# The collector for bug reports against stand-ins for walpe-daemon and walpe-ctl, in a
# home directory of its own. No engine is needed, none that runs is touched (the
# stand-in pkill and systemctl do nothing), no sound server is asked.
#   a video wallpaper on an engine that plays the sound track of videos (it answers
#   `walpe-ctl soundcheck`): the video is the wallpaper of the sound test, and nobody
#   is told that video wallpapers have no sound - although the engine calls itself
#   0.14.0, as the builds after 0.14.0 do;
#   the same wallpaper on an engine that does not know soundcheck (0.14.0): the
#   sentence is said, and a scene is asked for.
#   which engine is looked at: the version and the status member "source_id", or
#   "not told" of an engine without the member;
#   a web wallpaper whose sound sentence has quotation marks in it (\" in the status
#   reply): the tester reads the whole sentence, and report.txt has it whole;
#   a video wallpaper that is a folder whose file has a quotation mark in its name
#   (project.json "file", the path with \/ in the reply): the file is found.
# The stand-ins: FAKE_SOUNDCHECK=yes|no (the engine knows the command or not),
# FAKE_STATUS=<file> (the reply to `walpe-ctl status`).
script=$1
fails=0
tmp=$(mktemp -d "${TMPDIR:-/tmp}/walpe-ct.XXXXXX") || exit 1
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/bin" "$tmp/build" "$tmp/state"

cat > "$tmp/build/walpe-daemon" <<'EOF'
#!/bin/sh
echo $$ > "$FAKE_STATE/pid"
echo "** Message: 00:00:00.000: walpe-daemon 0.14.0 \"Stand-in\" up (pid $$, backend=x11)"
exec sleep 300
EOF
cat > "$tmp/build/walpe-ctl" <<'EOF'
#!/bin/sh
pid=$(cat "$FAKE_STATE/pid" 2> /dev/null)
if [ -z "$pid" ] || ! kill -0 "$pid" 2> /dev/null; then
    echo "walpe-daemon is not running" >&2
    exit 2
fi
case "$1" in
status) cat "$FAKE_STATUS" ;;
quit) kill "$pid"; rm -f "$FAKE_STATE/pid"; echo '{"ok":true}' ;;
soundcheck)
    if [ "${FAKE_SOUNDCHECK:-no}" = yes ]; then
        echo '{"ok":true,"sound":"nothing is playing","sound_server":"not asked"}'
    else
        echo '{"ok":false,"error":"unknown cmd"}'
        exit 3
    fi ;;
gpu) echo '{"ok":true}' ;;
set) echo "$*" >> "$FAKE_STATE/set"; echo '{"ok":true}' ;;
*) echo '{"ok":false,"error":"unknown cmd"}'; exit 3 ;;
esac
EOF
# nothing of the machine is asked or stopped: every tool the script may call is a stand-in
for t in pkill systemctl pactl wpctl ffprobe nvidia-smi kscreen-doctor busctl dbus-monitor; do
    printf '#!/bin/sh\nexit 1\n' > "$tmp/bin/$t"
done
chmod +x "$tmp/build/walpe-daemon" "$tmp/build/walpe-ctl" "$tmp/bin/"*

status_video='{"ok":true,"state":{"path":"\/w\/clip.mp4","type":"video","source_id":"077a0dd","volume":0.5,"scene_sound":true,"mute":false,"sound":"playing 1 sound","monitors":[{"index":0,"type":"video"}]}}'

# collect NAME STATUS-JSON INPUT [VAR=value ...]: one run of --sound-only; what it printed
# is in $tmp/NAME.out, its report in $tmp/NAME.home/walpe-report
collect () {
    _name=$1
    _json=$2
    _in=$3
    shift 3
    mkdir -p "$tmp/$_name.home"
    printf '%s\n' "$_json" > "$tmp/$_name.json"
    printf '%b' "$_in" | env "$@" HOME="$tmp/$_name.home" PATH="$tmp/bin:$PATH" WALPE_BUILD="$tmp/build" \
        FAKE_STATE="$tmp/state" FAKE_STATUS="$tmp/$_name.json" XDG_SESSION_TYPE=x11 \
        sh "$script" --sound-only > "$tmp/$_name.out" 2>&1
    _rc=$?
    if [ "$_rc" -ne 0 ]; then
        echo "FAIL: $_name: exit status $_rc"
        sed 's/^/    /' "$tmp/$_name.out"
        fails=$((fails + 1))
    fi
    if [ -f "$tmp/state/pid" ]; then
        echo "FAIL: $_name: the stand-in engine was left running"
        kill "$(cat "$tmp/state/pid")" 2> /dev/null
        rm -f "$tmp/state/pid"
        fails=$((fails + 1))
    fi
}
# has NAME TEXT / hasnot NAME TEXT: in what the run printed
has () {
    grep -q -F -e "$2" "$tmp/$1.out" && return 0
    echo "FAIL: $1: the output lacks: $2"
    sed 's/^/    /' "$tmp/$1.out"
    fails=$((fails + 1))
}
hasnot () {
    grep -q -F -e "$2" "$tmp/$1.out" || return 0
    echo "FAIL: $1: the output has: $2"
    fails=$((fails + 1))
}
# noted NAME TEXT: in the run's report.txt
noted () {
    grep -q -F -e "$2" "$tmp/$1.home/walpe-report/report.txt" && return 0
    echo "FAIL: $1: report.txt lacks: $2"
    fails=$((fails + 1))
}

# ---- a video on an engine with video sound: tested as it is, the sentence is not said
collect vsound "$status_video" '\n' FAKE_SOUNDCHECK=yes
hasnot vsound "video wallpapers have no sound at all"
hasnot vsound "The test needs a SCENE wallpaper"
hasnot vsound "Put a SCENE wallpaper with sound on"
has vsound "The engine says about sound: playing 1 sound"
noted vsound "the engine plays the sound track of video wallpapers: yes"
noted vsound "the wallpaper for the sound test is shown (type video)"
# ... and which engine it is: its version and the commit of its source, as the engine says
has vsound "the engine looked at: WalPE 0.14.0, source 077a0dd"
noted vsound "# the engine looked at: WalPE 0.14.0, source 077a0dd"
[ -f "$tmp/vsound.home/walpe-report/soundcheck.txt" ] || { echo "FAIL: vsound: no soundcheck.txt in the report"; fails=$((fails + 1)); }

# ---- the same video on an engine without (0.14.0): said, and a scene is asked for
collect novsound "$status_video" '\n' FAKE_SOUNDCHECK=no
has novsound "In WalPE 0.14.0 video wallpapers have no sound at all"
has novsound "The test needs a SCENE wallpaper"
has novsound "Put a SCENE wallpaper with sound on"
noted novsound "the engine plays the sound track of video wallpapers: no"
[ ! -e "$tmp/novsound.home/walpe-report/soundcheck.txt" ] || { echo "FAIL: novsound: a soundcheck.txt of an engine without the command"; fails=$((fails + 1)); }

# ---- a web wallpaper: the sentence has \" in the reply and is printed whole, unescaped
status_web='{"ok":true,"state":{"path":"\/w\/page","type":"web","volume":0.5,"scene_sound":true,"mute":false,"sound":"1 web wallpaper is heard at 50 % (what the page plays, through WebKit: the stream \"WebKit\" in the system\u0027s mixer); a back\\slash","monitors":[{"index":0,"type":"web"}]}}'
collect web "$status_web" '\ny\nn\n' FAKE_SOUNDCHECK=yes
has web 'The engine says about sound: 1 web wallpaper is heard at 50 % (what the page plays, through WebKit: the stream "WebKit" in the system\u0027s mixer); a back\slash'
has web "the engine looked at: WalPE 0.14.0, source not told"
noted web 'the engine says about sound after the set: 1 web wallpaper is heard at 50 % (what the page plays, through WebKit: the stream "WebKit" in the system\u0027s mixer); a back\slash'

# ---- a video in a folder: project.json names the file, with a quotation mark in its name
mkdir -p "$tmp/w p"
printf '{\n  "title" : "t",\n  "file" : "cl\\"ip.mp4",\n  "type" : "video"\n}\n' > "$tmp/w p/project.json"
: > "$tmp/w p/cl\"ip.mp4"
esc=$(printf '%s' "$tmp/w p" | sed 's#/#\\/#g')
collect folder '{"ok":true,"state":{"path":"'"$esc"'","type":"video","volume":0.5,"scene_sound":true,"mute":false,"sound":"nothing is playing","monitors":[]}}' '\n' FAKE_SOUNDCHECK=yes
noted folder "video-streams.txt (sound test): ffprobe exit status"

if [ "$fails" -eq 0 ]; then
    echo "collect_test: all passed"
    exit 0
fi
echo "collect_test: $fails failed"
exit 1
