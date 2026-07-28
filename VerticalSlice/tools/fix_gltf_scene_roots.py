#!/usr/bin/env python3
"""Repair glTF files whose scene points at a node that already has a parent.

    python VerticalSlice/tools/fix_gltf_scene_roots.py <dir-or-file>...

Kenney's nature kit is exported by UniGLTF, which wraps the model in a
`tmpParent` node and then lists the *child* as the scene root. glTF requires a
scene root to be parentless, so cgltf — and therefore SaidaEngine — rejects the
whole file with `cgltf_result_invalid_gltf` before a single mesh is read.

The fix is in the asset, not in the loader: point each scene at the nodes that
genuinely have no parent. The hierarchy, the transforms and the binary chunk are
untouched, so the repaired file is the same model, only well-formed.
"""
import json
import os
import struct
import sys

GLB_MAGIC = 0x46546C67
CHUNK_JSON = 0x4E4F534A


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
    header = b"glTF" + struct.pack("<II", 2, 12 + 8 + len(payload) + len(rest))
    with open(path, "wb") as f:
        f.write(header)
        f.write(struct.pack("<II", len(payload), CHUNK_JSON))
        f.write(payload)
        f.write(rest)


def repair(gltf):
    """Returns True when the document needed a fix."""
    nodes = gltf.get("nodes") or []
    scenes = gltf.get("scenes") or []
    if not nodes or not scenes:
        return False

    parented = set()
    for n in nodes:
        for child in n.get("children", []):
            parented.add(child)
    roots = [i for i in range(len(nodes)) if i not in parented]
    if not roots:
        return False  # a cycle: not something this script may guess at

    changed = False
    for scene in scenes:
        listed = scene.get("nodes", [])
        if any(i in parented for i in listed):
            scene["nodes"] = roots
            changed = True
    return changed


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
    fixed = skipped = failed = 0
    for path in walk(argv):
        parsed = read_glb(path)
        if parsed is None:
            failed += 1
            print("  ?? not a GLB:", path)
            continue
        gltf, rest = parsed
        if repair(gltf):
            write_glb(path, gltf, rest)
            fixed += 1
        else:
            skipped += 1
    print("repaired %d, already valid %d, unreadable %d" % (fixed, skipped, failed))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
