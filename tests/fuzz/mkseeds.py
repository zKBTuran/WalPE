#!/usr/bin/env python3
"""mkseeds.py - writes the seed corpus of the fuzz harnesses (tests/fuzz/seeds).

Every seed is made here from the formats' descriptions in the sources
(src/engine/pkg.c, src/common/tex.c, src/engine/mp4demux.c,
src/engine/puppet.c, ...): small valid files, one per feature a parser
knows. Nothing is taken from a wallpaper.

    python3 tests/fuzz/mkseeds.py [OUTDIR]      (default: tests/fuzz/seeds)
"""
import json
import os
import struct
import sys
import zlib

out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "seeds")


def put(target, name, data):
    d = os.path.join(out, target)
    os.makedirs(d, exist_ok=True)
    if isinstance(data, str):
        data = data.encode()
    with open(os.path.join(d, name), "wb") as f:
        f.write(data)


def u32(*v):
    return b"".join(struct.pack("<I", x) for x in v)


def be32(*v):
    return b"".join(struct.pack(">I", x) for x in v)


def f32(*v):
    return b"".join(struct.pack("<f", x) for x in v)


# ---------------------------------------------------------------- pictures

def png(w, h, rgba=(200, 100, 50, 255)):
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    raw = b"".join(b"\0" + bytes(rgba) * w for _ in range(h))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


GIF_FRAME = b"!\xf9\x04\x00\x05\x00\x00\x00,\x00\x00\x00\x00\x01\x00\x01\x00\x00\x02\x02D\x01\x00"
GIF_HEAD = b"GIF89a\x01\x00\x01\x00\x80\x00\x00\xff\xff\xff\x00\x00\x00"
gif1 = GIF_HEAD + GIF_FRAME + b";"
gif2 = GIF_HEAD + b"!\xff\x0bNETSCAPE2.0\x03\x01\x00\x00\x00" + GIF_FRAME * 3 + b";"
bmp = (b"BM" + u32(54 + 16, 0, 54) + u32(40, 2, 2) + struct.pack("<HH", 1, 32) + u32(0, 16, 2835, 2835, 0, 0) +
       bytes(range(16)))
# a baseline JPEG header (SOI, SOF0 16x16 with three components, SOS): what the scanners read
jpeg = (b"\xff\xd8\xff\xe0\x00\x10JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00" +
        b"\xff\xc0\x00\x11\x08\x00\x10\x00\x10\x03\x01\x22\x00\x02\x11\x01\x03\x11\x01" +
        b"\xff\xda\x00\x0c\x03\x01\x00\x02\x11\x03\x11\x00\x3f\x00" + b"\x00" * 8 + b"\xff\xd9")
jpeg_prog = jpeg.replace(b"\xff\xc0", b"\xff\xc2")

for name, data in (("one.gif", gif1), ("three-frames.gif", gif2), ("small.png", png(3, 2)), ("small.bmp", bmp),
                   ("baseline.jpg", jpeg), ("progressive.jpg", jpeg_prog)):
    put("gif", name, data)

# ------------------------------------------------------------------- .tex

def lz4_literals(d):
    """an LZ4 block that stores d as literals"""
    n = len(d)
    o = bytes([min(n, 15) << 4])
    if n >= 15:
        n -= 15
        o += b"\xff" * (n // 255) + bytes([n % 255])
    return o + d


def tex(fmt, iw, ih, levels, flags=0, ver=2, tail=b"", images=1):
    """levels: [(w, h, compressed, payload)] of one image"""
    o = b"TEXV0005\0TEXI0001\0" + u32(fmt, flags, levels[0][0], levels[0][1], iw, ih, 0)
    o += ("TEXB000%d" % ver).encode() + b"\0" + u32(images)
    if ver == 3:
        o += u32(0)
    if ver == 4:
        o += u32(0, 0)
    for _ in range(images):
        o += u32(len(levels))
        for (w, h, comp, payload) in levels:
            if ver == 1:
                o += u32(w, h, len(payload)) + payload
            elif comp:
                packed = lz4_literals(payload)
                o += u32(w, h, 1, len(payload), len(packed)) + packed
            else:
                o += u32(w, h, 0, len(payload), len(payload)) + payload
    return o + tail


def rgba(w, h):
    return bytes((x * 7 + y * 13 + c * 50) & 255 for y in range(h) for x in range(w) for c in range(4))


def dxt(w, h, bs):
    return bytes((i * 37 + 11) & 255 for i in range(((w + 3) // 4) * ((h + 3) // 4) * bs))


def chain(w, h, make, comp=0):
    lv = []
    while True:
        lv.append((w, h, comp, make(w, h)))
        if w == 1 and h == 1:
            return lv
        w, h = max(1, w // 2), max(1, h // 2)


def frames(n, w, h, ver=3):
    o = ("TEXS000%d" % ver).encode() + b"\0" + u32(n)
    if ver >= 3:
        o += u32(w, h)
    for i in range(n):
        o += u32(0) + f32(0.1, i * w, 0, w, 0, 0, h)
    return o


put("tex", "rgba-raw.tex", tex(0, 4, 4, [(4, 4, 0, rgba(4, 4))]))
put("tex", "rgba-raw-v1.tex", tex(0, 4, 4, [(4, 4, 0, rgba(4, 4))], ver=1))
put("tex", "rgba-lz4-mips.tex", tex(0, 8, 8, chain(8, 8, rgba, 1), ver=3))
put("tex", "rgba-padded.tex", tex(0, 6, 3, chain(8, 4, rgba), ver=4))
put("tex", "dxt1-mips.tex", tex(7, 16, 16, chain(16, 16, lambda w, h: dxt(w, h, 8))))
put("tex", "dxt5-lz4-mips.tex", tex(4, 16, 8, chain(16, 8, lambda w, h: dxt(w, h, 16), 1), ver=3))
put("tex", "dxt3-padded.tex", tex(6, 10, 6, chain(16, 8, lambda w, h: dxt(w, h, 16))))
put("tex", "r8.tex", tex(9, 8, 8, chain(8, 8, lambda w, h: bytes(w * h))))
put("tex", "rg88-lz4.tex", tex(8, 5, 8, [(8, 8, 1, bytes(range(128)))], ver=3))
put("tex", "sprites.tex", tex(0, 8, 4, [(8, 4, 0, rgba(8, 4))], flags=4, ver=3, tail=frames(2, 4, 4)))
put("tex", "sprites-v2.tex", tex(0, 8, 4, [(8, 4, 1, rgba(8, 4))], flags=4, tail=frames(2, 4, 4, 2)))
put("tex", "png-inside.tex", tex(0, 3, 2, [(4, 4, 0, png(3, 2))], ver=3))
put("tex", "two-images.tex", tex(0, 4, 4, [(4, 4, 0, rgba(4, 4))], flags=4, images=2, tail=frames(2, 4, 4)))
put("tex", "plain.png", png(3, 2))
put("tex", "plain.gif", gif1)
put("tex", "plain.bmp", bmp)

# ------------------------------------------------------------------- .mp4

def box(kind, body):
    return be32(8 + len(body)) + kind + body


def mp4(count=3, fixed=0, timescale=600, extra=b"", co64=False, sync=False, ctts=False):
    avcc = bytes([1, 0x64, 0, 0x1f, 0xff, 0xe1, 0, 4, 0x67, 0x64, 0, 0x1f, 1, 0, 2, 0x68, 0xee])
    entry = bytearray(78)
    entry[25] = 16
    entry[27] = 16
    entry = bytes(entry) + box(b"avcC", avcc) + box(b"pasp", be32(4, 3))
    stbl = box(b"stsd", be32(0, 1) + box(b"avc1", entry))
    stbl += box(b"stts", be32(0, 1, count, 10))
    stbl += box(b"stsc", be32(0, 1, 1, count, 1))
    stbl += box(b"stsz", be32(0, fixed, count) + (b"" if fixed else be32(*([8] * count))))
    stbl += box(b"co64", be32(0, 1) + struct.pack(">Q", 0)) if co64 else box(b"stco", be32(0, 1, 0))
    if sync:
        stbl += box(b"stss", be32(0, 1, 1))
    if ctts:
        stbl += box(b"ctts", be32(0, 1, count, 5))
    mdhd = bytearray(24)
    mdhd[12:16] = be32(timescale)
    mdhd[16:20] = be32(count * 10)
    tkhd = bytearray(84)
    tkhd[40:76] = be32(0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000)
    trak = box(b"tkhd", bytes(tkhd)) + box(b"mdia", box(b"mdhd", bytes(mdhd)) + box(b"hdlr", bytes(8) + b"vide" + bytes(13)) +
                                         box(b"minf", box(b"stbl", stbl)))
    return box(b"ftyp", b"isom\0\0\0\0isom") + box(b"moov", box(b"trak", trak)) + extra + bytes(64)


put("mp4", "three-samples.mp4", mp4())
put("mp4", "fixed-size.mp4", mp4(count=5, fixed=4))
put("mp4", "co64-sync-ctts.mp4", mp4(co64=True, sync=True, ctts=True))
put("mp4", "mdat-first.mp4", box(b"ftyp", b"isom\0\0\0\0isom") + box(b"mdat", bytes(40)) + mp4()[20:])
# the same clips with the four bytes the harness reads for its second, ranged open
put("mp4", "ranged.mp4", bytes([0, 0, 0, 0]) + mp4())
# a video texture: the MP4 as level 0 of a container
put("tex", "video.tex", tex(0, 16, 16, [(16, 16, 0, mp4())], flags=0x20, ver=3))

# ------------------------------------------------------------------- .pkg

def pkg(files, version=b"PKGV0016"):
    table = b""
    blob = b""
    for name, data in files:
        n = name.encode()
        table += u32(len(n)) + n + u32(len(blob), len(data))
        blob += data
    return u32(len(version)) + version + u32(len(files)) + table + blob


scene = {"camera": {"center": "0 0 -1", "eye": "0 0 0", "up": "0 1 0"},
         "general": {"orthogonalprojection": {"width": 64, "height": 64}, "clearcolor": "0 0 0"},
         "objects": [{"id": 1, "name": "a", "image": "models/a.json", "origin": "32 32 0", "scale": "1 1 1",
                      "angles": "0 0 0", "size": "64 64", "visible": True}]}
put("pkg", "scene.pkg", pkg([("scene.json", json.dumps(scene).encode()),
                             ("models/a.json", b'{"material":"materials/a.json"}'),
                             ("materials/a.json", b'{"passes":[{"shader":"genericimage2","textures":["a"]}]}'),
                             ("materials/a.tex", tex(0, 4, 4, [(4, 4, 0, rgba(4, 4))]))]))
put("pkg", "empty.pkg", pkg([]))
put("pkg", "odd-names.pkg", pkg([("a//b/./c.txt", b"x"), ("../up", b"y"), ("/abs", b"z"), (".walpe-pkg-stamp", b"s"),
                                 ("dir/", b""), ("d/e/f/g/h/i.bin", bytes(40))], version=b"PKGV0001"))

# -------------------------------------------------------------- puppet .mdl

def cstr(s):
    return s.encode() + b"\0"


def mdl(nverts=4, nbones=2, nanims=1, nframes=2, attach=True):
    o = b"MDLV0013\0" + u32(1, 1, 1) + cstr("materials/a.json") + u32(0)
    verts = b""
    for i in range(nverts):
        x, y = (i % 2) * 2.0 - 1.0, (i // 2) * 2.0 - 1.0
        verts += f32(x, y, 0) + f32(0, 0, 1) + f32(1, 0, 0, 1) + u32(i % max(nbones, 1), 0, 0, 0) + f32(1, 0, 0, 0) + \
            f32((x + 1) / 2, (y + 1) / 2)
    idx = struct.pack("<6H", 0, 1, 2, 2, 1, 3)
    o += u32(0x0180000f, len(verts)) + verts + u32(len(idx)) + idx
    bones = b""
    for b in range(nbones):
        m = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, float(b), 0, 0, 1]
        bones += cstr("") + u32(0, (b - 1) & 0xffffffff, 64) + f32(*m) + cstr("{}")
    body = struct.pack("<HH", nbones, 0) + bones
    o += b"MDLS0003\0" + u32(len(o) + 9 + 4 + len(body)) + body
    if attach:
        o += b"MDAT0001\0" + u32(0) + struct.pack("<H", 1) + struct.pack("<H", nbones - 1) + cstr("hand") + \
            f32(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 3, 4, 0, 1)
    o += b"MDLA0005\0" + u32(0, nanims)
    for a in range(nanims):
        o += u32(a + 1, 0) + cstr("idle%d" % a) + cstr(("loop", "mirror", "single")[a % 3]) + f32(30.0) + \
            u32(nframes, 0, nbones)
        for b in range(nbones):
            fr = b"".join(f32(float(k), 0, 0, 0, 0, 0.1 * k, 1, 1, 1) for k in range(nframes))
            o += u32(0, len(fr)) + fr
        o += u32(0)
    return o


put("puppet", "two-bones.mdl", mdl())
put("puppet", "three-anims.mdl", mdl(nverts=8, nbones=3, nanims=3, nframes=4))
put("puppet", "no-bones.mdl", mdl(nbones=0, nanims=0, attach=False))

# ------------------------------------------------------------------- JSON

put("json", "scene.json", json.dumps(scene))
put("json", "bom-trailing-comma.json", b"\xef\xbb\xbf" + b'{"a":[1,2,],"b":{"c":"1 2 3",},}')
put("json", "two-values.json", '{"a":1} {"b":2}')
put("json", "comment.json", '// c\n{"a":/* x */1,"s":"\\u00e9\\n"}')
put("json", "numbers.json", '{"a":1e999,"b":-0,"c":[1.5,"2 3"],"d":null,"e":true,"f":{"user":"x","value":2}}')

particle = {
    "emitter": [{"name": "sphererandom", "rate": 20, "distancemax": 10, "distancemin": 0, "origin": "0 0 0",
                 "directions": "1 1 0", "id": 1}],
    "initializer": [{"name": "lifetimerandom", "min": 0.5, "max": 1}, {"name": "sizerandom", "min": 2, "max": 8},
                    {"name": "velocityrandom", "min": "-10 -10 0", "max": "10 10 0"},
                    {"name": "colorrandom", "min": "0 0 0", "max": "255 255 255"},
                    {"name": "alpharandom", "min": 0.2, "max": 1}, {"name": "rotationrandom"},
                    {"name": "angularvelocityrandom", "min": "0 0 -1", "max": "0 0 1"}],
    "operator": [{"name": "movement", "gravity": "0 -10 0", "drag": 0.1}, {"name": "alphafade", "fadeintime": 0.1},
                 {"name": "sizechange", "startvalue": 1, "endvalue": 0}, {"name": "colorchange"},
                 {"name": "angularmovement"}, {"name": "oscillatealpha"}, {"name": "oscillatesize"},
                 {"name": "oscillateposition"}, {"name": "turbulence"}, {"name": "vortex"},
                 {"name": "controlpointattract", "controlpoint": 1, "scale": 100}],
    "renderer": [{"name": "sprite"}],
    "controlpoint": [{"id": 1, "offset": "10 0 0", "flags": 1}],
    "material": "materials/p.json", "maxcount": 50, "starttime": 1, "sequencemultiplier": 1,
    "animationmode": "sequence", "flags": 0,
}
put("particle", "sprite.json", json.dumps(particle))
rope = dict(particle, renderer=[{"name": "ropetrail", "length": 2, "maxlength": 8, "subdivision": 2}],
            emitter=[{"name": "boxrandom", "rate": 5, "distancemax": "4 4 0", "instantaneous": 3}], maxcount=8)
put("particle", "ropetrail.json", json.dumps(rope))
put("particle", "trail.json", json.dumps(dict(particle, renderer=[{"name": "spritetrail", "length": 0.1, "maxlength": 4}])))

# ---------------------------------------------------------------- shaders

FRAG = """#include "common.h"
// [COMBO] {"material":"mode","combo":"MODE","type":"options","default":1}
varying vec4 v_TexCoord;
uniform float g_Time;
uniform sampler2D g_Texture0; // {"hidden":true}
uniform sampler2D g_Texture1; // {"material":"mask","combo":"MASK","default":"util/white"}
uniform float g_Speed; // {"material":"speed","default":2,"range":[0,10]}
uniform vec3 g_Tint; // {"material":"tint","type":"color","default":"1 0.5 0"}
void main() {
	vec4 c = texSample2D(g_Texture0, v_TexCoord.xy + frac(g_Time * g_Speed) * 0.01);
#if MASK
	c.a *= texSample2D(g_Texture1, v_TexCoord.zw).r;
#endif
	gl_FragColor = vec4(mix(c.rgb, g_Tint, 0.5), c.a);
}
"""
VERT = """uniform mat4 g_ModelViewProjectionMatrix;
attribute vec3 a_Position;
attribute vec2 a_TexCoord;
varying vec4 v_TexCoord;
void main() {
	gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);
	v_TexCoord = a_TexCoord.xyxy;
}
"""
put("shader", "tint.frag", FRAG)
put("shader", "tint-with-vertex.frag", FRAG + "//@vertex\n" + VERT)
put("shader", "loop.frag", """varying vec2 v_TexCoord;
uniform sampler2D g_Texture0;
uniform vec4 g_Texture0Resolution;
#define N 4
float f(float x) { return x * 2.0; }
void main() {
	vec4 s = vec4(0.0);
	for (int i = 0; i < N; i++)
		s += texSample2D(g_Texture0, v_TexCoord + vec2(float(i), 0.0) / g_Texture0Resolution.xy);
	int k = 3 % 2;
	float a[2];
	a[0] = f(s.r); a[1] = pow(s.g, 2);
	gl_FragColor = s / float(N) + a[k] * 0.0;
}
""")

# ---------------------------------------------------------------- scenes

def archive(files):
    """the scene harness's input: "=== name" lines in front of each file"""
    o = b""
    for name, data in files:
        if isinstance(data, str):
            data = data.encode()
        o += b"=== " + name.encode() + b"\n" + data + b"\n"
    return o


def effect_files(name, body, extra_uniforms=""):
    return [
        ("effects/%s/effect.json" % name, json.dumps({
            "name": name, "group": "t", "passes": [{"material": "materials/effects/%s.json" % name}],
            "fbos": [{"name": "_rt_half", "scale": 2, "format": "rgba8888"}]})),
        ("materials/effects/%s.json" % name, json.dumps({
            "passes": [{"shader": "effects/" + name, "blending": "normal", "depthtest": "disabled",
                        "depthwrite": "disabled", "cullmode": "nocull",
                        "constantshadervalues": {"speed": 3}, "combos": {"MODE": 1}}]})),
        ("shaders/effects/%s.vert" % name, VERT),
        ("shaders/effects/%s.frag" % name, FRAG.replace('#include "common.h"\n', extra_uniforms).replace(
            "\tgl_FragColor", body + "\tgl_FragColor")),
    ]


def obj(i, **kw):
    o = {"id": i, "name": "o%d" % i, "origin": "%d 32 0" % (8 * i), "scale": "1 1 1", "angles": "0 0 0",
         "size": "16 16", "visible": True}
    o.update(kw)
    return o


project = json.dumps({"file": "scene.json", "title": "seed", "type": "scene",
                      "general": {"properties": {"speed": {"type": "slider", "value": 2, "min": 0, "max": 10},
                                                 "on": {"type": "bool", "value": True},
                                                 "tint": {"type": "color", "value": "1 0 0"},
                                                 "words": {"type": "textinput", "value": "hello"}}}})
general = {"ambientcolor": "0.3 0.3 0.3", "bloom": True, "bloomstrength": 1, "bloomthreshold": 0.6,
           "cameraparallax": True, "cameraparallaxamount": 0.5, "cameraparallaxdelay": 0.1,
           "cameraparallaxmouseinfluence": 0.5, "camerashake": True, "camerashakeamplitude": 0.5,
           "camerashakeroughness": 1, "camerashakespeed": 3, "clearcolor": "0.1 0.2 0.3", "clearenabled": True,
           "orthogonalprojection": {"width": 64, "height": 64}, "zoom": 1}
camera = {"center": "0 0 -1", "eye": "0 0 0", "up": "0 1 0"}
image_files = [("models/a.json", '{"material":"materials/a.json","puppet":"models/a_puppet.mdl","width":8,"height":8}'),
               ("materials/a.json", '{"passes":[{"shader":"genericimage2","textures":["a"],"blending":"translucent"}]}'),
               ("materials/a.tex", tex(0, 8, 8, chain(8, 8, rgba, 1), ver=3)),
               ("models/a_puppet.mdl", mdl()),
               ("models/d.json", '{"material":"materials/d.json"}'),
               ("materials/d.json", '{"passes":[{"shader":"genericimage2","textures":["d"]}]}'),
               ("materials/d.tex", tex(4, 16, 16, chain(16, 16, lambda w, h: dxt(w, h, 16)))),
               ("materials/m.tex", tex(9, 8, 8, chain(8, 8, lambda w, h: bytes(range(w * h))))),
               ("models/solid.json", '{"solidlayer":true}'),
               ("models/pass.json", '{"passthrough":true,"fullscreen":true}')]

layers = [
    obj(1, image="models/solid.json", color="0.2 0.4 0.6", alpha=1.0, size="64 64", origin="32 32 0"),
    obj(2, image="models/a.json", alpha={"user": "speed", "value": 0.5}, parallaxDepth="1 1",
        colorBlendMode=2, animationlayers=[{"animation": 1, "rate": 1, "blend": 1, "visible": True}],
        effects=[{"file": "effects/fx/effect.json", "id": 20, "name": "fx", "visible": {"user": "on", "value": True},
                  "passes": [{"constantshadervalues": {"speed": {"user": "speed", "value": 1}, "tint": "0 1 0"},
                              "combos": {"MASK": 1}, "textures": [None, "a"]}]}]),
    obj(3, image="models/solid.json", parent=2, attachment="hand", color="1 1 1", alpha=0.5,
        effects=[{"file": "effects/fx/effect.json", "id": 30, "passes": [{}]},
                 {"file": "effects/fx2/effect.json", "id": 31, "passes": [{"textures": [None, "_rt_FullFrameBuffer"]}]}]),
    obj(4, particle="particles/p.json", instanceoverride={"count": 2, "rate": 0.5, "size": 2, "alpha": 0.5,
                                                         "colorn": "1 0 0", "controlpoint1": "1 2 3"}),
    obj(5, text={"user": "words", "value": "Hi 12:00"}, font="systemfont_arial", pointsize=12, size="32 16",
        horizontalalign="center", verticalalign="center", color="1 1 1", backgroundcolor="0 0 0", padding=2),
    obj(6, sound=["sounds/a.wav"], volume=0.5, playbackmode="loop", startsilent=False),
    obj(7, image="models/pass.json", effects=[{"file": "effects/fx/effect.json", "id": 70, "passes": [{}]}]),
    obj(8, light="point", color="1 1 1", radius=100, intensity=1),
    obj(9, image="models/d.json", alpha=0.75, color="1 0.5 0.25", brightness=1.5,
        effects=[{"file": "effects/fx/effect.json", "id": 90, "passes": [{"textures": [None, "m"],
                                                                         "combos": {"MASK": 1}}]}]),
]
wav = b"RIFF" + u32(36 + 16) + b"WAVEfmt " + u32(16) + struct.pack("<HHIIHH", 1, 1, 8000, 16000, 2, 16) + \
    b"data" + u32(16) + bytes(16)
scene_files = [("project.json", project),
               ("scene.json", json.dumps({"camera": camera, "general": general, "objects": layers}))] + image_files + \
    effect_files("fx", "\tc.rgb *= 0.5;\n") + effect_files("fx2", "\tc.a = min(1.0, c.a + 0.5 * sin(g_Time));\n") + \
    [("particles/p.json", json.dumps(dict(particle, children=[{"name": "particles/c.json", "type": "eventfollow",
                                                              "maxcount": 4}]))),
     ("particles/c.json", json.dumps(rope)),
     ("materials/p.json", '{"passes":[{"shader":"genericparticle","textures":["a"],"blending":"additive"}]}'),
     ("sounds/a.wav", wav)]
put("scene", "everything.scene", archive(scene_files))
put("scene", "solid-only.scene", archive([("project.json", project), ("models/solid.json", '{"solidlayer":true}'),
                                          ("scene.json", json.dumps({"camera": camera, "general": general,
                                                                     "objects": layers[:1]}))]))
put("scene", "image-effect.scene", archive([("project.json", project),
                                            ("scene.json", json.dumps({"camera": camera, "general": general,
                                                                       "objects": layers[1:3]}))] +
                                           image_files + effect_files("fx", "\tc.rgb = c.bgr;\n") +
                                           effect_files("fx2", "")))
put("scene", "particles.scene", archive([("project.json", project),
                                         ("scene.json", json.dumps({"camera": camera, "general": general,
                                                                    "objects": [layers[3]]}))] + scene_files[-4:] +
                                        image_files[1:3]))
put("scene", "sprite-sheet.scene", archive([
    ("project.json", project),
    ("scene.json", json.dumps({"camera": camera, "general": general, "objects": [obj(1, image="models/s.json")]})),
    ("models/s.json", '{"material":"materials/s.json"}'),
    ("materials/s.json", '{"passes":[{"shader":"genericimage2","textures":["s"]}]}'),
    ("materials/s.tex", tex(0, 8, 4, [(8, 4, 0, rgba(8, 4))], flags=4, ver=3, tail=frames(2, 4, 4)))]))
put("scene", "video-texture.scene", archive([
    ("project.json", project),
    ("scene.json", json.dumps({"camera": camera, "general": general, "objects": [obj(1, image="models/v.json")]})),
    ("models/v.json", '{"material":"materials/v.json"}'),
    ("materials/v.json", '{"passes":[{"shader":"genericimage2","textures":["v"]}]}'),
    ("materials/v.tex", tex(0, 16, 16, [(16, 16, 0, mp4())], flags=0x20, ver=3))]))
# scene.json alone (fuzz_scenejson: the other files are those of everything.scene)
put("scenejson", "everything.json", json.dumps({"camera": camera, "general": general, "objects": layers}))
for k, name in ((1, "solid"), (2, "image-effect"), (3, "child-two-effects"), (4, "particle"), (5, "text"),
                (6, "sound"), (7, "passthrough"), (8, "light"), (9, "dxt-masked")):
    put("scenejson", name + ".json", json.dumps({"camera": camera, "general": general, "objects": [layers[k - 1]]}))
put("scene", "picture.scene", archive([("project.json", '{"file":"a.png","type":"image","title":"p"}'), ("a.png", png(3, 2))]))
put("scene", "gif.scene", archive([("project.json", '{"file":"a.gif","type":"gif"}'), ("a.gif", gif2)]))


# ----------------------------------------------------------------- sound

def wave(fmt, channels, rate, bits, frames, extensible=False, junk=b""):
    data = bytes((i * 31) & 255 for i in range(frames * channels * bits // 8))
    f = struct.pack("<HHIIHH", 0xFFFE if extensible else fmt, channels, rate, rate * channels * bits // 8,
                    channels * bits // 8, bits)
    if extensible:
        f += struct.pack("<HHIH", 22, bits, 3, fmt) + bytes(14)
    body = b"WAVE" + b"fmt " + u32(len(f)) + f + junk + b"data" + u32(len(data)) + data
    return b"RIFF" + u32(len(body)) + body


put("sound", "pcm16-mono.wav", wave(1, 1, 8000, 16, 64))
put("sound", "pcm8-stereo.wav", wave(1, 2, 22050, 8, 64))
put("sound", "pcm24-6ch.wav", wave(1, 6, 48000, 24, 16, extensible=True))
put("sound", "float.wav", wave(3, 2, 44100, 32, 32, junk=b"LIST" + u32(5) + b"INFO\0\0"))


def flac_without_padding(d):
    """ffmpeg's FLAC files carry 8 KiB of padding: a seed does not need it"""
    o, p, blocks = d[:4], 4, []
    while True:
        last, kind, n = d[p] & 0x80, d[p] & 0x7f, int.from_bytes(d[p + 1:p + 4], "big")
        if kind != 1:
            blocks.append(bytes([kind]) + d[p + 1:p + 4 + n])
        p += 4 + n
        if last:
            break
    blocks[-1] = bytes([blocks[-1][0] | 0x80]) + blocks[-1][1:]
    return o + b"".join(blocks) + d[p:]


# MP3, FLAC, Vorbis and Opus: 0.12 s of a sine tone, encoded by the ffmpeg
# program when there is one (the files in the tree were made this way)
import shutil
import subprocess
import tempfile
if shutil.which("ffmpeg"):
    with tempfile.TemporaryDirectory() as tmp:
        for name, args in (("tone.mp3", "-c:a libmp3lame -b:a 32k -ar 22050"),
                           ("tone-vorbis.ogg", "-c:a libvorbis -q:a 0 -ar 22050"),
                           ("tone-opus.ogg", "-c:a libopus -b:a 12k -f ogg"),
                           ("tone.flac", "-c:a flac -ar 8000 -sample_fmt s16")):
            f = os.path.join(tmp, name)
            r = subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", "sine=frequency=440:duration=0.12",
                                "-ac", "1"] + args.split() + ["-map_metadata", "-1", "-bitexact", f],
                               stdin=subprocess.DEVNULL)
            if r.returncode == 0:
                d = open(f, "rb").read()
                put("sound", name, flac_without_padding(d) if name.endswith(".flac") else d)
                if name == "tone.mp3":
                    put("sound", "tone-id3.mp3", b"ID3\x04\x00\x00\x00\x00\x00\x0a" + bytes(10) + d)

print("seeds written to", out)
