#!/usr/bin/env python3
"""Synthetic scene wallpapers (no Wallpaper Engine assets needed).

  python3 -I tools/dev/gensyn.py KIND OUTDIR

KIND: smoke | rain | rain4k | rainpersp | rainpersp4k | splash | stars. OUTDIR becomes a wallpaper
directory (project.json, scene.json, generated .tex files, particle systems) that
`walpe-ctl open OUTDIR` shows.
"""
import json, math, os, struct, sys

def tex_bytes(w, h, rgba, frames=None, fw=0, fh=0, flags=0):
    """TEXV0005/TEXI0001/TEXB0003 container, raw RGBA8888, optional TEXS0003 frame table.
    frames: list of (time, x, y, w, h)"""
    if frames:
        flags |= 4
    out = b'TEXV0005\0' + b'TEXI0001\0'
    out += struct.pack('<IIIIIII', 0, flags, w, h, w, h, 0)   # type, flags, full w/h, image w/h, unk
    out += b'TEXB0003\0' + struct.pack('<II', 1, 0xFFFFFFFF)   # image count, freeimage format (none)
    out += struct.pack('<I', 1)                                 # mips
    out += struct.pack('<IIIII', w, h, 0, len(rgba), len(rgba)) + bytes(rgba)
    if frames:
        out += b'TEXS0003\0' + struct.pack('<III', len(frames), fw, fh)
        for (t, x, y, ww, hh) in frames:
            out += struct.pack('<Ifffffff', 0, t, x, y, ww, 0.0, 0.0, hh)
    return out

def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    mode = 'wb' if isinstance(data, (bytes, bytearray)) else 'w'
    with open(path, mode) as f:
        f.write(data)

def jdump(path, obj):
    write(path, json.dumps(obj, indent=1))

def smoke_sheet(cols, rows, fs):
    """cols x rows frames of fs x fs: a soft puff plus a bar whose angle steps 20 deg per frame
    (so neighbouring frames differ visibly), white, alpha = shape."""
    n = cols * rows
    W, H = cols * fs, rows * fs
    px = bytearray(W * H * 4)
    for i in range(n):
        ang = math.radians(20.0 * i)
        ca, sa = math.cos(ang), math.sin(ang)
        grow = 0.45 + 0.5 * i / max(1, n - 1)        # the puff grows over the sequence
        ox, oy = (i % cols) * fs, (i // cols) * fs
        for y in range(fs):
            for x in range(fs):
                u = (x + 0.5) / fs * 2 - 1
                v = (y + 0.5) / fs * 2 - 1
                r = math.hypot(u, v)
                puff = max(0.0, 1.0 - r / grow)
                puff = puff * puff * (3 - 2 * puff) * 0.55
                # bar through the centre at angle ang
                d = abs(-sa * u + ca * v)
                along = abs(ca * u + sa * v)
                bar = 1.0 if (d < 0.07 and along < 0.8) else 0.0
                a = min(1.0, max(puff, bar * 0.95))
                o = ((oy + y) * W + ox + x) * 4
                px[o] = px[o + 1] = px[o + 2] = 255
                px[o + 3] = int(a * 255 + 0.5)
    frames = [(1.0 / 30, (i % cols) * fs, (i // cols) * fs, fs, fs) for i in range(n)]
    return W, H, px, frames

def streak_tex(w, h):
    """a soft vertical streak (rain drop texture), white with alpha"""
    px = bytearray(w * h * 4)
    for y in range(h):
        for x in range(w):
            u = (x + 0.5) / w * 2 - 1
            v = (y + 0.5) / h * 2 - 1
            a = max(0.0, 1 - abs(u)) ** 2 * max(0.0, 1 - v * v)
            o = (y * w + x) * 4
            px[o] = px[o + 1] = px[o + 2] = 255
            px[o + 3] = int(a * 255 + 0.5)
    return px

def dot_tex(n):
    px = bytearray(n * n * 4)
    for y in range(n):
        for x in range(n):
            u = (x + 0.5) / n * 2 - 1
            v = (y + 0.5) / n * 2 - 1
            a = max(0.0, 1 - math.hypot(u, v))
            a = a * a * (3 - 2 * a)
            o = (y * n + x) * 4
            px[o] = px[o + 1] = px[o + 2] = 255
            px[o + 3] = int(a * 255 + 0.5)
    return px

def scene(objects, w=1920, h=1080, clear="0.05 0.07 0.10"):
    return {
        "camera": {"center": "0 0 -1", "eye": "0 0 0", "up": "0 1 0"},
        "general": {"ambientcolor": "0.3 0.3 0.3", "bloom": False, "cameraparallax": False,
                    "clearcolor": clear, "orthogonalprojection": {"height": h, "width": w},
                    "skylightcolor": "0.3 0.3 0.3"},
        "objects": objects,
    }

def project(title):
    return {"file": "scene.json", "title": title, "type": "scene",
            "general": {"properties": {"schemecolor": {"order": 0, "text": "ui_browse_properties_scheme_color",
                                                         "type": "color", "value": "0 0 0"}}}}

def pobj(i, name, particle, origin, extra=None):
    o = {"angles": "0 0 0", "id": i, "name": name, "origin": origin, "parallaxDepth": "1 1",
         "particle": particle, "scale": "1 1 1", "visible": True}
    if extra:
        o.update(extra)
    return o

def gen_smoke(out):
    """sprite-sheet smoke: one frozen particle (A/B of the frame blend) + drifting puffs"""
    W, H, px, frames = smoke_sheet(8, 8, 96)
    write(f"{out}/materials/particle/synsmoke.tex", tex_bytes(W, H, px, frames, 96, 96))
    jdump(f"{out}/materials/particle/synsmoke.json",
          {"passes": [{"shader": "genericparticle", "blending": "translucent", "depthtest": "disabled",
                       "depthwrite": "disabled", "cullmode": "nocull", "textures": ["particle/synsmoke"]}]})
    # one particle, 640 s: 10 s per frame, so t = 15 s is half way from frame 1 to frame 2
    single = {"emitter": [{"name": "boxrandom", "id": 1, "distancemax": "0 0 0", "instantaneous": 1, "rate": 0}],
              "initializer": [{"name": "lifetimerandom", "id": 2, "min": 640, "max": 640},
                              {"name": "sizerandom", "id": 3, "min": 1200, "max": 1200},
                              {"name": "alpharandom", "id": 4, "min": 1, "max": 1}],
              "operator": [], "renderer": [{"name": "sprite", "id": 5}],
              "material": "materials/particle/synsmoke.json", "maxcount": 1, "sequencemultiplier": 1}
    jdump(f"{out}/particles/single.json", single)
    noblend = dict(single); noblend["flags"] = 2
    jdump(f"{out}/particles/single_noblend.json", noblend)
    puffs = {"emitter": [{"name": "boxrandom", "id": 1, "distancemax": "120 10 0", "rate": 3}],
             "initializer": [{"name": "lifetimerandom", "id": 2, "min": 6, "max": 6},
                             {"name": "sizerandom", "id": 3, "min": 400, "max": 520},
                             {"name": "velocityrandom", "id": 4, "min": "-15 40 0", "max": "15 70 0"},
                             {"name": "alpharandom", "id": 5, "min": 0.5, "max": 0.7}],
             "operator": [{"name": "movement", "id": 6, "gravity": "0 0 0"},
                          {"name": "alphafade", "id": 7, "fadeintime": 0.1, "fadeouttime": 0.7}],
             "renderer": [{"name": "sprite", "id": 8}],
             "material": "materials/particle/synsmoke.json", "maxcount": 40, "starttime": 6}
    jdump(f"{out}/particles/puffs.json", puffs)
    objs = [pobj(1, "single", "particles/single.json", "480 620 0"),
            pobj(2, "single no frame blending", "particles/single_noblend.json", "1440 620 0"),
            pobj(3, "puffs", "particles/puffs.json", "960 120 0")]
    jdump(f"{out}/scene.json", scene(objs))
    jdump(f"{out}/project.json", project("syn smoke sheet"))

def gen_rain(out, w=1920, h=1080, persp=False):
    """rain as WE scenes build it: spritetrail, fast fall, default length / maxlength"""
    write(f"{out}/materials/particle/syndrop.tex", tex_bytes(16, 64, streak_tex(16, 64)))
    write(f"{out}/materials/particle/syndot.tex", tex_bytes(32, 32, dot_tex(32)))
    for n in ("syndrop", "syndot"):
        jdump(f"{out}/materials/particle/{n}.json",
              {"passes": [{"shader": "genericparticle", "blending": "additive", "depthtest": "disabled",
                           "depthwrite": "disabled", "cullmode": "nocull", "textures": [f"particle/{n}"]}]})
    def rain(tex, size, length=None, maxlength=None, rate=120, z=0):
        r = {"name": "spritetrail", "id": 9}
        if length is not None: r["length"] = length
        if maxlength is not None: r["maxlength"] = maxlength
        d = {"emitter": [{"name": "boxrandom", "id": 1, "distancemax": f"{w/8:.0f} 0 {z}",
                          "directions": "1 0 1", "origin": f"0 {h*0.45:.0f} 0", "rate": rate}],
             "initializer": [{"name": "lifetimerandom", "id": 2, "min": 1.0, "max": 1.0},
                             {"name": "sizerandom", "id": 3, "min": size, "max": size},
                             {"name": "velocityrandom", "id": 4, "min": "0 -1500 0", "max": "0 -1500 0"},
                             {"name": "alpharandom", "id": 5, "min": 0.8, "max": 0.8}],
             "operator": [{"name": "movement", "id": 6, "gravity": "0 0 0"}],
             "renderer": [r], "material": f"materials/particle/{tex}.json", "maxcount": 400, "starttime": 2}
        if persp:
            d["flags"] = 4
            d["emitter"][0]["distancemax"] = f"{w/8:.0f} 0 {h*0.4:.0f}"
        return d
    cols = [("a_dot_s10", rain("syndot", 10)), ("b_dot_s20", rain("syndot", 20)),
            ("c_drop_s10", rain("syndrop", 10)), ("d_drop_s20_max3", rain("syndrop", 20, 0.02, 3))]
    objs = []
    for k, (name, d) in enumerate(cols):
        jdump(f"{out}/particles/{name}.json", d)
        objs.append(pobj(k + 1, name, f"particles/{name}.json", f"{w*(k+0.5)/4:.0f} {h/2:.0f} 0"))
    jdump(f"{out}/scene.json", scene(objs, w, h))
    jdump(f"{out}/project.json", project("syn rain"))

def ring_tex(n):
    """a ring (rain splash / ripple), white with alpha"""
    px = bytearray(n * n * 4)
    for y in range(n):
        for x in range(n):
            u = (x + 0.5) / n * 2 - 1
            v = (y + 0.5) / n * 2 - 1
            a = max(0.0, 1 - abs(math.hypot(u, v) - 0.8) / 0.12)
            o = (y * n + x) * 4
            px[o] = px[o + 1] = px[o + 2] = 255
            px[o + 3] = int(a * 255 + 0.5)
    return px

def gen_splash(out):
    """one frozen ring per column: plain; world-space system on a 3x scaled object (the ring
    keeps its size in WE); tilted 60 degrees about x (WE: a flat ellipse, half as high);
    world-space + tilt on the scaled object (WE's rain_splashes on a scaled layer)"""
    write(f"{out}/materials/particle/synring.tex", tex_bytes(128, 128, ring_tex(128)))
    jdump(f"{out}/materials/particle/synring.json",
          {"passes": [{"shader": "genericparticle", "blending": "additive", "depthtest": "disabled",
                       "depthwrite": "disabled", "cullmode": "nocull", "textures": ["particle/synring"]}]})
    def ring(flags=0, rot=None):
        d = {"emitter": [{"name": "boxrandom", "id": 1, "distancemax": "0 0 0", "instantaneous": 1, "rate": 0}],
             "initializer": [{"name": "lifetimerandom", "id": 2, "min": 600, "max": 600},
                             {"name": "sizerandom", "id": 3, "min": 200, "max": 200},
                             {"name": "alpharandom", "id": 4, "min": 1, "max": 1}],
             "operator": [], "renderer": [{"name": "sprite", "id": 5}],
             "material": "materials/particle/synring.json", "maxcount": 1}
        if flags: d["flags"] = flags
        if rot: d["initializer"].append({"name": "rotationrandom", "id": 6, "min": rot, "max": rot})
        return d
    cols = [("a_plain", ring(), "1 1 1"), ("b_worldspace_scale3", ring(1), "3 3 3"),
            ("c_tilt_x60", ring(0, "1.047 0 0"), "1 1 1"),
            ("d_worldspace_scale3_tilt", ring(1, "1.047 0 0"), "3 3 3")]
    objs = []
    for k, (name, d, sc) in enumerate(cols):
        jdump(f"{out}/particles/{name}.json", d)
        objs.append(pobj(k + 1, name, f"particles/{name}.json", f"{1920*(k+0.5)/4:.0f} 540 0", {"scale": sc}))
    jdump(f"{out}/scene.json", scene(objs))
    jdump(f"{out}/project.json", project("syn splash rings"))

def gen_stars(out):
    """perspective star field: particles fly at the camera (velocity along z only), drawn as
    sprite trails - the streaks must point away from the centre and grow towards the edge"""
    write(f"{out}/materials/particle/syndot.tex", tex_bytes(32, 32, dot_tex(32)))
    jdump(f"{out}/materials/particle/syndot.json",
          {"passes": [{"shader": "genericparticle", "blending": "additive", "depthtest": "disabled",
                       "depthwrite": "disabled", "cullmode": "nocull", "textures": ["particle/syndot"]}]})
    d = {"emitter": [{"name": "boxrandom", "id": 1, "distancemax": "900 500 0", "origin": "0 0 -1500", "rate": 150}],
         "initializer": [{"name": "lifetimerandom", "id": 2, "min": 1.2, "max": 1.2},
                         {"name": "sizerandom", "id": 3, "min": 8, "max": 14},
                         {"name": "velocityrandom", "id": 4, "min": "0 0 1500", "max": "0 0 2000"},
                         {"name": "alpharandom", "id": 5, "min": 1, "max": 1}],
         "operator": [{"name": "movement", "id": 6, "gravity": "0 0 0"}],
         "renderer": [{"name": "spritetrail", "id": 7, "length": 0.08, "maxlength": 20}],
         "material": "materials/particle/syndot.json", "maxcount": 400, "starttime": 3, "flags": 4}
    jdump(f"{out}/particles/stars.json", d)
    jdump(f"{out}/scene.json", scene([pobj(1, "stars", "particles/stars.json", "960 540 0")]))
    jdump(f"{out}/project.json", project("syn star field"))

if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1].startswith("-"):
        sys.stderr.write(__doc__)
        sys.exit(2)
    kind, out = sys.argv[1], sys.argv[2]
    if kind == "smoke": gen_smoke(out)
    elif kind == "rain": gen_rain(out)
    elif kind == "rain4k": gen_rain(out, 3840, 2160)
    elif kind == "rainpersp": gen_rain(out, persp=True)
    elif kind == "rainpersp4k": gen_rain(out, 3840, 2160, persp=True)
    elif kind == "splash": gen_splash(out)
    elif kind == "stars": gen_stars(out)
    else:
        sys.stderr.write(__doc__)
        sys.exit(2)
