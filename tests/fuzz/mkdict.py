#!/usr/bin/env python3
"""mkdict.py - writes the dictionaries of the JSON-driven fuzz targets
(tests/fuzz/dict/<target>.dict) from the sources: every member name a loader
asks for and every word it compares a value with is a string literal next to
a getter or a comparison, and a fuzzer that knows those words reaches the
code behind them. Run it again when a loader learns new members.

    python3 tests/fuzz/mkdict.py

The dictionaries of the binary formats (tex, pkg, mp4, gif, puppet, sound)
and of the shader target are written by hand and not touched.
"""
import os
import re

here = os.path.dirname(os.path.abspath(__file__))
top = os.path.normpath(os.path.join(here, "..", ".."))

# a line counts when it calls one of these (getters of JSON members,
# comparisons of names and values)
CALLS = re.compile(
    r"\b(?:wpj_\w+|json_object_(?:get|has)\w*|json_builder_set_member_name|jb_\w+|jdouble|layer_num|"
    r"prop_\w+|g?_?strcmp0?|g_str_equal|g_str_has_prefix|g_str_has_suffix|g_ascii_strcasecmp|"
    r"g_ascii_strncasecmp|strncmp|strstr|kv_\w+|cfg_\w+|get_\w+|read_\w+|opt_\w+|find_tag)\s*\(")
LITERAL = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
# also: tables of names ({ "name", ... } rows)
ROW = re.compile(r'^\s*\{\s*"([A-Za-z_][A-Za-z0-9_.-]{0,40})"\s*,')
WORD = re.compile(r"^[A-Za-z_][A-Za-z0-9_ ./:-]{0,40}$")
IDENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]{1,40}$")
LOG = re.compile(r"\b(?:g_warning|g_message|g_debug|g_critical|g_printerr|g_print|printf|g_set_error\w*|g_strdup_printf|"
                 r"g_string_append_printf|g_snprintf|snprintf|g_getenv|g_setenv|getenv)\s*\(")


def words(files):
    out = set()
    for f in files:
        path = os.path.join(top, f)
        if not os.path.exists(path):
            continue
        for line in open(path, encoding="utf-8", errors="replace"):
            m = ROW.match(line)
            if m:
                out.add(m.group(1))
            if LOG.search(line):
                continue
            call = CALLS.search(line) is not None
            for lit in LITERAL.findall(line):
                # a bare identifier anywhere (the loaders have many small
                # getters of their own); other words only next to a getter or
                # a comparison; no messages, no format strings
                if IDENT.match(lit) or (call and "%" not in lit and "\\" not in lit and
                                        lit.count(" ") <= 1 and WORD.match(lit)):
                    out.add(lit)
    return out


def entry(text):
    o = '"'
    for ch in text.encode():
        if ch in (0x22, 0x5c):
            o += "\\" + chr(ch)
        elif 0x20 <= ch < 0x7f:
            o += chr(ch)
        else:
            o += "\\x%02x" % ch
    return o + '"'


def write(target, files, quoted, extra=()):
    ws = sorted(words(files))
    lines = ["# written by tests/fuzz/mkdict.py from " + " ".join(files)]
    for w in ws:
        lines.append(entry('"%s"' % w if quoted else w))
    lines += [entry(e) for e in extra]
    with open(os.path.join(here, "dict", target + ".dict"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("%-10s %4d words" % (target, len(ws)))


JSON_VALUES = ['{"user":"speed","value":1}', '{"script":"","value":0}', "1e999", "-1", "4294967296", "1e30", "0.5",
               '"0 0 0"', '"1 1"', '"nan nan"', '"1e30 -1e30 0"', "null", "true", "false", "[]", "{}", ",", ":",
               '"', "\\u0000", "//", "/*", "﻿"]
ENGINE = ["src/engine/scene.c", "src/engine/scenenode.c", "src/engine/scenenode_effects.c",
          "src/engine/scenenode_effects.h", "src/engine/scenenode_layers.h", "src/engine/scenenode_post.h",
          "src/engine/scenenode_sound.h", "src/engine/weparticles.c", "src/engine/particles.c",
          "src/engine/puppet.c", "src/engine/content.c", "src/engine/propnotes.c", "src/engine/scenetext.c",
          "src/engine/fxclass.c", "src/engine/bloom.c", "src/engine/camshake.c", "src/engine/features.c",
          "src/engine/scenesound.c", "src/engine/videotex.h", "src/engine/fx_sim.h", "src/common/weassets.c"]
# the fixed files of the scene targets (tests/fuzz/mkseeds.py, everything.scene)
ASSETS = ["project.json", "scene.json", "models/a.json", "materials/a.json", "materials/a.tex",
          "models/a_puppet.mdl", "models/d.json", "materials/d.json", "materials/d.tex", "materials/m.tex",
          "models/solid.json", "models/pass.json", "effects/fx/effect.json", "materials/effects/fx.json",
          "shaders/effects/fx.vert", "shaders/effects/fx.frag", "effects/fx2/effect.json",
          "materials/effects/fx2.json", "shaders/effects/fx2.vert", "shaders/effects/fx2.frag",
          "particles/p.json", "particles/c.json", "materials/p.json", "sounds/a.wav"]
NAMES = ['"%s"' % a for a in ASSETS] + ['"a"', '"d"', '"m"', '"hand"', '"_rt_half"', '"_rt_FullFrameBuffer"',
                                        '"_rt_imageLayerComposite"', '"util/white"', '"speed"', '"on"', '"tint"',
                                        '"words"', '"MASK"', '"MODE"', '"idle0"', '"effects/fx"', '"genericimage2"',
                                        '"genericparticle"', '"systemfont_arial"']
MARKS = ["=== %s\n" % a for a in ASSETS]
BINARY = ["TEXV0005\0", "TEXI0001\0", "TEXB0003\0", "TEXS0003\0", "MDLV", "MDLS", "MDLA", "MDAT", "RIFF", "WAVE",
          "#include \"common.h\"", "// [COMBO] {\"combo\":\"A\",\"default\":1}", "uniform ", "g_Time"]

write("json", ENGINE, True, JSON_VALUES)
write("particle", ["src/engine/weparticles.c", "src/engine/particles.c"], True, JSON_VALUES)
write("scenejson", ENGINE, True, JSON_VALUES + NAMES)
write("scene", ENGINE, True, JSON_VALUES + NAMES + MARKS + BINARY)
write("scenefile", ENGINE, True, JSON_VALUES + NAMES + MARKS + BINARY)
write("notes", ["src/engine/propnotes.c"], True,
      JSON_VALUES + ["PKGV0016", "\x08\0\0\0", "scene.json", ".json", "particles/p.json", "materials/a.json",
                     '"particle"', '"instanceoverride"', '"count"'])
write("config", ["src/common/config.c", "src/common/layeradj.c", "src/common/quality.c", "src/common/resolution.c",
                 "src/common/gpusel.c", "src/common/setkeys.h", "src/engine/features.c",
                 "src/engine/video_hwsel.h", "src/common/fluidsim.h", "src/common/backend.h"], True,
      JSON_VALUES + ['"HDMI-0"', '"DP-2"', '"/w/a"'])
write("ipc", ["src/common/setkeys.h", "src/common/ipc.c", "src/daemon/main.c", "src/daemon/layers_ipc.h",
              "src/ctl/main.c"], True,
      JSON_VALUES + ['{"cmd":"set",', '{"cmd":"subscribe"}\n', '{"cmd":"big"}\n', '{"cmd":"none"}\n', "\n", "\0",
                     "\xff", '{"cmd":"status"}\n'])
write("library", ["src/ui/store.c", "src/engine/content.c", "src/common/library.c", "src/engine/propnotes.c"], True,
      JSON_VALUES + ['"7"', '"42"', '"preview.jpg"', '"preview.gif"', '"../7/index.html"', '"a.mp4"', '"scene.json"',
                     '"index.html"'])
write("steam", ["src/common/workshop_query.c", "src/common/workshop.c"], True,
      JSON_VALUES + ["<div", "</div>", "data-publishedfileid=\"", "sharedfiles/filedetails/?id=", "[b]", "[/b]",
                     "[url=", "[/url]", "[list]", "[*]", "[h1]", "[img]", "https://steamcommunity.com/",
                     "https://steamuserimages-a.akamaihd.net/", "http://", "&amp;", "&quot;", "&#39;", "\\u003c"])
write("import", ["src/common/import.c", "src/common/scenevideo.c"], True,
      JSON_VALUES + ['"h264"', '"hevc"', '"yuv420p"', '"yuv420p10le"', '"mov,mp4,m4a,3gp,3g2,mj2"', '"video"',
                     '"1:1"', '"4:3"', '"0:1"', '"-90"', '"90"', '"N/A"'])
