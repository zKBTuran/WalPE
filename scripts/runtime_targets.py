"""Small helpers for the installed ELF targets reported by Meson."""

from __future__ import annotations

import json
import os
import subprocess
from pathlib import Path
from typing import Any, List

LINKABLE_INSTALLED_TYPES = {"executable", "shared library", "shared module"}


def installed_linkable_outputs(meson: str, build_dir: Path) -> List[Path]:
    """Return installed executable/module output paths from Meson introspection."""
    build_dir = Path(os.path.abspath(str(build_dir)))
    if not build_dir.is_dir():
        raise RuntimeError("build directory does not exist: %s" % build_dir)

    try:
        result = subprocess.run(
            [meson, "introspect", str(build_dir), "--targets"],
            capture_output=True,
            text=True,
            check=False,
        )
    except OSError as error:
        raise RuntimeError("could not run Meson introspection: %s" % error) from error
    if result.returncode != 0:
        raise RuntimeError("Meson target introspection failed: %s" % result.stderr.strip())

    try:
        targets: Any = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise RuntimeError("Meson returned invalid target JSON: %s" % error) from error
    if not isinstance(targets, list):
        raise RuntimeError("Meson target response is not a list")

    outputs: List[Path] = []
    seen = set()
    for target in targets:
        if not isinstance(target, dict):
            continue
        if target.get("installed") is not True:
            continue
        if target.get("type") not in LINKABLE_INSTALLED_TYPES:
            continue
        filenames = target.get("filename", [])
        if isinstance(filenames, str):
            filenames = [filenames]
        if not isinstance(filenames, list):
            continue

        for filename in filenames:
            if not isinstance(filename, str) or not filename:
                continue
            path = Path(filename)
            if not path.is_absolute():
                path = build_dir / path
            path = Path(os.path.abspath(str(path)))
            try:
                path.relative_to(build_dir)
            except ValueError:
                # Meson-generated build outputs should remain under build_dir.
                continue
            if path not in seen:
                seen.add(path)
                outputs.append(path)
    return outputs
