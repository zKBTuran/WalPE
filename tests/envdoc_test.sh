#!/bin/sh
# envdoc_test.sh <source dir>
# Every environment variable of WalPE's own that a test program or a check
# script reads has a row in docs/ENVIRONMENT.md. Three test hooks had none
# (WALPE_PKGSOUND_STRESS, SCENEVIDEOCHECK_EXITS, WEBCHECK_PATIENCE): whoever
# ran a test under a memory checker or on a slow machine had to find the
# knob in the source.
#   read:   g_getenv ("NAME") / getenv ("NAME") in tests/*.c,
#           os.environ.get("NAME" / os.environ["NAME"] /
#           os.environ.setdefault("NAME" in scripts/*.py
#   own:    names that begin with WALPE_, and the knobs of single tests
#           without that prefix (NAMECHECK_..., ..._TEST_..., WEBSEC_...).
#           What belongs to the system (XDG_*, DISPLAY, LANG ...) is not
#           looked at; the fuzz targets (tests/fuzz) have their own notes.
# No display, no daemon, nothing is built.
S=$1
doc="$S/docs/ENVIRONMENT.md"
[ -f "$doc" ] || { echo "envdoc: $doc is not there"; exit 1; }
names=$( { grep -ohE 'getenv \("[A-Z][A-Z0-9_]*"' "$S"/tests/*.c
           grep -ohE 'environ(\.get|\.setdefault)?[[(]"[A-Z][A-Z0-9_]*"' "$S"/scripts/*.py; } |
         sed 's/.*"\([A-Z][A-Z0-9_]*\)"/\1/' | sort -u |
         grep -E '^WALPE_|^[A-Z]+CHECK_|_TEST_|^WEBSEC_')
fail=0; checks=0
for n in $names; do
    checks=$((checks + 1))
    if ! grep -qE "(^|[^A-Z0-9_])$n([^A-Z0-9_]|\$)" "$doc"; then
        echo "FAIL $n is read by a test or a check script and docs/ENVIRONMENT.md does not name it"
        fail=$((fail + 1))
    fi
done
# the scan itself found the names (a pattern that matches nothing passes everything)
[ "$checks" -ge 20 ] || { echo "FAIL only $checks names found: the scan no longer reads the sources"; fail=$((fail + 1)); }
echo "envdoc: $checks names, $fail failed"
[ "$fail" = 0 ]
