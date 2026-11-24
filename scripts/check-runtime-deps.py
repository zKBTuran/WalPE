#!/usr/bin/env python3
"""Audit runtime dependencies of every installed executable and shared module."""

from __future__ import annotations

import re
import shutil
import subprocess
import sys
from pathlib import Path

from runtime_targets import installed_linkable_outputs

MISSING_RE = re.compile(r"^\s*(\S+)\s+=>\s+not found\s*$", re.MULTILINE)


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: check-runtime-deps.py MESON BUILDDIR", file=sys.stderr)
        return 2
    meson = sys.argv[1]
    build_dir = Path(sys.argv[2]).absolute()
    ldd = shutil.which("ldd")
    if not ldd:
        print("runtime-deps: ldd unavailable; skipped")
        return 0

    try:
        outputs = installed_linkable_outputs(meson, build_dir)
    except RuntimeError as error:
        print("runtime-deps: %s" % error, file=sys.stderr)
        return 1

    checked = 0
    failed = 0
    for path in outputs:
        if not path.is_file():
            print("runtime-deps: expected installed target output is missing: %s" % path,
                  file=sys.stderr)
            failed += 1
            continue
        try:
            result = subprocess.run([ldd, str(path)], capture_output=True, text=True, check=False)
        except OSError as error:
            print("runtime-deps: could not inspect %s: %s" % (path, error), file=sys.stderr)
            failed += 1
            continue
        checked += 1
        missing = MISSING_RE.findall(result.stdout + result.stderr)
        if missing:
            print("runtime-deps: unresolved dependencies in %s:" % path, file=sys.stderr)
            for name in missing:
                print("  %s" % name, file=sys.stderr)
            failed += 1
        elif result.returncode != 0:
            print("runtime-deps: ldd could not inspect %s: %s" %
                  (path, (result.stderr or result.stdout).strip()), file=sys.stderr)
            failed += 1

    if failed:
        print("runtime-deps: %d problem(s) across %d installed ELF outputs" %
              (failed, checked), file=sys.stderr)
        return 1
    print("runtime-deps: all %d installed ELF outputs have resolvable shared dependencies" % checked)
    return 0


if __name__ == "__main__":
    sys.exit(main())
