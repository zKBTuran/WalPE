#!/usr/bin/env python3
"""Prints the key of a shader statement for the recogniser tables in src/engine/fx_keys.h.

  python3 -I tools/dev/fxkey.py 'offset = saturate(x);' ['another statement;' ...]
  python3 -I tools/dev/fxkey.py --file shader.frag        every statement of a file
  python3 -I tools/dev/fxkey.py --find 0x0123456789abcdef --len 48 shader.frag
                                                          where a key of the table matches

The engine never stores the text of a stock shader: it compares keys. A key is made from the
statement with all white space removed (the engine looks at compiled code in that form):
its length, a rolling fingerprint (for the search) and a 64-bit FNV-1a hash (with the basis
src/engine/fx_region.h uses for its pinned shaders, which is not the textbook one). Type the
statement as the shader has it, or the part of it the recogniser looks for (a key may be the
start of a statement or several statements in a row); the output is the table row.

--file cuts a file into statements at ';', '{' and '}' after removing comments, preprocessor
lines and white space. That is cruder than the engine (which also drops dead #if branches):
use it to look up statements, not to prove what the engine sees.
"""
import re, sys

FNV_BASIS, FNV_PRIME = 1469598103934665603, 1099511628211
ROLL_BASE = 16777619
M64, M32 = (1 << 64) - 1, (1 << 32) - 1


def strip(text):
    return "".join(ch for ch in text if ch not in " \t\n\r\f\v")


def fnv1a(data):
    h = FNV_BASIS
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & M64
    return h


def roll(data):
    r = 0
    for b in data:
        r = (r * ROLL_BASE + b) & M32
    return r


def key(text):
    data = strip(text).encode("utf-8")
    return len(data), roll(data), fnv1a(data)


def row(text):
    n, r, h = key(text)
    return "{ %d, 0x%08xu, 0x%016xull }" % (n, r, h)


def statements(src):
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    lines = [re.sub(r"//.*", "", ln) for ln in src.split("\n")]
    code = "\n".join(ln for ln in lines if not ln.lstrip().startswith("#"))
    out, cur = [], []
    for ch in strip(code):
        cur.append(ch)
        if ch in ";{}":
            out.append("".join(cur))
            cur = []
    if cur:
        out.append("".join(cur))
    return [s for s in out if len(s) > 1]


def main(argv):
    if not argv or argv[0] in ("-h", "--help"):
        sys.stdout.write(__doc__)
        return 0 if argv else 2
    if argv[0] == "--file" and len(argv) == 2:
        for s in statements(open(argv[1], encoding="utf-8", errors="replace").read()):
            print("%s   /* %s */" % (row(s), s if len(s) <= 60 else s[:57] + "..."))
        return 0
    if argv[0] == "--find" and len(argv) == 5 and argv[2] == "--len":
        want, n = int(argv[1], 0), int(argv[3], 0)
        data = strip(open(argv[4], encoding="utf-8", errors="replace").read()).encode("utf-8")
        hits = [i for i in range(0, len(data) - n + 1) if fnv1a(data[i:i + n]) == want]
        for i in hits:
            print("offset %d (white space removed): %s" % (i, data[i:i + n].decode("utf-8", "replace")))
        print("%d place(s)" % len(hits))
        return 0 if hits else 1
    for a in argv:
        print(row(a))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
