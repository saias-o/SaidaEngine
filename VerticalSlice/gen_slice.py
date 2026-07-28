#!/usr/bin/env python3
"""Generate the VerticalSlice project — "Verdance", a third-person nature slice
built to exercise SaidaEngine end to end: HDRI skybox + IBL, GI, SSAO, bloom,
fog, shadows, skinned characters, SaidaFX particles, physics bodies, areas,
signals, UI and QuickJS gameplay.

    python VerticalSlice/gen_slice.py

Writes, next to itself:
  * scenes/verdance.scene   — the whole level
  * VerticalSlice.saidaproj — project (main scene, audio aliases, autoload)

The art is Kenney's CC0 kits (see README.md); this script only places it. Every
placement is seeded, so regenerating the level reproduces it byte for byte.
"""
import json
import math
import os
import random

HERE = os.path.dirname(os.path.abspath(__file__))
RNG = random.Random(20260728)

# ── palette ─────────────────────────────────────────────────────────────────
# The kit's own colours, written the way an artist picks them (sRGB) and
# converted here — baseColor reaches the shader as linear, exactly like the
# nature models' factors after tools/linearize_gltf_factors.py.
def srgb(r, g, b, a=1.0):
    def channel(c):
        return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4
    return [channel(r), channel(g), channel(b), a]


def hexcolor(text, a=1.0):
    text = text.lstrip("#")
    return srgb(*[int(text[i:i + 2], 16) / 255.0 for i in (0, 2, 4)], a=a)


# Kept in step with tools/retint_nature_palette.py: the slabs this script emits
# have to be the same colour as the kit props standing on them.
GRASS = hexcolor("#63B345")
DIRT = hexcolor("#A2703F")
STONE = hexcolor("#AEB6B6")
WOOD = hexcolor("#BC8A55")
WATER = hexcolor("#5AA9C8")

NATURE = "assets/models/nature/%s.glb"
KIT = {
    "platformer": "assets/models/platformer",
    "blaster": "assets/models/blaster",
    "pets": "assets/models/pets",
    "survival": "assets/models/survival",
    "forest": "assets/models/forest",
}

# The nature kit is authored on a 1-unit tile; the characters are 1.8 m tall.
# Everything below is expressed in metres and scaled through these.
TILE = 2.0          # a nature tile becomes a 2 m tile
PROP = 2.0          # ground-level scenery
TREE = 4.2          # trees read as trees only well above the tile scale

_next_id = [7300000000000000000]


def nid():
    _next_id[0] += 1
    return _next_id[0]


def node(type_, name, pos=(0.0, 0.0, 0.0), scale=1.0, yaw=None, **kw):
    d = {
        "type": type_, "id": nid(), "name": name, "enabled": True,
        "behaviours": [], "children": [],
        "transform": {
            "position": [float(v) for v in pos],
            "rotation": quat_yaw(yaw if yaw is not None else 0.0),
            "scale": [scale, scale, scale] if not isinstance(scale, (list, tuple))
                     else [float(v) for v in scale],
        },
    }
    d.update(kw)
    return d


def quat_yaw(degrees):
    half = math.radians(degrees) * 0.5
    return [0.0, math.sin(half), 0.0, math.cos(half)]


def script(path, **props):
    b = {"type": "ScriptBehaviour", "enabled": True, "script": path, "hotReload": True}
    if props:
        b["properties"] = props
    return b


# ── mesh helpers ────────────────────────────────────────────────────────────
def nature(name, model, pos, scale=PROP, yaw=None):
    """A nature-kit prop. These carry their colours in their glTF materials, so
    they are imported rather than referenced as a mesh + atlas."""
    return node("Node", name, pos, scale, yaw, importedFrom=NATURE % model)


def kitmesh(name, kit, model, pos, scale=1.0, yaw=None, color=None, **mat):
    """A prop from an atlas kit: one shared colormap texture, so these instance
    cheaply where the nature props do not."""
    return node("MeshNode", name, pos, scale, yaw,
                mesh="%s/%s.obj" % (KIT[kit], model),
                texture="%s/colormap.png" % KIT[kit],
                baseColor=color or [1.0, 1.0, 1.0, 1.0],
                roughness=mat.get("roughness", 0.85),
                metallic=mat.get("metallic", 0.0),
                emissive=mat.get("emissive", [0, 0, 0, 0]),
                castShadows=mat.get("castShadows", True))


def slab(name, pos, size, color, **mat):
    """A flat coloured box from the built-in cube — the cheap way to cover a
    large surface in a palette colour the props already use."""
    return node("MeshNode", name, pos, [size[0], size[1], size[2]],
                mesh="cube", baseColor=color,
                roughness=mat.get("roughness", 0.9),
                metallic=mat.get("metallic", 0.0),
                emissive=mat.get("emissive", [0, 0, 0, 0]),
                castShadows=mat.get("castShadows", True))


def box_body(name, pos, half_extents, children=None, groups=None, static=True):
    kw = {}
    if groups:
        kw["groups"] = groups
    b = node("StaticBody" if static else "RigidBody", name, pos, **kw)
    b["children"] = [node("CollisionShape", "Shape", shapeType=1,
                          halfExtents=[float(v) for v in half_extents],
                          offset=[0, 0, 0])]
    b["children"] += children or []
    return b


def particles(name, pos, **kw):
    base = {
        "effectClass": 0, "maxParticles": 128, "spawnRate": 40.0, "lifetime": 1.4,
        "startSpeed": 1.0, "startSize": 0.18, "startColor": [1.0, 0.85, 0.35, 1.0],
        "endColor": [1.0, 0.2, 0.05, 0.0], "gravity": [0.0, 0.4, 0.0], "radius": 0.3,
        "shape": 1, "emissive": 2.5, "endSizeScale": 0.35, "stretch": 1.0,
        "drag": 0.0, "noiseStrength": 0.0, "noiseFrequency": 1.0,
        "blendMode": 1, "looping": True, "playing": True,
    }
    base.update(kw)
    return node("ParticleSystem", name, pos, **base)


# ── level pieces ────────────────────────────────────────────────────────────
def grass_island(name, centre, half_x, half_z, top_y, thickness=3.0, rim_step=TILE):
    """A grass plateau: one collider, a coloured slab for the surface, a dirt
    body below it and a rim of nature blocks that gives the silhouette its
    stylised edge. One body per island — a body only ever collides with one
    mesh, so the collision is authored, never inferred from the art."""
    cx, cy, cz = centre
    body = box_body(name, (cx, top_y - thickness * 0.5, cz),
                    (half_x, thickness * 0.5, half_z))

    body["children"].append(
        slab(name + "Top", (0.0, thickness * 0.5 - 0.15, 0.0),
             (half_x * 2.0, 0.3, half_z * 2.0), GRASS, roughness=0.95))
    body["children"].append(
        slab(name + "Rock", (0.0, -0.2, 0.0),
             (half_x * 2.0 - 0.6, thickness - 0.4, half_z * 2.0 - 0.6), DIRT,
             roughness=1.0))

    # Rim blocks sit just outside the collider, purely to break the straight edge.
    rim = node("Node", name + "Rim", (0.0, top_y, 0.0))
    x = -half_x
    while x <= half_x:
        for z in (-half_z, half_z):
            rim["children"].append(nature(
                "rim%d" % nid(), RNG.choice(("cliff_block_rock", "cliff_half_rock")),
                (cx + x, top_y - 1.9 * TILE + RNG.uniform(-0.1, 0.0), cz + z),
                TILE, RNG.choice((0, 90, 180, 270))))
        x += rim_step
    z = -half_z + rim_step
    while z < half_z:
        for x in (-half_x, half_x):
            rim["children"].append(nature(
                "rim%d" % nid(), RNG.choice(("cliff_block_rock", "cliff_half_rock")),
                (cx + x, top_y - 1.9 * TILE + RNG.uniform(-0.1, 0.0), cz + z),
                TILE, RNG.choice((0, 90, 180, 270))))
        z += rim_step
    return body, rim


def scatter_flora(parent, centre, half_x, half_z, top_y, density, avoid=()):
    """Trees, rocks and undergrowth over an island, kept off the play line."""
    cx, _, cz = centre
    trees = ("tree_pineTallA_detailed", "tree_pineTallB_detailed", "tree_detailed",
             "tree_oak", "tree_fat", "tree_pineRoundC", "tree_tall", "tree_thin")
    small = ("plant_bushDetailed", "plant_bushLarge", "grass_leafsLarge", "grass_large",
             "mushroom_redGroup", "mushroom_tanGroup", "flower_redA", "flower_yellowB",
             "flower_purpleC", "stone_smallA", "rock_smallB", "log", "stump_round")

    def free(x, z, radius):
        for ax, az, ar in avoid:
            if (x - ax) ** 2 + (z - az) ** 2 < (ar + radius) ** 2:
                return False
        return True

    for _ in range(density):
        x = cx + RNG.uniform(-half_x + 1.0, half_x - 1.0)
        z = cz + RNG.uniform(-half_z + 1.0, half_z - 1.0)
        edge = min(half_x - abs(x - cx), half_z - abs(z - cz))
        # Trees crowd the rim so the middle stays playable.
        wants_tree = edge < 6.0 and RNG.random() < 0.55
        if wants_tree:
            if not free(x, z, 2.0):
                continue
            parent["children"].append(nature(
                "tree%d" % nid(), RNG.choice(trees), (x, top_y - 0.1, z),
                TREE * RNG.uniform(0.8, 1.25), RNG.uniform(0, 360)))
        else:
            if not free(x, z, 0.8):
                continue
            parent["children"].append(nature(
                "flora%d" % nid(), RNG.choice(small), (x, top_y - 0.05, z),
                PROP * RNG.uniform(0.85, 1.4), RNG.uniform(0, 360)))


def target(name, pos, yaw, index):
    """A shootable target: a body so the ray hits it, a mesh, and the script
    that owns its own reaction."""
    body = box_body(name, pos, (0.55, 0.75, 0.18), groups=["target"])
    body["transform"]["rotation"] = quat_yaw(yaw)
    body["behaviours"] = [script("scripts/target.js", value=100, index=index)]
    body["children"].append(kitmesh(name + "Mesh", "blaster", "target-large",
                                    (0.0, -0.75, 0.0), 1.6))
    # The burst is a child, and a child's position is local — the script finds it
    # by a group named after its owner rather than by looking around.
    body["children"].append(particles(
        name + "Burst", (0.0, 0.0, 0.0), enabled=False, looping=False,
        groups=["burst_" + name],
        maxParticles=64, spawnRate=260.0, lifetime=0.6, startSpeed=5.0,
        startSize=0.16, startColor=[1.0, 0.95, 0.6, 1.0], endColor=[1.0, 0.35, 0.1, 0.0],
        gravity=[0.0, -6.0, 0.0], radius=0.35, shape=1, emissive=6.0))
    return body


def pickup(name, pos, kind, value):
    """Coin / star: an Area so it reports the player, a spinning mesh, an aura."""
    area = node("Area", name, pos, groups=["pickup"])
    area["behaviours"] = [
        {"type": "Rotator", "enabled": True, "speed": 110.0, "axis": [0.0, 1.0, 0.0]},
        script("scripts/pickup.js", kind=kind, value=value),
    ]
    area["children"] = [
        node("CollisionShape", "Shape", shapeType=2, radius=1.1, offset=[0, 0, 0]),
    ]
    if kind == "star":
        area["children"].append(kitmesh(name + "Mesh", "platformer", "star", (0, 0, 0), 1.5,
                                        emissive=[1.0, 0.85, 0.25, 1.0]))
        area["children"].append(particles(
            name + "Aura", (0, 0, 0), effectClass=2, maxParticles=96, spawnRate=34.0,
            lifetime=1.6, startSpeed=0.5, startSize=0.13,
            startColor=[1.0, 0.92, 0.45, 1.0], endColor=[1.0, 0.55, 0.1, 0.0],
            gravity=[0.0, 0.7, 0.0], radius=0.85, shape=1, emissive=5.0))
    else:
        area["children"].append(kitmesh(name + "Mesh", "platformer", "coin-gold", (0, 0, 0), 1.1,
                                        emissive=[0.55, 0.4, 0.05, 1.0]))
    return area


def enemy(name, pos, model, hp, speed):
    body = node("CharacterBody", name, pos, groups=["enemy"])
    body["behaviours"] = [
        {"type": "Character", "enabled": True, "moveSpeed": speed, "jumpForce": 5.0,
         "faceMovement": True, "readsInput": False, "turnMode": 1,
         "turnDegreesPerSecond": 420.0,
         "idleClip": "idle", "walkClip": "run", "jumpClip": "jump"},
        script("scripts/enemy.js", hp=hp, speed=speed, damage=12.0),
    ]
    body["children"] = [
        node("CollisionShape", "Shape", shapeType=3, radius=0.36, height=1.75,
             axis=1, offset=[0, 0, 0]),
        node("Node", "Body", (0.0, -0.88, 0.0), 1.0, 180.0,
             importedFrom="assets/models/characters/%s.glb" % model),
        particles("Death", (0.0, 0.0, 0.0), enabled=False, looping=False,
                  groups=["death_" + name],
                  maxParticles=80, spawnRate=300.0, lifetime=0.8, startSpeed=4.5,
                  startSize=0.2, startColor=[0.75, 1.0, 0.55, 1.0],
                  endColor=[0.15, 0.5, 0.1, 0.0], gravity=[0.0, -4.0, 0.0],
                  radius=0.5, shape=1, emissive=5.0),
    ]
    return body


def moving_platform(name, pos, axis, span, period, size=(2.4, 0.35, 2.4)):
    body = box_body(name, pos, (size[0], size[1], size[2]), groups=["platform"])
    body["behaviours"] = [script("scripts/platform.js", axis=axis, span=span, period=period)]
    body["children"].append(slab(name + "Top", (0.0, 0.05, 0.0),
                                 (size[0] * 2.0, size[1] * 1.4, size[2] * 2.0), GRASS))
    body["children"].append(slab(name + "Base", (0.0, -0.25, 0.0),
                                 (size[0] * 1.8, size[1] * 1.6, size[2] * 1.8), DIRT))
    return body


def static_platform(name, pos, half, decorate=True):
    body = box_body(name, pos, half)
    body["children"].append(slab(name + "Top", (0.0, half[1] - 0.1, 0.0),
                                 (half[0] * 2.0, 0.25, half[2] * 2.0), GRASS))
    body["children"].append(slab(name + "Base", (0.0, -0.15, 0.0),
                                 (half[0] * 1.85, half[1] * 1.7, half[2] * 1.85), DIRT))
    if decorate and RNG.random() < 0.75:
        body["children"].append(nature(
            "deco%d" % nid(), RNG.choice(("grass_leafsLarge", "plant_bushSmall",
                                          "mushroom_redTall", "flower_yellowA")),
            (RNG.uniform(-half[0] * 0.5, half[0] * 0.5), half[1],
             RNG.uniform(-half[2] * 0.5, half[2] * 0.5)),
            PROP * RNG.uniform(0.8, 1.2), RNG.uniform(0, 360)))
    return body


# ── the level ───────────────────────────────────────────────────────────────
def build():
    scene = node("Scene", "Verdance")
    world = scene["children"]

    # Sun matched to the HDRI's own sun so the cast shadows agree with the sky.
    world.append(node("LightNode", "Sun", (0.0, 60.0, 0.0),
                      lightType=0, color=[1.0, 0.95, 0.86], intensity=3.1,
                      direction=[-0.42, -0.78, -0.46], castShadows=True,
                      bakeMode=0, range=10.0, spotInnerAngle=25.0, spotOuterAngle=35.0))
    # A cool fill from the opposite side keeps the shadowed faces readable
    # instead of collapsing into the ambient term.
    world.append(node("LightNode", "SkyFill", (0.0, 40.0, 0.0),
                      lightType=0, color=[0.45, 0.62, 0.85], intensity=0.75,
                      direction=[0.55, -0.5, 0.66], castShadows=False,
                      bakeMode=0, range=10.0, spotInnerAngle=25.0, spotOuterAngle=35.0))

    # ── Zone A — la clairière ───────────────────────────────────────────────
    clearing_centre = (0.0, 0.0, 12.0)
    clearing, clearing_rim = grass_island("Clearing", clearing_centre, 17.0, 15.0, 0.0)
    world.append(clearing)
    world.append(clearing_rim)

    decor = node("Node", "ClearingDecor")
    world.append(decor)
    scatter_flora(decor, clearing_centre, 17.0, 15.0, 0.0, 170, avoid=(
        (0.0, 14.0, 3.0),    # spawn
        (0.0, 3.0, 4.0),     # the firing line to the targets
        (-7.0, 6.0, 3.5),    # camp
        (0.0, -3.0, 4.0),    # the mouth of the bridge
    ))

    # Camp: the one warm spot in the clearing, and the fire is a live emitter.
    camp = node("Node", "Camp", (-7.0, 0.0, 6.0))
    camp["children"] = [
        nature("CampTent", "tent_detailedOpen", (-1.6, 0.0, -0.4), PROP * 1.3, 35.0),
        nature("CampFire", "campfire_stones", (0.9, 0.0, 0.6), PROP * 1.1),
        nature("CampLog", "log_stack", (2.4, 0.0, -1.4), PROP, 20.0),
        particles("Fire", (0.9, 0.55, 0.6), effectClass=1, maxParticles=110,
                  spawnRate=52.0, lifetime=1.0, startSpeed=1.7, startSize=0.3,
                  startColor=[1.0, 0.78, 0.28, 1.0], endColor=[0.9, 0.15, 0.02, 0.0],
                  gravity=[0.0, 2.4, 0.0], radius=0.16, shape=1, emissive=7.0,
                  noiseStrength=0.6, noiseFrequency=1.8, endSizeScale=0.15),
        particles("Embers", (0.9, 0.8, 0.6), maxParticles=48, spawnRate=9.0,
                  lifetime=2.6, startSpeed=1.0, startSize=0.07,
                  startColor=[1.0, 0.6, 0.2, 1.0], endColor=[1.0, 0.25, 0.0, 0.0],
                  gravity=[0.15, 1.5, 0.0], radius=0.3, shape=1, emissive=8.0,
                  noiseStrength=1.1, noiseFrequency=0.7),
        node("LightNode", "FireLight", (0.9, 1.1, 0.6), lightType=1,
             color=[1.0, 0.62, 0.24], intensity=6.0, direction=[0, -1, 0],
             castShadows=False, bakeMode=0, range=11.0,
             spotInnerAngle=25.0, spotOuterAngle=35.0),
    ]

    # Pollen drifting over the whole clearing — the cheapest thing that makes a
    # static forest feel alive, and it is what the bloom pass reads as light.
    world.append(particles("Pollen", (0.0, 3.0, 12.0), effectClass=2, maxParticles=220,
                           spawnRate=26.0, lifetime=8.0, startSpeed=0.35, startSize=0.075,
                           startColor=[1.0, 0.97, 0.72, 0.85], endColor=[0.85, 1.0, 0.6, 0.0],
                           gravity=[0.1, 0.05, 0.0], radius=16.0, shape=1, emissive=3.2,
                           noiseStrength=0.5, noiseFrequency=0.35, endSizeScale=0.8))

    # Signpost + tutorial targets.
    world.append(nature("Signpost", "sign", (2.6, 0.0, 8.0), PROP * 1.2, -25.0))
    for i, (tx, tz, yaw) in enumerate(((-6.5, 4.5, 14.0), (0.0, 2.6, 0.0), (6.5, 4.5, -14.0))):
        world.append(target("Target%d" % i, (tx, 1.4, tz), yaw, i))

    # Fauna: static models with a scripted idle, so the clearing has residents.
    fauna = (("animal-deer", (9.5, 7.0), 1.5), ("animal-fox", (-11.0, 16.0), 1.2),
             ("animal-bunny", (5.0, 18.5), 1.0), ("animal-chick", (-3.0, 20.0), 0.9),
             ("animal-panda", (13.0, 19.0), 1.4), ("animal-beaver", (-13.5, 3.5), 1.1))
    for i, (model, (fx, fz), fs) in enumerate(fauna):
        holder = node("Node", "Fauna%d" % i, (fx, 0.0, fz), 1.0, RNG.uniform(0, 360),
                      groups=["fauna"])
        holder["behaviours"] = [script("scripts/fauna.js", phase=RNG.uniform(0.0, 6.28))]
        holder["children"] = [kitmesh("FaunaMesh%d" % i, "pets", model, (0, 0, 0), fs)]
        world.append(holder)

    # ── Zone B — le passage ────────────────────────────────────────────────
    # A bridge over the gap, then islands that climb toward the arena.
    bridge = box_body("Bridge", (0.0, -0.15, -7.5), (2.2, 0.3, 5.5))
    for i in range(6):
        bridge["children"].append(nature("BridgePlank%d" % i, "bridge_wood",
                                         (0.0, 0.2, -4.6 + i * 1.85), TILE * 1.1))
    world.append(bridge)

    gauntlet = (
        ("Isle0", (-6.5, 1.2, -17.0), (2.8, 0.6, 2.8)),
        ("Isle1", (5.0, 2.6, -21.5), (2.4, 0.6, 2.4)),
        ("Isle2", (-4.0, 4.2, -27.0), (2.6, 0.6, 2.6)),
        ("Isle3", (6.5, 5.8, -32.5), (2.2, 0.6, 2.2)),
        ("Isle4", (0.0, 7.4, -37.0), (3.2, 0.6, 3.2)),
    )
    for name, pos, half in gauntlet:
        world.append(static_platform(name, pos, half))

    world.append(moving_platform("Mover0", (0.0, 2.0, -19.0), "x", 6.5, 5.0))
    world.append(moving_platform("Mover1", (1.0, 5.0, -29.5), "y", 2.6, 4.0))
    world.append(moving_platform("Mover2", (-2.0, 6.6, -34.5), "z", 4.0, 6.0))

    # Coins arced over the jumps — the readable "go this way" of a platformer.
    coin_at = ((-6.5, 3.2, -17.0), (-1.0, 3.6, -19.2), (5.0, 4.6, -21.5),
               (0.5, 5.2, -24.5), (-4.0, 6.2, -27.0), (1.0, 7.0, -29.5),
               (6.5, 7.8, -32.5), (-2.0, 8.6, -34.5), (0.0, 9.4, -37.0))
    for i, p in enumerate(coin_at):
        world.append(pickup("Coin%d" % i, p, "coin", 50))

    # ── Zone C — l'arène ───────────────────────────────────────────────────
    arena_centre = (0.0, 0.0, -50.0)
    arena, arena_rim = grass_island("Arena", arena_centre, 14.0, 12.0, 8.6, thickness=4.0)
    world.append(arena)
    world.append(arena_rim)

    arena_decor = node("Node", "ArenaDecor")
    world.append(arena_decor)
    scatter_flora(arena_decor, arena_centre, 14.0, 12.0, 8.6, 46,
                  avoid=((0.0, -40.0, 5.0), (0.0, -50.0, 7.0)))

    # A ruin at the centre: something to circle-strafe around, and a silhouette.
    ruin = node("Node", "Ruin", (0.0, 8.6, -52.0))
    ruin["children"] = [
        nature("Obelisk", "statue_obelisk", (0.0, 0.0, 0.0), PROP * 1.8),
        nature("Column0", "statue_column", (-4.0, 0.0, 2.5), PROP * 1.5, 15.0),
        nature("Column1", "statue_columnDamaged", (4.2, 0.0, 1.8), PROP * 1.5, -40.0),
        nature("Ring", "statue_ring", (0.0, 0.0, -4.5), PROP * 1.6, 25.0),
        particles("RuinAura", (0.0, 2.4, 0.0), effectClass=2, maxParticles=120,
                  spawnRate=30.0, lifetime=2.8, startSpeed=0.6, startSize=0.12,
                  startColor=[0.55, 0.9, 1.0, 0.9], endColor=[0.2, 0.5, 1.0, 0.0],
                  gravity=[0.0, 0.9, 0.0], radius=2.6, shape=5, emissive=5.5),
    ]
    world.append(ruin)

    enemy_models = ("enemy_a", "enemy_b", "enemy_c", "enemy_a", "enemy_b", "enemy_c")
    for i, model in enumerate(enemy_models):
        angle = math.radians(i * 60.0 + 20.0)
        world.append(enemy("Enemy%d" % i,
                           (arena_centre[0] + math.cos(angle) * 9.0, 9.8,
                            arena_centre[2] + math.sin(angle) * 8.0),
                           model, hp=60.0 + i * 10.0, speed=3.0 + (i % 3) * 0.5))

    world.append(pickup("Star", (0.0, 11.4, -52.0), "star", 1000))

    # An ally at the arena entrance, for scale and life.
    ally = node("Node", "Ally", (-3.4, 8.6, -41.0), 1.0, 160.0, groups=["fauna"])
    ally["behaviours"] = [script("scripts/fauna.js", phase=1.1, amplitude=0.05)]
    ally["children"] = [node("Node", "Body", (0, 0, 0), 1.0, 180.0,
                             importedFrom="assets/models/characters/ally.glb")]
    world.append(ally)

    # ── the player ─────────────────────────────────────────────────────────
    player = node("CharacterBody", "Player", (0.0, 1.4, 14.0), groups=["player"])
    player["behaviours"] = [
        {"type": "Character", "enabled": True,
         "moveSpeed": 5.4, "sprintMultiplier": 1.7,
         "groundAcceleration": 45.0, "groundDeceleration": 38.0,
         "gravity": 20.0, "airControl": 0.65, "airAcceleration": 22.0,
         "fallGravityMultiplier": 1.45, "maxFallSpeed": 26.0,
         "apexGravityMultiplier": 0.62, "apexThreshold": 2.2,
         "jumpHeight": 2.35, "jumpCutoffMultiplier": 0.45,
         "coyoteTime": 0.14, "jumpBufferTime": 0.16, "jumpCount": 2,
         "faceMovement": True, "turnMode": 0, "turnSpeed": 16.0,
         "idleClip": "idle", "walkClip": "run", "jumpClip": "jump"},
        script("scripts/player.js"),
    ]
    player["children"] = [
        node("CollisionShape", "Shape", shapeType=3, radius=0.34, height=1.75,
             axis=1, offset=[0, 0, 0]),
        node("Node", "Body", (0.0, -0.88, 0.0), 1.0, 180.0,
             importedFrom="assets/models/characters/player.glb"),
    ]
    # Weapon + muzzle flash ride a node at hand height; the rig's bones are not
    # addressable from a scene, so the offset is authored here.
    hand = node("Node", "Hand", (0.30, -0.12, -0.30), 1.0, 0.0, groups=["hand"])
    hand["children"] = [
        kitmesh("Blaster", "blaster", "blaster-h", (0.0, 0.0, 0.0), 0.75,
                roughness=0.4, metallic=0.25),
        particles("Muzzle", (0.0, 0.06, -0.55), enabled=False, looping=False,
                  groups=["muzzle"],
                  maxParticles=40, spawnRate=420.0, lifetime=0.14, startSpeed=5.5,
                  startSize=0.16, startColor=[1.0, 0.95, 0.7, 1.0],
                  endColor=[1.0, 0.4, 0.05, 0.0], gravity=[0, 0, 0], radius=0.05,
                  shape=4, coneAngle=16.0, emissive=9.0, endSizeScale=0.2),
    ]
    player["children"].append(hand)
    world.append(player)

    world.append(node("Camera", "MainCamera", (0.0, 5.0, 22.0), groups=["camera"],
                      fovDegrees=64.0, nearZ=0.08, farZ=600.0, priority=0, active=True,
                      behaviours=[{
                          "type": "CameraFollow", "enabled": True, "targetGroup": "player",
                          "distance": 6.2, "height": 1.9,
                          # No shoulder offset: it displaces the rig sideways while it keeps
                          # looking at the pivot, so the camera axis — and with it the
                          # crosshair — no longer points where the character faces. Aiming
                          # has to mean what it shows.
                          "shoulderOffset": 0.0,
                          # CameraFollow's pitch sign is inverted from the intuition: a positive
                          # value drops the rig BELOW its target and aims it upward, which
                          # sent every shot over the targets. Level is the neutral start.
                          "initialPitch": 0.0, "minPitch": -30.0, "maxPitch": 55.0,
                          "positionDamping": 16.0, "verticalDamping": 9.0,
                          "lookAhead": 0.16, "lookAheadMaxDistance": 2.2,
                          "fovAtRest": 64.0, "fovAtSpeed": 72.0, "fovSpeedReference": 9.5,
                          "collisionMargin": 0.35, "minDistance": 1.1,
                          "recenterDelay": 2.5, "recenterSpeed": 70.0,
                          "recenterMinSpeed": 1.5, "recenterOnVelocity": True,
                      }]))

    # ── shared effect pools (no runtime node creation from scripts) ────────
    pool = node("Node", "Effects")
    for i in range(6):
        pool["children"].append(particles(
            "Impact%d" % i, (0.0, -200.0, 0.0), enabled=False, looping=False,
            groups=["impact"], maxParticles=56, spawnRate=340.0, lifetime=0.45,
            startSpeed=4.0, startSize=0.13, startColor=[1.0, 0.92, 0.55, 1.0],
            endColor=[1.0, 0.3, 0.05, 0.0], gravity=[0.0, -7.0, 0.0], radius=0.12,
            shape=1, emissive=7.0, endSizeScale=0.2))
    # Bolts: a script can move and turn a node but not scale one, so their
    # elongated shape is authored here and only their transform is animated.
    for i in range(6):
        pool["children"].append(node(
            "MeshNode", "Bolt%d" % i, (0.0, -200.0, 0.0), [0.09, 0.09, 1.7],
            groups=["tracer"], enabled=False,
            mesh="cube", baseColor=[1.0, 0.88, 0.42, 1.0],
            emissive=[4.5, 3.0, 0.7, 1.0], roughness=0.35, metallic=0.0,
            castShadows=False))
    world.append(pool)

    # Crossing this plane is what wakes the arena; an Area is the only thing in
    # the scene that can report the player entering a volume.
    gate = node("Area", "ArenaGate", (0.0, 10.5, -40.5))
    gate["behaviours"] = [script("scripts/trigger.js", call="enterArena")]
    gate["children"] = [node("CollisionShape", "Shape", shapeType=1,
                             halfExtents=[9.0, 3.0, 2.0], offset=[0, 0, 0])]
    world.append(gate)

    # ── HUD ────────────────────────────────────────────────────────────────
    hud = node("UICanvasNode", "HUD", width=1920.0, height=1080.0)
    hud["children"] = [
        # White text over a bright sky is unreadable; the panel is what makes the
        # HUD legible against the brightest thing in the frame.
        node("UIColorNode", "HudPanel", color=[0.04, 0.07, 0.10, 0.45],
             x=16.0, y=16.0, width=760.0, height=132.0,
             anchorX=0.0, anchorY=0.0, pivotX=0.0, pivotY=0.0),
        uitext("ScoreText", "hud_score", "", 34.0, [1.0, 0.95, 0.75, 1.0], 32.0, 28.0),
        uitext("HealthText", "hud_health", "", 30.0, [0.85, 1.0, 0.85, 1.0], 32.0, 74.0),
        uitext("ObjectiveText", "hud_objective", "", 26.0, [0.8, 0.93, 1.0, 1.0], 32.0, 118.0),
        uitext("BannerText", "hud_banner", "", 46.0, [1.0, 1.0, 1.0, 1.0], 32.0, 420.0,
               width=1856.0, height=90.0),
        uitext("HintText", "hud_hint", "", 22.0, [0.78, 0.85, 0.95, 1.0], 32.0, 1000.0,
               width=1856.0, height=40.0),
        # A shooter needs to show where it is pointing; the shot converges on
        # this exact point (see crosshairPoint in scripts/player.js).
        uitext("Crosshair", "hud_crosshair", "+", 40.0, [1.0, 1.0, 1.0, 0.85],
               930.0, 512.0, width=60.0, height=56.0),
    ]
    world.append(hud)

    # The look is direct-light led: one strong sun for contrast and shadow shape,
    # everything indirect kept low. A midday HDRI is bright enough that a generous
    # IBL/GI/ambient stack lifts the blacks and the whole frame reads as washed.
    scene["settings"] = {
        "ambient": srgb(0.10, 0.13, 0.19)[:3],
        "clearColor": srgb(0.45, 0.66, 0.90)[:3],
        "postProcessing": True,
        "lightingMode": 0,
        "giEnabled": True, "giMode": 1, "giIntensity": 0.75,
        "skyboxTexture": "assets/skies/sky.hdr",
        "skyboxExposure": 0.85, "skyboxRotation": 155.0,
        "iblEnabled": True, "iblDiffuseIntensity": 0.22, "iblSpecularIntensity": 0.7,
        "aoEnabled": True, "aoRadius": 0.9, "aoIntensity": 1.25, "aoPower": 1.6,
        "fogEnabled": True, "fogColor": srgb(0.60, 0.76, 0.92)[:3],
        "fogStart": 60.0, "fogDensity": 0.006,
        "bloomEnabled": True, "bloomThreshold": 1.9, "bloomIntensity": 0.22,
        "bloomRadius": 4.0,
        "changeRenderingAtLoad": True,
    }
    return scene


def uitext(name, group, text, size, color, x, y, width=760.0, height=48.0):
    return node("UITextNode", name, groups=[group], text=text, fontSize=size,
                color=color, x=x, y=y, width=width, height=height,
                anchorX=0.0, anchorY=0.0, pivotX=0.0, pivotY=0.0)


SOUNDS = ("shoot", "shoot_alt", "hit", "target_break", "enemy_die", "explosion",
          "step_a", "step_b", "land", "coin", "star", "hurt", "victory", "defeat",
          "jump", "spring")


def main():
    scene = build()
    doc = {"schema": 2, "version": 2, "scene": scene}
    scenes_dir = os.path.join(HERE, "scenes")
    os.makedirs(scenes_dir, exist_ok=True)
    with open(os.path.join(scenes_dir, "verdance.scene"), "w") as f:
        json.dump(doc, f, indent=1)

    project = {
        "schema": 1, "version": 1,
        "name": "Verdance",
        "engineVersion": "0.1.0",
        "mainScene": "scenes/verdance.scene",
        "autoloads": {"GameState": "scripts/game_state.mjs"},
        "audio": {
            "masterVolume": 0.85,
            "default": {"loop": False, "volume": 1.0, "spatialized": False,
                        "minDistance": 1.0, "maxDistance": 100.0},
            "aliases": {s: "assets/audio/%s.ogg" % s for s in SOUNDS},
        },
        "runtime": {"maxFps": 144, "vsync": True},
        "rendering": {"autoMeshLods": False, "shadowDistance": 70.0,
                      "shadowResolution": 4096, "shadowSoftness": 1.15,
                      "showColliders": False},
    }
    with open(os.path.join(HERE, "VerticalSlice.saidaproj"), "w") as f:
        json.dump(project, f, indent=2, sort_keys=True)

    def count(n):
        return 1 + sum(count(c) for c in n["children"])

    print("scenes/verdance.scene — %d nodes" % count(scene))


if __name__ == "__main__":
    main()
