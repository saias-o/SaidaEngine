#version 450
#extension GL_GOOGLE_include_directive : require

// One ring of a GrassNode's blades. Each cell grows a tuft: its blades are
// made here from a hash of the cell -- where in it, which way each faces,
// how tall, how it leans -- standing on the field's ground, triangle for
// triangle, in the colour and density of the cover under them. No vertex
// input, nothing stored: a ring is RING_CELLS^2 cells around the camera, the
// ring inside it cut out, each ring's tufts twice as far apart and their
// blades wider than the last's. What decides a whole tuft -- its cell, its
// cover, the view -- is tested before any blade is built, so a dropped tuft
// costs a handful of instructions a vertex.

#ifdef MULTIVIEW
#extension GL_EXT_multiview : require
#endif

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view[2];
    mat4 proj[2];
} cam;

#include "grass.glsl"
#include "grass_blade.glsl"
#include "noise.glsl"
#include "surface_variation.glsl"

layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec3 fragColor;
layout(location = 3) out float fragHeight;  // 0 at the root, 1 at the tip
layout(location = 4) out float fragGust;    // how hard the passing gust lays it, 0 to 1
layout(location = 5) out float fragDetail;  // how much of the blade's own look is drawn, 0 to 1

const float PI = 3.14159265359;
const float ROOT_SINK = 0.02;        // m below the ground, so no root floats over a crease
const float HEIGHT_SPREAD = 0.6;     // a blade's height, 1 +/- this times half
const float LEAN = 0.3;              // the tip's lean, in blade heights
const float TUFT_SPREAD = 0.9;       // of a cell, the square a tuft's blades root in
const float RING_WIDENING = 1.7;     // a ring's blades this much wider than the last's
const float FADE_START = 0.7;        // of the radius: blades shorten from here to nothing at it
const float DETAIL_START = 0.25;     // of the radius: a blade's own look gives way from here to FADE_START
const float CULL_MARGIN = 1.25;      // clip space: a tuft this far outside the view is dropped
const float GUST_SCALE = 0.045;      // gust cells a metre: waves a few tens of metres across
const float GUST_SPEED = 0.45;       // gust cells a second, along the wind

uint hashCell(ivec2 cell, uint salt) {
    uvec2 u = uvec2(cell);
    uint h = u.x * 1597334673u ^ u.y * 3812015801u ^ salt * 2654435761u;
    h ^= h >> 16; h *= 2246822519u; h ^= h >> 13; h *= 3266489917u; h ^= h >> 16;
    return h;
}
float random(ivec2 cell, uint salt) { return float(hashCell(cell, salt)) * (1.0 / 4294967296.0); }

// The ground's height at (u, v), and its slope there: dh/du, dh/dv of the
// triangle under it.
float groundAt(GpuGrass g, vec2 uv, out vec2 slope) {
    int n = g.sizes.x;
    vec2 at = clamp(uv, vec2(0.0), vec2(1.0)) * float(n - 1);
    ivec2 c = min(ivec2(floor(at)), ivec2(n - 2));
    vec2 f = at - vec2(c);
    int base = int(push.slot) * GROUND_STRIDE;
    float sw = ground[base + c.y * n + c.x], se = ground[base + c.y * n + c.x + 1];
    float nw = ground[base + (c.y + 1) * n + c.x], ne = ground[base + (c.y + 1) * n + c.x + 1];
    // The ground mesh's split: (sw, se, ne) and (sw, ne, nw).
    bool lower = f.x >= f.y;
    bool south = c.y == 0 && lower && g.sizes.z > 0;
    bool north = c.y == n-2 && !lower && g.sizes.w > 0;
    if (south || north) {
        float weight = south ? f.y : 1.0-f.y;
        float projected = south ? (f.x-f.y)/max(1e-6,1.0-f.y) : f.x/max(1e-6,f.y);
        float u = (float(c.x)+clamp(projected,0.0,1.0))/float(n-1);
        int offset = base+MAX_GROUND*MAX_GROUND+(north ? 2*MAX_BOUNDARY : 0);
        int lo=0, hi=(south ? g.sizes.z : g.sizes.w)-1;
        // At most nine probes, only in the two refined boundary strips.
        while(hi-lo>1) {
            int mid=(lo+hi)/2;
            if(ground[offset+2*mid]<=u)lo=mid;else hi=mid;
        }
        float ul=ground[offset+2*lo], ur=ground[offset+2*hi];
        float hl=ground[offset+2*lo+1], hr=ground[offset+2*hi+1];
        float ds=(hr-hl)/(ur-ul), edge=hl+(u-ul)*ds;
        float apex=south ? ne : sw;
        slope=vec2(ds,south ? (apex-edge)*float(n-1)+ds*(projected-1.0)
                                           : (edge-apex)*float(n-1)-ds*projected);
        return mix(edge,apex,weight);
    }
    slope = (lower ? vec2(se - sw, ne - se) : vec2(ne - nw, nw - sw)) * float(n - 1);
    return lower ? sw * (1.0 - f.x) + se * (f.x - f.y) + ne * f.y
                 : sw * (1.0 - f.y) + ne * f.x + nw * (f.y - f.x);
}
float groundAt(GpuGrass g, vec2 uv) {
    vec2 slope;
    return groundAt(g, uv, slope);
}

// The ground material's brightness at node-local `p`: its macro variation,
// as the surface shader draws it (surface_variation.glsl). Every octave is
// shown: within a grass radius a pixel never covers enough of the ground for
// the surface shader to fade one.
float groundVariation(GpuGrass g, vec2 p) {
    vec4 v = g.variation;
    if (v.y <= 0.0 || v.z <= 0.0) return 1.0;
    vec2 st = vec2(dot(vec3(p, 1.0), g.materialU.xyz), dot(vec3(p, 1.0), g.materialV.xyz));
    SurfaceCoords c = warpedCoords(st, vec2(0.0), vec2(0.0), v.x);
    return 1.0 + v.z * macroVariation(c.uv, vec2(0.0), vec2(0.0), v.y).x;
}

vec3 srgbToLinear(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

vec4 texel(int base, int c, ivec2 t) {
    t = clamp(t, ivec2(0), ivec2(c - 1));
    return unpackUnorm4x8(cover[base + t.y * c + t.x]);
}

// Bilinear colour (sRGB as stored, made linear) and density.
vec4 coverAt(GpuGrass g, vec2 uv) {
    int c = g.sizes.y;
    vec2 at = uv * float(c) - 0.5;
    ivec2 i = ivec2(floor(at));
    vec2 f = at - vec2(i);
    int base = int(push.slot) * MAX_COVER * MAX_COVER;
    vec4 v = mix(mix(texel(base, c, i), texel(base, c, i + ivec2(1, 0)), f.x),
                 mix(texel(base, c, i + ivec2(0, 1)), texel(base, c, i + ivec2(1, 1)), f.x), f.y);
    return vec4(srgbToLinear(v.rgb), v.a);
}

void drop() {
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);  // every corner the same: no area, nothing drawn
    fragWorldPos = vec3(0.0); fragNormal = vec3(0.0, 1.0, 0.0); fragColor = vec3(0.0);
    fragHeight = 0.0; fragGust = 0.0; fragDetail = 0.0;
}

void main() {
    GpuGrass g = grass.items[push.slot];
    uint blades = uint(g.tuft.x);
    uint perTuft = push.vertices * blades;
    uint tuft = uint(gl_VertexIndex) / perTuft;
    uint inTuft = uint(gl_VertexIndex) % perTuft;
    uint bladeIndex = inTuft / push.vertices;
    uint corner = inTuft % push.vertices;
    ivec2 cell = push.cellMin + ivec2(int(tuft % push.cellsX), int(tuft / push.cellsX));
    // The world-fixed cell, so a tuft keeps its place as the rings move.
    ivec2 key = ivec2(floor(push.origin / push.spacing + 0.5)) + cell;

    // The tuft: kept or dropped before any blade is built.
    vec2 centre = push.origin + (vec2(cell) + 0.5) * push.spacing;
    // The ring inside draws this square.
    if (push.hole.x > 0.0 && all(lessThan(abs(centre - (push.origin + 0.5 * float(RING_CELLS) * push.spacing)),
                                          push.hole))) { drop(); return; }
    float distance = length(centre - g.camera.xz);
    float radius = g.blades.w;
    if (distance > radius) { drop(); return; }
    vec2 uv = vec2(dot(vec3(centre, 1.0), g.uvFromLocalU.xyz), dot(vec3(centre, 1.0), g.uvFromLocalV.xyz));
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) { drop(); return; }
    vec4 here = coverAt(g, uv);
    // The cover's density is the share of cells that grow a tuft.
    if (random(key, 3u) >= here.a) { drop(); return; }
    float ground = groundAt(g, uv);
    vec4 clip = cam.proj[0] * cam.view[0] * g.localToWorld *
                vec4(centre.x, ground + 0.5 * g.blades.y, centre.y, 1.0);
    if (clip.w > 0.0 && any(greaterThan(abs(clip.xy), vec2(CULL_MARGIN * clip.w)))) { drop(); return; }

    // One blade of it.
    ivec2 bladeKey = key * 8 + ivec2(int(bladeIndex), int(bladeIndex) * 3);
    vec2 local = centre + (vec2(random(bladeKey, 1u), random(bladeKey, 2u)) - 0.5) * TUFT_SPREAD * push.spacing;
    vec2 bladeUv = vec2(dot(vec3(local, 1.0), g.uvFromLocalU.xyz), dot(vec3(local, 1.0), g.uvFromLocalV.xyz));
    vec2 slope;
    float y = groundAt(g, clamp(bladeUv, vec2(0.0), vec2(1.0)), slope) - ROOT_SINK;
    // The ground's normal under the blade, node-local: its slope along x and z.
    vec2 dhdxz = slope.x * vec2(g.uvFromLocalU.x, g.uvFromLocalU.y) + slope.y * vec2(g.uvFromLocalV.x, g.uvFromLocalV.y);
    vec3 groundUp = normalize(vec3(-dhdxz.x, 1.0, -dhdxz.y));

    float fade = 1.0 - smoothstep(FADE_START * radius, radius, distance);
    float detail = 1.0 - smoothstep(DETAIL_START * radius, FADE_START * radius, distance);
    float height = g.blades.y * (1.0 + HEIGHT_SPREAD * (random(bladeKey, 4u) - 0.5)) * fade *
                   mix(0.55, 1.0, here.a);
    float width = g.blades.z * pow(RING_WIDENING, float(push.ring)) * mix(0.7, 1.3, random(bladeKey, 5u));
    float facing = random(bladeKey, 6u) * 2.0 * PI;
    vec2 side = vec2(cos(facing), sin(facing));
    // Out of the tuft's centre, as a tuft opens.
    vec2 outward = local - centre;
    vec2 leanDir = (length(outward) > 1e-4 ? normalize(outward) : side) * (0.4 + 0.6 * random(bladeKey, 7u));

    // Wind: waves of gusts running across the field, each laying the grass
    // down as it passes, over a steady sway and each blade's own flutter.
    vec2 windDir = g.wind.xy;
    float gust = noised(local * GUST_SCALE - windDir * g.wind.w * GUST_SPEED).x;
    float wave = smoothstep(0.45, 0.85, gust) * g.tuft.y;
    float flutter = sin(g.wind.w * 3.1 + dot(local, windDir) * 1.3 + random(bladeKey, 8u) * 6.2831);
    vec2 sway = windDir * g.wind.z * (0.35 + 1.4 * wave) + vec2(-windDir.y, windDir.x) * g.wind.z * 0.15 * flutter;

    // Pushed aside by what walks through it.
    vec2 pushed = vec2(0.0);
    float flatten = 0.0;
    for (int i = 0; i < MAX_BENDERS; ++i) {
        vec4 b = g.benders[i];
        if (b.w <= 0.0) continue;
        vec2 away = local - b.xz;
        float d = length(away);
        if (d >= b.w || abs(y - b.y) > b.w + height) continue;
        float k = 1.0 - d / b.w;
        pushed += (d > 1e-4 ? away / d : side) * k;
        flatten = max(flatten, k);
    }

    // Segments up the blade: in the nearest ring two and a tip, past it the tip alone.
    uint segments = (push.vertices + 3u) / 6u;
    uint triangle = corner / 3u, vertex = corner % 3u;
    uint level;
    float across;
    if (triangle + 1u == segments * 2u - 1u) {
        level = vertex == 2u ? segments : segments - 1u;
        across = vertex == 0u ? -1.0 : vertex == 1u ? 1.0 : 0.0;
    } else {
        uint s = triangle / 2u;
        bool second = (triangle & 1u) == 1u;
        uvec3 levels = second ? uvec3(s, s + 1u, s + 1u) : uvec3(s, s, s + 1u);
        vec3 sides = second ? vec3(1.0, 1.0, -1.0) : vec3(-1.0, 1.0, -1.0);
        level = levels[vertex];
        across = sides[vertex];
    }
    float t = float(level) / float(segments);
    float bend = t * t;
    vec2 offset = (leanDir * LEAN * height + sway * height) * bend + pushed * height * 0.9 * t;
    // A blade keeps its length as it bends: what leans over comes down.
    float leaning = length(offset) / max(height, 1e-3);
    float rise = height * t * (1.0 - 0.7 * flatten) * inversesqrt(1.0 + leaning * leaning);
    float halfWidth = 0.5 * width * (1.0 - t * BLADE_TAPER);
    vec2 xz = local + side * across * halfWidth + offset;
    vec3 position = vec3(xz.x, y + rise, xz.y);

    // The blade's face, rounded across its width, then most of the way to
    // the ground's normal: a meadow is lit like the ground it covers. Away
    // from the camera, all the way: there the blades are lit as the ground.
    vec3 face = normalize(vec3(-side.y, 0.0, side.x) + vec3(side.x, 0.0, side.y) * across * 0.5 +
                          vec3(0.0, 0.4, 0.0));
    vec3 normal = normalize(mix(groundUp, mix(face, groundUp, 0.6), detail));

    vec4 world = g.localToWorld * vec4(position, 1.0);
#ifdef MULTIVIEW
    int viewIndex = gl_ViewIndex;
#else
    int viewIndex = 0;
#endif
    gl_Position = cam.proj[viewIndex] * cam.view[viewIndex] * world;
    fragWorldPos = world.xyz;
    fragNormal = normalize(mat3(g.localToWorld) * normal);
    // The ground's colour and brightness where the blade roots, each blade a
    // little lighter or darker than the next -- near; away, the ground's own.
    fragColor = here.rgb * groundVariation(g, local) * mix(1.0, mix(0.85, 1.15, random(bladeKey, 9u)), detail);
    fragHeight = t;
    fragGust = wave;
    fragDetail = detail;
}
