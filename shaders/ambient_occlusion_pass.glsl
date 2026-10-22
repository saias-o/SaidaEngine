#ifndef AMBIENT_OCCLUSION_PASS_GLSL
#define AMBIENT_OCCLUSION_PASS_GLSL

// What AmbientOcclusionPass's three fullscreen shaders share: the push block
// (AmbientOcclusionPass::PushConstants), the view's depth, how a viewport
// coordinate reaches it, and the AO grid the first two stages draw on.

#include "web_compat.glsl"

DECL_TEX2D(0, 0, 1, depthInput);

PUSH_QUALIFIER PushConstants {
    vec4 settings;        // x enabled, y radius (m), z intensity, w power
    vec4 invProjection;   // x invP00, y invP11, z invP23, w invP33
    vec4 invProjection2;  // x invP22, y invP32
    vec4 invOffsets;      // xy invP30/31, zw invP20/21 (off-centre eye frusta)
    vec4 sourceRect;      // xy the viewport's origin, zw its size, in UV of the depth target
    vec4 aoGrid;          // xy the viewport's origin in AO-grid texels of the targets, zw its size
} push;

vec2 sourceUV(vec2 viewportUV) {
    return push.sourceRect.xy + clamp(viewportUV, vec2(0.0), vec2(1.0)) * push.sourceRect.zw;
}

float sampleDepth(vec2 viewportUV) {
    // Explicit LOD: ssao.glsl samples from non-uniform control flow, where
    // WGSL forbids the implicit-derivative texture().
    return textureLod(TEX2D(depthInput), sourceUV(viewportUV), 0.0).r;
}

// The AO-grid texel of the viewport a fullscreen fragment of a grid stage covers.
ivec2 viewportGridTexel(vec2 viewportUV) {
    return ivec2(floor(viewportUV * push.aoGrid.zw));
}

// Where an AO-grid texel of the viewport sits in a grid-sized target.
ivec2 targetGridTexel(ivec2 aoTexel) {
    return aoTexel + ivec2(push.aoGrid.xy);
}

#endif  // AMBIENT_OCCLUSION_PASS_GLSL
