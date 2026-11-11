#!/usr/bin/env python3
"""Synthetic scenes for "Adjust layers" (no Wallpaper Engine assets needed).

  python3 -I tools/dev/genlayers.py widget DIR   a wallpaper with a now-playing widget
  python3 -I tools/dev/genlayers.py blocks DIR   flat coloured blocks for pixel checks

`widget` imitates what Workshop scenes do: a "Now Playing..." label, a square
cover and three right-aligned text lines, with layer names that say nothing.
The widget is made of layers of every kind the adjustment has to cope with:

  id  name           what                                   how it is placed
  30  Layer 12       text "Now Playing..."                  fixed origin
  31  cover_final2   image, the cover                       fixed origin
  32  Text 3         text, the title                        child of the cover (parent 31)
  33  Text 4         text, the artist                       SceneScript sets its origin every frame
  34  Text 5         text, the album                        text and visibility bound to options
  35  bar            solid layer, a progress bar            child of the cover
  36  bar fill       solid layer                            child of the bar; SceneScript scale
  40  sparkles       particle system over the cover         fixed origin
"""
import json, math, os, struct, sys


def tex_bytes(w, h, rgba):
    """TEXV0005/TEXI0001/TEXB0003 container, raw RGBA8888, one mip."""
    out = b'TEXV0005\0' + b'TEXI0001\0'
    out += struct.pack('<IIIIIII', 0, 0, w, h, w, h, 0)
    out += b'TEXB0003\0' + struct.pack('<II', 1, 0xFFFFFFFF)
    out += struct.pack('<I', 1)
    out += struct.pack('<IIIII', w, h, 0, len(rgba), len(rgba)) + bytes(rgba)
    return out


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    mode = 'wb' if isinstance(data, (bytes, bytearray)) else 'w'
    with open(path, mode) as f:
        f.write(data)


def jdump(path, obj):
    write(path, json.dumps(obj, indent=1))


def image_material(out, name, w, h, px):
    write(f"{out}/materials/{name}.tex", tex_bytes(w, h, px))
    jdump(f"{out}/materials/{name}.json",
          {"passes": [{"blending": "translucent", "cullmode": "nocull", "depthtest": "disabled",
                       "depthwrite": "disabled", "shader": "genericimage2", "textures": [name]}]})
    jdump(f"{out}/models/{name}.json", {"autosize": True, "material": f"materials/{name}.json"})


def background(w, h):
    """a night sky gradient, a skyline and a faint 120 px grid (so that a moved
    layer can be measured against something)"""
    px = bytearray(w * h * 4)
    import random
    rnd = random.Random(7)
    sky = []
    x = 0
    while x < w:
        bw = rnd.randint(40, 110)
        sky.append((x, x + bw - 6, rnd.randint(int(h * 0.55), int(h * 0.82))))
        x += bw
    tops = [h] * w
    for (x0, x1, top) in sky:
        for xx in range(x0, min(x1, w)):
            tops[xx] = top
    for y in range(h):
        t = y / (h - 1)
        r = int(14 + 50 * t * t)
        g = int(18 + 40 * t)
        b = int(46 + 70 * t)
        row = y * w * 4
        for xx in range(w):
            rr, gg, bb = r, g, b
            if y >= tops[xx]:
                rr, gg, bb = 10, 12, 22
                if (xx // 9 + y // 13) % 5 == 0 and (xx % 9) < 5 and (y % 13) < 7:
                    rr, gg, bb = 190, 160, 90      # lit windows
            if xx % 120 == 0 or y % 120 == 0:
                rr, gg, bb = min(255, rr + 14), min(255, gg + 14), min(255, bb + 14)
            o = row + xx * 4
            px[o] = rr
            px[o + 1] = gg
            px[o + 2] = bb
            px[o + 3] = 255
    return px


def cover(n):
    """an album cover: diagonal gradient, a ring and a white frame"""
    px = bytearray(n * n * 4)
    for y in range(n):
        for x in range(n):
            u, v = x / (n - 1), y / (n - 1)
            r = int(230 * (1 - u) + 40 * u)
            g = int(60 + 120 * v)
            b = int(120 + 120 * u * v)
            d = math.hypot(u - 0.5, v - 0.5)
            if 0.22 < d < 0.30:
                r, g, b = 250, 240, 200
            if x < 4 or y < 4 or x >= n - 4 or y >= n - 4:
                r, g, b = 255, 255, 255
            o = (y * n + x) * 4
            px[o] = r
            px[o + 1] = g
            px[o + 2] = b
            px[o + 3] = 255
    return px


def dot(n):
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


def scene(objects, w=1920, h=1080, clear="0.02 0.02 0.04", general=None):
    g = {"ambientcolor": "0.3 0.3 0.3", "bloom": False, "cameraparallax": False,
         "clearcolor": clear, "orthogonalprojection": {"height": h, "width": w},
         "skylightcolor": "0.3 0.3 0.3"}
    if general:
        g.update(general)
    return {"camera": {"center": "0 0 -1", "eye": "0 0 0", "up": "0 1 0"}, "general": g,
            "objects": objects}


def text_obj(i, name, value, origin, size="520 60", pointsize=8, align="right", **extra):
    o = {"id": i, "name": name, "text": {"value": value}, "font": "systemfont_arial",
         "pointsize": pointsize, "size": size, "origin": origin, "scale": "1 1 1",
         "angles": "0 0 0", "color": "1 1 1", "alpha": 1.0, "horizontalalign": align,
         "verticalalign": "center", "padding": 0, "visible": True}
    o.update(extra)
    return o


def image_obj(i, name, model, origin, size, **extra):
    o = {"id": i, "name": name, "image": f"models/{model}.json", "origin": origin, "size": size,
         "scale": "1 1 1", "angles": "0 0 0", "color": "1 1 1", "alpha": 1.0, "visible": True}
    o.update(extra)
    return o


def gen_widget(out):
    W, H = 1920, 1080
    image_material(out, "bg", W, H, background(W, H))
    image_material(out, "cover", 256, 256, cover(256))
    jdump(f"{out}/models/util/solidlayer.json", {"solidlayer": True})
    write(f"{out}/materials/particle/dot.tex", tex_bytes(32, 32, dot(32)))
    jdump(f"{out}/materials/particle/dot.json",
          {"passes": [{"shader": "genericparticle", "blending": "additive", "depthtest": "disabled",
                       "depthwrite": "disabled", "cullmode": "nocull", "textures": ["particle/dot"]}]})
    jdump(f"{out}/particles/sparkles.json",
          {"emitter": [{"name": "boxrandom", "id": 1, "distancemax": "120 120 0", "rate": 14}],
           "initializer": [{"name": "lifetimerandom", "id": 2, "min": 1.2, "max": 2.4},
                           {"name": "sizerandom", "id": 3, "min": 8, "max": 22},
                           {"name": "alpharandom", "id": 4, "min": 0.5, "max": 1},
                           {"name": "velocityrandom", "id": 5, "min": "-12 10 0", "max": "12 40 0"},
                           {"name": "colorrandom", "id": 6, "min": "255 220 140", "max": "255 255 255"}],
           "operator": [{"name": "movement", "id": 7, "gravity": "0 0 0"},
                        {"name": "alphafade", "id": 8, "fadeintime": 0.2, "fadeouttime": 0.6}],
           "renderer": [{"name": "sprite", "id": 9}],
           "material": "materials/particle/dot.json", "maxcount": 64, "starttime": 2})

    # the artist line follows the cover: a script writes its origin every frame
    follow = ("'use strict';\n"
              "export function update(value) {\n"
              "    let c = thisScene.getLayer('cover_final2').origin;\n"
              "    return new Vec3(c.x - 150, c.y - 6, 0);\n"
              "}\n")
    # the progress bar's fill grows with time (scale x 0..1, anchored on its left end)
    fill = ("'use strict';\n"
            "export function update(value) {\n"
            "    let t = (engine.runtime % 20) / 20;\n"
            "    return new Vec3(0.05 + 0.95 * t, 1, 1);\n"
            "}\n")
    objects = [
        image_obj(1, "bg", "bg", "960 540 0", "1920 1080"),
        text_obj(5, "Layer 3", "21:47", "1860 1010 0", size="300 90", pointsize=14),
        text_obj(6, "Layer 4", "Tuesday, 10 November", "1860 950 0", size="600 50", pointsize=6,
                 color="0.8 0.85 1"),
        # --- the widget, where the wallpaper's author put it ---
        text_obj(30, "Layer 12", "Now Playing...", "1760 742 0", size="420 50", pointsize=7,
                 color="0.75 0.85 1"),
        image_obj(31, "cover_final2", "cover", "1650 600 0", "220 220"),
        text_obj(32, "Text 3", "Color Your Night", "-150 46 0", parent=31),
        text_obj(33, "Text 4", "Lotus Juice, Azumi Takahashi", {"script": follow, "value": "1500 594 0"},
                 pointsize=6, color="0.85 0.85 0.9"),
        text_obj(34, "Text 5", "placeholder", "1500 552 0", pointsize=6, color="0.6 0.7 0.9",
                 text={"user": "albumtext", "value": "Persona 3 Reload OST"},
                 visible={"user": "showalbum", "value": True}),
        image_obj(35, "bar", "util/solidlayer", "0 -128 0", "220 6", parent=31, color="0.25 0.3 0.45"),
        image_obj(36, "bar fill", "util/solidlayer", "-110 0 0", "220 6", parent=35,
                  color="0.4 0.8 1", alignment="left",
                  scale={"script": fill, "value": "0.4 1 1"}),
        {"id": 40, "name": "sparkles", "particle": "particles/sparkles.json", "origin": "1650 600 0",
         "scale": "1 1 1", "angles": "0 0 0", "visible": True},
    ]
    jdump(f"{out}/scene.json", scene(objects, W, H))
    jdump(f"{out}/project.json",
          {"file": "scene.json", "title": "Synthetic: now-playing widget", "type": "scene",
           "general": {"properties": {
               "schemecolor": {"order": 0, "text": "ui_browse_properties_scheme_color",
                               "type": "color", "value": "0 0 0"},
               "showalbum": {"order": 1, "text": "Show album", "type": "bool", "value": True},
               "albumtext": {"order": 2, "text": "Album line", "type": "textinput",
                             "value": "Persona 3 Reload OST"}}}})


def gen_blocks(out):
    """flat blocks on black: positions can be read off a screenshot exactly.
    red 40:  200x100 at 400,300        green 41: 100x100, child of red at +200,+150
    blue 42: 80x80, a script sets its origin to 1000,700 every frame
    white text 43 "MOVE" at 1400,300   yellow 44: hidden by the scene (visible false)"""
    jdump(f"{out}/models/util/solidlayer.json", {"solidlayer": True})
    hold = ("'use strict';\nexport function update(value) {\n"
            "    return new Vec3(1000, 700, 0);\n}\n")
    objects = [
        image_obj(40, "red", "util/solidlayer", "400 300 0", "200 100", color="1 0 0"),
        image_obj(41, "green", "util/solidlayer", "200 150 0", "100 100", color="0 1 0", parent=40),
        image_obj(42, "blue", "util/solidlayer", {"script": hold, "value": "100 100 0"}, "80 80",
                  color="0 0 1"),
        text_obj(43, "label", "MOVE", "1400 300 0", size="300 80", pointsize=10, align="center"),
        image_obj(44, "yellow", "util/solidlayer", "1600 800 0", "120 60", color="1 1 0",
                  visible=False),
    ]
    jdump(f"{out}/scene.json", scene(objects, 1920, 1080, clear="0 0 0"))
    jdump(f"{out}/project.json",
          {"file": "scene.json", "title": "Synthetic: blocks", "type": "scene",
           "general": {"properties": {
               "schemecolor": {"order": 0, "text": "ui_browse_properties_scheme_color",
                               "type": "color", "value": "0 0 0"}}}})


def main():
    kinds = {"widget": gen_widget, "blocks": gen_blocks}
    if len(sys.argv) != 3 or sys.argv[1] not in kinds:
        sys.stderr.write(__doc__)
        return 1
    kinds[sys.argv[1]](sys.argv[2])
    print(f"wrote {sys.argv[1]} scene to {sys.argv[2]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
