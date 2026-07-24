#!/usr/bin/env python3
"""Generate the BeachDemo project — a stylised sunset beach showcasing SaidaEngine's
analytic shore water (WaterNode shoreMode = Beach).

Run once from anywhere:   python BeachDemo/gen_beach.py

It writes, next to itself:
  * assets/skies/sunset.png   — a procedural equirectangular sunset sky (skybox + IBL)
  * asset_registry.json       — maps the sky texture id -> file (absolute path, so it
                                loads whatever the working directory is)
  * scenes/beach.scene        — the scene (sand, sea, palms, rocks, sunset lights)
  * BeachDemo.saidaproj          — the project (main_scene = scenes/beach.scene)

Only the built-in "cube" mesh is used for props, so no external art is required.
The sky is the one generated texture.
"""
import json, math, os, struct, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
SKY_REL = "assets/skies/sunset.png"
SKY_ID = 4242424242  # fixed id referenced by the scene (kept small so it is exact JSON)

# The sun's position in the sky (a unit vector). The skybox image AND the directional
# light below are both derived from this single vector, so the painted sun, the water
# glitter and the cast shadows all agree.
SUN_DIR = (0.50, 0.24, -0.83)  # front (over the sea, -Z), low, slightly to +X


# ─────────────────────────────────────────────────────────────────────────────
# Sunset sky — procedural equirectangular PNG (matches skybox.frag's mapping:
#   u = atan2(dir.z, dir.x)/2pi + 0.5 ,  v = asin(-dir.y)/pi + 0.5 ).
# ─────────────────────────────────────────────────────────────────────────────
def _n(v):
    l = math.sqrt(sum(c * c for c in v))
    return tuple(c / l for c in v)


def _mix(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


def sky_color(dirx, diry, dirz, sun):
    height = diry  # +1 zenith, 0 horizon, -1 nadir

    if height >= 0.0:
        t = max(0.0, min(1.0, height / 0.9)) ** 0.6
        col = list(_mix((1.00, 0.48, 0.27), (0.11, 0.16, 0.40), t))  # orange -> dusk blue
    else:
        t = min(1.0, -height / 0.5)
        col = list(_mix((0.40, 0.22, 0.18), (0.03, 0.04, 0.08), t))  # sea haze -> dark

    # Warm haze band hugging the horizon.
    haze = math.exp(-(height / 0.05) ** 2)
    for i, c in enumerate((0.55, 0.30, 0.18)):
        col[i] += c * haze

    # The sun: a tight near-white core + a broad warm halo (round in 3D, not in UV).
    d = max(-1.0, min(1.0, dirx * sun[0] + diry * sun[1] + dirz * sun[2]))
    ang = math.acos(d)
    core = math.exp(-(ang / 0.05) ** 2)
    halo = math.exp(-(ang / 0.30) ** 2)
    for i, c in enumerate((1.00, 0.95, 0.82)):
        col[i] += c * core
    for i, c in enumerate((1.00, 0.55, 0.28)):
        col[i] += c * halo * 0.7

    return tuple(max(0, min(255, int(c * 255 + 0.5))) for c in col)


def write_sky_png(path, w=1024, h=512):
    sun = _n(SUN_DIR)
    two_pi, pi = 2.0 * math.pi, math.pi
    raw = bytearray()
    for y in range(h):
        raw.append(0)  # PNG filter type 0 (none) for this scanline
        v = (y + 0.5) / h
        el = (v - 0.5) * pi
        diry = -math.sin(el)
        r = math.cos(el)
        for x in range(w):
            u = (x + 0.5) / w
            phi = (u - 0.5) * two_pi
            raw += bytes(sky_color(r * math.cos(phi), diry, r * math.sin(phi), sun))

    def chunk(typ, data):
        return (struct.pack(">I", len(data)) + typ + data
                + struct.pack(">I", zlib.crc32(typ + data) & 0xffffffff))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))  # 8-bit RGB
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
        f.write(chunk(b"IEND", b""))


# ─────────────────────────────────────────────────────────────────────────────
# Scene helpers (same conventions as WitnessGame/gen_witness.py).
# ─────────────────────────────────────────────────────────────────────────────
IDENT = [0.0, 0.0, 0.0, 1.0]
SAND_SLOPE = 0.05  # must match the WaterNode shoreSlope: keeps the visible sand on the
                   # same plane as the water's analytic seabed (y = SAND_SLOPE * z).


def xform(pos, rot=IDENT, scale=(1, 1, 1)):
    return {"position": [float(p) for p in pos],
            "rotation": [float(r) for r in rot],
            "scale": [float(s) for s in scale]}


def axis_quat(axis, deg):
    a = math.radians(deg) * 0.5
    s = math.sin(a)
    return [axis[0] * s, axis[1] * s, axis[2] * s, math.cos(a)]


def quat_mul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return [aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz]


def rot_vec(q, v):
    x, y, z, w = q
    vx, vy, vz = v
    tx = 2 * (y * vz - z * vy)
    ty = 2 * (z * vx - x * vz)
    tz = 2 * (x * vy - y * vx)
    return (vx + w * tx + (y * tz - z * ty),
            vy + w * ty + (z * tx - x * tz),
            vz + w * tz + (x * ty - y * tx))


def sand_y(z):
    return SAND_SLOPE * z  # the sand surface height (matches the water seabed)


def mesh(name, color, scale, pos=(0, 0, 0), rot=IDENT, shadows=True):
    return {
        "type": "MeshNode", "name": name, "enabled": True, "behaviours": [],
        "mesh": "cube", "baseColor": [float(c) for c in color],
        "meshEnabled": True, "castShadows": shadows, "includeInLightBaking": False,
        "transform": xform(pos, rot, scale),
    }


def group(name, pos, children, rot=IDENT):
    # A transform-only parent (a MeshNode with its mesh hidden) to hold prop parts.
    return {
        "type": "MeshNode", "name": name, "enabled": True, "behaviours": [],
        "mesh": "cube", "baseColor": [1.0, 1.0, 1.0],
        "meshEnabled": False, "castShadows": False, "includeInLightBaking": False,
        "transform": xform(pos, rot), "children": children,
    }


def palm(name, x, z, lean_deg=5.0):
    trunk_h = 6.0
    lean = axis_quat((0, 0, 1), lean_deg)
    parts = [mesh("Trunk", (0.42, 0.30, 0.18), (0.55, trunk_h, 0.55),
                  pos=(0, trunk_h * 0.5, 0), rot=lean)]
    # Coconuts.
    for i, dx in enumerate((0.35, -0.3)):
        parts.append(mesh(f"Coco{i}", (0.30, 0.22, 0.14), (0.45, 0.45, 0.45),
                           pos=(dx, trunk_h - 0.4, 0.2 * (1 - i)), shadows=False))
    # A fan of fronds: yaw around the crown, each pitched down.
    for i in range(6):
        yaw = axis_quat((0, 1, 0), i * 60.0)
        q = quat_mul(yaw, axis_quat((1, 0, 0), -32.0))
        out = rot_vec(yaw, (0, 0, -1.7))
        parts.append(mesh(f"Frond{i}", (0.16, 0.42, 0.18), (0.55, 0.12, 3.4),
                          pos=(out[0], trunk_h + 0.1 + out[1], out[2]), rot=q))
    return group(name, (x, sand_y(z), z), parts, rot=lean)


def rock(name, x, z, s, tone=0.42):
    yaw = axis_quat((0, 1, 0), (x * 37.0 + z * 11.0) % 90.0)
    return mesh(name, (tone, tone * 0.96, tone * 0.9), (s, s * 0.7, s * 0.85),
                pos=(x, sand_y(z) + s * 0.2, z), rot=yaw)


def umbrella(name, x, z):
    top = quat_mul(axis_quat((0, 1, 0), 20.0), axis_quat((1, 0, 0), 8.0))
    parts = [
        mesh("Pole", (0.85, 0.82, 0.78), (0.12, 3.2, 0.12), pos=(0, 1.6, 0)),
        mesh("Canopy", (0.90, 0.25, 0.22), (3.4, 0.18, 3.4), pos=(0, 3.3, 0), rot=top),
    ]
    return group(name, (x, sand_y(z), z), parts)


def starfish(name, x, z):
    return mesh(name, (0.95, 0.55, 0.20), (0.9, 0.12, 0.9),
                pos=(x, sand_y(z) + 0.05, z),
                rot=axis_quat((0, 1, 0), (x * 53.0) % 90.0), shadows=False)


def sun_light():
    # Direction the light travels = away from the painted sun (-SUN_DIR).
    s = _n(SUN_DIR)
    return {
        "type": "LightNode", "name": "Sun", "enabled": True, "behaviours": [],
        "lightType": 0, "color": [1.0, 0.62, 0.38], "intensity": 3.3,
        "direction": [-s[0], -s[1], -s[2]], "range": 10.0,
        "spotInnerAngle": 25.0, "spotOuterAngle": 35.0, "castShadows": True,
        "bakeMode": 0, "transform": xform((0, 40, 0)),
    }


def water():
    # The star of the demo: an analytic BEACH shore. The waterline is the node centre
    # (z = 0); the sea is at z < 0 (toward the sun), dry sand at z > 0. The seabed
    # deepens at SAND_SLOPE — the same slope as the visible sand plane.
    return {
        "type": "Water", "name": "Sea", "enabled": True, "behaviours": [],
        "transform": xform((0.0, 0.0, 0.0)),
        "size": 400.0,
        "deepColor": [0.015, 0.07, 0.11], "shallowColor": [0.22, 0.42, 0.42],
        "foamColor": [1.0, 0.93, 0.85],
        "roughness": 0.06, "reflectivity": 0.62,
        "amplitude": 0.35, "wavelength": 16.0, "waveSpeed": 0.8, "choppiness": 0.30,
        "fresnelPower": 5.0, "specularPower": 220.0, "specularIntensity": 1.6,
        "foamIntensity": 0.06, "foamThreshold": 0.2,
        # Shore (beach):
        "shoreMode": 1, "shoreAngle": 90.0, "shoreWaterline": 0.0, "shoreSlope": SAND_SLOPE,
        "depthColorFalloff": 7.0, "edgeFade": 0.5,
        "shoreFoam": 0.95, "foamWidth": 0.7, "swashSpeed": 0.9, "swashAmount": 0.7,
        "waveFlatten": 1.3,
    }


def camera():
    return {
        "type": "Camera", "name": "Beach Camera", "enabled": True,
        "fovDegrees": 62.0, "nearZ": 0.1, "farZ": 600.0, "priority": 10, "active": True,
        "behaviours": [],
        # On the sand at z=+46, looking down the beach toward the sea and the sun (-Z).
        "transform": xform((2.0, sand_y(46) + 4.5, 46.0), axis_quat((1, 0, 0), -7.0)),
    }


def build_scene():
    children = [sun_light(), water(), camera()]

    # The beach: a big, gently tilted sand plate. Its top surface is y = SAND_SLOPE*z,
    # exactly the water's analytic seabed, so sea and sand meet seamlessly and the
    # shallow water reveals real sand. Tilt = atan(SAND_SLOPE) about X.
    tilt = axis_quat((1, 0, 0), -math.degrees(math.atan(SAND_SLOPE)))
    children.append(mesh("Sand", (0.78, 0.71, 0.52), (800, 4, 800),
                         pos=(0, -2.0, 0), rot=tilt, shadows=False))

    # Palms set back on the dry beach.
    children += [palm("Palm_A", -14, 12, lean_deg=6),
                 palm("Palm_B", 13, 20, lean_deg=-5),
                 palm("Palm_C", 26, 9, lean_deg=4),
                 palm("Palm_D", -28, 24, lean_deg=-7)]

    # Rocks scattered down to the waterline (some get their feet wet at z < 0).
    children += [rock("Rock_1", -9, -2, 1.8), rock("Rock_2", 17, -1, 1.3, tone=0.38),
                 rock("Rock_3", -22, 4, 2.4), rock("Rock_4", 31, 3, 1.6),
                 rock("Rock_5", 6, -3, 0.9, tone=0.46)]

    children.append(umbrella("Umbrella", -5, 15))
    children.append(mesh("Towel", (0.85, 0.20, 0.32), (2.4, 0.08, 4.0),
                         pos=(3.5, sand_y(13) + 0.06, 13), shadows=False))
    children += [starfish("Star_1", 7, 5), starfish("Star_2", -11, 7)]

    settings = {
        "ambientLight": [0.32, 0.24, 0.28], "clearColor": [0.95, 0.55, 0.40],
        "ambient": [0.32, 0.24, 0.28],  # legacy key, harmless if ignored
        "postProcessing": True, "lightingMode": 0,
        "giEnabled": True, "giIntensity": 1.0,
        "skyboxTexture": SKY_ID, "skyboxExposure": 1.15, "skyboxRotation": 0.0,
        "iblEnabled": True, "iblDiffuseIntensity": 0.5, "iblSpecularIntensity": 1.0,
        "aoEnabled": True,
        "fogEnabled": True, "fogColor": [0.95, 0.56, 0.40], "fogStart": 35.0,
        "fogDensity": 0.011,
        "bloomEnabled": True, "bloomThreshold": 0.85, "bloomIntensity": 0.45,
        "bloomRadius": 3.5,
        "changeRenderingAtLoad": True,
    }

    return {
        "version": 1,
        "scene": {
            "type": "Scene", "name": "beach", "enabled": True, "behaviours": [],
            "transform": xform((0, 0, 0)), "settings": settings, "children": children,
        },
    }


def main():
    os.makedirs(os.path.join(HERE, "assets", "skies"), exist_ok=True)
    os.makedirs(os.path.join(HERE, "scenes"), exist_ok=True)

    write_sky_png(os.path.join(HERE, "assets", "skies", "sunset.png"))
    print("wrote", SKY_REL)

    # Registry: project-relative path so the texture loads regardless of where the
    # project dir lives or what it is renamed to (the engine resolves it against the
    # project root). hash=0 keeps the editor's asset auto-sync from re-mapping it.
    with open(os.path.join(HERE, "asset_registry.json"), "w") as f:
        json.dump({str(SKY_ID): {"path": SKY_REL, "hash": 0, "type": "Texture"}}, f, indent=4)
    print("wrote asset_registry.json")

    with open(os.path.join(HERE, "scenes", "beach.scene"), "w") as f:
        json.dump(build_scene(), f, indent=2)
    print("wrote scenes/beach.scene")

    with open(os.path.join(HERE, "BeachDemo.saidaproj"), "w") as f:
        f.write("[SaidaEngine Project]\n")
        f.write("name=BeachDemo\n")
        f.write("engine_version=0.1.0\n")
        f.write("main_scene=scenes/beach.scene\n")
        f.write("max_fps=240\n")
        f.write("shadow_resolution=2048\n")
        f.write("shadow_dist=120\n")
        f.write("shadow_soft=1\n")
    print("wrote BeachDemo.saidaproj")


if __name__ == "__main__":
    main()
