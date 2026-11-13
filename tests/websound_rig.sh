#!/bin/bash
# tests/websound_rig.sh BUILDDIR [pipewire|pulse] [TMPDIR]
#
# The sound of web wallpapers against a real sound server, end to end: is a
# page heard when it should be, silent when it should be, and is nothing of
# WalPE's level or mute left in the sound server - neither on the page's
# own stream nor in what the server stores for later streams.
#
# NOT part of `meson test`: it needs a sound server of its own and WebKit,
# takes two to three minutes and measures real sound. Run it by hand after
# a change to the sound of web wallpapers (src/engine/web.c "the page's
# sound", gst/walpesound.c, src/daemon/main.c web_env_sound_sink):
#
#     tests/websound_rig.sh build            # PipeWire + WirePlumber + pipewire-pulse
#     tests/websound_rig.sh build pulse      # PulseAudio with module-stream-restore
#
# Everything is private and under one temporary directory: the sound server
# (a null sink, so no sound card is needed and nothing is heard), an Xvfb,
# HOME and the XDG directories, the daemon. Nothing of the user's session is
# contacted or changed. TMPDIR (default /tmp) has to be short: the names of
# unix sockets are limited to 107 bytes.
#
# Needs: Xvfb, pactl, parec, paplay, python3, and pipewire + wireplumber +
# pipewire-pulse + dbus-daemon, or pulseaudio. Exit 77 when one is missing,
# 0 when every check passed, 1 otherwise.
#
# WALPE_WEB_SOUND_SINK=0 in the environment runs the same checks without
# gst/walpesound.c: that shows what the plugin is for (with PipeWire the
# unmute, the stored level and the other player fail).
#
# What is checked, with the level measured on the monitor of the null sink:
#   1  a playing page: mute -> silent, unmute -> back at its level, three times
#   2  started muted, then unmuted
#   3  quit while muted; another program's stream with the same media.role
#      ("music") is at its full level; the next start, unmuted, is heard
#   4  volume 0 -> silent, 0.5 -> half the level; the other program is still
#      at its full level
#   5  wallpaper sound off -> silent, on -> heard
#   6  a page that fights the level (own volume 1, a second Audio from an
#      <iframe>'s prototypes, WebAudio): silent while muted and while off
#   7  WebAudio alone: mute, unmute
# and after each: the page's stream in the server was never muted and never
# away from 100 %, and (WirePlumber 0.4 and 0.5) nothing stored for an output
# stream is muted or below 1.
#
# Run so far: PipeWire 1.0.5 with WirePlumber 0.4.17, PulseAudio 16.1, and
# PipeWire 1.6.11 with WirePlumber 0.5.19 (WebKitGTK 2.54.2, GStreamer
# 1.28.9). In a chroot WebKit's sandbox needs "/" to be a mount point
# (bwrap: "Failed to make / slave"), or no page comes up.
B=$(cd "${1:?usage: websound_rig.sh BUILDDIR [pipewire|pulse] [TMPDIR]}" && pwd) || exit 1
KIND=${2:-pipewire}
BASE=${3:-/tmp}
for t in Xvfb pactl parec paplay python3; do
    command -v $t > /dev/null 2>&1 || { echo "websound-rig: no $t: skipped"; exit 77; }
done
if [ "$KIND" = pipewire ]; then
    for t in pipewire wireplumber pipewire-pulse dbus-daemon; do
        command -v $t > /dev/null 2>&1 || { echo "websound-rig: no $t: skipped"; exit 77; }
    done
else
    command -v pulseaudio > /dev/null 2>&1 || { echo "websound-rig: no pulseaudio: skipped"; exit 77; }
fi
[ -x "$B/walpe-daemon" ] && [ -x "$B/walpe-ctl" ] || { echo "websound-rig: no walpe-daemon / walpe-ctl in $B"; exit 1; }
[ -f "$B/walpe-web.so" ] || { echo "websound-rig: $B has no web module: skipped"; exit 77; }

T=$(mktemp -d "$BASE/wsr.XXXXXX") || exit 1
PIDS=$T/pids
stop_all () {
    [ -f "$PIDS" ] && tac "$PIDS" | while read -r p; do kill "$p" 2> /dev/null; done
    sleep 0.5
    [ -f "$PIDS" ] && tac "$PIDS" | while read -r p; do kill -9 "$p" 2> /dev/null; done
    rm -rf "$T"
}
trap stop_all EXIT
export XDG_RUNTIME_DIR=$T/rt XDG_CONFIG_HOME=$T/cfg XDG_CACHE_HOME=$T/cache XDG_DATA_HOME=$T/data
export XDG_STATE_HOME=$T/state HOME=$T/home
export DBUS_SESSION_BUS_ADDRESS=unix:path=$XDG_RUNTIME_DIR/bus PULSE_SERVER=unix:$XDG_RUNTIME_DIR/pulse/native
export LIBGL_ALWAYS_SOFTWARE=1 NO_AT_BRIDGE=1 GTK_A11Y=none WALPE_BACKEND=x11
unset WAYLAND_DISPLAY PULSE_SINK PULSE_COOKIE PIPEWIRE_REMOTE GST_PLUGIN_PATH GST_PLUGIN_PATH_1_0
mkdir -p -m 700 "$XDG_RUNTIME_DIR"
mkdir -p "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME" "$XDG_STATE_HOME" "$HOME" "$T/log" "$T/w"
bg () { name=$1; shift; setsid "$@" > "$T/log/$name.log" 2>&1 < /dev/null & echo $! >> "$PIDS"; }

# ---- content: a 440 Hz tone for the pages, an 880 Hz one for the other program
python3 -I - "$T/w" << 'EOF'
import json, math, os, struct, sys, wave
w = sys.argv[1]
def tone(path, hz, secs, peak=11585):       # (11585: an RMS of 0.25 of full scale)
    f = wave.open(path, "wb"); f.setnchannels(2); f.setsampwidth(2); f.setframerate(48000)
    n = int(48000 * secs)
    f.writeframes(b"".join(struct.pack("<hh", *(int(peak * math.sin(2 * math.pi * hz * i / 48000)),) * 2) for i in range(n)))
    f.close()
tone(os.path.join(w, "other.wav"), 880, 3)
pages = {
    "tag": '<audio id="a" src="tone.wav" autoplay loop></audio>',
    "osc": "<script>var c=new AudioContext(),o=c.createOscillator(),g=c.createGain();g.gain.value=0.35;"
           "o.frequency.value=440;o.connect(g);g.connect(c.destination);o.start();c.resume();</script>",
    # a page that wants to be heard whatever the user set
    "fight": "<script>var A=new Audio('tone.wav');A.loop=true;var P=null;function attack(){"
             "try{if(!P){var f=document.createElement('iframe');document.body.appendChild(f);P=f.contentWindow;}}catch(e){}"
             "try{A.muted=false;A.volume=1;A.play();}catch(e){}"
             "try{Object.getOwnPropertyDescriptor(P.HTMLMediaElement.prototype,'volume').set.call(A,1);}catch(e){}"
             "try{Object.getOwnPropertyDescriptor(P.HTMLMediaElement.prototype,'muted').set.call(A,false);}catch(e){}"
             "try{P.HTMLMediaElement.prototype.play.call(A);}catch(e){}"
             "try{if(!window.B){window.B=new P.Audio('tone.wav');B.loop=true;}B.volume=1;B.muted=false;B.play();}catch(e){}"
             "try{if(!window.X){var AC=P.AudioContext;window.X=new AC();var o=X.createOscillator(),g=X.createGain();"
             "o.frequency.value=440;g.gain.value=0.15;P.AudioNode.prototype.connect.call(o,g);"
             "P.AudioNode.prototype.connect.call(g,X.destination);o.start();}X.resume();}catch(e){}}"
             "setInterval(attack,300);</script>",
}
for name, body in pages.items():
    d = os.path.join(w, name); os.makedirs(d)
    tone(os.path.join(d, "tone.wav"), 440, 4, 4000 if name == "fight" else 11585)   # (three sources there: no clipping)
    open(os.path.join(d, "index.html"), "w").write(
        '<!doctype html><html><head><meta charset="utf-8"></head><body style="background:#234">' + body + "</body></html>")
    json.dump({"file": "index.html", "title": "websound " + name, "type": "web",
               "general": {"properties": {}}}, open(os.path.join(d, "project.json"), "w"))
EOF
# level.py FILE.raw (s16le 48 kHz stereo) -> four numbers:
#   the level (RMS) of a 440 Hz tone and of an 880 Hz tone in the last second, each the median of
#   twenty 50 ms pieces (a short gap or a jump in the tone's phase does not change it);
#   the share of those pieces in which the 440 Hz tone is there; the RMS of the whole recording
cat > "$T/level.py" << 'EOF'
import math, struct, sys
d = open(sys.argv[1], "rb").read()
n = len(d) // 4
s = struct.unpack("<%dh" % (2 * n), d[:4 * n])
m = [(s[2 * i] + s[2 * i + 1]) / 65536.0 for i in range(n)]
def piece(off, hz):
    re = im = 0.0
    for i in range(2400):
        re += m[off + i] * math.cos(2 * math.pi * hz * i / 48000); im += m[off + i] * math.sin(2 * math.pi * hz * i / 48000)
    return math.hypot(re, im) / 2400 * math.sqrt(2)
offs = [n - 2400 * (k + 1) for k in range(20) if n - 2400 * (k + 1) >= 0]
a = sorted(piece(o, 440) for o in offs); b = sorted(piece(o, 880) for o in offs)
med = lambda v: v[len(v) // 2] if v else 0.0
print("%.4f %.4f %.2f %.4f" % (med(a), med(b), sum(1 for v in a if v > 0.002) / max(1, len(a)),
                               math.sqrt(sum(v * v for v in m) / n) if n else 0.0))
EOF

# ---- the display and the sound server
bg xvfb Xvfb -displayfd 9 -screen 0 1280x720x24 -nolisten tcp -s 0 9> "$T/display"
for i in $(seq 50); do [ -s "$T/display" ] && break; sleep 0.1; done
[ -s "$T/display" ] || { echo "websound-rig: Xvfb did not start"; exit 1; }
export DISPLAY=:$(cat "$T/display")
if [ "$KIND" = pipewire ]; then
    bg dbus dbus-daemon --session --nofork --address="$DBUS_SESSION_BUS_ADDRESS"; sleep 0.5
    bg pipewire pipewire; sleep 0.7
    bg wireplumber wireplumber; sleep 0.7
    bg pipewire-pulse pipewire-pulse; sleep 1
else
    # (stream-restore is what stores stream volumes there; -n: nothing of the system's set-up)
    bg pulseaudio pulseaudio -n --daemonize=no --exit-idle-time=-1 --disable-shm=yes --use-pid-file=no \
        -L "module-native-protocol-unix auth-anonymous=1 socket=$XDG_RUNTIME_DIR/pulse/native" \
        -L module-always-sink -L "module-stream-restore restore_device=false"
    sleep 1
fi
for i in $(seq 30); do pactl info > /dev/null 2>&1 && break; sleep 0.5; done
pactl load-module module-null-sink sink_name=rig rate=48000 channels=2 > /dev/null || { echo "websound-rig: the sound server did not start ($T/log)"; exit 1; }
pactl set-default-sink rig; pactl set-sink-volume rig 100%; pactl set-sink-mute rig 0
echo "websound-rig: $(pactl info | sed -n 's/^Server Name: //p'); WALPE_WEB_SOUND_SINK=${WALPE_WEB_SOUND_SINK:-(not set)}"

# ---- helpers
fail=0; checks=0
check () {      # <0|1> <text>
    checks=$((checks + 1))
    if [ "$1" = 1 ]; then echo "ok   $2"; else echo "FAIL $2"; fail=$((fail + 1)); fi
}
ctl () { "$B/walpe-ctl" "$@" > /dev/null 2>&1; }
DPID=
daemon_start () {       # [setting ...] : a daemon without a past (the sound server keeps its own), sound on at 1
    rm -rf "$XDG_CONFIG_HOME/walpe" "$XDG_CACHE_HOME/walpe"
    ( cd "$T" && exec setsid "$B/walpe-daemon" >> "$T/log/daemon.log" 2>&1 < /dev/null ) &
    DPID=$!; echo $DPID >> "$PIDS"
    for i in $(seq 100); do ctl status && break; sleep 0.1; done
    ctl set scene_sound=on volume=1 mute=off
    [ $# = 0 ] || ctl set "$@"
}
daemon_quit () {
    ctl quit
    for i in $(seq 50); do kill -0 "$DPID" 2> /dev/null || break; sleep 0.1; done
    kill "$DPID" 2> /dev/null
}
open_page () {  # <page> : shown and loaded (the log says so), and two seconds more
    n=$(grep -c "web: loaded $T/w/$1/" "$T/log/daemon.log")
    ctl open "$T/w/$1"
    for i in $(seq 300); do [ "$(grep -c "web: loaded $T/w/$1/" "$T/log/daemon.log")" -gt "$n" ] && break; sleep 0.2; done
    sleep 2
}
level () {      # <seconds> -> 440 Hz and 880 Hz levels of the last second
    timeout -s INT "$1" parec -d rig.monitor --format=s16le --rate=48000 --channels=2 --raw --latency-msec=50 > "$T/c.raw" 2> /dev/null
    python3 -I "$T/level.py" "$T/c.raw"
}
near () { python3 -I -c "import sys; a, b = float(sys.argv[1]), float(sys.argv[2]); print(1 if abs(a - b) <= 0.12 * b + 0.002 else 0)" "$1" "$2"; }
# a level is given up to four recordings of two seconds to be there (a busy machine starts a
# page's sound late); silence has to be there at once
until_ok () {   # <text> <python test on v = the 440 Hz level> <what was expected>
    for try in 1 2 3 4; do
        set -- "$1" "$2" "$3" $(level 2)
        [ "$(python3 -I -c "v = $4; print(1 if $2 else 0)")" = 1 ] && break
    done
    check "$(python3 -I -c "v = $4; print(1 if $2 else 0)")" "$1 ($4, expected $3; recording $try; there in $6 of its last second)"
}
heard () { until_ok "$1: heard at its level" "abs(v - $REF) <= 0.12 * $REF" "$REF"; }
some ()  { until_ok "$1: heard" "v > 0.05" "more than 0.05"; }
half ()  { until_ok "$1: at half its level" "abs(v - 0.5 * $REF) <= 0.06 * $REF" "half of $REF"; }
silent () { set -- "$1" $(level 2); check "$([ "$5" = 0.0000 ] && echo 1 || echo 0)" "$1: silent (RMS of two seconds $5)"; }
# the page's streams as the server has them: never muted, never away from 100 %
streams () {
    bad=$(pactl list sink-inputs | awk '/^Sink Input/ { v = ""; m = "" } /^\tMute:/ { m = $2 } /^\tVolume:/ { v = $5 }
        /application.name = "(WebKit|walpe)/ { if (m != "no" || v != "100%") print "mute " m " volume " v }' | tr '\n' ';')
    check "$([ -z "$bad" ] && echo 1 || echo 0)" "$1: the page's stream in the server is not muted and at 100 % ($bad)"
}
# what the server keeps for later streams, and what a stream of another program gets
stored () {
    sleep 1.5
    if [ "$KIND" = pipewire ]; then
        # (WirePlumber 0.4: restore-stream, "key:mute=true"; 0.5: stream-properties, "key={... "mute":true ...}")
        bad=$(cat "$XDG_STATE_HOME"/wireplumber/restore-stream "$XDG_STATE_HOME"/wireplumber/stream-properties 2> /dev/null |
              grep -E '^Output/' |
              grep -E ':mute=true|:volume=0\.|:channelVolumes=0\.|"mute": *true|"volume": *0\.|"channelVolumes": *\[ *0\.' | tr '\n' ' ')
        check "$([ -z "$bad" ] && echo 1 || echo 0)" "$1: WirePlumber has stored no muted or lowered output stream ($bad)"
    fi
    paplay --property=media.role=music "$T/w/other.wav" &
    pp=$!; sleep 0.7
    set -- "$1" $(level 1.6)
    wait $pp 2> /dev/null
    check "$(near "$3" 0.25)" "$1: another program's stream of the role \"music\" is at its full level ($3 at 880 Hz, expected 0.25)"
}

# ---- 1: a playing page, mute and unmute
daemon_start; open_page tag
REF=0.25; heard "1 a page with an <audio> (the file's tone is 0.25)"
for n in 1 2 3; do
    ctl set mute=on; sleep 1.2; silent "1 mute ($n)"
    ctl set mute=off; sleep 1.8; heard "1 unmute ($n)"
done
streams 1; stored 1
daemon_quit
# ---- 2: started muted
daemon_start mute=on; open_page tag
silent "2 started muted"
ctl set mute=off; sleep 1.8; heard "2 unmuted"
streams 2
# ---- 3: quit while muted, the next start is not
ctl set mute=on; sleep 1.5; silent "3 muted"
daemon_quit; stored "3 after a quit while muted"
daemon_start; open_page tag
heard "3 the next start, not muted"
streams 3
# ---- 4: volume 0, then 0.5
ctl set volume=0; sleep 1.2; silent "4 volume 0"; stored "4 at volume 0"
ctl set volume=0.5; sleep 1.8; half "4 volume 0.5"; streams 4; stored "4 at volume 0.5"
ctl set volume=1; sleep 1.5
# ---- 5: wallpaper sound off and on
ctl set scene_sound=off; sleep 1.2; silent "5 wallpaper sound off"
ctl set scene_sound=on; sleep 1.8; heard "5 wallpaper sound on"
streams 5; stored 5
# ---- 6: a page that fights the level
open_page fight; sleep 2
some "6 the fighting page"
ctl set mute=on; sleep 1.5; silent "6 muted"; sleep 1; silent "6 still muted"
ctl set mute=off; sleep 2
some "6 after the unmute"
ctl set scene_sound=off; sleep 1.5; silent "6 wallpaper sound off"; sleep 1; silent "6 still off"
ctl set scene_sound=on; sleep 2
streams 6; stored 6
# ---- 7: WebAudio alone
open_page osc; sleep 1
some "7 WebAudio"
ctl set mute=on; sleep 1.5; silent "7 muted"
ctl set mute=off; sleep 2
some "7 after the unmute"
streams 7; stored 7
daemon_quit
grep -hE "web: (the level and the mute|libgstwalpesound)" "$T/log/daemon.log" | sort -u | sed 's/^.*web: /     daemon: web: /' | cut -c1-200
echo "websound-rig ($KIND): $((checks - fail)) of $checks checks passed"
[ $fail = 0 ]
