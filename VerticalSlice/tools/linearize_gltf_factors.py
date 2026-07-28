#!/usr/bin/env python3
"""Convert glTF material factors that were authored as sRGB into real linear values.

    python VerticalSlice/tools/linearize_gltf_factors.py <dir-or-file>...

glTF defines `baseColorFactor` and `emissiveFactor` in LINEAR space. Kenney's
nature kit is exported by UniGLTF, which writes the artist's sRGB hex straight
into those fields: `flower_redA`'s `colorRed` is (0.88, 0.29, 0.31), which is
exactly #E04A4F read as bytes — the colour the model is meant to be — but read
as linear by a correct renderer it comes out a pale salmon. The whole kit
therefore renders as pastel: the greens turn mint, the browns turn beige.

Converting once, in the asset, is the fix. The alternative — a renderer that
guesses which factors are secretly sRGB — would be wrong for every conforming
file.

Idempotence: a converted file is marked in `asset.extras`, so re-running this
never darkens a model twice.
"""
import json
import os
import struct
import sys

GLB_MAGIC = 0x46546C67
CHUNK_JSON = 0x4E4F534A
MARK = "saidaLinearizedFactors"


def srgb_to_linear(c):
    if c <= 0.04045:
        return c / 12.92
    return ((c + 0.055) / 1.055) ** 2.4


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


def convert(gltf):
    extras = gltf.setdefault("asset", {}).setdefault("extras", {})
    if extras.get(MARK):
        return False

    touched = False
    for material in gltf.get("materials") or []:
        pbr = material.get("pbrMetallicRoughness")
        if pbr and "baseColorFactor" in pbr:
            c = pbr["baseColorFactor"]
            # Only the colour channels; alpha is not a colour.
            pbr["baseColorFactor"] = [srgb_to_linear(v) for v in c[:3]] + list(c[3:])
            touched = True
        if "emissiveFactor" in material:
            material["emissiveFactor"] = [srgb_to_linear(v)
                                          for v in material["emissiveFactor"]]
            touched = True

    extras[MARK] = True
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
    converted = already = skipped = 0
    for path in walk(argv):
        parsed = read_glb(path)
        if parsed is None:
            skipped += 1
            continue
        gltf, rest = parsed
        marked = (gltf.get("asset") or {}).get("extras", {}).get(MARK)
        if marked:
            already += 1
            continue
        if convert(gltf):
            converted += 1
        else:
            already += 1
        write_glb(path, gltf, rest)
    print("linearized %d, already done %d, not a GLB %d" % (converted, already, skipped))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
