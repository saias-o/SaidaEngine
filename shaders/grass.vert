#version 450
#extension GL_GOOGLE_include_directive : require

// One ring of a GrassNode's blades. A blade is made here from a hash of the
// cell it grows in: where in the cell, which way it faces, how tall, how it
// leans; it stands on the field's ground, triangle for triangle, and takes
// the colour and density of the cover under it. No vertex input, nothing
// stored: a ring is RING_CELLS^2 cells around the camera, the ring inside it
// cut out, each ring's blades twice as far apart and wider than the last.

#ifdef MULTIVIEW
#extension GL_EXT_multiview : require
#endif

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view[2];
    mat4 proj[2];
} cam;

#include "grass.glsl"
#include "noise.glsl"

layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec3 fragColor;
layout(location = 3) out float fragHeight;  // 0 at the root, 1 at the tip

const float PI = 3.14159265359;
const float ROOT_SINK = 0.02;        // m below the ground, so no root floats over a crease
const float HEIGHT_SPREAD = 0.55;    // a blade's height, 1 +/- this times half
const float LEAN = 0.35;             // the tip's lean, in blade heights
const float RING_WIDENING = 1.7;     // a ring's blades this much wider than the last's
const float FADE_START = 0.7;        // of the radius: blades shorten from here to nothing at it
const float CULL_MARGIN = 1.2;       // clip space: a blade's root this far outside the view is dropped

uint hashCell(ivec2 cell, uint salt) {
    uvec2 u = uvec2(cell);
    uint h = u.x * 1597334673u ^ u.y * 3812015801u ^ salt * 2654435761u;
    h ^= h >> 16; h *= 2246822519u; h ^= h >> 13; h *= 3266489917u; h ^= h >> 16;
    return h;
}
float random(ivec2 cell, uint salt) { return float(hashCell(cell, salt)) * (1.0 / 4294967296.0); }

float groundAt(GpuGrass g, vec2 uv) {
    int n = g.sizes.x;
    vec2 at = clamp(uv, vec2(0.0), vec2(1.0)) * float(n - 1);
    ivec2 c = min(ivec2(floor(at)), ivec2(n - 2));
    vec2 f = at - vec2(c);
    int base = int(push.slot) * MAX_GROUND * MAX_GROUND;
    float sw = ground[base + c.y * n + c.x], se = ground[base + c.y * n + c.x + 1];
    float nw = ground[base + (c.y + 1) * n + c.x], ne = ground[base + (c.y + 1) * n + c.x + 1];
    // The ground mesh's split: (sw, se, ne) and (sw, ne, nw).
    return f.x >= f.y ? sw * (1.0 - f.x) + se * (f.x - f.y) + ne * f.y
                      : sw * (1.0 - f.y) + ne * f.x + nw * (f.y - f.x);
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
    return vec4(pow(v.rgb, vec3(2.2)), v.a);
}

void drop() {
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);  // every corner the same: no area, nothing drawn
    fragWorldPos = vec3(0.0); fragNormal = vec3(0.0, 1.0, 0.0); fragColor = vec3(0.0); fragHeight = 0.0;
}

void main() {
    GpuGrass g = grass.items[push.slot];
    uint blade = uint(gl_VertexIndex) / push.vertices;
    uint corner = uint(gl_VertexIndex) % push.vertices;
    ivec2 cell = push.cellMin + ivec2(int(blade % push.cellsX), int(blade / push.cellsX));
    // The world-fixed cell, so a blade keeps its place as the rings move.
    ivec2 key = ivec2(floor(push.origin / push.spacing + 0.5)) + cell;

    vec2 local = push.origin + (vec2(cell) + vec2(random(key, 1u), random(key, 2u))) * push.spacing;
    vec2 fromCamera = local - g.camera.xz;
    // The ring inside draws this square.
    if (push.hole.x > 0.0 && all(lessThan(abs(local - (push.origin + 0.5 * float(RING_CELLS) * push.spacing)),
                                          push.hole))) { drop(); return; }
    float distance = length(fromCamera);
    float radius = g.blades.w;
    if (distance > radius) { drop(); return; }

    vec2 uv = vec2(dot(vec3(local, 1.0), g.uvFromLocalU.xyz), dot(vec3(local, 1.0), g.uvFromLocalV.xyz));
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) { drop(); return; }
    vec4 here = coverAt(g, uv);
    // The cover's density is the share of cells that grow a blade.
    if (random(key, 3u) >= here.a) { drop(); return; }

    float y = groundAt(g, uv) - ROOT_SINK;
    vec4 rootClip = cam.proj[0] * cam.view[0] * g.localToWorld * vec4(local.x, y, local.y, 1.0);
    if (rootClip.w > 0.0 && any(greaterThan(abs(rootClip.xy), vec2(CULL_MARGIN * rootClip.w)))) {
        drop(); return;
    }

    float fade = 1.0 - smoothstep(FADE_START * radius, radius, distance);
    float height = g.blades.y * (1.0 + HEIGHT_SPREAD * (random(key, 4u) - 0.5)) * fade * mix(0.6, 1.0, here.a);
    float width = g.blades.z * pow(RING_WIDENING, float(push.ring)) * mix(0.7, 1.3, random(key, 5u));
    float facing = random(key, 6u) * 2.0 * PI;
    vec2 side = vec2(cos(facing), sin(facing));
    vec2 leanDir = vec2(-side.y, side.x) * (random(key, 7u) - 0.5) * 2.0;

    // Wind: a gust field drifting along the wind, felt most at the tip.
    vec2 windDir = g.wind.xy;
    float gust = noised(local * 0.08 - windDir * g.wind.w * 0.6).x;
    float flutter = sin(g.wind.w * 2.7 + dot(local, windDir) * 0.9 + random(key, 8u) * 6.2831);
    vec2 sway = windDir * g.wind.z * (0.6 * gust + 0.25 * flutter);

    // Pushed aside by what walks through it.
    vec2 push2 = vec2(0.0);
    float flatten = 0.0;
    for (int i = 0; i < MAX_BENDERS; ++i) {
        vec4 b = g.benders[i];
        if (b.w <= 0.0) continue;
        vec2 away = local - b.xz;
        float d = length(away);
        if (d >= b.w || abs(y - b.y) > b.w + height) continue;
        float k = 1.0 - d / b.w;
        push2 += (d > 1e-4 ? away / d : side) * k;
        flatten = max(flatten, k);
    }

    // Segments up the blade: ring 0 three, ring 1 two, ring 2 one.
    uint segments = (push.vertices - 3u) / 6u + 1u;
    uint triangle = corner / 3u, vertex = corner % 3u;
    uint level;
    float across;
    if (triangle + 1u == segments * 2u - 1u) {
        // The tip: a single triangle.
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
    vec2 offset = (leanDir * LEAN * height + sway) * bend + push2 * height * 0.9 * t;
    float rise = height * t * (1.0 - 0.7 * flatten);
    float halfWidth = 0.5 * width * (1.0 - t * 0.85);
    vec2 xz = local + side * across * halfWidth + offset;
    vec3 position = vec3(xz.x, y + rise, xz.y);

    // The blade's face, rounded across its width, then most of the way to
    // the ground's up: a meadow is lit like the ground it covers.
    vec3 face = normalize(vec3(-side.y, 0.0, side.x) + vec3(side.x, 0.0, side.y) * across * 0.5 +
                          vec3(0.0, 0.4, 0.0));
    vec3 normal = normalize(mix(face, vec3(0.0, 1.0, 0.0), 0.55));

    vec4 world = g.localToWorld * vec4(position, 1.0);
#ifdef MULTIVIEW
    int viewIndex = gl_ViewIndex;
#else
    int viewIndex = 0;
#endif
    gl_Position = cam.proj[viewIndex] * cam.view[viewIndex] * world;
    fragWorldPos = world.xyz;
    fragNormal = normalize(mat3(g.localToWorld) * normal);
    float shade = mix(0.85, 1.15, random(key, 9u));
    fragColor = here.rgb * shade;
    fragHeight = t;
}
