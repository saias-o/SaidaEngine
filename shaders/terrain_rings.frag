#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_multiview : require

// A TerrainRingsNode's surface: its layer's albedo under the scene's lights,
// the same Cook-Torrance terms as any lit mesh, the directional light cut by
// the rings' own shadow (terrain_rings_sun.comp). Distance haze is the
// tonemap pass's, from depth, like everything else drawn. Compiled
// -DDEPTH_ONLY for the depth pre-pass, it keeps the discards and nothing else.

#include "lighting.glsl"
#include "screen_ambient_occlusion.glsl"
#include "rain_ripples.glsl"
#include "terrain_rings.glsl"
#include "water_types.glsl"
#include "water_shading.glsl"
#include "water_distant.glsl"
#ifdef BINDLESS
#include "terrain_material.glsl"
#endif

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec2 fragLocalXZ;
layout(location = 3) in vec4 fragSurface;
layout(location = 4) in float fragSunlight;
layout(location = 5) in vec3 fragLocalPos;
layout(location = 6) in vec3 fragLocalNormal;

#ifndef DEPTH_ONLY
layout(location = 0) out vec4 outColor;
#endif

void main() {
    GpuTerrain t = terrains.items[push.slot];
    float footprint = waterFootprint(fragWorldPos, normalize(fragNormal));
    vec3 rainDx = vec3(0.0), rainDy = vec3(0.0);
    if (max(lights.rainParams.x, lights.rainParams.y) > 0.0) {
        rainDx = dFdx(fragWorldPos);
        rainDy = dFdy(fragWorldPos);
    }
#ifdef BINDLESS
    vec3 dx = dFdx(fragLocalPos), dy = dFdy(fragLocalPos);
#endif
    // Closer than the inner radius, or over a hole, the caller draws its own,
    // finer ground.
    if (length(fragLocalXZ - t.focus.xy) < t.focus.z) discard;
    for (int i = 0; i < t.holeCount.x; ++i)
        if (insideHole(t.holes[2 * i], t.holes[2 * i + 1], fragLocalXZ)) discard;
#ifndef DEPTH_ONLY

    vec4 level = t.levels[push.level];
    vec2 grid = (fragLocalXZ-level.xy)/level.z;
    float waterWeight; vec4 waterBody; vec2 waterWaves; vec4 waterDynamics;
    terrainWaterLayer(t,levelBase(push.slot,push.level),grid,waterWeight,waterBody,waterWaves,waterDynamics);
    if (push.level+1u < uint(t.focus.w)) {
        vec4 next = t.levels[push.level+1u];
        vec2 edge = abs(grid-.5*float(RES))/(.5*float(RES));
        float blend = smoothstep(RING_MORPH_START,RING_MORPH_END,max(edge.x,edge.y));
        if (next.w > .5 && blend > 0.0) {
            float weight; vec4 body; vec2 waves; vec4 dynamics;
            terrainWaterLayer(t,levelBase(push.slot,push.level+1u),(fragLocalXZ-next.xy)/next.z,weight,body,waves,dynamics);
            float merged = mix(waterWeight,weight,blend);
            waterBody = mix(waterBody*waterWeight,body*weight,blend)/max(merged,.0001);
            waterWaves = mix(waterWaves*waterWeight,waves*weight,blend)/max(merged,.0001);
            waterDynamics = mix(waterDynamics*waterWeight,dynamics*weight,blend)/max(merged,.0001);
            waterWeight = merged;
        }
    }
    vec4 waterColor = vec4(0);
    if (waterWeight > .0001) {
        waterColor = shadeWater(fragWorldPos,normalize(fragNormal),distantWater(waterBody,waterWaves,waterDynamics),push.time,footprint);
        if (waterWeight > .9999) { outColor = waterColor; return; }
    }
    vec4 layer = fragSurface;
    vec3 N = normalize(fragNormal);
    float metallic = 0.0;
    vec4 rainReception = vec4(0.0);
#ifdef BINDLESS
    TerrainSurface surface = terrainSurface(t, levelBase(push.slot, push.level),
        (fragLocalXZ - level.xy) / level.z, fragLocalPos, normalize(fragLocalNormal), dx, dy);
    if (push.level + 1u < uint(t.focus.w)) {
        vec4 next = t.levels[push.level + 1u];
        vec2 fromCentre = abs((fragLocalXZ - level.xy) / level.z - 0.5 * float(RES)) / (0.5 * float(RES));
        float blend = smoothstep(RING_MORPH_START, RING_MORPH_END, max(fromCentre.x, fromCentre.y));
        if (next.w > 0.5 && blend > 0.0) {
            TerrainSurface coarse = terrainSurface(t, levelBase(push.slot, push.level + 1u),
                (fragLocalXZ - next.xy) / next.z, fragLocalPos, normalize(fragLocalNormal), dx, dy);
            surface.albedo = mix(surface.albedo, coarse.albedo, blend);
            surface.normal = normalize(mix(surface.normal, coarse.normal, blend));
            surface.roughness = mix(surface.roughness, coarse.roughness, blend);
            surface.metallic = mix(surface.metallic, coarse.metallic, blend);
            surface.rain = mix(surface.rain, coarse.rain, blend);
        }
    }
    layer = vec4(surface.albedo, surface.roughness);
    N = normalize(mat3(t.localToWorld) * surface.normal);
    metallic = surface.metallic;
    rainReception = surface.rain;
#endif
    vec3 dielectricF0 = vec3(0.04);
    float dielectricF90 = 1.0;
    applyRainSurface(fragWorldPos, normalize(fragNormal), rainDx, rainDy, rainReception,
                     N, layer.rgb, layer.a, dielectricF0, dielectricF90);
    vec3 V = normalize(lights.cameraPos.xyz - fragWorldPos);
    LightTerms lit = accumulateReflectanceOccluded(N, V, fragWorldPos, layer.rgb, metallic, layer.a,
                                        int(t.sun.w + 0.5) - 1, clamp(fragSunlight, 0.0, 1.0),
                                        dielectricF0, dielectricF90, screenAmbientOcclusion());
    outColor = mix(vec4(lit.diffuse + lit.specular, 1.0),waterColor,waterWeight);
#endif  // DEPTH_ONLY
}
