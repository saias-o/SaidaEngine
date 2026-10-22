#version 450
#extension GL_GOOGLE_include_directive : require

// AmbientOcclusionPass, second half: the raw occlusion averaged over its
// pattern's block within each surface, and shaped (ssao_denoise.glsl), into
// the AO map the scene pass's lit shaders read (screen_ambient_occlusion.glsl).

#include "ambient_occlusion_pass.glsl"

DECL_TEX2D(0, 2, 3, rawVisibility);

float sampleRawVisibility(vec2 viewportUV) {
    return textureLod(TEX2D(rawVisibility), sourceUV(viewportUV), 0.0).r;
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
    outVisibility = vec4(ssaoDenoise(inputs, fragUV));
}
