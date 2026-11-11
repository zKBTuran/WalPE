#!/bin/sh
# walpe-gpu-report.sh - GPU numbers of a running walpe-daemon, for sending.
#
#   walpe-gpu-report.sh [SECONDS] [LABEL] [--pid PID] [--gpu N] [--no-profile]
#   walpe-gpu-report.sh [SECONDS] [LABEL] --baseline     (WalPE closed)
#
# Samples once a second for SECONDS (default 30):
#   - nvidia-smi: utilization.gpu, utilization.memory, memory.used, power.draw,
#     clocks.gr, clocks.mem, temperature.gpu, pstate (a field this GPU, driver
#     or nvidia-smi does not have is reported as n/a, the rest still works);
#   - nvidia-smi pmon: the daemon's own GPU memory and GPU share;
#   - /proc: the daemon's CPU %, resident memory and thread count, and which
#     of its threads used the CPU (decode, render, ...);
# and prints min / avg / max, with what `walpe-ctl status` says about the
# monitors (refresh rate, wallpaper, paused / covered) and whether a
# compositor runs. If the running daemon has the GPU profiler
# (walpe-ctl profile), its report is appended, measured after the sampling
# so that it does not disturb it. Works with any walpe build, version 0.12
# included (which has no profiler: that part is then skipped).
#
# --baseline (or simply no walpe-daemon running) measures the desktop
# without WalPE with the same GPU table: what X and the compositor cost.
#
# LABEL is a word for the header (old, new, ...). The output holds no file
# paths and nothing of the wallpapers except workshop ids and, in the
# profiler part, layer and effect names; of other programs only the names
# of those that use the GPU. POSIX sh + awk only.

secs=30
label=
pid=
gpu=0
profile=1
baseline=0
while [ $# -gt 0 ]; do
    case "$1" in
    --pid) pid=$2; shift ;;
    --pid=*) pid=${1#--pid=} ;;
    --gpu) gpu=$2; shift ;;
    --gpu=*) gpu=${1#--gpu=} ;;
    --no-profile) profile=0 ;;
    --baseline) baseline=1 ;;
    -h|--help) sed -n '2,28p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *[!0-9]*|'') [ -z "$label" ] && label=$1 ;;
    *) secs=$1 ;;
    esac
    shift
done
[ "$secs" -ge 1 ] 2>/dev/null || secs=30
[ "$secs" -le 3600 ] || secs=3600
case "$gpu" in *[!0-9]*|'') gpu=0 ;; esac
case "$pid" in *[!0-9]*) pid= ;; esac

tmp=$(mktemp -d "${TMPDIR:-/tmp}/walpe-gpu-report.XXXXXX") || exit 1
smi_pid=
pmon_pid=
cleanup () {
    [ -n "$smi_pid" ] && kill "$smi_pid" 2>/dev/null
    [ -n "$pmon_pid" ] && kill "$pmon_pid" 2>/dev/null
    rm -rf "$tmp"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

# ---- the daemon ----------------------------------------------------------
running=
if [ "$baseline" = 1 ]; then
    # the desktop alone is measured; a daemon that still runs is only named
    if command -v pgrep >/dev/null 2>&1; then
        running=$(pgrep -x walpe-daemon 2>/dev/null | head -n 1)
    fi
    pid=
    profile=0
elif [ -z "$pid" ]; then
    if command -v pgrep >/dev/null 2>&1; then
        pid=$(pgrep -x walpe-daemon 2>/dev/null | head -n 1)
    fi
    if [ -z "$pid" ]; then
        for d in /proc/[0-9]*; do
            if [ "$(cat "$d/comm" 2>/dev/null)" = "walpe-daemon" ]; then
                pid=${d#/proc/}
                break
            fi
        done
    fi
fi
[ -n "$pid" ] && [ ! -r "/proc/$pid/stat" ] && pid=

# walpe-ctl: next to the daemon that runs, else from PATH
ctl=
if [ -n "$pid" ]; then
    exe=$(readlink "/proc/$pid/exe" 2>/dev/null)
    [ -n "$exe" ] && [ -x "${exe%/*}/walpe-ctl" ] && ctl="${exe%/*}/walpe-ctl"
fi
[ -z "$ctl" ] && ctl=$(command -v walpe-ctl 2>/dev/null)

tck=$(getconf CLK_TCK 2>/dev/null)
[ "$tck" -gt 0 ] 2>/dev/null || tck=100

# ---- header --------------------------------------------------------------
printf 'WalPE GPU report  %s%s  %s s\n' "${label:+label=$label  }" "$(date '+%Y-%m-%d %H:%M:%S')" "$secs"

have_smi=0
if command -v nvidia-smi >/dev/null 2>&1; then
    if id_line=$(nvidia-smi -i "$gpu" --query-gpu=name,driver_version,memory.total --format=csv,noheader 2>/dev/null) && [ -n "$id_line" ]; then
        have_smi=1
        printf 'GPU %s: %s\n' "$gpu" "$id_line"
    else
        printf 'GPU: nvidia-smi is installed but does not answer for GPU %s (driver not loaded?): GPU numbers skipped\n' "$gpu"
    fi
else
    printf 'GPU: nvidia-smi not found: GPU numbers skipped (daemon numbers below still valid)\n'
fi

# mains power matters on a laptop: the GPU clocks down on battery
for ps in /sys/class/power_supply/*; do
    [ -r "$ps/type" ] || continue
    if [ "$(cat "$ps/type" 2>/dev/null)" = "Mains" ]; then
        case "$(cat "$ps/online" 2>/dev/null)" in
        1) printf 'power: on mains\n' ;;
        0) printf 'power: on battery (the GPU clocks down: compare runs on the same power source)\n' ;;
        esac
        break
    fi
done

if [ -n "$pid" ]; then
    printf 'daemon: walpe-daemon pid %s' "$pid"
    if [ -n "$ctl" ] && st=$("$ctl" status 2>/dev/null); then
        # workshop ids of what the monitors show (an all-digit folder name), fps setting
        ids=$(printf '%s' "$st" | tr ',{}' '\n\n\n' | sed -n 's/^ *"current" *: *"\(.*\)" *$/\1/p' |
              sed 's:/*$::; s:.*/::' | awk '{ if ($0 ~ /^[0-9]+$/) printf "%s ", $0; else printf "(local) " }')
        fps=$(printf '%s' "$st" | tr ',{}' '\n\n\n' | sed -n 's/^ *"fps" *: *\([0-9][0-9]*\).*$/\1/p' | head -n 1)
        [ -n "$ids" ] && printf ', showing: %s' "${ids% }"
        [ -n "$fps" ] && printf ', fps setting %s' "$fps"
        printf '\n'
        # one line per monitor: the frame rate a scene is paced at is the
        # fps setting, but no more than the monitor's refresh rate
        printf '%s' "$st" | sed 's/.*"monitors" *: *\[//' | tr ',{}' '\n\n\n' | awk -v fps="${fps:-0}" '
            function flush() {
                if (idx == "") return
                printf "  monitor %s", idx
                if (name != "") printf " (%s", name; else printf " (?"
                if (w != "") printf ", %sx%s", w, h
                if (hz + 0 >= 20) printf ", %.1f Hz", hz; else printf ", refresh unknown"
                printf "): %s %s%s", (type == "" ? "?" : type), wp, (paused == "true" ? ", paused" : "")
                if (follows != "") printf ", shows monitor %s\047s frame", follows
                else if (fps + 0 > 0 && type == "scene") {
                    r = (hz + 0 >= 20) ? int(hz + 0.995) : fps + 0
                    printf ", paced at up to %d fps", (fps + 0 < r ? fps + 0 : r)
                }
                printf "\n"
                idx = name = w = h = hz = type = wp = paused = follows = ""
            }
            /^ *"index" *:/ { flush(); v = $0; sub(/.*: */, "", v); idx = v; g = 0; next }
            /^ *"name" *:/ { v = $0; sub(/^[^:]*: *"/, "", v); sub(/" *$/, "", v); name = v; next }
            /^ *"geometry" *:/ { g = 1; next }
            g == 1 { g = 2; next }
            g == 2 { v = $0; gsub(/[^0-9]/, "", v); w = v; g = 3; next }
            g == 3 { v = $0; gsub(/[^0-9]/, "", v); h = v; g = 0; next }
            /^ *"current" *:/ { v = $0; sub(/^[^:]*: *"/, "", v); sub(/" *$/, "", v); sub(/\/*$/, "", v); sub(/.*\//, "", v)
                                wp = (v ~ /^[0-9]+$/) ? v : (v == "" ? "(none)" : "(local)"); next }
            /^ *"refresh" *:/ { v = $0; sub(/.*: */, "", v); hz = v; next }
            /^ *"type" *:/ { v = $0; sub(/^[^:]*: *"/, "", v); sub(/" *$/, "", v); type = v; next }
            /^ *"paused" *:/ { v = $0; sub(/.*: */, "", v); paused = v; next }
            /^ *"follows" *:/ { v = $0; sub(/.*: */, "", v); follows = v; next }
            /^ *\] *$/ || /"monitor_count"/ { flush(); exit }
            END { flush() }'
        ms=$(printf '%s' "$st" | sed -n 's/.*"monitors_state" *: *"\([^"]*\)".*/\1/p')
        [ -n "$ms" ] && printf '  state: %s\n' "$ms"
    else
        printf '\n'
    fi
elif [ "$baseline" = 1 ]; then
    printf 'daemon: not measured (--baseline: the desktop without WalPE)\n'
    [ -n "$running" ] && printf 'NOTE: a walpe-daemon is running (pid %s): quit it (walpe-ctl quit) for a real baseline\n' "$running"
else
    printf 'daemon: no running walpe-daemon found: this is a baseline of the desktop without WalPE (start the daemon, or give --pid, for its numbers)\n'
fi

# a compositor redraws the screen under the wallpaper windows: say if one runs
comp=
for d in /proc/[0-9]*; do
    { read -r c < "$d/comm"; } 2>/dev/null || continue
    case "$c" in
    picom|compton|xcompmgr|kwin_x11|mutter|gnome-shell|xfwm4)
        case " $comp " in *" $c "*) ;; *) comp="${comp:+$comp }$c" ;; esac ;;
    esac
done
printf 'compositor: %s\n' "${comp:-none found}"

# CPU time of every thread of the daemon: "<tid> <ticks> <name>" per line
threads_snapshot () {
    for t in "/proc/$pid/task"/[0-9]*; do
        line=$(cat "$t/stat" 2>/dev/null) || continue
        [ -n "$line" ] || continue
        nm=${line#*(}
        nm=${nm%)*}
        rest=${line##*) }
        # shellcheck disable=SC2086
        set -- $rest
        echo "${t##*/} $(( ${12} + ${13} )) $nm"
    done
}

# ---- which nvidia-smi fields exist here ----------------------------------
fields=
unknown=
if [ "$have_smi" = 1 ]; then
    for f in utilization.gpu utilization.memory memory.used power.draw clocks.gr clocks.mem temperature.gpu pstate clocks.max.gr; do
        if nvidia-smi -i "$gpu" --query-gpu="$f" --format=csv,noheader,nounits >/dev/null 2>&1; then
            fields="${fields:+$fields,}$f"
        else
            unknown="${unknown:+$unknown }$f"
        fi
    done
fi

# ---- sample --------------------------------------------------------------
if [ -n "$fields" ]; then
    nvidia-smi -i "$gpu" --query-gpu="$fields" --format=csv,noheader,nounits -l 1 > "$tmp/gpu.csv" 2>/dev/null &
    smi_pid=$!
fi
if [ "$have_smi" = 1 ]; then
    nvidia-smi pmon -i "$gpu" -c "$secs" -d 1 -s um > "$tmp/pmon.txt" 2>/dev/null &
    pmon_pid=$!
fi
: > "$tmp/proc.txt"
if [ -n "$pid" ]; then
    threads_snapshot > "$tmp/thr0.txt"
    read -r thr_t0 _ < /proc/uptime
fi
i=0
while [ "$i" -le "$secs" ]; do
    if [ -n "$pid" ] && st=$(cat "/proc/$pid/stat" 2>/dev/null) && [ -n "$st" ]; then
        st=${st##*) }            # behind "pid (comm) ": state ppid ... utime stime
        # shellcheck disable=SC2086
        set -- $st
        read -r up _ < /proc/uptime
        rss=$(awk '/^VmRSS:/ { print $2 }' "/proc/$pid/status" 2>/dev/null)
        thr=$(awk '/^Threads:/ { print $2 }' "/proc/$pid/status" 2>/dev/null)
        echo "$up $(( ${12} + ${13} )) ${rss:-0} ${thr:-0}" >> "$tmp/proc.txt"
    fi
    # every 5 s: are the wallpapers running? A fullscreen window or
    # a covering window pauses them, and a sampling that is partly paused
    # mixes two states (the first the earlier build report had that in its profile part)
    if [ -n "$pid" ] && [ -n "$ctl" ] && [ $((i % 5)) = 0 ]; then
        ms2=$("$ctl" status 2>/dev/null | sed -n 's/.*"monitors_state" *: *"\([^"]*\)".*/\1/p')
        [ -n "$ms2" ] && printf '%s\n' "$ms2" >> "$tmp/states.txt"
    fi
    [ "$i" -lt "$secs" ] && sleep 1
    i=$((i + 1))
done
if [ -n "$pid" ] && [ -d "/proc/$pid/task" ]; then
    threads_snapshot > "$tmp/thr1.txt"
    read -r thr_t1 _ < /proc/uptime
fi
if [ -n "$smi_pid" ]; then
    kill "$smi_pid" 2>/dev/null
    wait "$smi_pid" 2>/dev/null
    smi_pid=
fi
if [ -n "$pmon_pid" ]; then
    # it ends by itself after SECONDS samples; do not wait long for it
    n=0
    while kill -0 "$pmon_pid" 2>/dev/null && [ "$n" -lt 3 ]; do sleep 1; n=$((n + 1)); done
    kill "$pmon_pid" 2>/dev/null
    wait "$pmon_pid" 2>/dev/null
    pmon_pid=
fi

# ---- statistics ----------------------------------------------------------
printf '\n%-26s %-7s %9s %9s %9s  %s\n' metric unit min avg max samples

if [ -n "$fields" ]; then
    awk -F', *' -v fields="$fields" '
    function isnum(v) { return v ~ /^-?[0-9]+(\.[0-9]+)?$/ }
    BEGIN {
        nf = split(fields, name, ",")
        unit["utilization.gpu"] = "%"; unit["utilization.memory"] = "%"; unit["memory.used"] = "MiB"
        unit["power.draw"] = "W"; unit["clocks.gr"] = "MHz"; unit["clocks.mem"] = "MHz"
        unit["temperature.gpu"] = "C"; unit["pstate"] = "state"; unit["clocks.max.gr"] = "MHz"
        for (j = 1; j <= nf; j++) col[name[j]] = j
    }
    NF >= 1 && $0 !~ /^[ \t]*$/ {
        rows++
        for (j = 1; j <= nf; j++) {
            v = $j
            gsub(/^[ \t]+|[ \t\r]+$/, "", v)
            if (name[j] == "pstate") { if (v ~ /^P[0-9]+$/) { ps[v]++; sub(/^P/, "", v) } }
            if (isnum(v)) {
                v += 0
                if (!(j in n)) { mn[j] = v; mx[j] = v }
                if (v < mn[j]) mn[j] = v
                if (v > mx[j]) mx[j] = v
                sum[j] += v; n[j]++
                val[j] = v; ok[j] = 1
            } else ok[j] = 0
        }
        # load scaled to the full clock, when this GPU reports its clocks
        u = col["utilization.gpu"]; c = col["clocks.gr"]; m = col["clocks.max.gr"]
        if (u && c && m && ok[u] && ok[c] && ok[m] && val[m] > 0) { scaled += val[u] * val[c] / val[m]; nscaled++ }
    }
    END {
        for (j = 1; j <= nf; j++) {
            if (name[j] == "clocks.max.gr") continue
            if (n[j] > 0) {
                if (name[j] == "pstate") {
                    hist = ""
                    for (q = 0; q <= 15; q++) if (("P" q) in ps) hist = hist sprintf("%sP%d %.0f %%", hist == "" ? "" : ", ", q, 100 * ps["P" q] / n[j])
                    printf "%-26s %-7s %9s %9s %9s  %d/%d  (%s; P0 = full performance)\n", name[j], unit[name[j]], "P" mn[j], "-", "P" mx[j], n[j], rows, hist
                } else
                    printf "%-26s %-7s %9.1f %9.1f %9.1f  %d/%d\n", name[j], unit[name[j]], mn[j], sum[j] / n[j], mx[j], n[j], rows
            } else
                printf "%-26s %-7s %9s %9s %9s  0/%d  (not reported by this GPU / driver)\n", name[j], unit[name[j]], "n/a", "n/a", "n/a", rows
        }
        if (nscaled > 0)
            printf "%-26s %-7s %9s %9.1f %9s  %d/%d  (utilization.gpu x clocks.gr / clocks.max.gr: rough)\n", "load at full clock (est.)", "%", "-", scaled / nscaled, "-", nscaled, rows
        if (rows == 0) print "(nvidia-smi gave no samples)"
    }' "$tmp/gpu.csv"
fi
for f in $unknown; do
    [ "$f" = clocks.max.gr ] && continue
    printf '%-26s %-7s %9s %9s %9s  -  (this nvidia-smi does not know the field)\n' "$f" "" n/a n/a n/a
done

# the daemon on the GPU: nvidia-smi pmon (fb = its GPU memory, sm = its GPU
# share). pmon prints "-" in a sample for which the driver has no figure for
# the process: the average is given both over the samples that had a value
# and with the others counted as 0 - the truth lies between the two.
if [ "$have_smi" = 1 ] && [ -n "$pid" ]; then
    got=$(awk -v pid="$pid" '
    function isnum(v) { return v ~ /^[0-9]+(\.[0-9]+)?$/ }
    /^#[ \t]*gpu/ { for (i = 2; i <= NF; i++) col[$i] = i - 1; next }
    /^#/ { next }
    col["pid"] && $col["pid"] == pid {
        rows++
        if (col["fb"] && isnum($col["fb"])) { v = $col["fb"] + 0; if (!nf++) { fmn = v; fmx = v }; if (v < fmn) fmn = v; if (v > fmx) fmx = v; fs += v }
        if (col["sm"] && isnum($col["sm"])) { v = $col["sm"] + 0; if (!ns++) { smn = v; smx = v }; if (v < smn) smn = v; if (v > smx) smx = v; ss += v }
    }
    END {
        if (nf) printf "%-26s %-7s %9.1f %9.1f %9.1f  %d/%d\n", "daemon GPU memory", "MiB", fmn, fs / nf, fmx, nf, rows
        if (ns && ns == rows) printf "%-26s %-7s %9.1f %9.1f %9.1f  %d/%d\n", "daemon GPU share (sm)", "%", smn, ss / ns, smx, ns, rows
        else if (ns) printf "%-26s %-7s %9.1f %9.1f %9.1f  %d/%d  (average of the %d samples that had a value; %.1f %% with the other %d counted as 0)\n", "daemon GPU share (sm)", "%", smn, ss / ns, smx, ns, rows, ns, ss / rows, rows - ns
        else if (rows) printf "%-26s %-7s %9s %9s %9s  0/%d  (not reported by this GPU / driver)\n", "daemon GPU share (sm)", "%", "n/a", "n/a", "n/a", rows
        print "PMON " (nf ? 1 : 0)
    }' "$tmp/pmon.txt" 2>/dev/null)
    printf '%s\n' "$got" | grep -v '^PMON ' | grep .
    case "$got" in
    *"PMON 1"*) ;;
    *)
        # no pmon here: the process list of the plain table has the memory
        mem=$(nvidia-smi 2>/dev/null | awk -v pid="$pid" '
            /MiB *\|? *$/ { for (i = 1; i <= NF; i++) if ($i == pid) { for (k = NF; k > i; k--) if ($k ~ /^[0-9]+MiB$/) { sub(/MiB/, "", $k); print $k; exit } } }')
        if [ -n "$mem" ]; then
            printf '%-26s %-7s %9s %9.1f %9s  1  (from the process table, at the end)\n' "daemon GPU memory" MiB - "$mem" -
        else
            printf '%-26s %-7s %9s %9s %9s  -  (nvidia-smi does not list the process)\n' "daemon GPU memory" MiB n/a n/a n/a
        fi
        ;;
    esac
fi

# the daemon on the CPU
if [ -s "$tmp/proc.txt" ]; then
    awk -v tck="$tck" '
    {
        t[NR] = $1; c[NR] = $2
        r = $3 / 1024
        if (NR == 1) { rmn = r; rmx = r; tmn = $4; tmx = $4 }
        if (r < rmn) rmn = r; if (r > rmx) rmx = r; rs += r
        if ($4 < tmn) tmn = $4; if ($4 > tmx) tmx = $4; ts += $4
        if (NR > 1 && t[NR] > t[NR - 1]) {
            p = (c[NR] - c[NR - 1]) / tck / (t[NR] - t[NR - 1]) * 100
            if (!np++) { pmn = p; pmx = p }
            if (p < pmn) pmn = p; if (p > pmx) pmx = p
        }
    }
    END {
        if (NR > 1 && t[NR] > t[1])
            printf "%-26s %-7s %9.1f %9.1f %9.1f  %d  (100 %% = one core)\n", "daemon CPU", "%", pmn, (c[NR] - c[1]) / tck / (t[NR] - t[1]) * 100, pmx, np
        printf "%-26s %-7s %9.1f %9.1f %9.1f  %d\n", "daemon RSS", "MiB", rmn, rs / NR, rmx, NR
        printf "%-26s %-7s %9d %9.1f %9d  %d\n", "daemon threads", "", tmn, ts / NR, tmx, NR
    }' "$tmp/proc.txt"
elif [ -n "$pid" ]; then
    printf '(the daemon went away during the sampling)\n'
fi

# which threads of the daemon used the CPU (video decode: "av:..." and
# "walpe-video"; a monitor's render thread: "walpe-monN", in builds that do
# not name it "walpe-daemon"; the main thread has the daemon's pid)
if [ -s "$tmp/thr0.txt" ] && [ -s "$tmp/thr1.txt" ]; then
    printf '\ndaemon threads by CPU (%% of one core over the run; tid in brackets)\n'
    awk -v tck="$tck" -v t0="$thr_t0" -v t1="$thr_t1" -v pid="$pid" '
        NR == FNR { a[$1] = $2; next }
        {
            tid = $1; d = $2 - (tid in a ? a[tid] : 0)
            nm = $0; sub(/^[0-9]+ [0-9]+ /, "", nm)
            if (tid == pid) nm = nm " (main)"
            if (t1 > t0) printf "%.1f\t%s\t%s\n", d / tck / (t1 - t0) * 100, nm, tid
        }' "$tmp/thr0.txt" "$tmp/thr1.txt" | sort -t "$(printf '\t')" -k1,1 -rn | head -n 8 |
        awk -F '\t' '{ printf "  %-22s %6.1f %%  [%s]\n", $2, $1, $3; s += $1 } END { if (NR) printf "  %-22s %6.1f %%\n", "(these " NR " together)", s }'
fi

# the wallpapers' state during the sampling
if [ -s "$tmp/states.txt" ]; then
    nt=$(grep -c . "$tmp/states.txt")
    np=$(grep -c 'paused' "$tmp/states.txt")
    if [ "$np" -gt 0 ]; then
        printf '\nNOTE: a wallpaper was paused in %s of %s state checks during the sampling (%s).\n' "$np" "$nt" "$(sort -u "$tmp/states.txt" | tr '\n' ';' | sed 's/;$//; s/;/; /g')"
        printf '      The numbers above then mix running and paused time: run it again with every wallpaper\n'
        printf '      visible (no fullscreen window, nothing covering a monitor), or it is what you meant to measure.\n'
    else
        printf '\nwallpapers during the sampling: running in all %s state checks\n' "$nt"
    fi
fi

# every program on the GPU (nvidia-smi pmon): memory and GPU share, so that
# the desktop's own part (X, the compositor) is visible next to the daemon's
if [ "$have_smi" = 1 ] && [ -s "$tmp/pmon.txt" ]; then
    awk '
    function isnum(v) { return v ~ /^[0-9]+(\.[0-9]+)?$/ }
    /^#[ \t]*gpu/ { for (i = 2; i <= NF; i++) col[$i] = i - 1; next }
    /^#/ { next }
    col["pid"] && isnum($col["pid"]) {
        p = $col["pid"]; rows[p]++
        if (col["command"]) name[p] = $col["command"]
        if (col["type"]) type[p] = $col["type"]
        if (col["fb"] && isnum($col["fb"])) { fs[p] += $col["fb"]; nf[p]++ }
        if (col["sm"] && isnum($col["sm"])) { ss[p] += $col["sm"]; ns[p]++ }
    }
    END {
        for (p in rows)
            printf "%.1f\t%s\t%s\t%s\t%s\t%d\t%d\n", (nf[p] ? fs[p] / nf[p] : -1), name[p], type[p],
                   (ns[p] ? sprintf("%.1f", ss[p] / ns[p]) : "n/a"), (ns[p] ? sprintf("%.1f", ss[p] / rows[p]) : "n/a"), ns[p], rows[p]
    }' "$tmp/pmon.txt" 2>/dev/null | sort -t "$(printf '\t')" -k1,1 -rn | head -n 8 > "$tmp/procs.txt"
    if [ -s "$tmp/procs.txt" ]; then
        printf '\nprograms on the GPU (nvidia-smi pmon; sm = GPU share: average of the samples with a value / with gaps as 0)\n'
        awk -F '\t' '{ printf "  %-20s %-2s %8s MiB   sm %5s %% / %5s %%  (%d of %d samples)\n", $2, $3, ($1 < 0 ? "n/a" : $1), $4, $5, $6, $7 }' "$tmp/procs.txt"
    fi
fi

# the daemon counts its frames itself when started with WALPE_FPS_LOG=<s>
# and its messages go to a file: the last such line (monitor names only)
if [ -n "$pid" ]; then
    log=$(readlink "/proc/$pid/fd/2" 2>/dev/null)
    fl=
    [ -n "$log" ] && [ -f "$log" ] && fl=$(grep ' fps: mon' "$log" 2>/dev/null | tail -n 1 | sed 's/^.* fps: mon/mon/')
    if [ -n "$fl" ]; then
        printf '\nframes per second counted by the daemon (WALPE_FPS_LOG): %s\n' "$fl"
    fi
fi

cat <<'EOF'

how to read this
 - utilization.gpu is, in NVIDIA's words, the "percent of time over the past sample
   period during which one or more kernels was executing on the GPU": how much of the
   TIME the GPU was busy at the clock it was running at, not how much of its capacity
   was used. A laptop GPU that sits in a low performance state (low clocks.gr, a
   P-state far from P0) needs longer for the same work, so light work can show a high
   percentage there. Two builds are comparable by power.draw (W) and the clocks next to
   the percentage: lower watts at the same or a lower clock = really less GPU work.
 - utilization.memory is the share of time GPU memory was being read or written, not
   how full it is; memory.used is for the whole GPU (X, the compositor and every
   client), "daemon GPU memory" is walpe-daemon alone.
 - "GPU share (sm)" is nvidia-smi pmon's per-program figure. It prints "-" for a second
   in which the driver has no figure for the program, so two averages are given: over
   the seconds that had a value (too high when many are missing) and with the missing
   ones as 0 (too low if the driver merely did not report). Read the pair as a range.
 - Compare runs made the same way: same two wallpapers, same windows open, same power
   source, and wait until both wallpapers have finished loading before starting.
   `walpe-gpu-report.sh 30 idle --baseline` with WalPE closed gives what X and the
   compositor cost alone.
 - A build without the profiler does not say how many frames it really draws: start
   the daemon with WALPE_FPS_LOG=5 and its output in a file (walpe-daemon 2>~/walpe.log)
   and this report shows its count.
 - n/a = this GPU, driver or nvidia-smi does not report the value; nothing is wrong.
EOF

# ---- the daemon's own profile, when it has one ----------------------------
if [ "$profile" = 1 ] && [ -n "$pid" ] && [ -n "$ctl" ]; then
    printf '\n---- walpe-ctl profile %s ----\n' "$secs"
    "$ctl" profile "$secs" > "$tmp/prof.out" 2> "$tmp/prof.err"
    rc=$?
    case $rc in
    0) cat "$tmp/prof.out" ;;
    1) printf 'this build has no GPU profiler (version 0.12 or older): skipped\n' ;;
    *) sed 's/^/(walpe-ctl) /' "$tmp/prof.err" | head -n 3
       printf 'no GPU profile from this daemon (walpe-ctl exit status %s)\n' "$rc" ;;
    esac
elif [ "$profile" = 1 ] && [ -n "$pid" ]; then
    printf '\n(walpe-ctl not found: no GPU profile)\n'
fi
exit 0
