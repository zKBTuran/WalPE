#!/bin/sh
# Configure (or reconfigure) and build WalPE.
#   ./build.sh                      the generic build: runs on any CPU of this architecture
#                                   (x86-64 or aarch64, whichever this machine is)
#   ./build.sh --tuning=alderlake   the maintainer's make.conf flags (Alder Lake-class CPUs only;
#                                   x86-64: on another architecture the configure step refuses it)
#   ./build.sh --tuning=native      the CPU of this machine (-march=native, on aarch64 -mcpu=native)
#   --tuning=<generic|alderlake|native|auto> is passed on as -Dnative_tuning=<value>
#   Building for arm64 on an x86-64 machine: see cross/aarch64-linux-gnu.ini (meson setup
#   --cross-file ...; this script configures native builds only).
#   WALPE_JOBS  parallel jobs   (ninja -j, default: the number of CPUs; 0 = the default)
#   WALPE_LOAD  max load avg    (ninja -l, default: the number of CPUs + 2)
#               the maintainer's MAKEOPTS: WALPE_JOBS=16 WALPE_LOAD=18 ./build.sh --tuning=alderlake
#   WALPE_BUILDDIR  build dir   (default: build in the source directory; a
#               relative path is taken from where the script is called)
# Other arguments go to `meson setup`, e.g.
#   ./build.sh -Dweb=disabled
# An existing build dir is reconfigured and keeps the options it was set up
# with: give --tuning= once to change its tuning.
set -eu

# Ensure build directories and modules are never world-writable: the daemon
# refuses to load a module that is world-writable or in a world-writable
# directory (src/engine/modload.c).
case "$(umask)" in
	0000|000) umask 022 ;;
	*) umask o-w ;;
esac

# a relative WALPE_BUILDDIR is meant from where the script was called, not
# from the source directory the script changes to
case "${WALPE_BUILDDIR:-}" in
	'' | /*) ;;
	*) WALPE_BUILDDIR=$PWD/$WALPE_BUILDDIR ;;
esac

cd "$(dirname "$0")"

ncpu=$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
case "$ncpu" in '' | *[!0-9]*) ncpu=4 ;; esac
jobs=${WALPE_JOBS:-$ncpu}
# 0 would be `ninja -j 0`, which is no limit at all: 0 and anything that is
# no number mean the default
case "$jobs" in '' | *[!0-9]*) jobs=$ncpu ;; esac
[ "$jobs" -gt 0 ] || jobs=$ncpu
load=${WALPE_LOAD:-$((ncpu + 2))}
dir=${WALPE_BUILDDIR:-build}

# --tuning=<value> becomes -Dnative_tuning=<value>; everything else as given
n=$#
while [ "$n" -gt 0 ]; do
	a=$1
	shift
	n=$((n - 1))
	case "$a" in
		--tuning=*) set -- "$@" "-Dnative_tuning=${a#--tuning=}" ;;
		*) set -- "$@" "$a" ;;
	esac
done

if [ -f "$dir/build.ninja" ]; then
	meson setup --reconfigure "$dir" "$@"
else
	meson setup "$dir" "$@"
fi

# Ninja cannot always see that a library behind a stable -lfoo argument was
# replaced in place. Relink the installed executables/modules every time so
# updated SONAMEs and static archives are picked up, without recompiling the
# unchanged source objects.
is_linux=false
if [ "$(uname -s 2>/dev/null || echo unknown)" = Linux ]; then
	is_linux=true
	python3 scripts/force-relink-installed.py "$(command -v meson)" "$dir"
fi

# First link gives us the real app/module DT_NEEDED graph. On native Linux,
# inspect those outputs, wildcard-index the directories containing their
# resolved dependencies, then follow DT_NEEDED recursively and match exact
# DT_SONAMEs. If this changes the set of private directories, reconfigure with
# an inherited DT_RPATH and relink. This is intentionally app-specific: no
# unrelated plugin directories from the whole host are added to WalPE.
ninja -C "$dir" -j "$jobs" -l "$load" programs

if [ "$is_linux" = true ]; then
	runtime_update=$(python3 scripts/update-private-runtime-dirs.py "$(command -v meson)" "$dir" "$(command -v readelf)")
	case "$runtime_update" in
		changed)
			private_dirs=$(awk 'NF { if (out != "") out = out ":"; out = out $0 } END { print out }' "$dir/private-runtime-dirs.txt")
			meson setup --reconfigure "$dir" "-Dprivate_runtime_dirs=$private_dirs"
			python3 scripts/force-relink-installed.py "$(command -v meson)" "$dir"
			ninja -C "$dir" -j "$jobs" -l "$load" programs
			;;
		unchanged)
			;;
		*)
			echo "build: unexpected runtime discovery result: $runtime_update" >&2
			exit 1
			;;
	esac

	# Inspect the complete runtime dependency closure of every installed
	# executable/module after the final link.
	python3 scripts/check-runtime-deps.py "$(command -v meson)" "$dir"
fi

# A pre-existing build directory created under umask 0000 keeps world-writable
# permissions across reconfigures; clear o+w so modules pass trust checks.
find "$dir" -perm -0002 ! -type l -exec chmod o-w {} + 2>/dev/null || true
