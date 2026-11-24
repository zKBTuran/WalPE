#!/usr/bin/env python3
"""Discover non-cache directories holding transitive ELF shared dependencies.

The dynamic loader resolves an ELF DT_NEEDED entry by its exact SONAME; it
cannot expand a wildcard.  This build-time scanner uses version-agnostic
``lib*.so*`` globs to index installed DSOs, reads their ELF metadata, and
follows exact DT_NEEDED -> DT_SONAME matches. Only directories containing a
DSO actually required by a WalPE executable/module or one of its dependencies
are emitted.  This
keeps private companion libraries (for example PulseAudio's versioned
libpulsecommon DSO) discoverable without hardcoding a release number or
adding every plugin directory to the runtime search path.

The emitted paths are linked as DT_RPATH on WalPE's ELF outputs. DT_RPATH is
intentional here: unlike DT_RUNPATH it is inherited while resolving indirect
DT_NEEDED entries several libraries down the dependency tree.
"""

from __future__ import annotations

import argparse
import glob
import os
import re
import subprocess
import sys
from collections import deque
from pathlib import Path
from typing import Deque, Dict, List, Optional, Set, Tuple

NEEDED_RE = re.compile(r"\(NEEDED\).*?\[([^]]+)\]")
SONAME_RE = re.compile(r"\(SONAME\).*?\[([^]]+)\]")
CLASS_RE = re.compile(r"^\s*Class:\s+(\S+)", re.MULTILINE)
MACHINE_RE = re.compile(r"^\s*Machine:\s+(.+?)\s*$", re.MULTILINE)
CACHE_PATH_RE = re.compile(r"=>\s+(\S+)$")


def elf_identity(readelf: str, path: Path) -> Optional[Tuple[str, str]]:
    try:
        result = subprocess.run(
            [readelf, "-hW", str(path)], capture_output=True, text=True, check=False
        )
    except OSError:
        return None
    if result.returncode != 0:
        return None
    cls = CLASS_RE.search(result.stdout)
    machine = MACHINE_RE.search(result.stdout)
    if not cls or not machine:
        return None
    return cls.group(1), machine.group(1).strip()


def _run_readelf(readelf: str, path: Path) -> str:
    try:
        result = subprocess.run(
            [readelf, "-dW", str(path)], capture_output=True, text=True,
            check=False,
        )
    except OSError:
        return ""
    return result.stdout if result.returncode == 0 else ""


def _parse_ld_config(path: Path, seen: Set[Path], dirs: Set[Path]) -> None:
    try:
        real = path.resolve()
        if real in seen:
            return
        seen.add(real)
        lines = path.read_text(errors="replace").splitlines()
    except (OSError, RuntimeError):
        return

    for raw in lines:
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        if line.startswith("include "):
            pattern = line.split(None, 1)[1].strip()
            for include in glob.glob(pattern):
                _parse_ld_config(Path(include), seen, dirs)
        else:
            directory = Path(line)
            if directory.is_dir():
                dirs.add(directory.resolve())


def default_runtime_dirs() -> Set[Path]:
    """Directories the ELF loader searches by default without the cache.

    Do not treat every directory mentioned by ld.so.conf or represented by
    *some other* ld.so.cache entry as universally searchable. The cache maps
    individual SONAMEs to files; it does not mean the loader will scan every
    file in each directory. A private DSO omitted from the cache can still
    need an RPATH even when another library from that directory is cached.
    """
    directories: Set[Path] = set()
    for raw in ("/lib", "/lib64", "/usr/lib", "/usr/lib64"):
        path = Path(raw)
        if path.is_dir():
            directories.add(path.resolve())
    return directories


def system_library_dirs(readelf: str) -> Tuple[Set[Path], Dict[str, Set[Path]]]:
    loader_dirs: Set[Path] = set(default_runtime_dirs())
    cached: Dict[str, Set[Path]] = {}
    host_identity = elf_identity(readelf, Path(sys.executable))

    _parse_ld_config(Path("/etc/ld.so.conf"), set(), loader_dirs)

    try:
        result = subprocess.run(
            ["ldconfig", "-p"], capture_output=True, text=True, check=False
        )
        if result.returncode == 0:
            for line in result.stdout.splitlines():
                match = CACHE_PATH_RE.search(line)
                name_match = re.match(r"\s*(\S+)\s+\(.*\)\s+=>", line)
                if not match or not name_match:
                    continue
                soname = name_match.group(1)
                path = Path(match.group(1))
                identity = elf_identity(readelf, path)
                if host_identity and identity != host_identity:
                    continue
                cached.setdefault(soname, set()).add(path.resolve())
                if path.parent.is_dir():
                    loader_dirs.add(path.parent.resolve())
    except OSError:
        pass

    return loader_dirs, cached


def index_nested_dsos(roots: List[Path], max_depth: int) -> Dict[str, Set[Path]]:
    """Index lib*.so* candidates by filename, independent of version."""
    index: Dict[str, Set[Path]] = {}
    visited: Set[Path] = set()

    for root in roots:
        if not root.is_dir():
            continue
        root = root.resolve()
        for current, child_dirs, _filenames in os.walk(root):
            directory = Path(current)
            try:
                depth = len(directory.relative_to(root).parts)
            except ValueError:
                child_dirs[:] = []
                continue
            if depth >= max_depth:
                child_dirs[:] = []
            child_dirs[:] = [name for name in child_dirs if not name.startswith(".")]

            # Index files at the root as well as nested directories. Public
            # libraries will be skipped through the loader-cache SONAME map;
            # including roots also handles configured loader directories that
            # contain a DSO omitted from the cache.
            for pattern in ("lib*.so", "lib*.so.*"):
                for candidate in glob.iglob(str(directory / pattern)):
                    path = Path(candidate)
                    try:
                        real = path.resolve()
                    except OSError:
                        continue
                    if real in visited or not real.is_file():
                        continue
                    visited.add(real)
                    index.setdefault(path.name, set()).add(real)
    return index


def scan(
    readelf: str,
    roots: List[Path],
    search_roots: List[Path],
    max_depth: int,
    seed_files: Optional[List[Path]] = None,
    resolved_paths: Optional[List[Tuple[str, Path]]] = None,
    unresolved_sonames: Optional[Set[str]] = None,
) -> List[Path]:
    # --root is an isolated search tree used by the regression test. Normal
    # builds use the system loader cache to recognize public libraries, but
    # wildcard-scan only directories reported by ldd for this project's actual
    # linked outputs. This avoids walking unrelated parts of the host system.
    if roots:
        # Isolated roots are used by the regression test. They are scan roots,
        # not proof that every SONAME beneath them is in the host loader cache.
        loader_dirs = {path.resolve() for path in roots if path.is_dir()}
        cached: Dict[str, Set[Path]] = {}
        scan_roots = list(roots)
        default_dirs = default_runtime_dirs()
    else:
        loader_dirs, cached = system_library_dirs(readelf)
        scan_roots = [] if seed_files else sorted(loader_dirs, key=str)
        default_dirs = default_runtime_dirs()

    for directory in search_roots:
        if directory.is_dir():
            scan_roots.append(directory.resolve())

    host_identity = elf_identity(readelf, Path(sys.executable))
    # ldd can resolve dependencies through an existing RUNPATH/RPATH or a
    # distribution-specific loader rule that is not represented in ld.so.cache.
    # Preserve those exact resolutions so we don't add their directories to
    # WalPE's RPATH unnecessarily; still traverse each library's own children.
    for soname, path in resolved_paths or []:
        if not path.is_file():
            continue
        if host_identity and elf_identity(readelf, path) != host_identity:
            continue
        soname_match = SONAME_RE.search(_run_readelf(readelf, path))
        if soname_match and soname_match.group(1) == soname:
            cached.setdefault(soname, set()).add(path.resolve())

    index = index_nested_dsos(scan_roots, max_depth)
    extra_dirs: Set[Path] = set()
    # A SONAME may be reported both as "not found" and as resolved later in
    # the same ldd output. The successful occurrence can come from an
    # indirect library's own RPATH and does not prove that the seed executable
    # can resolve its direct DT_NEEDED. For these names, force exact
    # SONAME-checked wildcard discovery even when an indirect resolution was
    # also observed.
    unresolved_sonames = unresolved_sonames or set()
    inspected: Set[Path] = set()
    queue: Deque[Path] = deque()

    if seed_files:
        for seed in seed_files:
            if not seed.is_file():
                continue
            if host_identity and elf_identity(readelf, seed) != host_identity:
                continue
            queue.append(seed.resolve())
    else:
        # No explicit program seeds means a full system scan (useful for
        # diagnostics); normal WalPE builds always pass their linked outputs.
        for paths in cached.values():
            queue.extend(paths)

    while queue:
        library = queue.popleft()
        try:
            library = library.resolve()
        except OSError:
            continue
        if library in inspected:
            continue
        inspected.add(library)
        dynamic = _run_readelf(readelf, library)
        if not dynamic:
            continue

        for needed in NEEDED_RE.findall(dynamic):
            # A loader-cache hit is already in a normal search directory; add
            # its files to the queue so their own private dependencies are
            # traversed, but do not add an unnecessary RPATH entry.
            cached_paths = cached.get(needed)
            if cached_paths and needed not in unresolved_sonames:
                queue.extend(cached_paths)
                continue

            # If it is not cached, locate a filename via wildcard indexing and
            # accept it only when the file's actual ELF SONAME is an exact
            # match. Version-like filename matches alone are never sufficient.
            matches: List[Path] = []
            for candidate in sorted(index.get(needed, set()), key=str):
                if host_identity and elf_identity(readelf, candidate) != host_identity:
                    continue
                candidate_dynamic = _run_readelf(readelf, candidate)
                soname_match = SONAME_RE.search(candidate_dynamic)
                if soname_match and soname_match.group(1) == needed:
                    matches.append(candidate.resolve())

            for candidate in matches:
                # A match found by wildcard scanning was not resolved by
                # SONAME through the loader cache (otherwise cached_paths
                # above would have handled it). Add its directory unless it is
                # one of the loader's built-in default directories. In
                # particular, do NOT suppress it merely because ld.so.conf or
                # an unrelated cache entry mentions that directory.
                if candidate.parent.resolve() not in default_dirs:
                    extra_dirs.add(candidate.parent.resolve())
                cached.setdefault(needed, set()).add(candidate)
                queue.append(candidate)

    return sorted(extra_dirs, key=str)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--readelf", default="readelf", help="readelf executable")
    parser.add_argument("--root", action="append", default=[],
                        help="isolated library root (repeatable; for regression tests)")
    parser.add_argument("--search-root", action="append", default=[],
                        help="additional library directory discovered from ldd output (repeatable)")
    parser.add_argument("--seed", action="append", default=[],
                        help="project executable/module ELF to inspect (repeatable)")
    parser.add_argument("--resolved", action="append", default=[],
                        help="SONAME=absolute-path already resolved by ldd (repeatable)")
    parser.add_argument("--unresolved", action="append", default=[],
                        help="SONAME also reported as 'not found' by ldd (repeatable)")
    parser.add_argument("--max-depth", type=int, default=4,
                        help="maximum nested directory depth under each root")
    args = parser.parse_args()
    if args.max_depth < 1:
        parser.error("--max-depth must be at least 1")

    roots = [Path(value) for value in args.root]
    search_roots = [Path(value) for value in args.search_root]
    seeds = [Path(value) for value in args.seed]
    resolved_paths: List[Tuple[str, Path]] = []
    for value in args.resolved:
        if "=" not in value:
            parser.error("--resolved must be SONAME=absolute-path")
        soname, raw_path = value.split("=", 1)
        if not soname or not raw_path.startswith("/"):
            parser.error("--resolved must be SONAME=absolute-path")
        resolved_paths.append((soname, Path(raw_path)))
    if roots and (search_roots or resolved_paths):
        parser.error("--root and --search-root are separate modes; use --root only for isolated tests")
    for directory in scan(args.readelf, roots, search_roots, args.max_depth,
                          seeds or None, resolved_paths, set(args.unresolved)):
        print(directory)
    return 0


if __name__ == "__main__":
    sys.exit(main())
