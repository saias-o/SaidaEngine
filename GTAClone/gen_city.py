#!/usr/bin/env python3
"""Generates Sunset Strip: the open-world city, its road network and its beach.

This script is the source of truth for the scenes under `scenes/`. Edit it
rather than the generated `.scene` files, then re-run it — placement is seeded,
so the same script always produces the same city.

The road graph in `roadnet.py` is the single source of truth for the layout: the
road tiles, the sidewalks, the blocks the buildings fill and the lane graph the
traffic drives on all derive from it, so the city cannot drift out of agreement
with the traffic that runs on it.
"""

import json
import math
import os
import random

import roadnet
from roadnet import N, S, E, W, GRID, AVENUES, STREETS, BOULEVARD

HERE = os.path.dirname(os.path.abspath(__file__))
RNG = random.Random(20260801)


# ── scale ───────────────────────────────────────────────────────────────────
# The three city kits (commercial, suburban, roads) share one internal scale in
# which a road tile is exactly 1 unit; the car kit does not, and is scaled on
# its own. Everything below is expressed in metres and converted through these.
#
# TILE is what fixes the whole city: one road tile carries a two-lane
# carriageway, so 8 m gives two 4 m lanes. Read back through it the kits land on
# believable sizes — a commercial building is 7.0 x 7.5 m and 10.3 m tall, a
# skyscraper is 10 x 11 m and up to 32.6 m, a suburban house is 10.4 x 8.2 m and
# 6.6 m, a roundabout is 24 m across.
TILE = 8.0

# The car kit is authored chunkier than life: a sedan is 1.50 wide, 1.30 tall and
# 2.55 long. Against the 1.75 m character of the shared character kit, 1.25 puts
# it at 1.9 x 1.6 x 3.2 m — a small stylised city car that a person visibly fits
# in, which the native scale does not quite sell.
CAR = 1.25

# The pirate kit's rowing boat stands in for the jet ski; at 1.0 it is 2.75 m
# long, which is the real length of one.
BOAT = 1.0

CHARACTER_HEIGHT = 1.75  # the character kit's own height, at scale 1.0

# Where the player and their car start, and how much of that avenue stays clear
# of parked cars so the car has somewhere to pull out to.
HERO_CELL = (25, 19)
HERO_CLEAR_TILES = 8

SIDEWALK_W = 2.2   # metres between the kerb and a building's facade
BLOCK_GAP = 0.6    # metres left between neighbouring buildings
CORNER_KEEP = 7.0  # metres kept clear at each end of a block edge, so that
                   # buildings lining perpendicular edges cannot overlap

# ── ground levels ───────────────────────────────────────────────────────────
# Every horizontal surface gets its own height, because two coplanar faces
# z-fight: the bare ground, a block's paving and a building's base all sitting
# at 0 made the whole city read as if it were sinking into itself.
#
# A road tile is 0.02 units thick, which is 0.16 m once scaled by TILE, and its
# asphalt is the middle of its three levels. ROAD_Y is therefore the node height
# that lands that asphalt exactly on the plane the player walks on, leaving the
# kerb standing KERB_H proud of it.
ASPHALT_Y = 0.0          # road surface, and the top of the ground collider
ROAD_Y = -0.08           # tile origin, so its asphalt lands on ASPHALT_Y
KERB_H = 0.08            # top of the kerb, and so of the pavement beside it
GROUND_VISUAL_Y = -0.02  # bare ground, kept below the asphalt it abuts

# ── the shore ───────────────────────────────────────────────────────────────
# The water shader derives depth analytically:
#   depth = (shoreWaterline - dot(xz - centre, inlandDir)) * shoreSlope
# so the waterline sits where dot(...) equals shoreWaterline. With the sea south
# of the city the inland direction is -Z, which puts the waterline at
# z = SEA_CENTRE_Z - shoreWaterline. The sand is built on the same slope so the
# geometry and the analytic shore agree instead of crossing each other.
CITY_EDGE_Z = 152.0     # where the boulevard ends and the sand begins
BEACH_SLOPE = 0.05      # metres of drop per metre out to sea
SEA_Y = -1.5            # water surface height
WATERLINE_Z = CITY_EDGE_Z + (-SEA_Y) / BEACH_SLOPE   # sand meets water here
SAND_END_Z = 216.0
SEA_CENTRE_Z = 500.0
SEA_HALF = 700.0


# ── palette ─────────────────────────────────────────────────────────────────
def srgb(r, g, b, a=1.0):
    """The engine's colour fields are linear; the kits' palette is quoted sRGB."""
    def lin(c):
        c /= 255.0
        return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4
    return [lin(r), lin(g), lin(b), a]


def hexcolor(text, a=1.0):
    text = text.lstrip("#")
    return srgb(int(text[0:2], 16), int(text[2:4], 16), int(text[4:6], 16), a)


GROUND = hexcolor("#4A4E52")
PAVEMENT = hexcolor("#9BA1A7")
LAWN = hexcolor("#6E9E52")
DOCKWOOD = hexcolor("#8B6B49")
SAND = hexcolor("#D8C69A")


# ── kits ────────────────────────────────────────────────────────────────────
KIT = {
    "city": "assets/models/city",
    "suburb": "assets/models/suburb",
    "roads": "assets/models/roads",
    "cars": "assets/models/cars",
    "boats": "assets/models/boats",
    "blaster": "assets/models/blaster",
    "survival": "assets/models/survival",
}

CHARACTER = "assets/models/characters/%s.glb"

DOWNTOWN_TOWERS = ["building-skyscraper-%s" % c for c in "abcde"]
DOWNTOWN_BLOCKS = ["building-%s" % c for c in "abcdefghijklmn"]
SUBURB_HOUSES = ["building-type-%s" % c for c in "abcdefghijklmnopqrstu"]
# The low-detail family is not a set of LODs: every one of them is 0.5 x 0.5 on
# plan whatever its full-detail namesake measures, so they are used for what
# they actually are — cheap distant filler for the skyline.
SKYLINE_FILLER = ["low-detail-building-%s" % c for c in "abcdefghijklmn"]


# ── model measurement ───────────────────────────────────────────────────────
# The generator reads the meshes rather than carrying a table of hand-copied
# sizes, so a collider can never quietly disagree with the model it wraps.
_bounds_cache = {}
_facade_cache = {}


def _obj_path(kit, model):
    return os.path.join(HERE, KIT[kit], model + ".obj")


def _read_verts(kit, model):
    verts = []
    with open(_obj_path(kit, model), errors="ignore") as f:
        for line in f:
            if line.startswith("v "):
                p = line.split()
                verts.append((float(p[1]), float(p[2]), float(p[3])))
    return verts


def model_size(kit, model):
    """The model's bounding box in metres, at the kit's own scale of 1 tile."""
    key = (kit, model)
    if key not in _bounds_cache:
        v = _read_verts(kit, model)
        lo = [min(p[i] for p in v) for i in range(3)]
        hi = [max(p[i] for p in v) for i in range(3)]
        _bounds_cache[key] = tuple(hi[i] - lo[i] for i in range(3))
    return _bounds_cache[key]


def model_facade(kit, model):
    """Which side of a building carries its shopfront.

    The kit models put the shopfront, awnings, doors and most windows on the
    street side, so that wall holds far more geometry than the plain ones.
    Counting vertices in a thin shell inside each wall separates them; the roof
    is excluded because rooftop clutter would swamp the walls.
    """
    key = (kit, model)
    if key not in _facade_cache:
        v = _read_verts(kit, model)
        x0, x1 = min(p[0] for p in v), max(p[0] for p in v)
        z0, z1 = min(p[2] for p in v), max(p[2] for p in v)
        y1 = max(p[1] for p in v)
        dx, dz, shell = x1 - x0, z1 - z0, 0.12
        body = [p for p in v if p[1] < y1 * 0.92] or v
        counts = {
            W: sum(1 for p in body if p[0] <= x0 + dx * shell),
            E: sum(1 for p in body if p[0] >= x1 - dx * shell),
            N: sum(1 for p in body if p[2] <= z0 + dz * shell),
            S: sum(1 for p in body if p[2] >= z1 - dz * shell),
        }
        _facade_cache[key] = max(counts, key=counts.get)
    return _facade_cache[key]


def yaw_to_face(source, target):
    """The yaw that turns direction `source` onto `target`."""
    for yaw in (0, 90, 180, 270):
        if roadnet.rotate(source, yaw) == target:
            return yaw
    raise AssertionError("no yaw turns %r onto %r" % (source, target))


# ── tile space ──────────────────────────────────────────────────────────────
def X(i):
    return (i - (GRID - 1) / 2.0) * TILE


def Z(j):
    return (j - (GRID - 1) / 2.0) * TILE


# ── scene emission ──────────────────────────────────────────────────────────
_next_id = [7400000000000000000]


def nid():
    _next_id[0] += 1
    return _next_id[0]


def quat_yaw(degrees):
    half = math.radians(degrees) * 0.5
    return [0.0, math.sin(half), 0.0, math.cos(half)]


def quat_pitch(degrees):
    half = math.radians(degrees) * 0.5
    return [math.sin(half), 0.0, 0.0, math.cos(half)]


def node(type_, name, pos=(0.0, 0.0, 0.0), scale=1.0, yaw=None, rot=None, **kw):
    d = {
        "type": type_, "id": nid(), "name": name, "enabled": True,
        "behaviours": [], "children": [],
        "transform": {
            "position": [float(v) for v in pos],
            "rotation": rot if rot is not None
                        else quat_yaw(yaw if yaw is not None else 0.0),
            "scale": [scale, scale, scale] if not isinstance(scale, (list, tuple))
                     else [float(v) for v in scale],
        },
    }
    d.update(kw)
    return d


def script(path, **props):
    b = {"type": "ScriptBehaviour", "enabled": True, "script": path, "hotReload": True}
    if props:
        b["properties"] = props
    return b


def kitmesh(name, kit, model, pos, scale=1.0, yaw=None, color=None, groups=None, **mat):
    """A prop from an atlas kit: one shared colormap texture per kit, so these
    instance cheaply."""
    kw = {"groups": groups} if groups else {}
    return node("MeshNode", name, pos, scale, yaw,
                mesh="%s/%s.obj" % (KIT[kit], model),
                texture="%s/colormap.png" % KIT[kit],
                baseColor=color or [1.0, 1.0, 1.0, 1.0],
                roughness=mat.get("roughness", 0.85),
                metallic=mat.get("metallic", 0.0),
                emissive=mat.get("emissive", [0, 0, 0, 0]),
                castShadows=mat.get("castShadows", True), **kw)


def slab(name, pos, size, color, rot=None, **mat):
    """A flat coloured box from the built-in cube, for large flat surfaces."""
    return node("MeshNode", name, pos, [size[0], size[1], size[2]], rot=rot,
                mesh="cube", baseColor=color,
                roughness=mat.get("roughness", 0.9),
                metallic=mat.get("metallic", 0.0),
                emissive=mat.get("emissive", [0, 0, 0, 0]),
                castShadows=mat.get("castShadows", False))


def box_body(name, pos, half_extents, children=None, groups=None, static=True,
             yaw=None, rot=None):
    """A body whose collision is an explicit box.

    Buildings are authored this way on purpose: a body's collision shape is built
    from the FIRST drawable mesh in its subtree, so a body holding a multi-mesh
    kit model would collide with one piece of it and let the player walk through
    the rest.
    """
    kw = {"groups": groups} if groups else {}
    b = node("StaticBody" if static else "RigidBody", name, pos, yaw=yaw, rot=rot, **kw)
    b["children"] = [node("CollisionShape", "Shape", shapeType=1,
                          halfExtents=[float(v) for v in half_extents],
                          offset=[0, 0, 0])]
    b["children"] += children or []
    return b


def water(name, pos, size, **kw):
    """The sea, in the realistic style with an analytic beach shore.

    The plane is square and cannot be cut to a coastline, so it is placed at the
    waterline and left to the depth buffer: the land hides it everywhere except
    the bay. `shoreAngle` is the inland direction.
    """
    base = {
        "style": 0, "size": size, "shoreMode": 1,
        "deepColor": [0.012, 0.075, 0.115], "shallowColor": [0.18, 0.55, 0.60],
        "foamColor": [0.85, 0.93, 0.97],
        "roughness": 0.04, "reflectivity": 0.6,
        "amplitude": 0.22, "wavelength": 14.0, "waveSpeed": 0.75, "choppiness": 0.28,
        "fresnelPower": 5.0, "specularPower": 40.0, "specularIntensity": 0.3,
        "foamThreshold": 0.22, "foamIntensity": 0.07,
        "depthColorFalloff": 7.0, "edgeFade": 0.7, "shoreSlope": BEACH_SLOPE,
        "shoreFoam": 0.85, "foamWidth": 0.6, "swashSpeed": 1.15, "swashAmount": 0.45,
        "waveFlatten": 1.5, "warpAmount": 0.12, "detailFadeDistance": 220.0,
        # -Z is inland, and the waterline sits `shoreWaterline` along it from
        # the node centre.
        "shoreAngle": 270.0, "shoreWaterline": SEA_CENTRE_Z - WATERLINE_Z,
    }
    base.update(kw)
    return node("Water", name, pos, groups=["sea"], **base)


# ── districts ───────────────────────────────────────────────────────────────
DOWNTOWN_CENTRE = (25.0, 13.0)


def district_of(ci, cj):
    """Which district a block belongs to, from its centre in tile space."""
    if cj > 35:
        return "port"
    d = math.hypot(ci - DOWNTOWN_CENTRE[0], cj - DOWNTOWN_CENTRE[1])
    if d < 9.5:
        return "downtown"
    return "residential"


def pick_building(district, ci, cj):
    """A model for one plot, and the kit it comes from."""
    if district == "downtown":
        d = math.hypot(ci - DOWNTOWN_CENTRE[0], cj - DOWNTOWN_CENTRE[1])
        # The towers cluster at the core so the skyline has a peak rather than
        # an even wall.
        if d < 5.0 and RNG.random() < 0.55:
            return "city", RNG.choice(DOWNTOWN_TOWERS)
        return "city", RNG.choice(DOWNTOWN_BLOCKS)
    if district == "port":
        return "city", RNG.choice(DOWNTOWN_BLOCKS[:7])
    return "suburb", RNG.choice(SUBURB_HOUSES)


# ── city pieces ─────────────────────────────────────────────────────────────
def lay_roads(net):
    """Every road cell, as its matching kit tile at the right yaw."""
    group = node("Node", "Roads")
    for (i, j), model, yaw in net.tiles():
        # A pedestrian crossing replaces the straight tile just off a junction;
        # it carries the same openings, so it drops in at the same yaw.
        if model == "road-straight" and _next_to_junction(net, (i, j)) and RNG.random() < 0.45:
            model = "road-crossing"
        group["children"].append(
            kitmesh("R%d_%d" % (i, j), "roads", model, (X(i), ROAD_Y, Z(j)), TILE,
                    yaw=yaw, groups=["road"], castShadows=False))
    return group


def _next_to_junction(net, cell):
    i, j = cell
    return any(net.is_junction((i + d[0], j + d[1])) for d in roadnet.CARDINALS)


def fill_block(parent, i0, i1, j0, j1):
    """Line a block's four edges with buildings whose facades face the street."""
    ci, cj = (i0 + i1) / 2.0, (j0 + j1) / 2.0
    district = district_of(ci, cj)
    x0, x1 = X(i0) - TILE / 2, X(i1) + TILE / 2
    z0, z1 = Z(j0) - TILE / 2, Z(j1) + TILE / 2

    # The paving is a body, not just a slab: it stands KERB_H above the asphalt,
    # so without a collider the player would walk with their feet inside it.
    # Its underside is buried well below the bare ground so no two faces meet.
    surface = LAWN if district == "residential" else PAVEMENT
    parent["children"].append(box_body(
        "Plot%d_%d" % (i0, j0), ((x0 + x1) / 2, KERB_H - 0.6, (z0 + z1) / 2),
        ((x1 - x0) / 2, 0.6, (z1 - z0) / 2), groups=["pavement"],
        children=[slab("Face", (0.0, 0.0, 0.0), (x1 - x0, 1.2, z1 - z0), surface)]))

    # (facing, along-axis span, fixed coordinate of the street-side edge)
    edges = (
        (N, (x0, x1), z0), (S, (x0, x1), z1),
        (W, (z0, z1), x0), (E, (z0, z1), x1),
    )
    for facing, (a0, a1), fixed in edges:
        along_x = facing in (N, S)
        cursor = a0 + CORNER_KEEP
        limit = a1 - CORNER_KEEP
        while cursor < limit:
            kit, model = pick_building(district, ci, cj)
            sx, sy, sz = (v * TILE for v in model_size(kit, model))
            yaw = yaw_to_face(model_facade(kit, model), facing)
            # A quarter turn swaps which axis the footprint spans.
            fw, fd = (sx, sz) if yaw in (0, 180) else (sz, sx)
            width, depth = (fw, fd) if along_x else (fd, fw)
            if cursor + width > limit:
                break
            centre = cursor + width / 2
            inset = SIDEWALK_W + depth / 2
            base = KERB_H + sy / 2   # the building stands ON the paving
            if facing == N:
                pos = (centre, base, fixed + inset)
            elif facing == S:
                pos = (centre, base, fixed - inset)
            elif facing == W:
                pos = (fixed + inset, base, centre)
            else:
                pos = (fixed - inset, base, centre)
            half = (width / 2, sy / 2, depth / 2) if along_x else (depth / 2, sy / 2, width / 2)
            parent["children"].append(box_body(
                "B%d_%d_%d" % (i0, j0, int(cursor)), pos, half, groups=["building"],
                children=[kitmesh("Mesh", kit, model, (0.0, -sy / 2, 0.0), TILE, yaw=yaw)]))
            cursor += width + BLOCK_GAP


def street_furniture(net):
    """Street lights down the avenues, on the pavement rather than the road."""
    group = node("Node", "StreetFurniture")
    for i in AVENUES:
        for j in range(STREETS[0] + 2, BOULEVARD, 4):
            if (i, j) not in net.cells or net.is_junction((i, j)):
                continue
            for side, yaw in ((-1, 90), (1, 270)):
                group["children"].append(kitmesh(
                    "Lamp%d_%d_%d" % (i, j, side), "roads", "light-curved",
                    (X(i) + side * (TILE / 2 - 0.6), KERB_H, Z(j)), TILE, yaw=yaw,
                    groups=["streetlight"]))
    return group


def beach_and_sea():
    """The sand, the sea and the dock the watercraft launches from."""
    group = node("Node", "Beach")

    # The sand is one tilted slab whose surface follows the same slope the water
    # shader uses, so the analytic waterline lands where the geometry does.
    pitch = math.degrees(math.atan(BEACH_SLOPE))
    length = (SAND_END_Z - CITY_EDGE_Z) / math.cos(math.radians(pitch))
    mid_z = (CITY_EDGE_Z + SAND_END_Z) / 2
    mid_y = -(mid_z - CITY_EDGE_Z) * BEACH_SLOPE
    half_t = 3.0
    # Step the body down along its own up axis so the tilted top face passes
    # through the midpoint of the intended slope.
    c = math.cos(math.radians(pitch))
    s = math.sin(math.radians(pitch))
    body = box_body("Sand", (0.0, mid_y - half_t * c, mid_z + half_t * s),
                    (240.0, half_t, length / 2), rot=quat_pitch(pitch),
                    groups=["sand"])
    body["children"].append(slab("SandTop", (0.0, half_t - 0.02, 0.0),
                                 (480.0, 0.04, length), SAND))
    group["children"].append(body)

    # A timber dock reaching past the waterline, and the boat tied to its end.
    dock_z0, dock_z1 = WATERLINE_Z - 14.0, WATERLINE_Z + 16.0
    dock_y = SEA_Y + 0.9
    group["children"].append(box_body(
        "Dock", (34.0, dock_y - 0.3, (dock_z0 + dock_z1) / 2), (4.0, 0.3, (dock_z1 - dock_z0) / 2),
        groups=["dock"],
        children=[slab("Deck", (0.0, 0.0, 0.0), (8.0, 0.6, dock_z1 - dock_z0), DOCKWOOD)]))
    group["children"].append(kitmesh(
        "Boat", "boats", "boat-row-small", (44.0, SEA_Y - 0.1, dock_z1 - 5.0), BOAT,
        yaw=15.0, groups=["boat"]))
    return group


def skyline():
    """Distant filler beyond the playable grid, to give the city a horizon.

    These are the low-detail models, 36 to 112 vertices each, so a hundred of
    them cost less than one real building.
    """
    group = node("Node", "Skyline")
    edge = X(GRID - 1) + TILE  # the outer face of the city
    for k in range(150):
        band = RNG.uniform(28.0, 190.0)
        along = RNG.uniform(-edge - 120.0, edge + 120.0)
        side = RNG.choice(("n", "s", "e", "w"))
        if side == "n":
            x, z = along, -edge - band
        elif side == "s":
            # Never in the bay: the sea is south and must stay open water.
            continue
        elif side == "e":
            x, z = edge + band, along
        else:
            x, z = -edge - band, along
        if z > CITY_EDGE_Z:
            continue
        model = RNG.choice(SKYLINE_FILLER)
        height = RNG.uniform(0.8, 2.2)
        group["children"].append(kitmesh(
            "Sky%d" % k, "city", model, (x, GROUND_VISUAL_Y, z), [TILE, TILE * height, TILE],
            yaw=RNG.choice((0.0, 90.0, 180.0, 270.0)), groups=["skyline"],
            castShadows=False))
    return group


# ── interiors ───────────────────────────────────────────────────────────────
# The building models are solid, so an interior cannot be carved out of one.
# Each room is therefore built sealed and well below the city and reached by a
# teleport, the way the games this is modelled on did it. INTERIOR_Y is far
# enough down that nothing in a room can ever touch the street above it.
INTERIOR_Y = -60.0
INTERIOR_SPACING = 60.0


def door(name, pos, half, destination, groups=None):
    """A one-way volume that moves the player to the other side of a doorway."""
    area = node("Area", name, pos, groups=groups or ["door"])
    area["behaviours"] = [script("scripts/door.js",
                                 toX=destination[0], toY=destination[1], toZ=destination[2])]
    area["children"] = [node("CollisionShape", "Shape", shapeType=1,
                             halfExtents=[float(v) for v in half], offset=[0, 0, 0])]
    return area


def interior_room(name, origin, size, street_exit, props):
    """A sealed room: floor, four walls, a ceiling, a light and its way out.

    The exit volume sits at the door and the arrival point of the street door is
    placed clear of it, so a player who has just walked in is not immediately
    sent back out.
    """
    w, h, d = size
    room = node("Node", name, origin, groups=["interior"])
    c = room["children"]

    def wall(tag, pos, half):
        c.append(box_body("%s_%s" % (name, tag), pos, half,
                          children=[slab("Face", (0.0, 0.0, 0.0),
                                         (half[0] * 2, half[1] * 2, half[2] * 2),
                                         PAVEMENT)]))

    c.append(box_body("%s_Floor" % name, (0.0, -0.25, 0.0), (w / 2, 0.25, d / 2),
                      children=[slab("Face", (0.0, 0.0, 0.0), (w, 0.5, d), DOCKWOOD)]))
    wall("Ceil", (0.0, h + 0.25, 0.0), (w / 2, 0.25, d / 2))
    wall("WallN", (0.0, h / 2, -d / 2 - 0.25), (w / 2, h / 2, 0.25))
    wall("WallS", (0.0, h / 2, d / 2 + 0.25), (w / 2, h / 2, 0.25))
    wall("WallW", (-w / 2 - 0.25, h / 2, 0.0), (0.25, h / 2, d / 2))
    wall("WallE", (w / 2 + 0.25, h / 2, 0.0), (0.25, h / 2, d / 2))

    c.append(node("LightNode", "%s_Light" % name, (0.0, h - 0.6, 0.0), lightType=1,
                  color=[1.0, 0.93, 0.82], intensity=14.0, direction=[0, -1, 0],
                  castShadows=False, bakeMode=0, range=max(w, d) * 1.4,
                  spotInnerAngle=25.0, spotOuterAngle=35.0))

    # The way out sits against the south wall; the street door lands the player
    # two metres north of it, which is outside this volume.
    c.append(door("%s_Exit" % name, (0.0, 1.0, d / 2 - 1.0), (2.0, 1.6, 0.9),
                  street_exit, groups=["door", "interior_exit"]))
    c += props
    return room


def interior_entry_point(index, depth):
    """Where the street door lands the player inside room `index`."""
    return (index * INTERIOR_SPACING, INTERIOR_Y + 1.0, depth / 2 - 3.4)


def build_interiors(street_points):
    """Four furnished rooms, each reached from a door on the street.

    `street_points` gives, per room, where its exit puts the player back on the
    pavement. Returns the group and, per room, the point its street door must
    land the player on.
    """
    group = node("Node", "Interiors")
    entries = []

    # (label, size, the props that make it read as what it is)
    def store(ox):
        p = []
        for k in range(4):
            p.append(kitmesh("Shelf%d" % k, "survival", "box-large",
                             (-3.6 + k * 2.4, 0.0, -2.2), 2.0))
            p.append(kitmesh("Crate%d" % k, "survival", "box",
                             (-3.4 + k * 2.4, 0.0, 0.4), 2.0, yaw=25.0 * k))
        p.append(kitmesh("Counter", "survival", "workbench", (3.0, 0.0, 2.0), 2.4, yaw=90.0))
        p.append(kitmesh("Barrel", "survival", "barrel", (-5.0, 0.0, 2.4), 2.0))
        del ox
        return p

    def garage(ox):
        p = [kitmesh("Anvil", "survival", "workbench-anvil", (-4.0, 0.0, -2.0), 2.4),
             kitmesh("Grind", "survival", "workbench-grind", (-1.0, 0.0, -2.4), 2.4),
             kitmesh("Barrel", "survival", "barrel-open", (4.4, 0.0, -2.0), 2.0),
             kitmesh("Planks", "survival", "resource-planks", (4.0, 0.0, 1.6), 2.0, yaw=30.0)]
        p.append(parked_car("Wreck", (0.4, 0.0, 0.6), 90.0, "hatchback-sports"))
        del ox
        return p

    def safehouse(ox):
        del ox
        return [kitmesh("Bed", "survival", "bedroll", (-3.8, 0.0, -2.0), 2.2, yaw=90.0),
                kitmesh("Chest", "survival", "chest", (-4.2, 0.0, 1.4), 2.2),
                kitmesh("Bench", "survival", "workbench", (3.2, 0.0, -1.8), 2.4),
                kitmesh("Bottle", "survival", "bottle-large", (3.0, 0.75, -1.8), 2.0),
                kitmesh("Box", "survival", "box-open", (4.4, 0.0, 2.0), 2.0)]

    def lockup(ox):
        del ox
        p = [kitmesh("Rack", "survival", "structure-metal", (-4.6, 0.0, -1.2), 2.4),
             kitmesh("Panel", "survival", "metal-panel", (4.6, 0.0, -1.0), 2.4, yaw=90.0),
             kitmesh("Stone", "survival", "resource-stone-large", (3.6, 0.0, 2.2), 2.0)]
        for k in range(3):
            p.append(kitmesh("Case%d" % k, "blaster", "crate-medium", (-1.6 + k * 1.7, 0.0, 1.8), 1.6))
        return p

    rooms = (("Store", (14.0, 4.0, 10.0), store),
             ("Garage", (16.0, 5.0, 12.0), garage),
             ("Safehouse", (13.0, 4.0, 10.0), safehouse),
             ("Lockup", (13.0, 4.0, 10.0), lockup))

    for index, (label, size, furnish) in enumerate(rooms):
        ox = index * INTERIOR_SPACING
        group["children"].append(interior_room(
            label, (ox, INTERIOR_Y, 0.0), size,
            street_exit=street_points[index], props=furnish(ox)))
        entries.append(interior_entry_point(index, size[2]))
    return group, entries


def parked_car(name, pos, yaw, model="sedan"):
    """Set dressing: one mesh, no body, no wheels of its own.

    The kit's car models already contain their four wheels, as the groups
    `body` and `wheel-front-left` and so on inside the one mesh — adding the
    separate `wheel-*` models on top of them put a second, larger set of wheels
    over the first and made the chassis look far too small for them. A car that
    is only ever looked at wants exactly this; `drivable_car` below is the one
    that pays for a body and four separate wheels.
    """
    car = node("Node", name, pos, 1.0, yaw, groups=["parked_car"])
    car["children"] = [kitmesh("Body", "cars", model, (0.0, 0.0, 0.0), CAR)]
    return car


# ── driven cars ─────────────────────────────────────────────────────────────
# Measured by tools/split_car_wheels.py off the art and written to vehicles.json,
# so a car's suspension can never quietly disagree with the model hanging off it.
# Everything in that file is in kit units; CAR converts to metres.
_vehicles_cache = [None]


def vehicle_specs():
    if _vehicles_cache[0] is None:
        path = os.path.join(HERE, KIT["cars"], "vehicles.json")
        with open(path, encoding="utf-8") as f:
            _vehicles_cache[0] = json.load(f)["vehicles"]
    return _vehicles_cache[0]


def drivable_car(name, pos, yaw, model="sedan", mass=1200.0, groups=None):
    """A car the Vehicle behaviour drives: a body, four wheels it moves, a box.

    The collider is an explicit box lifted clear of the road, never the mesh.
    That is not only the multi-mesh rule the rest of the city follows — a raycast
    vehicle *requires* it. The wheels carry the car on four rays, so a chassis
    that reached the ground would rest on the road itself and the suspension
    would have nothing left to do.

    The four wheel meshes must be direct children named <prefix>FL/FR/RL/RR:
    that is what `VehicleBehaviour::layOutWheels` looks up, and a missing one is
    simply not driven rather than reported. One wheel mesh serves all four — the
    behaviour turns the right-hand pair about to mirror it.
    """
    spec = vehicles_spec_for(model)
    lo, hi = spec["bodyMin"], spec["bodyMax"]
    # Half-extents and centre of the body's own bounds, in metres.
    half = [(hi[a] - lo[a]) * 0.5 * CAR for a in range(3)]
    centre_y = (lo[1] + hi[1]) * 0.5 * CAR

    front = spec["wheels"]["wheel-front-left"]
    rear = spec["wheels"]["wheel-back-left"]
    radius = spec["wheelRadius"] * CAR
    # The kit rests its wheel centres exactly one radius up, so a car whose
    # origin sits on the asphalt is already standing on its tyres. Hanging the
    # suspension a rest-length above that keeps it there.
    rest = 0.35
    anchor_h = front[1] * CAR + rest

    car = node("RigidBody", name, pos, 1.0, yaw, groups=groups or ["vehicle"],
               mass=mass, gravityFactor=1.0, linearDamping=0.0, angularDamping=0.35,
               kinematic=False)
    car["behaviours"] = [{
        "type": "Vehicle", "enabled": True,
        "wheelHalfTrack": round(front[0] * CAR, 4),
        "wheelBaseFront": round(front[2] * CAR, 4),
        "wheelBaseRear": round(-rear[2] * CAR, 4),
        "wheelRadius": round(radius, 4),
        "wheelAnchorHeight": round(anchor_h, 4),
        "suspensionRest": rest, "suspensionTravel": 0.22,
        "suspensionStiffness": 14.0, "suspensionDamping": 2.6,
        "driveForce": 9000.0, "brakeForce": 11000.0, "handbrakeForce": 6000.0,
        "maxSpeed": 26.0, "reverseFactor": 0.4,
        "rollingResistance": 0.012, "airDrag": 0.42,
        "maxSteerAngle": 34.0, "steerSpeed": 5.0,
        "steerSpeedFalloff": 0.6, "steerReturnSpeed": 7.0,
        "lateralGripFront": 0.92, "lateralGripRear": 0.88,
        "handbrakeGripRear": 0.30, "tyreLoadSensitivity": 1.0,
        "downforce": 0.0, "antiRoll": 0.4,
        "wheelNodePrefix": "Wheel",
        "frontWheelDrive": False, "rearWheelDrive": True,
        # Off, always. A character and a vehicle read the SAME movement actions,
        # so a car left listening steers itself off the kerb whenever the player
        # walks. scripts/car.js hands it the wheel only while someone is sitting
        # in it, and a traffic AI plugs into that same seam.
        "readsInput": False,
    }]
    car["children"] = [
        node("CollisionShape", "Shape", shapeType=1,
             halfExtents=[round(v, 4) for v in half],
             offset=[0.0, round(centre_y, 4), 0.0]),
        kitmesh("Body", "cars", model + "-body", (0.0, 0.0, 0.0), CAR),
    ]
    # Placed at rest height; the behaviour overwrites position and rotation every
    # frame from the suspension, and leaves the scale alone. Grouped because a
    # wheel is otherwise unreachable from a script — nothing in the NodeRef API
    # walks to a child by name — and the placement it is given is the only
    # visible evidence that the suspension is doing anything.
    for suffix, sx, sz in (("FL", 1.0, 1.0), ("FR", -1.0, 1.0),
                           ("RL", 1.0, -1.0), ("RR", -1.0, -1.0)):
        anchor = (front[0] * CAR * sx, radius,
                  (front[2] if sz > 0 else rear[2]) * CAR * abs(sz))
        car["children"].append(
            kitmesh("Wheel" + suffix, "cars", model + "-wheel", anchor, CAR,
                    groups=["vehicle_wheel", name + "_wheel"]))
    return car


def vehicles_spec_for(model):
    specs = vehicle_specs()
    if model not in specs:
        raise SystemExit(
            "drivable_car: '%s' has no entry in vehicles.json. Re-run "
            "tools/split_car_wheels.py after adding a car." % model)
    spec = specs[model]
    missing = [w for w in ("wheel-front-left", "wheel-back-left")
               if w not in spec["wheels"]]
    if missing:
        raise SystemExit("drivable_car: '%s' is missing %s in vehicles.json"
                         % (model, ", ".join(missing)))
    return spec


# ── the city ────────────────────────────────────────────────────────────────
def build_city(net):
    scene = node("Scene", "SunsetStrip")
    world = scene["children"]

    # Sun matched to the HDRI's own sun so cast shadows agree with the sky.
    world.append(node("LightNode", "Sun", (0.0, 90.0, 0.0),
                      lightType=0, color=[1.0, 0.95, 0.86], intensity=3.1,
                      direction=[-0.42, -0.78, -0.46], castShadows=True,
                      bakeMode=0, range=10.0, spotInnerAngle=25.0, spotOuterAngle=35.0))
    world.append(node("LightNode", "SkyFill", (0.0, 60.0, 0.0),
                      lightType=0, color=[0.45, 0.62, 0.85], intensity=0.7,
                      direction=[0.55, -0.5, 0.66], castShadows=False,
                      bakeMode=0, range=10.0, spotInnerAngle=25.0, spotOuterAngle=35.0))

    # One collider carries the whole buildable area; the road tiles and block
    # slabs are drawn on top of it and need none of their own.
    half = X(GRID - 1) + TILE / 2
    depth = CITY_EDGE_Z - Z(0) + TILE / 2
    ground = box_body("Ground", (0.0, ASPHALT_Y - 3.0, (Z(0) - TILE / 2 + CITY_EDGE_Z) / 2),
                      (half, 3.0, depth / 2), groups=["ground"])
    # Drawn just under the asphalt it abuts, so the two never share a plane.
    ground["children"].append(slab(
        "GroundTop", (0.0, 3.0 + GROUND_VISUAL_Y - ASPHALT_Y - 1.0, 0.0),
        (half * 2, 2.0, depth), GROUND))
    world.append(ground)

    world.append(lay_roads(net))

    blocks = node("Node", "Blocks")
    for i0, i1, j0, j1 in net.blocks():
        fill_block(blocks, i0, i1, j0, j1)
    world.append(blocks)

    world.append(street_furniture(net))
    world.append(skyline())
    world.append(beach_and_sea())
    world.append(water("Sea", (0.0, SEA_Y, SEA_CENTRE_Z), SEA_HALF))

    # Cars standing at the kerb. Every one of them drives: a city where one car
    # in thirty-three opens is worse than one where none do, because nothing
    # tells the player which. The cost is four raycasts and a body each, which is
    # why they are counted rather than assumed — see the triangle and body
    # budget in README.
    cars = node("Node", "ParkedCars")
    fleet = ("sedan", "suv", "taxi", "van", "hatchback-sports", "police", "delivery")
    for k in range(34):
        i = RNG.choice(AVENUES)
        j = RNG.randrange(STREETS[0] + 1, BOULEVARD - 1)
        if (i, j) not in net.cells or net.is_junction((i, j)):
            continue
        # Nothing parks in the player's own lane for the length of street either
        # side of their car. A car parks at X(i) +/- 2.4, which is exactly the
        # lane the hero car sits in, so without this the first car anyone opens
        # is boxed in nose to tail and driving it away is a collision rather
        # than a drive.
        if i == HERO_CELL[0] and abs(j - HERO_CELL[1]) <= HERO_CLEAR_TILES:
            continue
        side = RNG.choice((-1, 1))
        cars["children"].append(drivable_car(
            "Car%d" % k, (X(i) + side * 2.4, ASPHALT_Y, Z(j) + RNG.uniform(-2.0, 2.0)),
            0.0 if side < 0 else 180.0, RNG.choice(fleet),
            groups=["vehicle", "parked_car"]))
    world.append(cars)

    # Four enterable buildings. Each door sits on the pavement of a named
    # avenue, and the pair of teleports is wired end to end so neither side can
    # land the player inside the other's volume.
    # The pavement is the only clear band: the kerb is TILE/2 from the avenue
    # centre and the facades stand SIDEWALK_W behind it, so a volume placed any
    # deeper sits inside a building and the character controller pushes the
    # player out of it before the trigger can fire.
    door_cells = ((18, 12), (32, 20), (11, 26), (25, 38))
    door_x = [X(i) + TILE / 2 + SIDEWALK_W / 2 for i, _ in door_cells]
    # Coming back out lands the player further up the same pavement, clear of the
    # door's own volume, so the two teleports cannot bounce them between rooms.
    street_points = [(door_x[k], 1.4, Z(j) + 4.0) for k, (_, j) in enumerate(door_cells)]
    interiors, entries = build_interiors(street_points)
    world.append(interiors)

    doors = node("Node", "Doors")
    for k, ((i, j), entry) in enumerate(zip(door_cells, entries)):
        doors["children"].append(door(
            "Door%d" % k, (door_x[k], 1.2, Z(j)), (0.9, 1.6, 2.0), entry,
            groups=["door", "street_door"]))
    world.append(doors)

    # The player starts on a downtown pavement, facing the towers.
    spawn = (X(25) + 6.0, 1.4, Z(19) + 6.0)

    # One car that actually drives, left at the kerb of the avenue the player
    # spawns on, pointing up it. Parked on the asphalt rather than the pavement:
    # its wheels are rays, and starting it astride a kerb would settle it into a
    # lean before the player ever touches it.
    world.append(drivable_car(
        "HeroCar", (X(HERO_CELL[0]) + 2.4, ASPHALT_Y, Z(HERO_CELL[1]) + 2.0),
        180.0, "sedan", groups=["vehicle", "hero_car"]))
    # `camera_target` is what the camera follows, and it moves to the car while
    # the player is sitting in it. Kept separate from `player` so everything else
    # can still find the character wherever the view happens to be.
    player = node("CharacterBody", "Player", spawn, groups=["player", "camera_target"])
    player["behaviours"] = [
        {"type": "Character", "enabled": True,
         "moveSpeed": 5.2, "sprintMultiplier": 1.7,
         "groundAcceleration": 45.0, "groundDeceleration": 38.0,
         "gravity": 20.0, "airControl": 0.65, "airAcceleration": 22.0,
         "fallGravityMultiplier": 1.45, "maxFallSpeed": 26.0,
         "jumpHeight": 1.2, "coyoteTime": 0.14, "jumpBufferTime": 0.16, "jumpCount": 1,
         "faceMovement": True, "turnMode": 0, "turnSpeed": 16.0,
         "idleClip": "idle", "walkClip": "run", "jumpClip": "jump"},
    ]
    player["children"] = [
        node("CollisionShape", "Shape", shapeType=3, radius=0.34,
             height=CHARACTER_HEIGHT, axis=1, offset=[0, 0, 0]),
        # The rig faces +Z while faceMovement assumes -Z, so the model container
        # carries a 180 degree turn. Only the visible facing needs it.
        node("Node", "Body", (0.0, -CHARACTER_HEIGHT * 0.5, 0.0), 1.0, 180.0,
             importedFrom=CHARACTER % "player"),
    ]
    world.append(player)

    camera = node("Camera", "MainCamera", (spawn[0], spawn[1] + 2.0, spawn[2] + 6.0),
                  groups=["camera"], fov=64.0, nearPlane=0.1, farPlane=900.0, isMain=True)
    camera["behaviours"] = [{
        "type": "CameraFollow", "enabled": True, "targetGroup": "camera_target",
        "distance": 5.0, "height": 1.5,
        # A positive initialPitch drops the rig BELOW its target and aims it up;
        # 0 is the level, neutral start.
        "initialPitch": 0.0, "minPitch": -30.0, "maxPitch": 55.0,
        "shoulderOffset": 0.0,
        "positionDamping": 16.0, "verticalDamping": 9.0,
        "collisionMargin": 0.35, "minDistance": 1.1,
    }]
    world.append(camera)

    # The driver owns the handover between walking and driving. It gets a node of
    # its own because seating someone disables the player's node, and a disabled
    # node stops running its behaviours — a driver riding on the player would
    # switch itself off the moment it got in.
    control = node("Node", "DriverControl", groups=["control"])
    control["behaviours"] = [script("scripts/driver.js")]
    world.append(control)

    scene["settings"] = {
        "ambient": [0.014, 0.019, 0.032],
        "clearColor": [0.17, 0.39, 0.79],
        "postProcessing": True,
        "lightingMode": 0,
        "giEnabled": True, "giMode": 1, "giIntensity": 0.7,
        "skyboxTexture": "assets/skies/sky.hdr",
        "skyboxExposure": 0.9, "skyboxRotation": 155.0,
        "iblEnabled": True, "iblDiffuseIntensity": 0.24, "iblSpecularIntensity": 0.7,
        "aoEnabled": True, "aoRadius": 0.9, "aoIntensity": 1.2, "aoPower": 1.6,
        "fogEnabled": True, "fogColor": [0.33, 0.55, 0.83],
        "fogStart": 150.0, "fogDensity": 0.0025,
        "bloomEnabled": True, "bloomThreshold": 1.9, "bloomIntensity": 0.2,
        "bloomRadius": 4.0,
        "changeRenderingAtLoad": True,
    }
    return scene


# ── output ──────────────────────────────────────────────────────────────────
def count_nodes(n):
    return 1 + sum(count_nodes(c) for c in n.get("children", []))


def write_json(doc, path):
    full = os.path.join(HERE, path)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    with open(full, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)


def main():
    net = roadnet.RoadNetwork()

    scene = build_city(net)
    write_json({"schema": 2, "version": 2, "scene": scene}, "scenes/city.scene")

    graph = net.lane_graph(lambda i, j: (X(i), Z(j)))
    write_json({"schema": 1, "version": 1, "tile": TILE,
                "laneOffset": TILE * 0.25, **graph}, "scenes/city.roadnet")

    def group_count(n, name):
        hit = 1 if name in (n.get("groups") or []) else 0
        return hit + sum(group_count(c, name) for c in n.get("children", []))

    print("scenes/city.scene      %6d nodes" % count_nodes(scene))
    for g in ("road", "building", "parked_car", "vehicle", "streetlight", "skyline"):
        print("  %-12s %5d" % (g, group_count(scene, g)))
    print("scenes/city.roadnet    %6d lane nodes, %d edges"
          % (len(graph["nodes"]), len(graph["edges"])))


if __name__ == "__main__":
    main()
