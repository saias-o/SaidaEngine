#version 450
#extension GL_GOOGLE_include_directive : require

// AmbientOcclusionPass, first stage, on the AO grid: the raw screen-space
// ambient occlusion of one view, each grid texel from its own turn of the
// kernel (ssao.glsl). The denoise and upsample stages make it what the scene
// pass reads.

#include "ambient_occlusion_pass.glsl"
#include "ssao.glsl"

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
    outVisibility = vec4(ssaoRawVisibility(inputs, viewportGridTexel(fragUV)));
}
