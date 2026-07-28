#!/usr/bin/env python3
"""Generate the VerticalSlice project — "Verdance", a third-person nature slice
built to exercise SaidaEngine end to end: HDRI skybox + IBL, GI, SSAO, bloom,
fog, shadows, skinned characters, SaidaFX particles, physics bodies, areas,
signals, UI and QuickJS gameplay.

    python VerticalSlice/gen_slice.py

Writes, next to itself:
  * scenes/verdance.scene   — the opening level
  * scenes/deep_wilds.scene — the large, dense second level
  * scenes/main_menu.scene  — the menu's 3D stage
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

# ── The Deep Wilds' extent ──────────────────────────────────────────────────
# The second level is the slice's large, dense-geometry stress case. Its
# landmarks, corridors, shorelines and perimeter all derive from the numbers
# below, so the map is resized by editing these rather than by re-authoring a
# few hundred coordinates by hand. The river band is deliberately left alone:
# the bridges are authored to that exact span.
WILDS_HALF_X = 84.0      # the map runs 168 m east to west
WILDS_SOUTH_FAR = 84.0   # southern shore, behind the player's spawn
WILDS_NORTH_FAR = -76.0  # northern shore, behind the Ancient Heart
WILDS_RIVER_Z = 8.0      # centre of the channel splitting the two land masses
WILDS_RIVER_HALF = 4.0   # half-width of the channel — the bridges match this
WILDS_DENSITY = 0.22     # vegetation placement attempts per m² of ground

# The shorelines, and the centre/half-extent of each land mass between them.
WILDS_SOUTH_NEAR = WILDS_RIVER_Z + WILDS_RIVER_HALF
WILDS_NORTH_NEAR = WILDS_RIVER_Z - WILDS_RIVER_HALF
WILDS_SOUTH_C = (WILDS_SOUTH_FAR + WILDS_SOUTH_NEAR) * 0.5
WILDS_SOUTH_H = (WILDS_SOUTH_FAR - WILDS_SOUTH_NEAR) * 0.5
WILDS_NORTH_C = (WILDS_NORTH_FAR + WILDS_NORTH_NEAR) * 0.5
WILDS_NORTH_H = (WILDS_NORTH_NEAR - WILDS_NORTH_FAR) * 0.5

# Five crossings over a 168 m river, so neither shore is a long detour.
WILDS_BRIDGES = (-60.0, -30.0, 0.0, 30.0, 60.0)

# The waterline the stone river bed is built around: 0.88 m below the ground
# the player walks on, so the banks read as banks. The sea plane is centred on
# the map and reaches well past the tree wall, closing the horizon with water.
WILDS_WATER_Y = -0.88
WILDS_WATER_C = (WILDS_SOUTH_FAR + WILDS_NORTH_FAR) * 0.5
WILDS_SEA_HALF = 532.0

# The spawn sits a clear margin inside the southern tree wall: the perimeter is
# an unbroken horizon and does not consult the scatter's avoid circles, so the
# player would otherwise start with their head inside a trunk.
WILDS_SPAWN_Z = WILDS_SOUTH_FAR - 8.0

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


def cartoon_water(name, pos, size, **kw):
    """A WaterNode in its stepped cartoon style.

    `size` is a half-extent and the plane is square, so it cannot be cut to the
    shape of a channel. It does not need to be: the water pipeline is
    depth-tested, so a plane sunk to the waterline is occluded by the land
    masses and shows only where there is no ground above it — the river between
    the two shores, and the open sea past them. The banks are therefore authored
    in terrain, not in the water's own shoreMode, which is left at None: an
    analytic beach or lake edge would cut across the channel.
    """
    props = {
        "style": 1,                     # WaterNode::Style::Cartoon
        "size": size,
        "shoreMode": 0,                 # ShoreMode::None — the terrain is the shore
        "deepColor": hexcolor("#12506E")[:3],
        "shallowColor": hexcolor("#5AA9C8")[:3],   # the palette's WATER
        "foamColor": hexcolor("#F2FBFF")[:3],
        "roughness": 0.12, "reflectivity": 0.5,
        "fresnelPower": 4.0, "specularPower": 90.0, "specularIntensity": 0.7,
        # cartoon*Scale is a spatial frequency in radians per metre, so the
        # wavelength is 2*pi/scale: 2.33 m for the primary, 1.44 m for the
        # detail. A channel eight metres across needs ripples on that order —
        # anything near the default 0.22 puts a single 28 m crest across the
        # whole river.
        #
        # The two frequencies are deliberately in the golden ratio (4.37/2.70 =
        # 1.618) and travel on unrelated headings that are not square to the
        # banks. Near-harmonic pairs re-align every few crests and read as a
        # repeating pattern; an irrational ratio never quite repeats.
        "cartoonWaveScale": 2.70, "cartoonWaveSpeed": 1.9,
        "cartoonWaveAngle": 8.0, "cartoonWaveSharpness": 0.45,
        "cartoonDetailScale": 4.37, "cartoonDetailSpeed": 1.6,
        "cartoonDetailAngle": 74.0, "cartoonDetailStrength": 0.65,
        # The detail wave both sums into the surface and gates the crests
        # (breakup in cartoon_water.frag), so a strong one is what stops the
        # white caps arriving in even rows.
        #
        # Fewer, softer bands and a narrow crest: still stepped and graphic,
        # but no longer shouting.
        "cartoonColorSteps": 4.0, "cartoonColorContrast": 0.22,
        "cartoonCrestWidth": 0.06, "cartoonCrestIntensity": 0.5,
    }
    props.update(kw)
    return node("Water", name, pos, **props)


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
    elif kind == "relic":
        area["children"].append(kitmesh(
            name + "Mesh", "platformer", "jewel", (0, 0, 0), 1.45,
            emissive=[0.25, 1.1, 0.75, 1.0]))
        area["children"].append(particles(
            name + "Aura", (0, 0, 0), effectClass=2, maxParticles=128,
            spawnRate=42.0, lifetime=2.0, startSpeed=0.65, startSize=0.14,
            startColor=[0.45, 1.0, 0.78, 1.0], endColor=[0.1, 0.55, 1.0, 0.0],
            gravity=[0.0, 0.85, 0.0], radius=1.2, shape=5, emissive=6.0))
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


def wilds_land(name, centre, half_x, half_z, top_y=0.0, thickness=3.0):
    """A broad collidable terrain section without the island rim cost."""
    cx, _, cz = centre
    body = box_body(name, (cx, top_y - thickness * 0.5, cz),
                    (half_x, thickness * 0.5, half_z))
    body["children"].append(
        slab(name + "Top", (0.0, thickness * 0.5 - 0.12, 0.0),
             (half_x * 2.0, 0.24, half_z * 2.0), GRASS, roughness=0.98))
    body["children"].append(
        slab(name + "Earth", (0.0, -0.1, 0.0),
             (half_x * 2.0 - 0.5, thickness - 0.25, half_z * 2.0 - 0.5),
             DIRT, roughness=1.0))
    return body


def scatter_dense_wilds(visual_parent, collision_parent, centre, half_x, half_z,
                        top_y, density, avoid=()):
    """Populate a dense forest while preserving authored travel corridors."""
    cx, _, cz = centre
    canopy = (
        "tree_pineTallA_detailed", "tree_pineTallB_detailed",
        "tree_pineTallC_detailed", "tree_pineTallD_detailed",
        "tree_detailed", "tree_oak", "tree_fat", "tree_tall",
    )
    understory = (
        "plant_bushDetailed", "plant_bushLarge", "plant_bushLargeTriangle",
        "grass_leafsLarge", "grass_large", "grass_leafs", "hanging_moss",
        "mushroom_redGroup", "mushroom_tanGroup", "stump_roundDetailed",
        "log_large", "rock_largeA", "rock_largeC", "rock_largeE",
    )
    ground_cover = (
        "flower_redA", "flower_redB", "flower_yellowA", "flower_yellowC",
        "flower_purpleA", "flower_purpleC", "grass", "plant_flatShort",
        "plant_flatTall", "rock_smallA", "rock_smallD", "stone_smallB",
    )

    def free(x, z, radius):
        for ax, az, ar in avoid:
            if (x - ax) ** 2 + (z - az) ** 2 < (ar + radius) ** 2:
                return False
        return True

    trees = 0
    props = 0
    for _ in range(density):
        x = cx + RNG.uniform(-half_x + 1.2, half_x - 1.2)
        z = cz + RNG.uniform(-half_z + 1.2, half_z - 1.2)
        roll = RNG.random()
        if roll < 0.42:
            if not free(x, z, 1.35):
                continue
            model = RNG.choice(canopy)
            scale = TREE * RNG.uniform(1.0, 1.65)
            yaw = RNG.uniform(0.0, 360.0)
            trees += 1
            # A sampled subset gets a simple trunk collider. The detailed model
            # remains visual; one compound body cannot represent all its meshes.
            if trees % 7 == 0:
                trunk = box_body("WildTrunk%d" % nid(), (x, top_y + 1.8, z),
                                 (0.48, 1.8, 0.48))
                tree_visual = nature(
                    "WildTree%d" % nid(), model, (0.0, -1.8, 0.0),
                    scale, yaw)
                tree_visual["groups"] = ["wild_vegetation"]
                trunk["children"].append(tree_visual)
                collision_parent["children"].append(trunk)
            else:
                tree_visual = nature(
                    "WildTree%d" % nid(), model, (x, top_y - 0.08, z),
                    scale, yaw)
                tree_visual["groups"] = ["wild_vegetation"]
                visual_parent["children"].append(tree_visual)
        elif roll < 0.82:
            if not free(x, z, 0.75):
                continue
            props += 1
            understory_visual = nature(
                "WildUnderstory%d" % nid(), RNG.choice(understory),
                (x, top_y - 0.04, z), PROP * RNG.uniform(0.9, 1.7),
                RNG.uniform(0.0, 360.0))
            understory_visual["groups"] = ["wild_vegetation"]
            visual_parent["children"].append(understory_visual)
        else:
            if not free(x, z, 0.35):
                continue
            props += 1
            ground_visual = nature(
                "WildGroundCover%d" % nid(), RNG.choice(ground_cover),
                (x, top_y - 0.03, z), PROP * RNG.uniform(0.75, 1.35),
                RNG.uniform(0.0, 360.0))
            ground_visual["groups"] = ["wild_vegetation"]
            visual_parent["children"].append(ground_visual)
    return trees, props


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

    # ── Zone A — the clearing ───────────────────────────────────────────────
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

    # ── Zone B — the passage ────────────────────────────────────────────────
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

    # ── Zone C — the arena ─────────────────────────────────────────────────
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


def build_level_two():
    """Build The Deep Wilds, a broad objective-driven forest level."""
    scene = node("Scene", "TheDeepWilds")
    world = scene["children"]

    director = node("Node", "DeepWildsDirector", groups=["deep_wilds"])
    director["behaviours"] = [script("scripts/level2_init.js")]
    world.append(director)

    world.append(node("LightNode", "WildsSun", (0.0, 70.0, 0.0),
                      lightType=0, color=[1.0, 0.92, 0.78], intensity=3.25,
                      direction=[-0.48, -0.76, -0.43], castShadows=True,
                      bakeMode=0, range=10.0, spotInnerAngle=25.0,
                      spotOuterAngle=35.0))
    world.append(node("LightNode", "WildsSkyFill", (0.0, 45.0, 0.0),
                      lightType=0, color=[0.38, 0.58, 0.78], intensity=0.68,
                      direction=[0.52, -0.55, 0.64], castShadows=False,
                      bakeMode=0, range=10.0, spotInnerAngle=25.0,
                      spotOuterAngle=35.0))

    # Two large land masses leave a real river channel between them. The level
    # is continuous and freely explorable; bridges are landmarks, not a linear
    # platforming sequence.
    world.append(wilds_land("SouthWilds", (0.0, 0.0, WILDS_SOUTH_C),
                            WILDS_HALF_X, WILDS_SOUTH_H))
    world.append(wilds_land("NorthWilds", (0.0, 0.0, WILDS_NORTH_C),
                            WILDS_HALF_X, WILDS_NORTH_H))

    river = box_body("RiverBed", (0.0, -2.3, WILDS_RIVER_Z),
                     (WILDS_HALF_X, 1.3, WILDS_RIVER_HALF))
    river["children"].append(
        slab("RiverStone", (0.0, 0.05, 0.0),
             (WILDS_HALF_X * 2.0, 2.4, WILDS_RIVER_HALF * 2.0), STONE,
             roughness=0.95))
    world.append(river)

    # One cartoon water plane serves the whole level: it fills the channel
    # between the shores and carries on past them as open sea. It sits at the
    # waterline the stone bed was already built for, well below the ground the
    # player walks on, so the land masses hide everything but the river.
    world.append(cartoon_water("WildsSea", (0.0, WILDS_WATER_Y, WILDS_WATER_C),
                               WILDS_SEA_HALF))

    for bridge_index, bridge_x in enumerate(WILDS_BRIDGES):
        bridge = box_body("WildBridge%d" % bridge_index,
                          (bridge_x, 0.0, WILDS_RIVER_Z), (2.6, 0.25, 4.8))
        for plank in range(6):
            bridge["children"].append(
                nature("WildBridge%dPlank%d" % (bridge_index, plank),
                       "bridge_wood", (0.0, 0.22, -4.1 + plank * 1.65),
                       TILE * 1.15))
        world.append(bridge)

    # Rocks articulate the banks and make the river readable across the whole
    # map without turning it into an impassable wall.
    river_decor = node("Node", "RiverDecor")
    for x in range(-int(WILDS_HALF_X) + 2, int(WILDS_HALF_X) - 1, 4):
        if any(abs(x - bridge_x) < 4.5 for bridge_x in WILDS_BRIDGES):
            continue
        for z in (WILDS_NORTH_NEAR - 0.2, WILDS_SOUTH_NEAR + 0.2):
            river_decor["children"].append(
                nature("RiverRock%d" % nid(),
                       RNG.choice(("rock_largeA", "rock_largeB", "rock_largeD",
                                   "stone_largeC")),
                       (x + RNG.uniform(-0.6, 0.6), -0.15, z),
                       PROP * RNG.uniform(0.9, 1.35), RNG.uniform(0.0, 360.0)))
    world.append(river_decor)

    # Pushed out towards the four quarters of the enlarged map: reaching one
    # is now a walk through forest rather than a glance across a clearing.
    sanctuaries = (
        ("RootSanctuary", (-56.0, 0.0, 54.0), 0.0),
        ("SunSanctuary", (56.0, 0.0, 44.0), 120.0),
        ("MoonSanctuary", (-54.0, 0.0, -44.0), 240.0),
    )

    for sanctuary_index, (name, (sx, sy, sz), rotation) in enumerate(sanctuaries):
        shrine = node("Node", name, (sx, sy, sz), 1.0, rotation)
        shrine["children"] = [
            nature(name + "Ring", "statue_ring", (0.0, 0.0, 0.0),
                   PROP * 2.1, 0.0),
            nature(name + "Obelisk", "statue_obelisk", (0.0, 0.0, 0.0),
                   PROP * 1.8, 0.0),
            nature(name + "ColumnA", "statue_column",
                   (-4.2, 0.0, 2.8), PROP * 1.45, 18.0),
            nature(name + "ColumnB", "statue_columnDamaged",
                   (4.0, 0.0, 2.4), PROP * 1.45, -28.0),
            nature(name + "MossA", "hanging_moss",
                   (-2.4, 0.15, -2.0), PROP * 1.3, 20.0),
            nature(name + "MossB", "plant_bushDetailed",
                   (2.7, 0.0, -2.5), PROP * 1.35, -15.0),
            particles(name + "Aura", (0.0, 2.2, 0.0), effectClass=2,
                      maxParticles=180, spawnRate=38.0, lifetime=3.2,
                      startSpeed=0.7, startSize=0.13,
                      startColor=[0.4, 1.0, 0.72, 0.9],
                      endColor=[0.12, 0.45, 1.0, 0.0],
                      gravity=[0.0, 0.8, 0.0], radius=3.2, shape=5,
                      emissive=5.5, noiseStrength=0.4,
                      noiseFrequency=0.55),
        ]
        world.append(shrine)

        for guardian_index in range(4):
            angle = math.radians(rotation + guardian_index * 90.0 + 25.0)
            gx = sx + math.cos(angle) * 6.5
            gz = sz + math.sin(angle) * 6.5
            model = ("enemy_a", "enemy_b", "enemy_c")[
                (sanctuary_index + guardian_index) % 3]
            world.append(enemy(
                "WildGuardian%d_%d" % (sanctuary_index, guardian_index),
                (gx, 1.2, gz), model,
                hp=75.0 + guardian_index * 10.0,
                speed=3.15 + (guardian_index % 2) * 0.45))

        relic = pickup("WildRelic%d" % sanctuary_index,
                       (sx, 2.3, sz), "relic", 500)
        relic["enabled"] = False
        relic["groups"].append("wild_relic")
        world.append(relic)

    # The final heart is visible as a dormant landmark only after all three
    # relics respond. Its objective pickup is enabled by GameState.
    heart_site = node("Node", "AncientHeartSite", (50.0, 0.0, -52.0))
    heart_site["children"] = [
        nature("HeartRingA", "statue_ring", (0.0, 0.0, 0.0), PROP * 2.7, 0.0),
        nature("HeartRingB", "statue_ring", (0.0, 1.2, 0.0), PROP * 2.1, 90.0),
        nature("HeartHead", "statue_head", (0.0, 0.0, -3.0), PROP * 1.7, 180.0),
        nature("HeartColumnA", "statue_column", (-5.0, 0.0, 2.5), PROP * 1.7, 10.0),
        nature("HeartColumnB", "statue_columnDamaged", (5.0, 0.0, 2.0),
               PROP * 1.7, -25.0),
        particles("HeartMist", (0.0, 2.6, 0.0), effectClass=2,
                  maxParticles=240, spawnRate=44.0, lifetime=4.0,
                  startSpeed=0.55, startSize=0.18,
                  startColor=[0.35, 0.8, 1.0, 0.85],
                  endColor=[0.1, 0.25, 0.8, 0.0],
                  gravity=[0.0, 0.65, 0.0], radius=4.5, shape=5,
                  emissive=6.2, noiseStrength=0.75, noiseFrequency=0.42),
    ]
    world.append(heart_site)
    heart = pickup("AncientHeart", (50.0, 2.8, -52.0), "star", 2500)
    heart["enabled"] = False
    heart["groups"].append("wild_heart")
    world.append(heart)

    # Circular avoids reserve landmarks and a network of broad paths. Dense
    # vegetation fills everything else, including deep canopy at the perimeter.
    avoids = [
        (0.0, WILDS_SPAWN_Z, 9.0),
        (-56.0, 54.0, 10.0), (56.0, 44.0, 10.0),
        (-54.0, -44.0, 10.0), (50.0, -52.0, 11.0),
    ]
    # Both bridgeheads of every crossing stay clear.
    for bridge_x in WILDS_BRIDGES:
        avoids.append((bridge_x, WILDS_RIVER_Z, 5.0))

    def reserve_corridor(start, end, radius=2.8, spacing=4.5):
        dx = end[0] - start[0]
        dz = end[1] - start[1]
        distance = math.sqrt(dx * dx + dz * dz)
        steps = max(1, int(distance / spacing))
        for step in range(steps + 1):
            t = step / float(steps)
            avoids.append((start[0] + dx * t, start[1] + dz * t, radius))

    # A north-south spine over the centre bridge, spurs out to each landmark,
    # and one east-west run per shore so the far corners of a 168 m map are
    # reachable without pushing through unbroken canopy.
    reserve_corridor((0.0, WILDS_SPAWN_Z),
                     (0.0, WILDS_NORTH_FAR + 8.0), 3.4)
    reserve_corridor((0.0, 58.0), (-56.0, 54.0))
    reserve_corridor((0.0, 48.0), (56.0, 44.0))
    reserve_corridor((0.0, -34.0), (-54.0, -44.0))
    reserve_corridor((0.0, -44.0), (50.0, -52.0))
    reserve_corridor((-72.0, 40.0), (72.0, 40.0), 2.6)
    reserve_corridor((-72.0, -28.0), (72.0, -28.0), 2.6)

    wilds_visual = node("Node", "DenseWildsVegetation")
    wilds_collision = node("Node", "DenseWildsTrunkColliders")
    # Attempts scale with ground area, so the forest keeps a constant density
    # whatever WILDS_* extent the map is built at.
    def wilds_attempts(half_z):
        return int(WILDS_HALF_X * 2.0 * half_z * 2.0 * WILDS_DENSITY)

    south_trees, south_props = scatter_dense_wilds(
        wilds_visual, wilds_collision, (0.0, 0.0, WILDS_SOUTH_C),
        WILDS_HALF_X, WILDS_SOUTH_H, 0.0, wilds_attempts(WILDS_SOUTH_H),
        avoids)
    north_trees, north_props = scatter_dense_wilds(
        wilds_visual, wilds_collision, (0.0, 0.0, WILDS_NORTH_C),
        WILDS_HALF_X, WILDS_NORTH_H, 0.0, wilds_attempts(WILDS_NORTH_H),
        avoids)

    # A high-detail living wall closes the horizon. This is intentionally not
    # LOD-reduced: the second level is also the slice's dense-geometry stress.
    perimeter = []
    edge_x = WILDS_HALF_X - 3.0
    for x in range(-int(edge_x), int(edge_x) + 1, 4):
        perimeter.append((float(x), WILDS_SOUTH_FAR - 3.0))
        perimeter.append((float(x), WILDS_NORTH_FAR + 3.0))
    for z in range(int(WILDS_NORTH_FAR) + 4, int(WILDS_SOUTH_FAR) - 3, 4):
        # The east and west walls skip the river mouth: there is no ground
        # under the channel, and a tree there would hang over open water.
        if abs(z - WILDS_RIVER_Z) <= WILDS_RIVER_HALF + 1.5:
            continue
        perimeter.append((-edge_x, float(z)))
        perimeter.append((edge_x, float(z)))
    for x, z in perimeter:
        perimeter_tree = nature(
            "PerimeterTree%d" % nid(),
            RNG.choice(("tree_pineTallA_detailed",
                        "tree_pineTallB_detailed",
                        "tree_pineTallC_detailed",
                        "tree_pineTallD_detailed")),
            (x + RNG.uniform(-0.5, 0.5), -0.1,
             z + RNG.uniform(-0.5, 0.5)),
            TREE * RNG.uniform(1.35, 1.85), RNG.uniform(0.0, 360.0))
        perimeter_tree["groups"] = ["wild_vegetation"]
        wilds_visual["children"].append(perimeter_tree)
    world.append(wilds_visual)
    world.append(wilds_collision)

    # Collectibles loosely follow the path network but remain optional.
    # Eighteen coins, laid along the corridor network so the paths through the
    # forest read as paths. The count is part of the level's contract: with the
    # three relics and the heart it is the 22 pickups e2e_level2.js expects.
    coin_positions = (
        (0.0, 1.2, 68.0), (0.0, 1.2, 62.0), (0.0, 1.2, 50.0),
        (0.0, 1.2, 30.0), (0.0, 1.2, 18.0),
        (0.0, 1.2, 12.0), (0.0, 1.2, 8.0), (0.0, 0.2, 4.0),
        (0.0, 1.2, -8.0), (0.0, 1.2, -22.0), (0.0, 1.2, -40.0),
        (-30.0, 1.2, 56.0), (30.0, 1.2, 46.0),
        (-30.0, 1.2, -40.0), (28.0, 1.2, -50.0),
        (-60.0, 1.2, 40.0), (60.0, 1.2, 40.0), (-40.0, 1.2, -28.0),
    )
    for coin_index, coin_pos in enumerate(coin_positions):
        world.append(pickup("WildCoin%d" % coin_index, coin_pos, "coin", 75))

    fauna_models = (
        "animal-deer", "animal-fox", "animal-bunny", "animal-panda",
        "animal-beaver", "animal-cow", "animal-hog", "animal-chick",
        "animal-deer", "animal-fox", "animal-bunny", "animal-panda",
    )
    fauna_positions = (
        (-24.0, 70.0), (22.0, 66.0), (-68.0, 48.0), (70.0, 52.0),
        (-40.0, 30.0), (36.0, 24.0), (-70.0, -12.0), (68.0, -16.0),
        (-26.0, -34.0), (14.0, -58.0), (70.0, -60.0), (-68.0, -64.0),
    )
    for fauna_index, (model, (fx, fz)) in enumerate(
            zip(fauna_models, fauna_positions)):
        holder = node("Node", "WildFauna%d" % fauna_index,
                      (fx, 0.0, fz), 1.0, RNG.uniform(0.0, 360.0),
                      groups=["fauna"])
        holder["behaviours"] = [
            script("scripts/fauna.js", phase=RNG.uniform(0.0, 6.28),
                   amplitude=0.06)]
        holder["children"] = [
            kitmesh("WildFaunaMesh%d" % fauna_index, "pets", model,
                    (0.0, 0.0, 0.0), RNG.uniform(1.0, 1.45))]
        world.append(holder)

    world.append(particles(
        "WildsPollen", (0.0, 4.0, 4.0), effectClass=2, maxParticles=620,
        spawnRate=64.0, lifetime=10.0, startSpeed=0.42, startSize=0.08,
        startColor=[1.0, 0.96, 0.68, 0.82],
        endColor=[0.65, 1.0, 0.52, 0.0],
        gravity=[0.12, 0.05, 0.0], radius=82.0, shape=1, emissive=3.7,
        noiseStrength=0.65, noiseFrequency=0.32, endSizeScale=0.75))

    player = node("CharacterBody", "Player", (0.0, 1.4, WILDS_SPAWN_Z),
                  groups=["player"])
    player["behaviours"] = [
        {"type": "Character", "enabled": True,
         "moveSpeed": 5.8, "sprintMultiplier": 1.75,
         "groundAcceleration": 45.0, "groundDeceleration": 38.0,
         "gravity": 20.0, "airControl": 0.65, "airAcceleration": 22.0,
         "fallGravityMultiplier": 1.45, "maxFallSpeed": 26.0,
         "apexGravityMultiplier": 0.62, "apexThreshold": 2.2,
         "jumpHeight": 2.35, "jumpCutoffMultiplier": 0.45,
         "coyoteTime": 0.14, "jumpBufferTime": 0.16, "jumpCount": 2,
         "faceMovement": True, "turnMode": 0, "turnSpeed": 16.0,
         "idleClip": "idle", "walkClip": "run", "jumpClip": "jump"},
        script("scripts/player.js", respawnX=0.0, respawnY=1.4,
               respawnZ=WILDS_SPAWN_Z, killPlaneY=-12.0, range=110.0),
    ]
    player["children"] = [
        node("CollisionShape", "Shape", shapeType=3, radius=0.34,
             height=1.75, axis=1, offset=[0, 0, 0]),
        node("Node", "Body", (0.0, -0.88, 0.0), 1.0, 180.0,
             importedFrom="assets/models/characters/player.glb"),
    ]
    hand = node("Node", "Hand", (0.30, -0.12, -0.30), 1.0, 0.0,
                groups=["hand"])
    hand["children"] = [
        kitmesh("Blaster", "blaster", "blaster-h", (0.0, 0.0, 0.0),
                0.75, roughness=0.4, metallic=0.25),
        particles("Muzzle", (0.0, 0.06, -0.55), enabled=False,
                  looping=False, groups=["muzzle"], maxParticles=40,
                  spawnRate=420.0, lifetime=0.14, startSpeed=5.5,
                  startSize=0.16, startColor=[1.0, 0.95, 0.7, 1.0],
                  endColor=[1.0, 0.4, 0.05, 0.0], gravity=[0, 0, 0],
                  radius=0.05, shape=4, coneAngle=16.0, emissive=9.0,
                  endSizeScale=0.2),
    ]
    player["children"].append(hand)
    world.append(player)

    world.append(node(
        "Camera", "MainCamera", (0.0, 5.2, WILDS_SPAWN_Z + 8.0),
        groups=["camera"],
        fovDegrees=66.0, nearZ=0.08, farZ=900.0, priority=0, active=True,
        behaviours=[{
            "type": "CameraFollow", "enabled": True, "targetGroup": "player",
            "distance": 6.6, "height": 2.0, "shoulderOffset": 0.0,
            "initialPitch": -2.0, "minPitch": -30.0, "maxPitch": 55.0,
            "positionDamping": 16.0, "verticalDamping": 9.0,
            "lookAhead": 0.18, "lookAheadMaxDistance": 2.5,
            "fovAtRest": 66.0, "fovAtSpeed": 74.0,
            "fovSpeedReference": 10.0, "collisionMargin": 0.35,
            "minDistance": 1.1, "recenterDelay": 2.5,
            "recenterSpeed": 70.0, "recenterMinSpeed": 1.5,
            "recenterOnVelocity": True,
        }]))

    effects = node("Node", "Effects")
    for effect_index in range(10):
        effects["children"].append(particles(
            "WildImpact%d" % effect_index, (0.0, -200.0, 0.0),
            enabled=False, looping=False, groups=["impact"],
            maxParticles=56, spawnRate=340.0, lifetime=0.45,
            startSpeed=4.0, startSize=0.13,
            startColor=[1.0, 0.92, 0.55, 1.0],
            endColor=[1.0, 0.3, 0.05, 0.0],
            gravity=[0.0, -7.0, 0.0], radius=0.12, shape=1,
            emissive=7.0, endSizeScale=0.2))
        effects["children"].append(node(
            "MeshNode", "WildBolt%d" % effect_index,
            (0.0, -200.0, 0.0), [0.09, 0.09, 1.7],
            groups=["tracer"], enabled=False, mesh="cube",
            baseColor=[1.0, 0.88, 0.42, 1.0],
            emissive=[4.5, 3.0, 0.7, 1.0], roughness=0.35,
            metallic=0.0, castShadows=False))
    world.append(effects)

    hud = node("UICanvasNode", "HUD", width=1920.0, height=1080.0)
    hud["children"] = [
        node("UIColorNode", "HudPanel", color=[0.025, 0.055, 0.045, 0.56],
             x=16.0, y=16.0, width=860.0, height=132.0,
             anchorX=0.0, anchorY=0.0, pivotX=0.0, pivotY=0.0),
        uitext("ScoreText", "hud_score", "", 34.0,
               [1.0, 0.95, 0.75, 1.0], 32.0, 28.0, width=840.0),
        uitext("HealthText", "hud_health", "", 30.0,
               [0.85, 1.0, 0.85, 1.0], 32.0, 74.0, width=840.0),
        uitext("ObjectiveText", "hud_objective", "", 26.0,
               [0.72, 1.0, 0.86, 1.0], 32.0, 118.0, width=1100.0),
        uitext("BannerText", "hud_banner", "", 46.0,
               [0.78, 1.0, 0.88, 1.0], 32.0, 420.0,
               width=1856.0, height=90.0),
        uitext("HintText", "hud_hint", "", 22.0,
               [0.78, 0.85, 0.95, 1.0], 32.0, 1000.0,
               width=1856.0, height=40.0),
        uitext("Crosshair", "hud_crosshair", "+", 40.0,
               [1.0, 1.0, 1.0, 0.85], 930.0, 512.0,
               width=60.0, height=56.0),
    ]
    world.append(hud)

    scene["settings"] = {
        "ambient": srgb(0.075, 0.12, 0.105)[:3],
        "clearColor": srgb(0.34, 0.56, 0.72)[:3],
        "postProcessing": True,
        "lightingMode": 0,
        "giEnabled": True, "giMode": 1, "giIntensity": 0.82,
        "skyboxTexture": "assets/skies/sky.hdr",
        "skyboxExposure": 0.78, "skyboxRotation": 168.0,
        "iblEnabled": True, "iblDiffuseIntensity": 0.25,
        "iblSpecularIntensity": 0.68,
        "aoEnabled": True, "aoRadius": 1.05, "aoIntensity": 1.42,
        "aoPower": 1.72,
        "fogEnabled": True, "fogColor": srgb(0.36, 0.57, 0.62)[:3],
        "fogStart": 92.0, "fogDensity": 0.009,
        "bloomEnabled": True, "bloomThreshold": 1.75,
        "bloomIntensity": 0.26, "bloomRadius": 4.5,
        "changeRenderingAtLoad": True,
    }
    return scene


def build_menu():
    """Build the lightweight 3D stage that lives behind the main menu."""
    scene = node("Scene", "VerdanceMainMenu")
    world = scene["children"]

    world.append(node("LightNode", "MenuSun", (0.0, 20.0, 8.0),
                      lightType=0, color=[1.0, 0.91, 0.72], intensity=3.4,
                      direction=[-0.38, -0.72, -0.58], castShadows=True,
                      bakeMode=0, range=10.0, spotInnerAngle=25.0, spotOuterAngle=35.0))
    world.append(node("LightNode", "MenuFill", (4.0, 5.0, 6.0),
                      lightType=1, color=[0.45, 0.76, 0.63], intensity=7.5,
                      direction=[0.0, -1.0, -1.0], castShadows=False,
                      bakeMode=0, range=16.0, spotInnerAngle=25.0, spotOuterAngle=35.0))

    stage = node("Node", "MenuStage")
    stage["children"] = [
        slab("MenuGround", (2.5, -1.45, -1.0), (20.0, 1.2, 12.0), GRASS,
             roughness=0.95),
        nature("MenuTreeA", "tree_pineTallA", (-5.5, -0.85, -3.5), TREE * 1.25, 20.0),
        nature("MenuTreeB", "tree_oak_fall", (7.0, -0.85, -5.0), TREE * 1.2, -28.0),
        nature("MenuTreeC", "tree_pineTallB", (10.5, -0.85, 0.0), TREE, 12.0),
        nature("MenuRockA", "rock_largeA", (6.0, -0.85, 1.2), PROP * 1.35, 40.0),
        nature("MenuRockB", "rock_tallB", (-3.2, -0.85, -1.5), PROP * 1.1, -15.0),
        nature("MenuFernA", "plant_bushLarge", (1.5, -0.85, -0.8), PROP * 1.15, 10.0),
        nature("MenuFernB", "grass_leafsLarge", (5.5, -0.85, -0.5), PROP, -25.0),
        nature("MenuFlowerA", "flower_yellowA", (2.0, -0.85, 1.0), PROP, 0.0),
        nature("MenuFlowerB", "flower_purpleB", (4.8, -0.85, 0.8), PROP, 15.0),
        nature("MenuRuin", "statue_ring", (7.2, -0.85, -3.0), PROP * 1.6, -25.0),
    ]
    world.append(stage)

    hero = node("Node", "MenuHero", (3.9, -0.82, 0.0), 1.16, 0.0,
                importedFrom="assets/models/characters/player.glb")
    hero["behaviours"] = [script("scripts/menu_hero.js")]
    world.append(hero)

    world.append(particles(
        "MenuPollen", (3.5, 2.4, -0.5), effectClass=2, maxParticles=180,
        spawnRate=24.0, lifetime=7.0, startSpeed=0.32, startSize=0.075,
        startColor=[1.0, 0.96, 0.68, 0.85], endColor=[0.7, 1.0, 0.5, 0.0],
        gravity=[0.08, 0.04, 0.0], radius=9.0, shape=1, emissive=3.8,
        noiseStrength=0.55, noiseFrequency=0.4, endSizeScale=0.75))

    camera = node("Camera", "MenuCamera", (0.0, 2.15, 10.5),
                  fovDegrees=52.0, nearZ=0.08, farZ=300.0,
                  priority=10, active=True)
    pitch = math.radians(-7.0) * 0.5
    camera["transform"]["rotation"] = [math.sin(pitch), 0.0, 0.0, math.cos(pitch)]
    world.append(camera)

    world.append(node(
        "WebCanvasNode", "MainMenu", width=1920, height=1080, mode=0,
        url="ui/main_menu.html", html="", hotReload=True, startupScripts=[],
        worldWidth=1.0, interactive=True, renderOrder=1000))

    scene["settings"] = {
        "ambient": srgb(0.055, 0.085, 0.075)[:3],
        "clearColor": srgb(0.12, 0.21, 0.17)[:3],
        "postProcessing": True,
        "lightingMode": 0,
        "giEnabled": True, "giMode": 1, "giIntensity": 0.6,
        "skyboxTexture": "assets/skies/sky.hdr",
        "skyboxExposure": 0.62, "skyboxRotation": 155.0,
        "iblEnabled": True, "iblDiffuseIntensity": 0.18, "iblSpecularIntensity": 0.55,
        "aoEnabled": True, "aoRadius": 0.8, "aoIntensity": 1.3, "aoPower": 1.7,
        "fogEnabled": True, "fogColor": srgb(0.16, 0.27, 0.21)[:3],
        "fogStart": 18.0, "fogDensity": 0.028,
        "bloomEnabled": True, "bloomThreshold": 1.55, "bloomIntensity": 0.30,
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
    menu_scene = build_menu()
    menu_doc = {"schema": 2, "version": 2, "scene": menu_scene}
    level_two_scene = build_level_two()
    level_two_doc = {"schema": 2, "version": 2, "scene": level_two_scene}
    scenes_dir = os.path.join(HERE, "scenes")
    os.makedirs(scenes_dir, exist_ok=True)
    with open(os.path.join(scenes_dir, "verdance.scene"), "w",
              encoding="utf-8", newline="\n") as f:
        json.dump(doc, f, indent=1)
    with open(os.path.join(scenes_dir, "deep_wilds.scene"), "w",
              encoding="utf-8", newline="\n") as f:
        json.dump(level_two_doc, f, indent=1)
    with open(os.path.join(scenes_dir, "main_menu.scene"), "w",
              encoding="utf-8", newline="\n") as f:
        json.dump(menu_doc, f, indent=1)

    project = {
        "schema": 1, "version": 1,
        "name": "Verdance",
        "engineVersion": "0.1.0",
        "mainScene": "scenes/main_menu.scene",
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
    with open(os.path.join(HERE, "VerticalSlice.saidaproj"), "w",
              encoding="utf-8", newline="\n") as f:
        json.dump(project, f, indent=2, sort_keys=True)

    def count(n):
        return 1 + sum(count(c) for c in n["children"])

    def imported_count(n):
        return (1 if "importedFrom" in n else 0) + sum(
            imported_count(c) for c in n["children"])

    print("scenes/verdance.scene — %d nodes, %d imported models" %
          (count(scene), imported_count(scene)))
    print("scenes/deep_wilds.scene — %d nodes, %d imported models" %
          (count(level_two_scene), imported_count(level_two_scene)))
    print("scenes/main_menu.scene - %d nodes" % count(menu_scene))


if __name__ == "__main__":
    main()
