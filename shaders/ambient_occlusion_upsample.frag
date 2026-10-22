#version 450
#extension GL_GOOGLE_include_directive : require

// AmbientOcclusionPass, last stage, at full resolution: the denoised AO grid
// brought back to every texel of the view, depth-aware (ssao_upsample.glsl),
// into the AO map the scene pass's lit shaders read
// (screen_ambient_occlusion.glsl).

#include "ambient_occlusion_pass.glsl"

DECL_TEX2D(0, 2, 3, denoisedVisibility);

float sampleDenoisedVisibility(ivec2 aoTexel) {
    return texelFetch(TEX2D(denoisedVisibility), targetGridTexel(aoTexel), 0).r;
}

#include "ssao_upsample.glsl"

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
    outVisibility = vec4(ssaoUpsample(inputs, fragUV));
}
