"""Capture front/back views of imported single- and double-sided materials.

Run with Pillow and a native build: python tools/verify_material_sidedness.py
--build build-rel. This is a semantic pixel check, independent of GPU vendor.
"""
import argparse
import base64
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile

from PIL import Image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=Path("build"))
    args = parser.parse_args()
    build = args.build.resolve()
    runs = build / "material-sidedness-run"
    runs.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix="run-", dir=runs))
    project = root / "source"
    (project / "scenes").mkdir(parents=True, exist_ok=True)
    (project / "assets" / "models").mkdir(parents=True, exist_ok=True)
    payload = struct.pack("<12f", -.75, -.75, 0, .75, -.75, 0,
                          .75, .75, 0, -.75, .75, 0)
    payload += struct.pack("<12f", *([0, 0, 1] * 4))
    payload += struct.pack("<6H", 0, 1, 2, 0, 2, 3)
    model = {
        "asset": {"version": "2.0"},
        "buffers": [{"byteLength": len(payload), "uri": "data:application/octet-stream;base64," + base64.b64encode(payload).decode()}],
        "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 48},
                        {"buffer": 0, "byteOffset": 48, "byteLength": 48},
                        {"buffer": 0, "byteOffset": 96, "byteLength": 12}],
        "accessors": [{"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3", "min": [-.75, -.75, 0], "max": [.75, .75, 0]},
                      {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3"},
                      {"bufferView": 2, "componentType": 5123, "count": 6, "type": "SCALAR"}],
        "materials": [{"doubleSided": sided,
                       "pbrMetallicRoughness": {"baseColorFactor": color, "metallicFactor": 0}}
                      for sided, color in [(False, [1, 0, 0, 1]), (True, [0, 1, 0, 1])]],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2, "material": i}]} for i in range(2)],
        "nodes": [{"mesh": 0, "translation": [-1.1, 1, 0]}, {"mesh": 1, "translation": [1.1, 1, 0]}],
        "scenes": [{"nodes": [0, 1]}], "scene": 0,
    }
    (project / "assets" / "models" / "panels.gltf").write_text(json.dumps(model), encoding="utf-8")
    node = {"type": "Node", "name": "Panels", "id": 2, "enabled": True,
            "behaviours": [], "children": [], "importedFrom": "assets/models/panels.gltf"}
    scene = {"schema": 2, "version": 2, "scene": {"type": "Scene", "name": "Sidedness", "id": 1, "enabled": True, "behaviours": [], "children": [node]}}
    (project / "scenes" / "main.scene").write_text(json.dumps(scene), encoding="utf-8")
    (project / "Sidedness.saidaproj").write_text(json.dumps({"schema": 1, "version": 1, "name": "Sidedness", "engineVersion": "0.1.0", "mainScene": "scenes/main.scene"}), encoding="utf-8")
    bundle = root / "bundle"
    env = dict(os.environ, SAIDA_WINDOW_HIDDEN="1")
    subprocess.run([str(build / "bin" / "saida_tool.exe"), "export-game", str(project / "Sidedness.saidaproj"), "--platform", "windows", "--out", str(bundle)], env=env, check=True, stdout=subprocess.DEVNULL)
    counts = {}
    for view, z in [("front", 5), ("back", -5)]:
        png = root / f"{view}.png"
        with (root / f"{view}.log").open("w", encoding="utf-8") as log:
            subprocess.run([str(bundle / "Sidedness.exe"), "--screenshot", str(png), "--after-frames", "30", "--camera-pos", f"0,1,{z}", "--camera-look", "0,1,0"], env=env, check=True, stdout=log, stderr=subprocess.STDOUT, timeout=60)
        with Image.open(png) as source:
            rgb = source.convert("RGB")
            pixels = list(rgb.get_flattened_data() if hasattr(rgb, "get_flattened_data") else rgb.getdata())
        counts[view] = (sum(r > 100 and r > 2*g and r > 2*b for r, g, b in pixels),
                        sum(g > 100 and g > 2*r and g > 2*b for r, g, b in pixels))
    assert min(counts["front"]) > 1000, counts
    assert counts["back"][0] < 50 and counts["back"][1] > 1000, counts
    print(f"PASS material sidedness: front red/green={counts['front']}, back={counts['back']}")
    print(f"Captures: {root}")


if __name__ == "__main__":
    main()
