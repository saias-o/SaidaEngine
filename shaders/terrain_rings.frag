#version 450
#extension GL_GOOGLE_include_directive : require

// A TerrainRingsNode's surface: its layer's albedo under the scene's lights,
// the same Cook-Torrance terms as any lit mesh, the directional light cut by
// the rings' own shadow (terrain_rings_sun.comp). Distance haze is the
// tonemap pass's, from depth, like everything else drawn.

#include "lighting.glsl"
#include "terrain_rings.glsl"

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec2 fragLocalXZ;
layout(location = 3) in vec4 fragSurface;
layout(location = 4) in float fragSunlight;

layout(location = 0) out vec4 outColor;

float edgeSide(vec2 a, vec2 b, vec2 p) {
    vec2 e = b - a, d = p - a;
    return e.x * d.y - e.y * d.x;
}

// Inside a convex quadrilateral: on the same side of its four edges.
bool insideHole(vec4 ab, vec4 cd, vec2 p) {
    float s0 = edgeSide(ab.xy, ab.zw, p), s1 = edgeSide(ab.zw, cd.xy, p);
    float s2 = edgeSide(cd.xy, cd.zw, p), s3 = edgeSide(cd.zw, ab.xy, p);
    return (s0 >= 0.0 && s1 >= 0.0 && s2 >= 0.0 && s3 >= 0.0) ||
           (s0 <= 0.0 && s1 <= 0.0 && s2 <= 0.0 && s3 <= 0.0);
}

void main() {
    GpuTerrain t = terrains.items[push.slot];
    // Closer than the inner radius, or over a hole, the caller draws its own,
    // finer ground.
    if (length(fragLocalXZ - t.focus.xy) < t.focus.z) discard;
    for (int i = 0; i < t.holeCount.x; ++i)
        if (insideHole(t.holes[2 * i], t.holes[2 * i + 1], fragLocalXZ)) discard;

    vec4 layer = fragSurface;
    vec3 N = normalize(fragNormal);
    vec3 V = normalize(lights.cameraPos.xyz - fragWorldPos);
    LightTerms lit = accumulateOccluded(N, V, fragWorldPos, layer.rgb, 0.0, layer.a,
                                        int(t.sun.w + 0.5) - 1, clamp(fragSunlight, 0.0, 1.0));
    outColor = vec4(lit.diffuse + lit.specular, 1.0);
}
