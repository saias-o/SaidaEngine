"""Fetch the MIT Godot OpenXR demo hands and pack their glTF into standalone GLBs.

Run with Python 3; no Blender or third-party Python modules are required.
The upstream skeleton uses Godot humanoid axes (see XRHandPose.hpp).
"""

import hashlib
import json
from pathlib import Path
import struct
from urllib.request import urlopen


REVISION = "b761b4cd5718a7118f2c95eb55f181d0a5f004f1"
BASE = f"https://raw.githubusercontent.com/godotengine/godot-demo-projects/{REVISION}/"
SOURCE = "xr/openxr_hand_tracking_demo/assets/gltf/"
ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "assets/xr/hands"


def fetch(path):
    with urlopen(BASE + path, timeout=30) as response:
        return response.read()


def pack_hand(side):
    name = side + "HandHumanoid"
    document = json.loads(fetch(SOURCE + name + ".gltf"))
    assert len(document["buffers"]) == 1
    binary = bytearray(fetch(SOURCE + document["buffers"][0].pop("uri")))
    for image in document.get("images", []):
        data = fetch(SOURCE + image.pop("uri"))
        binary.extend(b"\0" * (-len(binary) % 4))
        image["bufferView"] = len(document["bufferViews"])
        document["bufferViews"].append(
            {"buffer": 0, "byteOffset": len(binary), "byteLength": len(data)}
        )
        binary.extend(data)
    document["buffers"][0]["byteLength"] = len(binary)
    document["asset"]["copyright"] = "Copyright Godot Engine contributors; MIT license"
    document["asset"]["extras"] = {"sourceRevision": REVISION, "source": BASE + SOURCE}
    encoded = json.dumps(document, separators=(",", ":")).encode("utf-8")
    encoded += b" " * (-len(encoded) % 4)
    binary.extend(b"\0" * (-len(binary) % 4))
    result = (struct.pack("<III", 0x46546C67, 2, 28 + len(encoded) + len(binary))
              + struct.pack("<II", len(encoded), 0x4E4F534A) + encoded
              + struct.pack("<II", len(binary), 0x004E4942) + binary)
    path = OUTPUT / (side + "Hand.glb")
    path.write_bytes(result)
    print(f"{path.name}: {len(result)} bytes, sha256 {hashlib.sha256(result).hexdigest()}")


if __name__ == "__main__":
    OUTPUT.mkdir(parents=True, exist_ok=True)
    for side in ("Left", "Right"):
        pack_hand(side)
    (OUTPUT / "LICENSE.txt").write_bytes(fetch("LICENSE.md"))
