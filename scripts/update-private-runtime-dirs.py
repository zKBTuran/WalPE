#!/usr/bin/env python3
"""Discover app-specific directories for transitive, versioned ELF dependencies.

The built WalPE executable and modules are the seeds.  Their ldd output tells
us where the direct runtime dependencies actually came from; the ELF scanner
then follows DT_NEEDED recursively and uses lib*.so* wildcard indexing plus
exact DT_SONAME checks to find private companion DSOs in nested directories.
The result is a newline-separated manifest in the Meson build directory.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Dict, Iterable, List, Set, Tuple

from runtime_targets import installed_linkable_outputs


def run(command: List[str]) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(command, capture_output=True, text=True, check=False)
    except OSError as error:
        raise RuntimeError("could not run %s: %s" % (command[0], error)) from error


def ldd_search_roots(
    outputs: Iterable[Path], ldd: str
) -> Tuple[List[Path], List[Tuple[str, Path]], List[str]]:
    roots: Set[Path] = set()
    resolved: Dict[str, Path] = {}
    unresolved: Set[str] = set()
    for output in outputs:
        if not output.is_file():
            raise RuntimeError("expected installed ELF output is missing: %s" % output)
        clean_env = {
            key: value for key, value in os.environ.items()
            if key not in {"LD_LIBRARY_PATH", "LD_PRELOAD", "LD_AUDIT"}
        }
        try:
            result = subprocess.run(
                [ldd, str(output)], capture_output=True, text=True,
                check=False, env=clean_env,
            )
        except OSError as error:
            raise RuntimeError("could not run ldd for %s: %s" % (output, error)) from error
        text = result.stdout + "\n" + result.stderr
        if result.returncode != 0 and "not found" not in text:
            raise RuntimeError("ldd could not inspect %s: %s" % (output, text.strip()))

        # ldd emits both "name => /absolute/path (address)" and the loader's
        # direct form "/absolute/path (address)". Record "not found" SONAMEs
        # separately; ignore virtual entries such as linux-vdso.
        for line in text.splitlines():
            fields = line.strip().split()
            candidate = None
            if len(fields) >= 4 and fields[1] == "=>" and fields[2:4] == ["not", "found"]:
                # The same SONAME can appear more than once in ldd output:
                # an executable's direct DT_NEEDED may be unresolved while
                # an indirect dependency finds that SONAME through its own
                # RUNPATH/RPATH. Preserve the unresolved occurrence instead
                # of letting a later successful occurrence mask it.
                unresolved.add(fields[0])
            elif len(fields) >= 3 and fields[1] == "=>" and fields[2].startswith("/"):
                candidate = fields[2]
                resolved.setdefault(fields[0], Path(candidate).resolve())
            elif fields and fields[0].startswith("/"):
                candidate = fields[0]
            if candidate:
                path = Path(candidate)
                if path.is_file():
                    roots.add(path.resolve().parent)
    return (
        sorted(roots, key=str),
        sorted(resolved.items(), key=lambda item: item[0]),
        sorted(unresolved),
    )


def main() -> int:
    if len(sys.argv) != 4:
        print("usage: update-private-runtime-dirs.py MESON BUILDDIR READELF", file=sys.stderr)
        return 2

    meson, build_dir, readelf = sys.argv[1:]
    build_path = Path(build_dir).absolute()
    ldd = shutil.which("ldd")
    if not ldd:
        print("runtime-discovery: ldd is required on native Linux builds", file=sys.stderr)
        return 1

    try:
        outputs = installed_linkable_outputs(meson, build_path)
        if not outputs:
            raise RuntimeError("Meson reported no installed executable or module outputs")
        search_roots, resolved_paths, unresolved_sonames = ldd_search_roots(outputs, ldd)
        scanner = Path(__file__).with_name("discover-private-libdirs.py")
        command = [sys.executable, str(scanner), "--readelf", readelf]
        for output in outputs:
            command.extend(["--seed", str(output)])
        for root in search_roots:
            command.extend(["--search-root", str(root)])
        for soname, path in resolved_paths:
            command.extend(["--resolved", "%s=%s" % (soname, path)])
        for soname in unresolved_sonames:
            command.extend(["--unresolved", soname])
        result = run(command)
        if result.returncode != 0:
            raise RuntimeError("ELF dependency scan failed: %s" % result.stderr.strip())

        directories = sorted({line.strip() for line in result.stdout.splitlines() if line.strip()})
        if any(":" in directory for directory in directories):
            raise RuntimeError("a discovered library directory contains ':', which ELF RPATH cannot represent")

        manifest = build_path / "private-runtime-dirs.txt"
        old = manifest.read_text().splitlines() if manifest.exists() else []
        changed = directories != old
        temporary = manifest.with_suffix(".txt.tmp")
        temporary.write_text("".join(directory + "\n" for directory in directories))
        temporary.replace(manifest)

        if changed:
            print("runtime-discovery: updated private runtime directories:", file=sys.stderr)
            for directory in directories:
                print("  " + directory, file=sys.stderr)
            print("changed")
        else:
            print("runtime-discovery: private runtime directories unchanged (%d)" % len(directories),
                  file=sys.stderr)
            print("unchanged")
        return 0
    except (OSError, RuntimeError) as error:
        print("runtime-discovery: %s" % error, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
