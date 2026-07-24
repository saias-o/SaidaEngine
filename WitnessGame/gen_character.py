#!/usr/bin/env python3
"""Generates assets/models/totem.gltf: a minimal rigged "totem" for the
witness game — a column of 3 segments skinned to 3 bones (root/spine/head),
with "Idle" (slow sway) and "Walk" (pronounced sway + root bounce) clips.
Buffer embedded as base64, deterministic."""

import base64
import json
import math
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))

SEG_W, SEG_H = 0.35, 0.6            # half-width, height of one segment
BONE_Y = [0.0, 0.6, 1.2]            # root / spine / head bones (model space)

FLOAT, USHORT = 5126, 5123
ARRAY_BUFFER, ELEMENT_ARRAY_BUFFER = 34962, 34963


def box_segment(y0, y1, joint):
    """8 vertices of a box [y0,y1], all weighted 100% to `joint`."""
    w = SEG_W
    corners = [(-w, y0, -w), (w, y0, -w), (w, y0, w), (-w, y0, w),
               (-w, y1, -w), (w, y1, -w), (w, y1, w), (-w, y1, w)]
    positions, normals, joints, weights = [], [], [], []
    for c in corners:
        positions.append(c)
        n = (c[0], 0.0, c[2])
        length = math.hypot(n[0], n[2]) or 1.0
        normals.append((n[0] / length, 0.0, n[2] / length))
        joints.append((joint, 0, 0, 0))
        weights.append((1.0, 0.0, 0.0, 0.0))
    quads = [(0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7),
             (4, 5, 6, 7), (3, 2, 1, 0)]
    indices = []
    for a, b, c, d in quads:
        indices += [a, b, c, a, c, d]
    return positions, normals, joints, weights, indices


def build_mesh():
    P, N, J, W, I = [], [], [], [], []
    for seg in range(3):
        p, n, j, w, i = box_segment(seg * SEG_H, (seg + 1) * SEG_H, seg)
        base = len(P)
        P += p; N += n; J += j; W += w
        I += [base + k for k in i]
    return P, N, J, W, I


def anim_samplers(times, spine_deg, head_deg, root_bob):
    """Keys: sinusoidal Z rotations for spine/head + Y translation for root."""
    def quat_z(deg):
        h = math.radians(deg) * 0.5
        return (0.0, 0.0, math.sin(h), math.cos(h))
    spine = [quat_z(spine_deg * math.sin(2 * math.pi * t / times[-1])) for t in times]
    head = [quat_z(-head_deg * math.sin(2 * math.pi * t / times[-1])) for t in times]
    root = [(0.0, BONE_Y[0] + root_bob * abs(math.sin(2 * math.pi * t / times[-1])), 0.0)
            for t in times]
    return spine, head, root


class Buffer:
    def __init__(self):
        self.data = bytearray()
        self.views = []
        self.accessors = []

    def add(self, values, comp_fmt, comp_type, type_name, target=None, minmax=False):
        while len(self.data) % 4:
            self.data.append(0)
        offset = len(self.data)
        flat = [x for v in values for x in (v if isinstance(v, tuple) else (v,))]
        self.data += struct.pack('<' + comp_fmt * len(flat), *flat)
        view = {"buffer": 0, "byteOffset": offset, "byteLength": len(self.data) - offset}
        if target: view["target"] = target
        self.views.append(view)
        accessor = {"bufferView": len(self.views) - 1, "componentType": comp_type,
                    "count": len(values), "type": type_name}
        if minmax:
            cols = list(zip(*[v if isinstance(v, tuple) else (v,) for v in values]))
            accessor["min"] = [min(c) for c in cols]
            accessor["max"] = [max(c) for c in cols]
        self.accessors.append(accessor)
        return len(self.accessors) - 1


def main():
    P, N, J, W, I = build_mesh()
    buf = Buffer()
    a_pos = buf.add(P, 'f', FLOAT, "VEC3", ARRAY_BUFFER, minmax=True)
    a_nrm = buf.add(N, 'f', FLOAT, "VEC3", ARRAY_BUFFER)
    a_jnt = buf.add(J, 'H', USHORT, "VEC4", ARRAY_BUFFER)
    a_wgt = buf.add(W, 'f', FLOAT, "VEC4", ARRAY_BUFFER)
    a_idx = buf.add(I, 'H', USHORT, "SCALAR", ELEMENT_ARRAY_BUFFER)

    # Inverse bind matrices: bones aligned on Y, translation -BONE_Y.
    ibms = []
    for y in BONE_Y:
        m = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -y, 0, 1]
        ibms.append(tuple(float(x) for x in m))
    a_ibm = buf.add(ibms, 'f', FLOAT, "MAT4")

    animations = []
    for name, spine_deg, head_deg, bob, duration in [
            ("Idle", 4.0, 3.0, 0.0, 2.0), ("Walk", 14.0, 10.0, 0.05, 0.8)]:
        times = [duration * k / 16 for k in range(17)]
        spine, head, root = anim_samplers(times, spine_deg, head_deg, bob)
        a_t = buf.add(times, 'f', FLOAT, "SCALAR", minmax=True)
        a_spine = buf.add(spine, 'f', FLOAT, "VEC4")
        a_head = buf.add(head, 'f', FLOAT, "VEC4")
        a_root = buf.add(root, 'f', FLOAT, "VEC3")
        animations.append({
            "name": name,
            "samplers": [
                {"input": a_t, "output": a_spine, "interpolation": "LINEAR"},
                {"input": a_t, "output": a_head, "interpolation": "LINEAR"},
                {"input": a_t, "output": a_root, "interpolation": "LINEAR"},
            ],
            "channels": [
                {"sampler": 0, "target": {"node": 2, "path": "rotation"}},
                {"sampler": 1, "target": {"node": 3, "path": "rotation"}},
                {"sampler": 2, "target": {"node": 1, "path": "translation"}},
            ],
        })

    gltf = {
        "asset": {"version": "2.0", "generator": "gen_character.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        # 0 = skinned mesh carrier, 1..3 = root/spine/head skeleton.
        "nodes": [
            {"name": "Totem", "mesh": 0, "skin": 0},
            {"name": "bone_root", "translation": [0, BONE_Y[0], 0], "children": [2]},
            {"name": "bone_spine", "translation": [0, BONE_Y[1], 0], "children": [3]},
            {"name": "bone_head", "translation": [0, BONE_Y[2] - BONE_Y[1], 0]},
        ],
        "skins": [{"joints": [1, 2, 3], "inverseBindMatrices": a_ibm, "skeleton": 1}],
        "meshes": [{"name": "TotemMesh", "primitives": [{
            "attributes": {"POSITION": a_pos, "NORMAL": a_nrm,
                           "JOINTS_0": a_jnt, "WEIGHTS_0": a_wgt},
            "indices": a_idx, "material": 0}]}],
        "materials": [{"name": "TotemMat", "pbrMetallicRoughness": {
            "baseColorFactor": [0.85, 0.55, 0.2, 1.0],
            "metallicFactor": 0.0, "roughnessFactor": 0.7}}],
        "animations": animations,
        "buffers": [{
            "byteLength": len(buf.data),
            "uri": "data:application/octet-stream;base64," +
                   base64.b64encode(bytes(buf.data)).decode()}],
        "bufferViews": buf.views,
        "accessors": buf.accessors,
    }

    # Is nodes[1] the scene child of the root? No: the skeleton must be in
    # the scene to be animated — attached under the carrier.
    gltf["nodes"][0]["children"] = [1]
    # bone_spine is a child of bone_root: relative translation.
    gltf["nodes"][2]["translation"] = [0, BONE_Y[1] - BONE_Y[0], 0]

    out = os.path.join(HERE, "assets", "models")
    os.makedirs(out, exist_ok=True)
    path = os.path.join(out, "totem.gltf")
    with open(path, "w", newline="\n") as f:
        json.dump(gltf, f, indent=1)
        f.write("\n")
    print("wrote", path, f"({len(buf.data)} buffer bytes)")


if __name__ == "__main__":
    main()
