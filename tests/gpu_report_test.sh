#!/bin/sh
# gpu_report_test.sh <walpe-gpu-report.sh> <fake-nvidia-smi>
# scripts/walpe-gpu-report.sh against a stand-in nvidia-smi: all fields,
# a laptop GPU's [N/A] fields and unknown field names, an old driver's
# wording and no pmon, a tool that cannot reach the driver, no tool at
# all, no daemon; then with a stand-in walpe-ctl of version 0.12 (status, no
# profile command). The "daemon" is this shell (its /proc numbers are real).
script=$1
fake=$2
fails=0
tmp=$(mktemp -d "${TMPDIR:-/tmp}/walpe-grt.XXXXXX") || exit 1
trap 'rm -rf "$tmp"' EXIT
mkdir "$tmp/bin" "$tmp/nobin"
cp "$fake" "$tmp/bin/nvidia-smi"
chmod +x "$tmp/bin/nvidia-smi"
# the tools the script needs, without whatever nvidia-smi the machine has
for t in sh awk sed grep cat date sleep head tail sort tr mktemp rm getconf readlink kill id env; do
    p=$(command -v "$t" 2>/dev/null) && ln -s "$p" "$tmp/nobin/$t" 2>/dev/null
done

want () {   # file, fixed string, what
    if ! grep -F -q -- "$2" "$1"; then
        fails=$((fails + 1))
        echo "FAIL: $3: no '$2' in:"
        sed 's/^/    | /' "$1"
    fi
}
wantnot () {   # the same, in the part above the reading notes
    if sed '/^how to read this/,$d' "$1" | grep -F -q -- "$2"; then
        fails=$((fails + 1))
        echo "FAIL: $3: unexpected '$2'"
    fi
}

run () {   # mode, outfile, extra args...
    mode=$1; out=$2; shift 2
    FAKE_SMI=$mode FAKE_SMI_PID=$$ PATH="$tmp/bin:$tmp/nobin" sh "$script" 3 testlabel --pid $$ --no-profile "$@" > "$out" 2> "$out.err"
    rc=$?
    if [ $rc != 0 ]; then
        fails=$((fails + 1))
        echo "FAIL: mode $mode: exit status $rc"
        cat "$out.err"
    fi
    if [ -s "$out.err" ]; then
        fails=$((fails + 1))
        echo "FAIL: mode $mode: something on stderr:"
        cat "$out.err"
    fi
}

# 1. every field supported
run full "$tmp/full.txt"
f=$tmp/full.txt
want "$f" "WalPE GPU report  label=testlabel" "full: header"
want "$f" "GPU 0: NVIDIA GeForce RTX 4050 Laptop GPU, 550.120, 6141 MiB" "full: GPU line"
want "$f" "daemon: walpe-daemon pid $$" "full: daemon line"
want "$f" "utilization.gpu            %            20.0" "full: utilization min"
want "$f" "40.0  " "full: utilization max"
want "$f" "power.draw                 W " "full: power"
want "$f" "clocks.gr                  MHz         600.0     600.0     600.0" "full: clock"
want "$f" "temperature.gpu            C            51.0      51.0      51.0" "full: temperature"
want "$f" "pstate                     state          P5         -        P5" "full: pstate"
want "$f" "(P5 100 %; P0 = full performance)" "full: pstate histogram"
want "$f" "load at full clock (est.)" "full: scaled load"
want "$f" "daemon GPU memory          MiB         400.0" "full: daemon GPU memory from pmon"
want "$f" "daemon GPU share (sm)      %            10.0" "full: daemon GPU share"
wantnot "$f" "counted as 0" "full: no gaps, one average"
want "$f" "programs on the GPU (nvidia-smi pmon" "full: programs on the GPU"
want "$f" "  walpe-daemon         G " "full: the daemon among them"
want "$f" "  Xorg                 G     180.0 MiB   sm   3.0 % /   3.0 %" "full: X among them"
want "$f" "  picom                G      60.0 MiB   sm   8.0 % /   8.0 %" "full: the compositor among them"
want "$f" "daemon CPU                 %" "full: daemon CPU"
want "$f" "daemon RSS                 MiB" "full: daemon RSS"
want "$f" "daemon threads  " "full: daemon thread count"
want "$f" "daemon threads by CPU" "full: per-thread CPU"
want "$f" "(main)" "full: the main thread is marked"
want "$f" "compositor: " "full: compositor line"
want "$f" "P5 100 %" "full: pstate share"
want "$f" "percent of time over the past sample" "full: the utilization note"
want "$f" "power.draw (W) and the clocks" "full: what to compare"
wantnot "$f" "n/a" "full: nothing unsupported"
wantnot "$f" "walpe-ctl profile" "full: --no-profile"
# utilization 20 30 40 20 ...: the average of 3-4 samples lies in between
avg=$(awk '$1 == "utilization.gpu" { print $4 }' "$f")
case "$avg" in 2[5-9].*|3[0-5].*) ;; *) fails=$((fails + 1)); echo "FAIL: full: utilization avg '$avg'" ;; esac

# 2. a laptop GPU: [N/A] fields, an unknown field, pmon without numbers
run laptop "$tmp/laptop.txt"
f=$tmp/laptop.txt
want "$f" "utilization.gpu            %            20.0" "laptop: supported fields still there"
want "$f" "power.draw                 W             n/a       n/a       n/a" "laptop: power n/a"
want "$f" "clocks.gr                  MHz           n/a       n/a       n/a" "laptop: clock n/a"
want "$f" "(not reported by this GPU / driver)" "laptop: why n/a"
want "$f" "pstate                     state          P0         -        P8" "laptop: pstate range"
# 3 s give 4 samples, or 3 on a loaded machine (integration: seen at load 60 on 4 CPUs): the stand-in
# alternates P8, P0, so the shares are 50 / 50 or 33 / 67; the order (P0 first) is what is checked
if ! grep -F -q -- "(P0 50 %, P8 50 %; P0 = full performance)" "$f" &&
   ! grep -F -q -- "(P0 33 %, P8 67 %; P0 = full performance)" "$f"; then
    fails=$((fails + 1))
    echo "FAIL: laptop: pstate shares, in order: neither 50 / 50 nor 33 / 67 in:"
    sed 's/^/    | /' "$f"
fi
want "$f" "P0 = full performance" "laptop: pstate legend"
wantnot "$f" "load at full clock" "laptop: no scaled load without clocks"
wantnot "$f" "clocks.max.gr" "laptop: the helper field is not listed"
want "$f" "daemon GPU memory          MiB             -     377.0" "laptop: memory from the process table"
want "$f" "daemon GPU share (sm)      %             n/a" "laptop: GPU share n/a"
want "$f" "daemon RSS" "laptop: daemon numbers"

# 3. an old driver: other wording, no pmon
run old "$tmp/old.txt"
f=$tmp/old.txt
want "$f" "power.draw                 W             n/a" "old: [Not Supported] is n/a"
want "$f" "pstate                     state         n/a" "old: pstate n/a"
want "$f" "memory.used                MiB         900.0" "old: supported field"
want "$f" "daemon GPU memory          MiB             -     377.0" "old: memory from the process table"

# 4. the tool cannot reach the driver
run broken "$tmp/broken.txt"
f=$tmp/broken.txt
want "$f" "nvidia-smi is installed but does not answer" "broken: said so"
want "$f" "daemon RSS" "broken: daemon numbers still there"
wantnot "$f" "utilization.gpu " "broken: no GPU rows"

# 5. no nvidia-smi at all
PATH="$tmp/nobin" sh "$script" 2 --pid $$ --no-profile > "$tmp/none.txt" 2> "$tmp/none.err"
rc=$?
[ $rc = 0 ] || { fails=$((fails + 1)); echo "FAIL: none: exit status $rc"; cat "$tmp/none.err"; }
f=$tmp/none.txt
want "$f" "nvidia-smi not found: GPU numbers skipped" "none: said so"
want "$f" "daemon CPU" "none: daemon numbers still there"
want "$f" "how to read this" "none: notes"

# 6. no daemon (a pid that does not exist)
FAKE_SMI=full PATH="$tmp/bin:$tmp/nobin" sh "$script" 2 --pid 4194000 --no-profile > "$tmp/nod.txt" 2> "$tmp/nod.err"
rc=$?
[ $rc = 0 ] || { fails=$((fails + 1)); echo "FAIL: no daemon: exit status $rc"; cat "$tmp/nod.err"; }
f=$tmp/nod.txt
want "$f" "no running walpe-daemon found: this is a baseline" "no daemon: said so"
want "$f" "programs on the GPU" "no daemon: programs on the GPU"
want "$f" "utilization.gpu            %" "no daemon: GPU numbers still there"
wantnot "$f" "daemon RSS" "no daemon: no daemon rows"

# 7. odd arguments do not break it
FAKE_SMI=full PATH="$tmp/bin:$tmp/nobin" sh "$script" 0 --gpu x --pid abc --no-profile > "$tmp/odd.txt" 2>&1 &
oddpid=$!
n=0
while kill -0 $oddpid 2>/dev/null && [ $n -lt 8 ]; do sleep 1; n=$((n + 1)); done
if kill -0 $oddpid 2>/dev/null; then
    # seconds "0" fell back to the default 30: fine, but do not wait for it
    kill $oddpid 2>/dev/null
fi
wait $oddpid 2>/dev/null

# 9. pmon without a figure for the daemon every other second: both averages
run gaps "$tmp/gaps.txt"
f=$tmp/gaps.txt
want "$f" "daemon GPU share (sm)      %            10.0      10.0      10.0  2/3  (average of the 2 samples that had a value; 6.7 % with the other 1 counted as 0)" "gaps: both averages and the count"
want "$f" "  walpe-daemon         G " "gaps: the daemon in the program list"
want "$f" "sm  10.0 % /   6.7 %  (2 of 3 samples)" "gaps: both averages in the program list"

# 10. --baseline: the desktop without WalPE - the GPU table, no daemon rows
FAKE_SMI=full FAKE_SMI_PID=$$ PATH="$tmp/bin:$tmp/nobin" sh "$script" 2 idle --baseline > "$tmp/base.txt" 2> "$tmp/base.err"
rc=$?
[ $rc = 0 ] || { fails=$((fails + 1)); echo "FAIL: baseline: exit status $rc"; cat "$tmp/base.err"; }
[ -s "$tmp/base.err" ] && { fails=$((fails + 1)); echo "FAIL: baseline: something on stderr:"; cat "$tmp/base.err"; }
f=$tmp/base.txt
want "$f" "WalPE GPU report  label=idle" "baseline: header"
want "$f" "daemon: not measured (--baseline: the desktop without WalPE)" "baseline: said so"
want "$f" "utilization.gpu            %" "baseline: GPU table"
want "$f" "power.draw                 W " "baseline: power"
want "$f" "programs on the GPU" "baseline: programs on the GPU"
want "$f" "  Xorg                 G     180.0 MiB" "baseline: X in the list"
wantnot "$f" "daemon RSS" "baseline: no daemon rows"
wantnot "$f" "daemon GPU" "baseline: no daemon GPU rows"
wantnot "$f" "daemon threads" "baseline: no thread table"
wantnot "$f" "walpe-ctl profile" "baseline: no profile"

# 8. what the daemon says about its monitors, with a walpe-ctl that has no
#    "profile" command (version 0.12 prints its usage and exits 1)
cat > "$tmp/bin/walpe-ctl" <<'CTL'
#!/bin/sh
case "$1" in
status)
    printf '%s\n' '{"ok":true,"state":{"path":"/home/u/wp/2901582518","type":"scene","paused":false,"fullscreen_pause":false,"monitors_state":"mon0:running mon1:paused,covered","shared_followers":0,"fx_cache":true,"modules":"video","fps":200,"volume":0.5,"backend":"ffmpeg","monitors":[{"index":0,"name":"DP-0","geometry":[0,0,1920,1080],"current":"/home/u/wp/2901582518","paused":false,"refresh":144.0,"type":"scene"},{"index":1,"name":"HDMI-0","geometry":[1920,0,2560,1440],"current":"/home/u/My Videos/clip.mp4","paused":true,"refresh":59.939999999999998,"type":"video"},{"index":2,"name":"mon2","geometry":[4480,0,1920,1080],"current":"/home/u/wp/3292662040/","paused":false,"refresh":0.0,"type":"scene"}],"monitor_count":3,"props_by_path":true,"gl":"gl"}}'
    ;;
*)
    echo "usage:"
    echo "  walpe-ctl open <path>"
    exit 1
    ;;
esac
CTL
chmod +x "$tmp/bin/walpe-ctl"
FAKE_SMI=full FAKE_SMI_PID=$$ PATH="$tmp/bin:$tmp/nobin" sh "$script" 2 old --pid $$ > "$tmp/ctl.txt" 2> "$tmp/ctl.err"
rc=$?
[ $rc = 0 ] || { fails=$((fails + 1)); echo "FAIL: ctl: exit status $rc"; cat "$tmp/ctl.err"; }
[ -s "$tmp/ctl.err" ] && { fails=$((fails + 1)); echo "FAIL: ctl: something on stderr:"; cat "$tmp/ctl.err"; }
f=$tmp/ctl.txt
want "$f" "showing: 2901582518 (local) 3292662040, fps setting 200" "ctl: wallpapers and fps setting"
want "$f" "  monitor 0 (DP-0, 1920x1080, 144.0 Hz): scene 2901582518, paced at up to 144 fps" "ctl: monitor 0"
want "$f" "  monitor 1 (HDMI-0, 2560x1440, 59.9 Hz): video (local), paused" "ctl: monitor 1"
want "$f" "  monitor 2 (mon2, 1920x1080, refresh unknown): scene 3292662040, paced at up to 200 fps" "ctl: monitor 2"
want "$f" "  state: mon0:running mon1:paused,covered" "ctl: pause state"
# the state is asked again while sampling: a paused wallpaper is named
want "$f" "NOTE: a wallpaper was paused in 1 of 1 state checks during the sampling (mon0:running mon1:paused,covered)." "ctl: paused during the sampling"
wantnot "$f" "wallpapers during the sampling: running" "ctl: no all-clear when one was paused"
want "$f" "---- walpe-ctl profile 2 ----" "ctl: profile section"
want "$f" "this build has no GPU profiler (version 0.12 or older): skipped" "ctl: version 0.12 has no profiler"
if grep -F -q -e "/home" -e "usage:" -e "My Videos" "$f"; then
    fails=$((fails + 1))
    echo "FAIL: ctl: a path or walpe-ctl's usage text is in the report"
fi

if [ $fails != 0 ]; then
    echo "$fails check(s) FAILED"
    exit 1
fi
echo "gpu-report: all checks passed"
exit 0
