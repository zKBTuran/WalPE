#!/usr/bin/env python3
"""Remove installed ELF target outputs so Ninja relinks against current DSOs."""

from __future__ import annotations

import sys
from pathlib import Path

from runtime_targets import installed_linkable_outputs


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: force-relink-installed.py MESON BUILDDIR", file=sys.stderr)
        return 2
    build_dir = Path(sys.argv[2]).absolute()
    try:
        outputs = installed_linkable_outputs(sys.argv[1], build_dir)
    except RuntimeError as error:
        print("relink: %s" % error, file=sys.stderr)
        return 1

    removed = 0
    for path in outputs:
        if path.is_file() or path.is_symlink():
            path.unlink()
            removed += 1
            print("relink: queued %s" % path.relative_to(build_dir))
    print("relink: removed %d installed executable/module output(s) for a fresh link" % removed)
    return 0


if __name__ == "__main__":
    sys.exit(main())
