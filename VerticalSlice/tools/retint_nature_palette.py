#!/usr/bin/env python3
"""Re-tint Kenney's nature kit from its mint palette to a naturalistic one.

    python VerticalSlice/tools/retint_nature_palette.py <dir-or-file>...

This is an art decision for this demo, not a correction: the kit's grass really
is #2CD8B8 and its stone really is ice blue — a deliberate, coherent look. It
just is not the look this slice is going for, which is a lush forest, so the
palette is remapped once here rather than fought with lighting.

The kit names its materials consistently across all 329 models (`grass`,
`leafsDark`, `woodBark`, …), so a per-name table recolours the whole set while
keeping every model's material split intact. Names not in the table — the flower
colours, the birch, the autumn leaves — are already right and are left alone.

Values below are written the way they are picked, in sRGB hex, and converted to
the linear space glTF's baseColorFactor actually holds.
"""
import json
import os
import struct
import sys

GLB_MAGIC = 0x46546C67
CHUNK_JSON = 0x4E4F534A
MARK = "saidaNaturePalette"

PALETTE = {
    # ground and foliage: mint/teal -> green
    "grass": "#63B345",
    "leafsGreen": "#4E9C3B",
    "leafsDark": "#37752E",
    # earth and wood: orange -> brown
    "dirt": "#A2703F",
    "dirtDark": "#82552F",
    "woodBark": "#8A6242",
    "woodBarkDark": "#6B4A31",
    "wood": "#BC8A55",
    "woodDark": "#8A6140",
    # rock: ice blue -> grey
    "stone": "#AEB6B6",
    "stoneDark": "#7C8688",
}


def hex_to_linear(text):
    text = text.lstrip("#")
    out = []
    for i in range(0, 6, 2):
        c = int(text[i:i + 2], 16) / 255.0
        out.append(c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4)
    return out


LINEAR = {name: hex_to_linear(value) for name, value in PALETTE.items()}


def read_glb(path):
    data = open(path, "rb").read()
    if len(data) < 20 or struct.unpack("<I", data[:4])[0] != GLB_MAGIC:
        return None
    json_len, chunk_type = struct.unpack("<II", data[12:20])
    if chunk_type != CHUNK_JSON:
        return None
    return json.loads(data[20:20 + json_len]), data[20 + json_len:]


def write_glb(path, gltf, rest):
    payload = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    payload += b" " * ((4 - len(payload) % 4) % 4)
    with open(path, "wb") as f:
        f.write(b"glTF" + struct.pack("<II", 2, 12 + 8 + len(payload) + len(rest)))
        f.write(struct.pack("<II", len(payload), CHUNK_JSON))
        f.write(payload)
        f.write(rest)


def retint(gltf):
    touched = False
    for material in gltf.get("materials") or []:
        target = LINEAR.get(material.get("name"))
        if target is None:
            continue
        pbr = material.setdefault("pbrMetallicRoughness", {})
        alpha = pbr.get("baseColorFactor", [1, 1, 1, 1])[3:] or [1.0]
        pbr["baseColorFactor"] = list(target) + list(alpha)
        touched = True
    gltf.setdefault("asset", {}).setdefault("extras", {})[MARK] = True
    return touched


def walk(paths):
    for p in paths:
        if os.path.isdir(p):
            for entry in sorted(os.listdir(p)):
                if entry.lower().endswith(".glb"):
                    yield os.path.join(p, entry)
        elif p.lower().endswith(".glb"):
            yield p


def main(argv):
    if not argv:
        print(__doc__)
        return 1
    changed = untouched = 0
    for path in walk(argv):
        parsed = read_glb(path)
        if parsed is None:
            continue
        gltf, rest = parsed
        if (gltf.get("asset") or {}).get("extras", {}).get(MARK):
            untouched += 1
            continue
        if retint(gltf):
            changed += 1
        else:
            untouched += 1
        write_glb(path, gltf, rest)
    print("retinted %d, unchanged %d" % (changed, untouched))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
