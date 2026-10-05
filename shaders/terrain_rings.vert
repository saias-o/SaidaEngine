#version 450
#extension GL_GOOGLE_include_directive : require

// One ring of a TerrainRingsNode: a procedural RES x RES grid of quads whose
// heights come from the storage buffer. The finer level's square is cut out,
// and near its outer edge the ring slides its odd samples onto the even ones
// -- the next level's samples -- so the two meet without a crack.

#ifdef MULTIVIEW
#extension GL_EXT_multiview : require
#endif

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view[2];
    mat4 proj[2];
} cam;

#include "terrain_rings.glsl"

layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec2 fragLocalXZ;
layout(location = 3) out vec4 fragSurface;  // albedo, roughness: the cells around, blended
layout(location = 4) out float fragSunlight;

float sampleAt(int base, int i, int j) {
    i = clamp(i, 0, RES);
    j = clamp(j, 0, RES);
    return heights[base * SAMPLES * SAMPLES + j * SAMPLES + i];
}

uint layerOf(int base, int cx, int cz) {
    cx = clamp(cx, 0, RES - 1);
    cz = clamp(cz, 0, RES - 1);
    int cell = base * RES * RES + cz * RES + cx;
    return (layerWords[cell >> 2] >> (uint(cell & 3) * 8u)) & 0xFFu;
}

// What the ground is made of at a sample: the four cells that share it,
// averaged, so a forest's edge is a gradient a cell wide, not a staircase.
const float CELLS_AT_A_SAMPLE = 4.0;
vec4 surfaceAt(GpuTerrain t, int base, ivec2 at) {
    vec4 sum = vec4(0.0);
    for (int dz = -1; dz <= 0; ++dz)
        for (int dx = -1; dx <= 0; ++dx)
            sum += t.layers[min(layerOf(base, at.x + dx, at.y + dz), uint(MAX_LAYERS - 1))];
    return sum / CELLS_AT_A_SAMPLE;
}

float sunlightAt(int base, vec2 g) {
    g = clamp(g, vec2(0.0), vec2(float(RES)));
    ivec2 i0 = min(ivec2(floor(g)), ivec2(RES - 1));
    vec2 f = g - vec2(i0);
    int row = base * SAMPLES * SAMPLES + i0.y * SAMPLES + i0.x;
    float a = sunlight[row], b = sunlight[row + 1];
    float c = sunlight[row + SAMPLES], d = sunlight[row + SAMPLES + 1];
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// Bilinear between samples, at fractional grid coordinates.
float heightAt(int base, vec2 g) {
    g = clamp(g, vec2(0.0), vec2(float(RES)));
    ivec2 i0 = min(ivec2(floor(g)), ivec2(RES - 1));
    vec2 f = g - vec2(i0);
    float a = sampleAt(base, i0.x, i0.y), b = sampleAt(base, i0.x + 1, i0.y);
    float c = sampleAt(base, i0.x, i0.y + 1), d = sampleAt(base, i0.x + 1, i0.y + 1);
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

void main() {
    GpuTerrain t = terrains.items[push.slot];
    vec4 L = t.levels[push.level];
    int base = levelBase(push.slot, push.level);

    int quad = gl_VertexIndex / 6;
    int corner = gl_VertexIndex % 6;
    int qx = quad % RES;
    int qz = quad / RES;

    // The finer level's square, in this level's cells: whole cells, since a
    // level's centre is snapped to twice its spacing.
    if (push.level > 0u) {
        vec4 F = t.levels[push.level - 1u];
        if (F.w > 0.5) {
            vec2 lo = (F.xy - L.xy) / L.z;
            vec2 hi = lo + vec2(float(RES) * F.z / L.z);
            vec2 q = vec2(float(qx) + 0.5, float(qz) + 0.5);
            if (all(greaterThan(q, lo)) && all(lessThan(q, hi))) {
                gl_Position = vec4(2.0, 2.0, 2.0, 1.0);  // degenerate: nothing drawn
                fragWorldPos = vec3(0.0);
                fragNormal = vec3(0.0, 1.0, 0.0);
                fragLocalXZ = vec2(0.0);
                fragSurface = vec4(0.0);
                fragSunlight = 1.0;
                return;
            }
        }
    }

    const ivec2 OFF[6] = ivec2[6](ivec2(0, 0), ivec2(1, 0), ivec2(1, 1),
                                  ivec2(0, 0), ivec2(1, 1), ivec2(0, 1));
    vec2 g = vec2(ivec2(qx, qz) + OFF[corner]);

    // Morph: over the outer fifth of the ring, odd samples slide onto the
    // even sample before them, fully by 95% of the half-extent.
    float halfRes = 0.5 * float(RES);
    vec2 fromCentre = abs(g - vec2(halfRes)) / halfRes;
    float alpha = clamp((max(fromCentre.x, fromCentre.y) - 0.8) / 0.15, 0.0, 1.0);
    vec2 odd = mod(g, 2.0);
    vec2 gm = g - odd * alpha;

    float h = heightAt(base, gm);
    float hx0 = heightAt(base, gm - vec2(1.0, 0.0)), hx1 = heightAt(base, gm + vec2(1.0, 0.0));
    float hz0 = heightAt(base, gm - vec2(0.0, 1.0)), hz1 = heightAt(base, gm + vec2(0.0, 1.0));
    vec3 n = normalize(vec3(-(hx1 - hx0), 2.0 * L.z, -(hz1 - hz0)));

    vec3 local = vec3(L.x + gm.x * L.z, h, L.y + gm.y * L.z);
    vec4 world = t.localToWorld * vec4(local, 1.0);
    fragWorldPos = world.xyz;
    fragNormal = normalize(mat3(t.localToWorld) * n);
    fragLocalXZ = local.xz;
    fragSunlight = t.sun.w > 0.5 ? sunlightAt(base, gm) : 1.0;

    // Morphed samples take the surface of the even sample they slide onto.
    fragSurface = surfaceAt(t, base, ivec2(gm + 0.5));

#ifdef MULTIVIEW
    int vi = gl_ViewIndex;
#else
    int vi = 0;
#endif
    gl_Position = cam.proj[vi] * cam.view[vi] * world;
}
