#!/usr/bin/env python3
"""Synthetic wallpapers for checks of the display backends, the pointer, video decoding and
the render resolution (no Wallpaper Engine assets needed; everything is made here).

  python3 -I gencheck.py quad DIR [--size WxH]
        four flat quarters, each its own colour (top left red, top right green, bottom
        left blue, bottom right white): wrong when mirrored, turned, shifted or scaled
  python3 -I gencheck.py pointer DIR
        one full-screen layer whose colour is the pointer's place: red = x, green = y,
        blue = 1 - x (left edge blue, right edge red, bottom adds green; purple-grey in the
        middle and without a pointer)
  python3 -I gencheck.py anim DIR
        one full-screen layer whose red changes all the time: a scene that draws every frame
  python3 -I gencheck.py ruler DIR --size WxH
        a scene of exactly WxH with marks one pixel wide: white lines at x = 8 and x = 24,
        at y = 8 and y = 24 (from the top), and in the middle a 32x32 patch of single-pixel
        checkerboard. Shown at WxH pixels every mark is exactly one pixel wide and the
        patch alternates black / white; at any other render size the lines widen or fade
        and the patch turns grey. --size is the output's size in pixels.
  python3 -I gencheck.py big DIR [--size WxH]
        one picture layer larger than 1080p (default 3840x2160, raw RGBA: 33 MB): an 8x8
        grid of flat cells, cell (column, row) has colour (16 + 32 * column, 16 + 32 * row,
        128), so what is on the screen says which part of the picture it is and how it
        was reduced
  python3 -I gencheck.py clip FILE.mp4 [--size WxH] [--fps N] [--seconds N] [--codec NAME] [--pixfmt NAME]
        a test clip made with the ffmpeg program (default 640x360, 30 fps, 2 s, h264,
        yuv420p). --codec h264 | hevc | vp9 | av1 and --pixfmt yuv420p | yuv420p10le |
        yuv444p give the clips a decoder choice has to tell apart; exit status 77 when this
        ffmpeg cannot write that codec
  python3 -I gencheck.py videoscene DIR [--clip FILE.mp4] [clip options]
        a scene with a video layer: the clip (made as above unless given) wrapped as a
        video texture, shown full-screen, with a flat yellow block in the top left corner
        on top of it (a still layer over a moving one)
  python3 -I gencheck.py layouts
        the monitor layouts the checks use, by name, in the form scripts/displayenv.py
        takes as --outputs (and WALPE_MONITORS takes on X11)

A scene is 1920x1080 unless --size says otherwise; scene coordinates have y pointing up."""
import json, os, shutil, struct, subprocess, sys

LAYOUTS = (
    ("single", "640x360+0+0", "one monitor"),
    ("pair-same", "640x360+0+0,640x360+640+0", "two of one size: one renders, the other can follow it"),
    ("pair-mixed", "640x360+0+0,480x270+640+0", "two of different sizes: each renders its own"),
    ("pair-stacked", "640x360+0+0,640x360+0+360", "one above the other"),
    ("pair-gap", "640x360+0+0,640x360+1000+100", "not touching, not aligned"),
    ("big-small", "1920x1080+0+0,640x360+1920+0", "a 1080p monitor next to a small one"),
    ("uhd", "3840x2160+0+0", "one 4K monitor (render resolution work; heavy on a software renderer)"),
    ("hidpi (Wayland)", "1280x720+0+0 at scale 2, 640x360+640+0", "mixed scales: displayenv output(scale=2)"),
    ("portrait (Wayland)", "640x360+0+0, 640x360+640+0 turned 90", "a turned output: displayenv output(transform='90')"),
)
VERT = ("uniform mat4 g_ModelViewProjectionMatrix;\nattribute vec3 a_Position;\nattribute vec2 a_TexCoord;\n"
        "varying vec2 v_TexCoord;\nvoid main() {\n"
        "\tgl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);\n\tv_TexCoord = a_TexCoord;\n}\n")
POINTER_FRAG = ("varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0;\nuniform vec2 g_PointerPosition;\n"
                "void main() {\n\tvec4 c = texSample2D(g_Texture0, v_TexCoord);\n"
                "\tc.rgb = vec3(g_PointerPosition.x, g_PointerPosition.y, 1.0 - g_PointerPosition.x);\n"
                "\tgl_FragColor = c;\n}\n")
TIME_FRAG = ("varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0;\nuniform float g_Time;\n"
             "void main() {\n\tvec4 c = texSample2D(g_Texture0, v_TexCoord);\n"
             "\tc.rgb = vec3(0.5 + 0.5 * sin(g_Time * 5.0), 0.2, 0.2);\n\tgl_FragColor = c;\n}\n")
ENCODERS = {"h264": ("libx264",), "hevc": ("libx265",), "vp9": ("libvpx-vp9",), "av1": ("libsvtav1", "libaom-av1")}


def write(path, data):
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "wb" if isinstance(data, (bytes, bytearray)) else "w") as f:
        f.write(data)


def jdump(path, obj):
    write(path, json.dumps(obj, indent=1))


def tex_bytes(w, h, payload, flags=0):
    """TEXV0005 / TEXI0001 / TEXB0003 container with one level: raw RGBA8888, or (flags 0x20)
    an MP4 file as the level's bytes, which is how a video texture is stored"""
    out = b"TEXV0005\0" + b"TEXI0001\0" + struct.pack("<IIIIIII", 0, flags, w, h, w, h, 0)
    out += b"TEXB0003\0" + struct.pack("<II", 1, 0xFFFFFFFF) + struct.pack("<I", 1)
    return out + struct.pack("<IIIII", w, h, 0, len(payload), len(payload)) + bytes(payload)


def scene(out, title, objects, w=1920, h=1080, clear="0 0 0"):
    jdump(f"{out}/scene.json", {
        "camera": {"center": "0 0 -1", "eye": "0 0 0", "up": "0 1 0"},
        "general": {"ambientcolor": "0.3 0.3 0.3", "bloom": False, "cameraparallax": False,
                    "clearcolor": clear, "orthogonalprojection": {"height": h, "width": w},
                    "skylightcolor": "0.3 0.3 0.3"},
        "objects": objects})
    jdump(f"{out}/project.json", {"file": "scene.json", "title": title, "type": "scene"})


def solid(out, i, name, x, y, w, h, colour, **extra):
    """a flat rectangle: centre x, y (y up), size w x h"""
    jdump(f"{out}/models/util/solidlayer.json", {"solidlayer": True})
    o = {"id": i, "name": name, "image": "models/util/solidlayer.json", "origin": f"{x} {y} 0",
         "size": f"{w} {h}", "scale": "1 1 1", "angles": "0 0 0", "color": colour, "alpha": 1.0, "visible": True}
    o.update(extra)
    return o


def picture(out, i, name, x, y, w, h, tex):
    """a picture layer shown at its own size: centre x, y, texture file contents tex"""
    write(f"{out}/materials/{name}.tex", tex)
    jdump(f"{out}/materials/{name}.json",
          {"passes": [{"blending": "translucent", "cullmode": "nocull", "depthtest": "disabled",
                       "depthwrite": "disabled", "shader": "genericimage2", "textures": [name]}]})
    jdump(f"{out}/models/{name}.json", {"autosize": True, "material": f"materials/{name}.json"})
    return {"id": i, "name": name, "image": f"models/{name}.json", "origin": f"{x} {y} 0", "size": f"{w} {h}",
            "scale": "1 1 1", "angles": "0 0 0", "color": "1 1 1", "alpha": 1.0, "visible": True}


def effect_layer(out, name, frag, w, h):
    """one full-screen solid layer with a one-pass effect of its own"""
    write(f"{out}/shaders/effects/{name}.vert", VERT)
    write(f"{out}/shaders/effects/{name}.frag", frag)
    jdump(f"{out}/materials/effects/{name}.json",
          {"passes": [{"shader": f"effects/{name}", "blending": "normal", "depthtest": "disabled",
                       "depthwrite": "disabled", "cullmode": "nocull"}]})
    jdump(f"{out}/effects/{name}/effect.json",
          {"name": name, "group": "t", "passes": [{"material": f"materials/effects/{name}.json"}]})
    return solid(out, 40, "fill", w / 2, h / 2, w, h, "1 1 1",
                 effects=[{"file": f"effects/{name}/effect.json", "id": 9, "name": name, "visible": True,
                           "passes": [{}]}])


def gen_quad(out, o):
    w, h = o["size"]
    quarters = ((w / 4, 3 * h / 4, "1 0 0"), (3 * w / 4, 3 * h / 4, "0 1 0"),
                (w / 4, h / 4, "0 0 1"), (3 * w / 4, h / 4, "1 1 1"))
    scene(out, "quad", [solid(out, 50 + i, f"q{i}", x, y, w / 2, h / 2, c) for i, (x, y, c) in enumerate(quarters)],
          w, h)


def gen_pointer(out, o):
    w, h = o["size"]
    scene(out, "pointer", [effect_layer(out, "pointer", POINTER_FRAG, w, h)], w, h)


def gen_anim(out, o):
    w, h = o["size"]
    scene(out, "anim", [effect_layer(out, "anim", TIME_FRAG, w, h)], w, h)


def gen_ruler(out, o):
    w, h = o["size"]
    objs = []
    for i, x in enumerate((8, 24)):                 # a line covering exactly pixel column x
        objs.append(solid(out, 60 + i, f"col{x}", x + 0.5, h / 2, 1, h, "1 1 1"))
    for i, y in enumerate((8, 24)):                 # pixel row y counted from the top
        objs.append(solid(out, 70 + i, f"row{y}", w / 2, h - y - 0.5, w, 1, "1 1 1"))
    n = 32
    px = bytearray()
    for y in range(n):
        for x in range(n):
            px += b"\xff\xff\xff\xff" if (x + y) % 2 == 0 else b"\0\0\0\xff"
    # the patch's texels fall on pixels when its corner is on a pixel corner
    objs.append(picture(out, 80, "checker", w // 2, h // 2, n, n, tex_bytes(n, n, px)))
    scene(out, "ruler %dx%d" % (w, h), objs, w, h)


def gen_big(out, o):
    w, h = o["size"] if o["sized"] else (3840, 2160)
    cw, ch = w // 8, h // 8
    rows = []
    for r in range(8):
        row = b"".join(bytes((16 + 32 * c, 16 + 32 * r, 128, 255)) * (cw if c < 7 else w - 7 * cw) for c in range(8))
        rows.append(row * (ch if r < 7 else h - 7 * ch))
    scene(out, "big %dx%d" % (w, h), [picture(out, 7, "big", w // 2, h // 2, w, h, tex_bytes(w, h, b"".join(rows)))],
          w, h)


def gen_clip(path, o):
    """a clip of ffmpeg's test picture; -> 0, or 77 when this machine cannot make it"""
    if not shutil.which("ffmpeg"):
        print("gencheck: no ffmpeg program here")
        return 77
    w, h = o["size"] if o["sized"] else (640, 360)
    last = None
    for enc in ENCODERS[o["codec"]]:
        cmd = ["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", f"testsrc2=size={w}x{h}:rate={o['fps']}",
               "-t", str(o["seconds"]), "-c:v", enc, "-pix_fmt", o["pixfmt"], "-movflags", "+faststart", path]
        try:
            last = subprocess.run(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  timeout=1800)
        except (OSError, subprocess.TimeoutExpired):
            continue
        if last.returncode == 0 and os.path.exists(path) and os.path.getsize(path) > 1000:
            return 0
    print("gencheck: this ffmpeg cannot write %s / %s" % (o["codec"], o["pixfmt"]))
    if last is not None:
        print(last.stdout.decode(errors="replace")[-300:])
    return 77


def gen_videoscene(out, o):
    clip = o["clip"]
    if not clip:
        clip = os.path.join(out, "clip.mp4")
        os.makedirs(out, exist_ok=True)
        status = gen_clip(clip, o)
        if status:
            return status
    data = open(clip, "rb").read()
    if data[4:8] != b"ftyp":
        print("gencheck: %s does not start with an ftyp box (not an MP4 a video texture can hold)" % clip)
        return 1
    vw, vh = o["size"] if o["sized"] else (640, 360)
    w, h = 1920, 1080
    film = picture(out, 7, "film", w // 2, h // 2, w, h, tex_bytes(vw, vh, data, flags=0x20))
    jdump(f"{out}/models/film.json", {"material": "materials/film.json"})      # stretched over the scene
    scene(out, "videoscene", [film, solid(out, 41, "block", 160, h - 90, 320, 180, "1 1 0")], w, h)
    if not o["clip"]:
        os.unlink(clip)
    return 0


KINDS = {"quad": gen_quad, "pointer": gen_pointer, "anim": gen_anim, "ruler": gen_ruler, "big": gen_big,
         "clip": gen_clip, "videoscene": gen_videoscene}


def main():
    args = sys.argv[1:]
    if args == ["layouts"]:
        for name, spec, what in LAYOUTS:
            print("%-20s %-44s %s" % (name, spec, what))
        return 0
    o = {"size": (1920, 1080), "sized": False, "fps": 30, "seconds": 2, "codec": "h264", "pixfmt": "yuv420p",
         "clip": None}
    try:
        kind, out, rest = args[0], args[1], args[2:]
        if kind not in KINDS or out.startswith("-"):
            raise ValueError(kind)
        while rest:
            a = rest.pop(0)
            if a == "--size":
                w, h = rest.pop(0).split("x")
                o["size"], o["sized"] = (int(w), int(h)), True
            elif a in ("--fps", "--seconds"):
                o[a[2:]] = int(rest.pop(0))
            elif a in ("--codec", "--pixfmt", "--clip"):
                o[a[2:]] = rest.pop(0)
            else:
                raise ValueError(a)
        if o["codec"] not in ENCODERS or min(o["size"]) < 1 or (kind == "ruler" and not o["sized"]):
            raise ValueError("options")
    except (ValueError, IndexError):
        sys.stderr.write(__doc__ + "\n")
        return 2
    status = KINDS[kind](out, o) or 0
    if status == 0:
        print(f"wrote {kind} to {out}")
    return status


if __name__ == "__main__":
    sys.exit(main())
