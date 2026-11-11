#!/bin/sh
# fake-gpustat-tree.sh ROOT SCENARIO STEP [DAEMON_PID]
# fake-gpustat-tree.sh ROOT SCENARIO live COUNT [DAEMON_PID]
# fake-gpustat-tree.sh ROOT clean
#
# Writes ROOT/proc and ROOT/sys as a machine with a GPU would show them, for
# src/common/gpustat.c (tests/gpustat_test.c, tests/gpu_report_test.sh).
# STEP is a whole number of seconds since the first sample: every counter
# is "start value + rate x STEP", so a test that samples STEP 0, 1, 2 ...
# one second apart knows each share exactly. "live" rewrites the tree once
# a second COUNT times (the report script samples a tree that moves).
# "clean" makes everything removable again (one scenario takes read
# permission away from a directory).
#
# ILLUSTRATIVE DATA. Every fdinfo text and every sysfs value below was
# written by hand from the kernel's description of the format ("DRM client
# usage stats": drm-driver, drm-pdev, drm-client-id, drm-engine-<name>,
# drm-engine-capacity-<name>, drm-cycles-<name>, drm-total-cycles-<name>,
# drm-memory-/-total-/-shared-/-resident-/-active-/-purgeable-<region>) and
# from the names the drivers are documented to use for engines and memory
# regions. Nothing here was captured from a real machine; the numbers are
# chosen so that the shares come out round. The devices' PCI ids are only
# plausible.
#
# Scenarios (the "daemon" is DAEMON_PID, default 4242, named walpe-daemon;
# Xorg is 811, picom 902):
#   amdgpu       one AMD card: engine gfx; memory vram / gtt / cpu; sysfs
#                busy and memory, hwmon power / temperature / clocks.
#                daemon 10 % gfx + 2 % dec, 310 MiB; Xorg 3 %; picom 8 %.
#                The daemon holds its client through two descriptors.
#   i915         Intel integrated: engines render / copy / video (two of
#                them) / video-enhance; memory region system0; clocks in
#                sysfs; no hwmon. daemon 20 % render + 5 % video; Xorg 5 %.
#   xe           Intel xe: cycles against total cycles per engine class
#                (rcs, bcs, vcs x 2, vecs, ccs); regions system / gtt /
#                vram0 / stolen. daemon 25 % rcs + 10 % vcs; Xorg 5 %.
#   nofdinfo     a card of the NVIDIA driver: its descriptors show no drm-
#                keys at all.
#   nostats      a driver that prints only who the client is (drm-driver,
#                drm-client-id, drm-pdev): no engine time, no memory.
#   shared       amdgpu; picom holds a copy of the daemon's client 7 next
#                to its own client 5.
#   backwards    amdgpu; the daemon's counter is seen lower once (STEP 2)
#                and catches up; picom's wraps at STEP 2 and stays low;
#                Xorg's jumps by ten seconds at STEP 3.
#   exit         amdgpu; picom is gone from STEP 2 on.
#   daemon-exit  amdgpu; the daemon is gone from STEP 3 on.
#   denied       amdgpu; process 950 does not let its fdinfo be read.
#   twogpu       Intel (boot GPU, card0) + AMD (card1); the daemon has a
#                client on each and works on the AMD one.
#   suspended    one AMD card that is powered down; nobody has a client.
#   platform     v3d, a platform device without a PCI address: engines bin
#                / render / tfu / csd / cache-clean, no memory keys.
#   flat         the amdgpu machine with a sysfs tree of plain files only
#                (no symbolic links; driver, ids and address in the uevent
#                file): the layout the GPU-choice tests write.
#   empty        no GPU at all: only the daemon's process.

root=$1
scen=$2
step=$3
dpid=${4:-4242}
if [ -z "$root" ] || [ -z "$scen" ]; then
    echo "usage: fake-gpustat-tree.sh ROOT SCENARIO STEP [DAEMON_PID]" >&2
    exit 2
fi
if [ "$scen" = clean ]; then
    [ -d "$root" ] && chmod -R u+rwx "$root" 2>/dev/null
    exit 0
fi
if [ "$step" = live ]; then
    count=${4:-10}
    dpid=${5:-4242}
    # Told to stop (the caller's kill), the loop ends only when the step it
    # is in has been written to its end - a trap runs after the foreground
    # command - and at once while it waits for the next second. Without the
    # trap the signal ended this shell and left the step's shell writing
    # into a tree the caller had removed or handed to the next scenario.
    trap 'exit 0' TERM INT HUP
    n=0
    while [ "$n" -lt "$count" ]; do
        sh "$0" "$root" "$scen" "$n" "$dpid" || exit 1
        n=$((n + 1))
        sleep 1 > /dev/null 2>&1 &
        wait $!
    done
    exit 0
fi
case "$step" in ''|*[!0-9]*) echo "STEP must be a number" >&2; exit 2 ;; esac

P=$root/proc
S=$root/sys
mkdir -p "$P" "$S/class/drm" "$S/bus/pci/devices" "$S/bus/pci/drivers" || exit 1
TAB=$(printf '\t')

# One file of the tree: FILE, then the words of an `echo` (put) or the
# arguments of a `printf` (putf). Written aside and renamed, as fd () below
# does: `echo x > file` empties the file before it writes, and a reader of a
# tree that "live" rewrites under it then finds an attribute empty for a
# moment (seen: a device without its PCI ids in the report), which the
# kernel's own files never are.
put () {
    _pf=$1; shift
    echo "$@" > "$root/.put.tmp" && mv -f "$root/.put.tmp" "$_pf"
}
putf () {
    _pf=$1; shift
    printf "$@" > "$root/.put.tmp" && mv -f "$root/.put.tmp" "$_pf"
}

# a PCI card: card NAME, PCI SLOT, DRIVER, VENDOR, DEVICE, BOOT_VGA, RENDER node, power STATE
pci_card () {
    d=$S/devices/pci0000:00/$2
    mkdir -p "$d/drm/$1" "$d/drm/$7" "$d/power" "$S/bus/pci/drivers/$3"
    put "$d/vendor" "$4"
    put "$d/device" "$5"
    put "$d/class" 0x030000
    put "$d/boot_vga" "$6"
    put "$d/power/runtime_status" "$8"
    putf "$d/uevent" 'DRIVER=%s\nPCI_SLOT_NAME=%s\n' "$3" "$2"
    ln -sfn "../../../bus/pci/drivers/$3" "$d/driver"
    ln -sfn ../.. "$d/drm/$1/device"
    ln -sfn "../../devices/pci0000:00/$2/drm/$1" "$S/class/drm/$1"
    ln -sfn "../../../devices/pci0000:00/$2" "$S/bus/pci/devices/$2"
    # a connector directory next to the card: must not be taken for a card
    mkdir -p "$S/class/drm/$1-DP-1"
}

# a process: PID, NAME. Descriptor 0 is an ordinary file.
proc () {
    mkdir -p "$P/$1/fdinfo"
    put "$P/$1/comm" "$2"
    putf "$P/$1/fdinfo/0" 'pos:\t0\nflags:\t02\nmnt_id:\t27\nino:\t9\n'
}

# fdinfo text from stdin into PID's descriptor N ("key: value" lines; the
# kernel separates them with a tab). Written aside and renamed: a reader of
# a tree that is being rewritten ("live") sees the old text or the new one,
# as with the kernel's files, never half of one.
fd () {
    {
        printf 'pos:\t0\nflags:\t02100002\nmnt_id:\t27\nino:\t412\n'
        sed "s/: */:$TAB/"
    } > "$root/.fd.tmp" && mv -f "$root/.fd.tmp" "$P/$1/fdinfo/$2"
}

# ---- amdgpu -----------------------------------------------------------------
amd_card () {   # card name, render node, boot_vga, power state
    pci_card "$1" 0000:03:00.0 amdgpu 0x1002 0x73df "$3" "$2" "$4"
    d=$S/devices/pci0000:00/0000:03:00.0
    put "$d/gpu_busy_percent" $((20 + 5 * (step % 3)))
    put "$d/mem_busy_percent" 4
    put "$d/mem_info_vram_used" $(((1200 + step) * 1048576))
    put "$d/mem_info_vram_total" $((8176 * 1048576))
    put "$d/mem_info_gtt_used" $((300 * 1048576))
    put "$d/mem_info_gtt_total" $((15900 * 1048576))
    putf "$d/pp_dpm_sclk" '0: 500Mhz \n1: 2575Mhz *\n'
    mkdir -p "$d/hwmon/hwmon3"
    put "$d/hwmon/hwmon3/name" amdgpu
    put "$d/hwmon/hwmon3/power1_average" 24500000
    put "$d/hwmon/hwmon3/temp1_input" 51000
    put "$d/hwmon/hwmon3/freq1_input" 1287000000
    put "$d/hwmon/hwmon3/freq2_input" 875000000
}

amd_client () {   # pid, fd, client id, gfx ns, dec ns, vram KiB, gtt KiB, cpu KiB
    fd "$1" "$2" <<EOF
drm-driver: amdgpu
drm-client-id: $3
drm-pdev: 0000:03:00.0
drm-memory-vram: $6 KiB
drm-memory-gtt: $7 KiB
drm-memory-cpu: $8 KiB
amd-memory-visible-vram: $6 KiB
amd-evicted-vram: 0 KiB
amd-requested-vram: $6 KiB
drm-engine-gfx: $4 ns
drm-engine-compute: 0 ns
drm-engine-dec: $5 ns
EOF
}

amd_daemon () {   # gfx ns
    proc "$dpid" walpe-daemon
    # one client through two descriptors (a dup): the same id, the same numbers
    amd_client "$dpid" 5 7 "$1" $((1000000 + 20000000 * step)) 307200 8192 2048
    amd_client "$dpid" 9 7 "$1" $((1000000 + 20000000 * step)) 307200 8192 2048
}

amd_others () {   # Xorg gfx ns, picom gfx ns ("" = no picom)
    proc 811 Xorg
    amd_client 811 14 3 "$1" 0 184320 4096 0
    if [ -n "$2" ]; then
        proc 902 picom
        amd_client 902 11 5 "$2" 0 61440 1024 0
    fi
}

scen_amdgpu () {
    amd_card card0 renderD128 1 active
    amd_daemon $((5000000000 + 100000000 * step))
    amd_others $((90000000000 + 30000000 * step)) $((7000000000 + 80000000 * step))
}

# ---- Intel ------------------------------------------------------------------
i915_card () {   # card name, render node, boot_vga
    pci_card "$1" 0000:00:02.0 i915 0x8086 0x46a6 "$3" "$2" active
    c=$S/devices/pci0000:00/0000:00:02.0/drm/$1
    put "$c/gt_act_freq_mhz" 650
    put "$c/gt_cur_freq_mhz" 700
    put "$c/gt_max_freq_mhz" 1300
    put "$c/gt_RP0_freq_mhz" 1300
}

i915_client () {   # pid, fd, client id, render ns, video ns, resident KiB
    fd "$1" "$2" <<EOF
drm-driver: i915
drm-client-id: $3
drm-pdev: 0000:00:02.0
drm-total-system0: $(($6 + 40960)) KiB
drm-shared-system0: 16384 KiB
drm-active-system0: 4096 KiB
drm-resident-system0: $6 KiB
drm-purgeable-system0: 0
drm-total-stolen-system0: 0
drm-shared-stolen-system0: 0
drm-active-stolen-system0: 0
drm-resident-stolen-system0: 0
drm-purgeable-stolen-system0: 0
drm-engine-render: $4 ns
drm-engine-copy: 0 ns
drm-engine-video: $5 ns
drm-engine-capacity-video: 2
drm-engine-video-enhance: 0 ns
EOF
}

scen_i915 () {
    i915_card card0 renderD128 1
    proc "$dpid" walpe-daemon
    i915_client "$dpid" 6 21 $((3000000000 + 200000000 * step)) $((40000000 + 100000000 * step)) 245760
    proc 811 Xorg
    i915_client 811 14 9 $((60000000000 + 50000000 * step)) 0 102400
}

xe_client () {   # pid, fd, client id, rcs cycles, vcs cycles
    total=$((500000000 + 19200000 * step))
    fd "$1" "$2" <<EOF
drm-driver: xe
drm-client-id: $3
drm-pdev: 0000:00:02.0
drm-total-system: 0
drm-shared-system: 0
drm-active-system: 0
drm-resident-system: 0
drm-purgeable-system: 0
drm-total-gtt: 1024 KiB
drm-shared-gtt: 0
drm-active-gtt: 0
drm-resident-gtt: 1024 KiB
drm-total-vram0: 204800 KiB
drm-shared-vram0: 8 MiB
drm-active-vram0: 0
drm-resident-vram0: 196608 KiB
drm-total-stolen: 0
drm-shared-stolen: 0
drm-active-stolen: 0
drm-resident-stolen: 0
drm-cycles-rcs: $4
drm-total-cycles-rcs: $total
drm-cycles-bcs: 0
drm-total-cycles-bcs: $total
drm-cycles-vcs: $5
drm-total-cycles-vcs: $total
drm-engine-capacity-vcs: 2
drm-cycles-vecs: 0
drm-total-cycles-vecs: $total
drm-engine-capacity-vecs: 2
drm-cycles-ccs: 0
drm-total-cycles-ccs: $total
EOF
}

scen_xe () {
    pci_card card0 0000:00:02.0 xe 0x8086 0x64a0 1 renderD128 active
    f=$S/devices/pci0000:00/0000:00:02.0/tile0/gt0/freq0
    mkdir -p "$f"
    put "$f/act_freq" 800
    put "$f/cur_freq" 900
    put "$f/max_freq" 2000
    put "$f/rp0_freq" 2050
    proc "$dpid" walpe-daemon
    xe_client "$dpid" 6 12 $((1000000 + 4800000 * step)) $((2000 + 3840000 * step))
    proc 811 Xorg
    xe_client 811 14 4 $((70000000 + 960000 * step)) 0
}

# ---- drivers without statistics -----------------------------------------------
scen_nofdinfo () {
    pci_card card0 0000:01:00.0 nvidia 0x10de 0x28a1 1 renderD128 active
    proc "$dpid" walpe-daemon
    # its DRM descriptor: the driver adds nothing to the generic lines
    putf "$P/$dpid/fdinfo/12" 'pos:\t0\nflags:\t02100002\nmnt_id:\t27\nino:\t501\n'
    proc 811 Xorg
    putf "$P/811/fdinfo/14" 'pos:\t0\nflags:\t02100002\nmnt_id:\t27\nino:\t501\n'
}

scen_nostats () {
    pci_card card0 0000:01:00.0 radeon 0x1002 0x6810 1 renderD128 active
    proc "$dpid" walpe-daemon
    fd "$dpid" 5 <<EOF
drm-driver: radeon
drm-client-id: 2
drm-pdev: 0000:01:00.0
EOF
}

# ---- special cases on amdgpu ---------------------------------------------------
scen_shared () {
    scen_amdgpu
    # picom holds a copy of the daemon's client (passed over a socket)
    amd_client 902 14 7 $((5000000000 + 100000000 * step)) $((1000000 + 20000000 * step)) 307200 8192 2048
}

scen_backwards () {
    amd_card card0 renderD128 1 active
    # the daemon: 10 % a second, but read 50 ms lower than before at STEP 2
    case $step in
    2) g=5050000000 ;;
    *) g=$((5000000000 + 100000000 * step)) ;;
    esac
    amd_daemon "$g"
    # Xorg: 3 % a second, with a jump of ten seconds between STEP 2 and 3
    if [ "$step" -ge 3 ]; then x=$((90000000000 + 10000000000 + 30000000 * step)); else x=$((90000000000 + 30000000 * step)); fi
    # picom: 8 % a second on a counter that wraps between STEP 1 and 2
    case $step in
    0) p=18446744073000000000 ;;
    1) p=18446744073080000000 ;;
    *) p=$((80000000 * (step - 2) + 30000000)) ;;
    esac
    amd_others "$x" "$p"
}

scen_exit () {
    amd_card card0 renderD128 1 active
    amd_daemon $((5000000000 + 100000000 * step))
    if [ "$step" -lt 2 ]; then
        amd_others $((90000000000 + 30000000 * step)) $((7000000000 + 80000000 * step))
    else
        amd_others $((90000000000 + 30000000 * step)) ""
        rm -rf "$P/902"
    fi
}

scen_daemon_exit () {
    amd_card card0 renderD128 1 active
    if [ "$step" -lt 3 ]; then
        amd_daemon $((5000000000 + 100000000 * step))
    else
        rm -rf "$P/$dpid"
    fi
    amd_others $((90000000000 + 30000000 * step)) $((7000000000 + 80000000 * step))
}

scen_denied () {
    scen_amdgpu
    chmod u+rwx "$P/950/fdinfo" 2>/dev/null
    proc 950 secretive
    amd_client 950 4 31 $((100000000 * step)) 0 1024 0 0
    chmod 000 "$P/950/fdinfo"
}

scen_twogpu () {
    i915_card card0 renderD128 1
    pci_card card1 0000:03:00.0 amdgpu 0x1002 0x73df 0 renderD129 active
    d=$S/devices/pci0000:00/0000:03:00.0
    put "$d/gpu_busy_percent" $((20 + 5 * (step % 3)))
    put "$d/mem_info_vram_used" $((1200 * 1048576))
    put "$d/mem_info_vram_total" $((8176 * 1048576))
    proc "$dpid" walpe-daemon
    # it shows its picture through the Intel GPU and draws on the AMD one
    i915_client "$dpid" 6 21 $((1000000 + 5000000 * step)) 0 8192
    amd_client "$dpid" 5 7 $((5000000000 + 100000000 * step)) 0 307200 8192 2048
    proc 811 Xorg
    i915_client 811 14 9 $((60000000000 + 50000000 * step)) 0 102400
}

scen_suspended () {
    amd_card card0 renderD128 1 suspended
    # a reader that touched it would see this and wake the card
    put "$S/devices/pci0000:00/0000:03:00.0/gpu_busy_percent" 99
    proc "$dpid" walpe-daemon
}

scen_platform () {
    d=$S/devices/platform/gpu
    mkdir -p "$d/drm/card0" "$d/drm/renderD128" "$S/bus/platform/drivers/v3d"
    putf "$d/uevent" 'DRIVER=v3d\n'
    ln -sfn ../../../bus/platform/drivers/v3d "$d/driver"
    ln -sfn ../.. "$d/drm/card0/device"
    ln -sfn ../../devices/platform/gpu/drm/card0 "$S/class/drm/card0"
    proc "$dpid" walpe-daemon
    fd "$dpid" 4 <<EOF
drm-driver: v3d
drm-client-id: 4
drm-engine-bin: $((200000000 + 10000000 * step)) ns
drm-engine-render: $((900000000 + 150000000 * step)) ns
drm-engine-tfu: 0 ns
drm-engine-csd: 0 ns
drm-engine-cache-clean: 0 ns
EOF
}

scen_flat () {
    d=$S/class/drm/card0/device
    mkdir -p "$d/drm/renderD128" "$d/hwmon/hwmon0" "$S/class/drm/card0-eDP-1"
    putf "$d/uevent" 'DRIVER=amdgpu\nPCI_ID=1002:73DF\nPCI_SLOT_NAME=0000:03:00.0\n'
    put "$d/boot_vga" 1
    put "$S/class/drm/card0-eDP-1/status" connected
    put "$d/gpu_busy_percent" $((20 + 5 * (step % 3)))
    put "$d/mem_info_vram_used" $((1200 * 1048576))
    put "$d/mem_info_vram_total" $((8176 * 1048576))
    put "$d/hwmon/hwmon0/power1_average" 24500000
    amd_daemon $((5000000000 + 100000000 * step))
}

scen_empty () {
    proc "$dpid" walpe-daemon
}

case "$scen" in
amdgpu) scen_amdgpu ;;
i915) scen_i915 ;;
xe) scen_xe ;;
nofdinfo) scen_nofdinfo ;;
nostats) scen_nostats ;;
shared) scen_shared ;;
backwards) scen_backwards ;;
exit) scen_exit ;;
daemon-exit) scen_daemon_exit ;;
denied) scen_denied ;;
twogpu) scen_twogpu ;;
suspended) scen_suspended ;;
platform) scen_platform ;;
flat) scen_flat ;;
empty) scen_empty ;;
*) echo "unknown scenario: $scen" >&2; exit 2 ;;
esac
exit 0
