#version 450
#extension GL_GOOGLE_include_directive : require

// AmbientOcclusionPass, second stage, on the AO grid: the raw occlusion
// averaged over the 3 x 3 grid texels around it within each surface
// (ssao_denoise.glsl).

#include "ambient_occlusion_pass.glsl"

DECL_TEX2D(0, 2, 3, rawVisibility);

float sampleRawVisibility(ivec2 aoTexel) {
    return texelFetch(TEX2D(rawVisibility), targetGridTexel(aoTexel), 0).r;
}

#include "ssao_denoise.glsl"

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outVisibility;

void main() {
    SsaoInputs inputs;
    inputs.settings = push.settings;
    inputs.invProjection = push.invProjection;
    inputs.invProjection2 = push.invProjection2;
    inputs.invOffsets = push.invOffsets;
    inputs.pixels = max(vec2(textureSize(TEX2D(depthInput), 0)) * push.sourceRect.zw, vec2(1.0));
    inputs.aoPixels = push.aoGrid.zw;
    outVisibility = vec4(ssaoDenoise(inputs, viewportGridTexel(fragUV)));
}
