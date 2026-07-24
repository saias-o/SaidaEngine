#!/usr/bin/env python3
"""Génère les scènes du jeu témoin (greybox, cube builtin uniquement).

Le jeu témoin est l'instrument de mesure de la V1 :
il traverse scènes, physique, scripts JS, particules, UI, save/load et
changement de scène. Relancer ce script réécrit scenes/*.scene de façon
déterministe (ids stables dérivés des noms).
"""

import hashlib
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SCENE_SCHEMA = 2

# CollisionShapeType (src/physics/CollisionShapeNode.hpp)
SHAPE_AUTO, SHAPE_BOX, SHAPE_SPHERE, SHAPE_CAPSULE = 0, 1, 2, 3


def stable_id(path):
    """Id de nœud 64 bits stable dérivé du chemin logique du nœud."""
    digest = hashlib.sha256(path.encode()).digest()
    value = int.from_bytes(digest[:8], "little")
    return value or 1


def node(path, type_, name, pos=(0, 0, 0), rot=(0, 0, 0, 1), scale=(1, 1, 1),
         behaviours=None, children=None, groups=None, **extra):
    n = {
        "type": type_,
        "id": stable_id(path),
        "name": name,
        "enabled": True,
        "behaviours": behaviours or [],
        "children": children or [],
        "transform": {
            "position": list(pos),
            "rotation": list(rot),
            "scale": list(scale),
        },
    }
    if groups:
        n["groups"] = groups
    n.update(extra)
    return n


def cube(path, name, pos, scale, color, emissive=(0, 0, 0, 0), **extra):
    return node(path, "MeshNode", name, pos=pos, scale=scale,
                mesh=1, baseColor=list(color), emissive=list(emissive),
                roughness=0.7, metallic=0.0, **extra)


def box_shape(path, half, offset=(0, 0, 0)):
    return node(path, "CollisionShape", "Shape",
                shapeType=SHAPE_BOX, halfExtents=list(half), offset=list(offset))


def script(rel_path, properties=None):
    b = {"type": "ScriptBehaviour", "enabled": True,
         "script": rel_path, "hotReload": True}
    if properties:
        b["properties"] = properties
    return b


def sun(path):
    return node(path, "LightNode", "Sun", pos=(0, 30, 0),
                lightType=0, color=[1.0, 0.96, 0.9], intensity=3.0,
                direction=[-0.45, -0.75, 0.35], castShadows=True,
                bakeMode=0, range=10.0, spotInnerAngle=25.0, spotOuterAngle=35.0)


def floor_and_walls(prefix, extent=14.0):
    """Sol + 4 murs en StaticBody (extent = demi-taille du sol)."""
    e, wall_h, t = extent, 3.0, 0.5
    parts = [node(f"{prefix}/floor", "StaticBody", "Floor", pos=(0, -0.5, 0), children=[
        box_shape(f"{prefix}/floor/shape", (e, 0.5, e)),
        cube(f"{prefix}/floor/mesh", "FloorMesh", (0, 0, 0), (2 * e, 1, 2 * e),
             (0.42, 0.45, 0.5, 1.0)),
    ])]
    for name, pos, half in [
        ("WallN", (0, wall_h - 0.5, -e), (e, wall_h, t)),
        ("WallS", (0, wall_h - 0.5, e), (e, wall_h, t)),
        ("WallE", (e, wall_h - 0.5, 0), (t, wall_h, e)),
        ("WallW", (-e, wall_h - 0.5, 0), (t, wall_h, e)),
    ]:
        parts.append(node(f"{prefix}/{name}", "StaticBody", name, pos=pos, children=[
            box_shape(f"{prefix}/{name}/shape", half),
            cube(f"{prefix}/{name}/mesh", name + "Mesh", (0, 0, 0),
                 tuple(2 * h for h in half), (0.32, 0.34, 0.4, 1.0)),
        ]))
    return parts


def player(prefix, pos):
    # Le corps est un glTF riggé (gen_character.py) rechargé via importedFrom ;
    # l'import attache l'Animator que Character pilote (Idle/Walk).
    body = node(f"{prefix}/player/body", "Node", "Body", pos=(0, -0.9, 0))
    body["importedFrom"] = "assets/models/totem.gltf"
    return node(f"{prefix}/player", "CharacterBody", "Player", pos=pos,
                groups=["player"], children=[
                    node(f"{prefix}/player/shape", "CollisionShape", "Shape",
                         shapeType=SHAPE_CAPSULE, radius=0.4, height=1.8, axis=1),
                    body,
                ],
                behaviours=[{"type": "Character", "enabled": True,
                             "moveSpeed": 6.0, "jumpForce": 6.0,
                             "faceMovement": True,
                             "graph": "anim/locomotion.sgraph"},
                            # Store gameplay partagé : traversé par le driver
                            # E2E via node.setData/getData (P0.4).
                            {"type": "Blackboard", "enabled": True}])


def statue(prefix, pos):
    """Totem de cinématique : cible de anim/intro.sseq (SequenceDirector)."""
    body = node(f"{prefix}/statue/body", "Node", "Body", pos=(0, -0.9, 0))
    body["importedFrom"] = "assets/models/totem.gltf"
    return node(f"{prefix}/statue", "Node", "SeqStatue", pos=pos,
                groups=["sequence"], children=[body],
                behaviours=[{"type": "SequenceDirector", "enabled": True,
                             "sequence": "anim/intro.sseq", "autoplay": True}])


def camera(prefix):
    return node(f"{prefix}/camera", "Camera", "MainCamera", pos=(0, 4, 8),
                groups=["camera"], fovDegrees=60.0, nearZ=0.1, farZ=300.0,
                priority=0, active=True,
                behaviours=[{"type": "CameraFollow", "enabled": True,
                             "targetGroup": "player", "distance": 7.0,
                             "height": 2.5}])


def door(prefix, name, pos, target_scene, color):
    return node(f"{prefix}/{name}", "Area", name, pos=pos, children=[
        box_shape(f"{prefix}/{name}/shape", (1.2, 1.6, 0.6)),
        cube(f"{prefix}/{name}/mesh", name + "Mesh", (0, 0, 0), (2.4, 3.2, 0.4),
             color, emissive=(color[0], color[1], color[2], 2.0)),
    ], behaviours=[script("scripts/door.js", {"targetScene": target_scene})])


def hud(prefix, label):
    return node(f"{prefix}/hud", "UICanvasNode", "HUD",
                width=1920.0, height=1080.0, children=[
                    node(f"{prefix}/hud/score", "UITextNode", "ScoreText",
                         groups=["witness_hud"],
                         text=label, fontSize=28.0,
                         color=[1.0, 1.0, 1.0, 1.0],
                         x=24.0, y=24.0, width=600.0, height=48.0,
                         anchorX=0.0, anchorY=0.0, pivotX=0.0, pivotY=0.0,
                         behaviours=[script("scripts/hud.js")]),
                    # Prompt adaptatif (P0.6) : le label du binding de
                    # mouvement suit input.lastActiveDevice.
                    node(f"{prefix}/hud/prompt", "UITextNode", "PromptText",
                         groups=["witness_prompt"],
                         text="Move: ?", fontSize=22.0,
                         color=[0.85, 0.9, 1.0, 1.0],
                         x=24.0, y=76.0, width=600.0, height=36.0,
                         anchorX=0.0, anchorY=0.0, pivotX=0.0, pivotY=0.0,
                         behaviours=[script("scripts/prompt.js")]),
                ])


def relic(prefix, index, pos):
    name = f"Relic{index}"
    return node(f"{prefix}/{name}", "Area", name, pos=pos,
                groups=["relic"], children=[
        box_shape(f"{prefix}/{name}/shape", (0.6, 0.6, 0.6)),
        cube(f"{prefix}/{name}/mesh", name + "Mesh", (0, 0, 0), (0.7, 0.7, 0.7),
             (0.95, 0.8, 0.2, 1.0), emissive=(1.0, 0.85, 0.2, 3.0)),
        node(f"{prefix}/{name}/sparks", "ParticleSystem", "Sparks",
             maxParticles=64, spawnRate=12.0, lifetime=1.2,
             startSize=0.06, looping=True, playing=True,
             startColor=[1.0, 0.9, 0.3, 1.0], endColor=[1.0, 0.5, 0.1, 0.0],
             gravity=[0.0, 0.5, 0.0], radius=0.5, shape=1),
    ], behaviours=[
        {"type": "Rotator", "enabled": True, "speed": 90.0,
         "axis": [0.0, 1.0, 0.0]},
        script("scripts/pickup.js", {"points": 1}),
    ])


def crate(prefix, index, pos):
    name = f"Crate{index}"
    return node(f"{prefix}/{name}", "RigidBody", name, pos=pos,
                mass=2.0, children=[
                    box_shape(f"{prefix}/{name}/shape", (0.5, 0.5, 0.5)),
                    cube(f"{prefix}/{name}/mesh", name + "Mesh", (0, 0, 0),
                         (1, 1, 1), (0.55, 0.4, 0.25, 1.0)),
                ])


def write_probe_obj():
    """Écrit assets/models/probe.obj : une grille de plan (~34 Ko GPU) chargée
    par l'AssetLoader (.obj async). Sert de matière au test E2E du budget GPU
    mi-scène : une fois son nœud queueFree, elle devient évincable en LRU."""
    n = 16
    lines = ["# grille probe E2E (budget GPU)"]
    for z in range(n + 1):
        for x in range(n + 1):
            lines.append(f"v {x / n - 0.5:.4f} 0 {z / n - 0.5:.4f}")
    lines += [f"vt {x / n:.4f} {z / n:.4f}" for z in range(n + 1) for x in range(n + 1)]
    lines += ["vn 0 1 0"]
    for z in range(n):
        for x in range(n):
            a = z * (n + 1) + x + 1
            b = a + 1
            c = a + n + 1
            d = c + 1
            lines.append(f"f {a}/{a}/1 {b}/{b}/1 {d}/{d}/1")
            lines.append(f"f {a}/{a}/1 {d}/{d}/1 {c}/{c}/1")
    path = os.path.join(HERE, "assets", "models", "probe.obj")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print("wrote", path)


def write_corrupt_assets():
    """Contenu hostile embarqué (P0.5) : un .obj poubelle et un .glb tronqué.

    L'arène les référence volontairement — le moteur doit les refuser avec
    diagnostic et continuer (fallbacks), sur desktop comme dans le player
    wasm où une lecture hors limites serait fatale."""
    models = os.path.join(HERE, "assets", "models")
    os.makedirs(models, exist_ok=True)
    with open(os.path.join(models, "corrupt.obj"), "wb") as f:
        f.write(bytes((i * 37) % 251 for i in range(2048)))
    # En-tête GLB valide, chunk JSON tronqué en plein milieu.
    import struct
    json_part = b'{"asset":{"version":"2.0"},"meshes":[{"primi'
    glb = b"glTF" + struct.pack("<II", 2, 4096) + struct.pack("<II", 2048, 0x4E4F534A) + json_part
    with open(os.path.join(models, "corrupt.glb"), "wb") as f:
        f.write(glb)
    print("wrote corrupt.obj / corrupt.glb")


def gpu_probe(prefix, pos=(-6.0, 0.05, 6.0)):
    """Mesh .obj décoratif, cible du test budget GPU du driver E2E."""
    return node(f"{prefix}/gpuprobe", "Node", "GpuProbe", pos=pos,
                groups=["gpu_probe"], children=[
                    node(f"{prefix}/gpuprobe/mesh", "MeshNode", "GpuProbeMesh",
                         scale=(2.0, 1.0, 2.0), mesh="assets/models/probe.obj",
                         baseColor=[0.2, 0.5, 0.8, 1.0], roughness=0.8,
                         metallic=0.0),
                ])


def corrupt_probes(prefix):
    """Contenu hostile référencé par la scène : le .obj poubelle échoue en
    async (assets.stats().failed), le .glb tronqué est refusé à l'import —
    dans les deux cas la scène continue (le driver l'atteste)."""
    glb_node = node(f"{prefix}/corruptglb", "Node", "CorruptGlb", pos=(0, -20, 0))
    glb_node["importedFrom"] = "assets/models/corrupt.glb"
    return [
        node(f"{prefix}/corruptobj", "MeshNode", "CorruptObj", pos=(0, -20, 0),
             mesh="assets/models/corrupt.obj"),
        glb_node,
    ]


def pendulum(prefix, pos=(6.0, 4.0, 6.0)):
    """Bob dynamique suspendu à un PointJoint ancré au monde (P0.4).

    Le pivot est 1.5 m au-dessus du bob ; sans la contrainte il tomberait au
    sol (y ~ 0.3). Le driver E2E atteste que le joint le retient en l'air, sur
    desktop comme dans le player web."""
    name = "PendulumBob"
    return node(f"{prefix}/{name}", "RigidBody", name, pos=pos, mass=1.5,
                groups=["pendulum"], children=[
                    box_shape(f"{prefix}/{name}/shape", (0.3, 0.3, 0.3)),
                    cube(f"{prefix}/{name}/mesh", name + "Mesh", (0, 0, 0),
                         (0.6, 0.6, 0.6), (0.9, 0.75, 0.2, 1.0),
                         emissive=(0.6, 0.45, 0.1, 1.0)),
                    node(f"{prefix}/{name}/joint", "PointJoint", "PendulumJoint",
                         pos=(0, 1.5, 0)),
                ])


def scene_doc(root_name, children):
    return {
        "schema": SCENE_SCHEMA,
        "version": SCENE_SCHEMA,
        "scene": node(root_name, "Scene", root_name, children=children),
    }


def hub():
    p = "hub"
    children = [sun(f"{p}/sun")]
    children += floor_and_walls(p)
    children += [
        player(p, (0, 1.5, 4)),
        statue(p, (-6, 0.9, -2)),
        camera(p),
        door(p, "DoorToArena", (0, 1.1, -13.4), "scenes/arena.scene",
             (0.2, 0.7, 1.0, 1.0)),
        node(f"{p}/savepoint", "Area", "SavePoint", pos=(6, 0.6, 6), children=[
            box_shape(f"{p}/savepoint/shape", (1.0, 0.8, 1.0)),
            cube(f"{p}/savepoint/mesh", "SavePointMesh", (0, -0.35, 0),
                 (2.0, 0.3, 2.0), (0.3, 0.9, 0.4, 1.0),
                 emissive=(0.2, 0.9, 0.3, 1.5)),
        ], behaviours=[script("scripts/savepoint.js")]),
        hud(p, "Relics: ?"),
    ]
    return scene_doc("Hub", children)


def arena():
    p = "arena"
    children = [sun(f"{p}/sun")]
    children += floor_and_walls(p, extent=12.0)
    children += [
        player(p, (0, 1.5, 9)),
        camera(p),
        relic(p, 0, (-7, 1.0, -5)),
        relic(p, 1, (7, 1.0, -5)),
        relic(p, 2, (0, 1.0, -9)),
        crate(p, 0, (-3, 1.0, 0)),
        crate(p, 1, (-3, 2.2, 0)),
        crate(p, 2, (3, 1.0, 2)),
        pendulum(p),
        gpu_probe(p),
        *corrupt_probes(p),
        door(p, "DoorToHub", (0, 1.1, 11.4), "scenes/hub.scene",
             (1.0, 0.5, 0.2, 1.0)),
        hud(p, "Relics: ?"),
    ]
    return scene_doc("Arena", children)


def main():
    write_probe_obj()
    write_corrupt_assets()
    scenes = {"hub.scene": hub(), "arena.scene": arena()}
    out_dir = os.path.join(HERE, "scenes")
    os.makedirs(out_dir, exist_ok=True)
    for name, doc in scenes.items():
        path = os.path.join(out_dir, name)
        with open(path, "w", newline="\n") as f:
            json.dump(doc, f, indent=1)
            f.write("\n")
        print("wrote", path)


if __name__ == "__main__":
    main()
