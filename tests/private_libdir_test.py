#!/usr/bin/env python3
"""Test wildcard discovery and inherited RPATH for a versioned private DSO."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(command, check=False, text=True, capture_output=True, **kwargs)
    if result.returncode != 0:
        raise RuntimeError("command failed: %r\n%s\n%s" % (command, result.stdout, result.stderr))
    return result


def main() -> int:
    if len(sys.argv) < 3:
        print("usage: private_libdir_test.py READelf CC [CC_ARGS...]", file=sys.stderr)
        return 2
    readelf = sys.argv[1]
    compiler = sys.argv[2:]

    with tempfile.TemporaryDirectory(prefix="walpe-private-libdir-") as temporary:
        root = Path(temporary) / "lib"
        private_dir = root / "private-99.7"
        decoy_dir = root / "private-decoy"
        private_dir.mkdir(parents=True)
        decoy_dir.mkdir(parents=True)
        private_source = Path(temporary) / "private.c"
        parent_source = Path(temporary) / "parent.c"
        main_source = Path(temporary) / "main.c"
        audit_source = Path(temporary) / "audit.c"
        private_source.write_text(
            "int walpe_private_test_symbol(void) { return 7; }\n"
        )
        parent_source.write_text(
            "extern int walpe_private_test_symbol(void);\n"
            "int walpe_parent_test_symbol(void) { return walpe_private_test_symbol(); }\n"
        )
        main_source.write_text(
            "extern int walpe_parent_test_symbol(void);\n"
            "int main(void) { return walpe_parent_test_symbol(); }\n"
        )
        audit_source.write_text("int main(void) { return 0; }\n")

        private_library = private_dir / "libwalpe-private-99.7.so"
        decoy_library = decoy_dir / "libwalpe-private-99.7.so"
        parent_library = root / "libwalpe-parent.so.1"
        executable = root / "walpe-rpath-test"

        run(compiler + [
            "-fPIC", "-shared", "-Wl,-soname,libwalpe-private-99.7.so",
            "-o", str(private_library), str(private_source),
        ])
        # Same filename, deliberately wrong SONAME: wildcard discovery must
        # reject it instead of selecting a merely similar versioned file.
        run(compiler + [
            "-fPIC", "-shared", "-Wl,-soname,libwalpe-wrong.so.1",
            "-o", str(decoy_library), str(private_source),
        ])
        run(compiler + [
            "-fPIC", "-shared", "-Wl,-soname,libwalpe-parent.so.1",
            "-Wl,--no-as-needed", "-L" + str(private_dir),
            "-Wl,-l:libwalpe-private-99.7.so", "-o", str(parent_library),
            str(parent_source),
        ])

        scanner = Path(__file__).resolve().parents[1] / "scripts" / "discover-private-libdirs.py"
        scan = run([
            sys.executable, str(scanner), "--readelf", readelf,
            "--root", str(root),
            # Simulate a private directory that appears in ld.so.conf or has
            # an unrelated cache entry, while the requested versioned SONAME
            # itself is not in ld.so.cache. Discovery must still add its RPATH.
            "--root", str(private_dir),
            "--seed", str(parent_library),
        ])
        discovered = [line for line in scan.stdout.splitlines() if line]
        if discovered != [str(private_dir)]:
            raise RuntimeError(
                "expected private DSO directory %s; discovered %r" % (private_dir, discovered)
            )

        # Exercise the real build helper with a linked WalPE-like executable:
        # RUNPATH finds the direct parent DSO, but is not inherited for its
        # versioned private child. The helper must discover both directories
        # from this app's dependency graph and persist the private one.
        update_build = root / "update-build"
        update_build.mkdir()
        discovery_executable = update_build / "walpe-discovery-test"
        run(compiler + [
            "-o", str(discovery_executable), str(main_source),
            "-L" + str(root), "-Wl,--no-as-needed",
            "-Wl,-l:libwalpe-parent.so.1", "-Wl,-rpath," + str(root),
            "-Wl,-rpath-link," + str(private_dir),
        ])
        fake_meson_update = Path(temporary) / "fake-meson-update"
        fake_meson_update.write_text(
            "#!/usr/bin/env python3\n"
            "import json\n"
            "print(json.dumps(["
            "{'type':'executable','installed':True,'filename':[" + repr(str(discovery_executable)) + "]}"
            "]))\n"
        )
        fake_meson_update.chmod(0o755)
        update_script = Path(__file__).resolve().parents[1] / "scripts" / "update-private-runtime-dirs.py"
        update = run([sys.executable, str(update_script), str(fake_meson_update),
                      str(update_build), readelf])
        manifest = update_build / "private-runtime-dirs.txt"
        manifest_dirs = manifest.read_text().splitlines()
        if update.stdout.strip() != "changed" or str(private_dir) not in manifest_dirs:
            raise RuntimeError(
                "build helper did not discover/persist the nested versioned DSO: %r %r" %
                (update.stdout, manifest_dirs)
            )
        update_again = run([sys.executable, str(update_script), str(fake_meson_update),
                            str(update_build), readelf])
        if update_again.stdout.strip() != "unchanged":
            raise RuntimeError("build helper did not recognize stable dependency directories")

        # Reproduce the Gentoo PulseAudio case: ldd reports the exact same
        # SONAME as not found for the executable's direct DT_NEEDED, then
        # reports it as resolved through a dependency's own search path. A
        # successful indirect occurrence must not mask the unresolved direct
        # occurrence when the build helper decides whether to add an RPATH.
        duplicate_build = root / "duplicate-ldd-build"
        duplicate_build.mkdir()
        duplicate_executable = duplicate_build / "walpe-discovery-test"
        shutil.copy2(discovery_executable, duplicate_executable)
        fake_meson_duplicate = Path(temporary) / "fake-meson-duplicate"
        fake_meson_duplicate.write_text(
            "#!/usr/bin/env python3\n"
            "import json\n"
            "print(json.dumps(["
            "{'type':'executable','installed':True,'filename':[" + repr(str(duplicate_executable)) + "]}"
            "]))\n"
        )
        fake_meson_duplicate.chmod(0o755)
        fake_bin = Path(temporary) / "fake-bin"
        fake_bin.mkdir()
        fake_ldd = fake_bin / "ldd"
        fake_ldd.write_text(
            "#!" + sys.executable + "\n"
            "print('libwalpe-parent.so.1 => " + str(parent_library) + " (0x1)')\n"
            "print('libwalpe-private-99.7.so => not found')\n"
            "print('libwalpe-private-99.7.so => " + str(private_library) + " (0x2)')\n"
        )
        fake_ldd.chmod(0o755)
        fake_env = os.environ.copy()
        fake_env["PATH"] = str(fake_bin) + os.pathsep + fake_env.get("PATH", "")
        duplicate_update = run([
            sys.executable, str(update_script), str(fake_meson_duplicate),
            str(duplicate_build), readelf,
        ], env=fake_env)
        duplicate_manifest = duplicate_build / "private-runtime-dirs.txt"
        duplicate_dirs = duplicate_manifest.read_text().splitlines()
        if duplicate_update.stdout.strip() != "changed" or str(private_dir) not in duplicate_dirs:
            raise RuntimeError(
                "an indirect ldd resolution masked the unresolved direct SONAME: %r %r" %
                (duplicate_update.stdout, duplicate_dirs)
            )

        rpath = os.pathsep.join([str(root), duplicate_dirs[0]])
        run(compiler + [
            "-o", str(executable), str(main_source),
            "-L" + str(root), "-Wl,-l:libwalpe-parent.so.1",
            "-Wl,--disable-new-dtags", "-Wl,-rpath," + rpath,
        ])

        dynamic = run([readelf, "-dW", str(executable)]).stdout
        if "(RPATH)" not in dynamic or "(RUNPATH)" in dynamic:
            raise RuntimeError("expected inherited DT_RPATH on the test executable")

        environment = os.environ.copy()
        environment["LD_LIBRARY_PATH"] = ""
        result = subprocess.run([str(executable)], env=environment, check=False)
        if result.returncode != 7:
            raise RuntimeError(
                "transitive private DSO did not load through inherited RPATH (exit %d)"
                % result.returncode
            )

        # The post-build audit must recurse into nested Meson output dirs.
        audit_build = root / "audit-build"
        nested_output = audit_build / "nested"
        nested_output.mkdir(parents=True)
        audit_executable = nested_output / "walpe-audit-fixture"
        run(compiler + ["-o", str(audit_executable), str(audit_source)])
        check_script = Path(__file__).resolve().parents[1] / "scripts" / "check-runtime-deps.py"
        if shutil.which("ldd"):
            audit_meson = Path(temporary) / "fake-meson-audit"
            audit_meson.write_text(
                "#!/usr/bin/env python3\n"
                "import json\n"
                "print(json.dumps(["
                "{'type':'executable','installed':True,'filename':[" + repr(str(audit_executable)) + "]},"
                "{'type':'executable','installed':False,'filename':[" + repr(str(audit_executable)) + "]}"
                "]))\n"
            )
            audit_meson.chmod(0o755)
            audit = run([sys.executable, str(check_script), str(audit_meson), str(audit_build)])
            if "all 1 installed ELF outputs" not in audit.stdout:
                raise RuntimeError("runtime dependency audit did not inspect nested installed ELF output")

        # Re-link preparation removes only installed executable/module outputs
        # reported by Meson, leaving test helpers and out-of-tree paths alone.
        installed_output = audit_build / "walpe-installed-fixture"
        helper_output = audit_build / "test-helper"
        outside_output = Path(temporary) / "outside-output"
        installed_output.write_text("old executable\n")
        helper_output.write_text("keep test helper\n")
        outside_output.write_text("do not touch\n")
        fake_meson = Path(temporary) / "fake-meson"
        fake_meson.write_text(
            "#!/usr/bin/env python3\n"
            "import json, sys\n"
            "print(json.dumps(["
            "{'type':'executable','installed':True,'filename':[" + repr(str(installed_output)) + "]},"
            "{'type':'executable','installed':False,'filename':[" + repr(str(helper_output)) + "]},"
            "{'type':'shared module','installed':True,'filename':[" + repr(str(outside_output)) + "]}"
            "]))\n"
        )
        fake_meson.chmod(0o755)
        relink_script = Path(__file__).resolve().parents[1] / "scripts" / "force-relink-installed.py"
        relink = run([sys.executable, str(relink_script), str(fake_meson), str(audit_build)])
        if installed_output.exists():
            raise RuntimeError("relink preparation did not remove installed output")
        if not helper_output.exists() or not outside_output.exists():
            raise RuntimeError("relink preparation removed a non-installed or out-of-tree file")
        if "removed 1 installed" not in relink.stdout:
            raise RuntimeError("relink preparation did not report the removed output")

    print("private-library-discovery: wildcard SONAME, app-specific discovery, inherited RPATH, recursive audit, and relink passed")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, subprocess.CalledProcessError, RuntimeError) as error:
        print("private-library-discovery: %s" % error, file=sys.stderr)
        sys.exit(1)
