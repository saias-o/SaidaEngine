#version 450
#extension GL_GOOGLE_include_directive : require

// AmbientOcclusionPass, last stage, at full resolution: the denoised AO grid
// brought back to every texel of the view, depth-aware (ssao_upsample.glsl),
// into the AO map's alpha the scene pass's lit shaders read
// (screen_ambient_occlusion.glsl); the GI stage's irradiance into its colour.

#include "ambient_occlusion_pass.glsl"

DECL_TEX2D(0, 2, 3, denoisedVisibility);
DECL_TEX2D(0, 4, 5, gridIrradiance);

float sampleDenoisedVisibility(ivec2 aoTexel) {
    return texelFetch(TEX2D(denoisedVisibility), targetGridTexel(aoTexel), 0).r;
}

#include "ssao_upsample.glsl"

// The GI stage's irradiance, by the same four grid texels and weights as the
// occlusion. A sliver none of them shares takes their bilinear average: unlike
// occlusion, irradiance has no neutral value.
vec3 irradianceUpsample(SsaoInputs inputs, vec2 uv) {
    vec2 texel = clamp(floor(uv * inputs.pixels), vec2(0.0), inputs.pixels - 1.0);
    vec2 texelUV = (texel + 0.5) / inputs.pixels;
    float depth = sampleDepth(texelUV);
    if (nothingDrawn(depth)) return vec3(0.0);
    float centreZ = ssaoViewPosition(inputs, texelUV, depth).z;
    vec2 grid = texel / float(AO_RESOLUTION_DIVISOR);
    ivec2 corner = ivec2(floor(grid));
    vec2 fraction = grid - floor(grid);
    ivec2 last = ivec2(inputs.aoPixels) - 1;
    vec3 sum = vec3(0.0), plain = vec3(0.0);
    float weights = 0.0, plainWeights = 0.0;
    for (int j = 0; j < 2; ++j) {
        for (int i = 0; i < 2; ++i) {
            ivec2 aoTexel = clamp(corner + ivec2(i, j), ivec2(0), last);
            vec2 aoUV = ssaoRepresentativeUV(aoTexel, inputs.pixels);
            float aoDepth = sampleDepth(aoUV);
            if (nothingDrawn(aoDepth)) continue;
            float bilinear = (i == 0 ? 1.0 - fraction.x : fraction.x) * (j == 0 ? 1.0 - fraction.y : fraction.y);
            vec3 value = texelFetch(TEX2D(gridIrradiance), targetGridTexel(aoTexel), 0).rgb;
            float weight = bilinear * ssaoDepthWeight(centreZ, ssaoViewPosition(inputs, aoUV, aoDepth).z);
            sum += weight * value;
            weights += weight;
            plain += bilinear * value;
            plainWeights += bilinear;
        }
    }
    if (weights > AO_MIN_UPSAMPLE_WEIGHT) return sum / weights;
    return plainWeights > 0.0 ? plain / plainWeights : vec3(0.0);
}

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
    vec3 irradiance = push.stages.x > 0.5 ? irradianceUpsample(inputs, fragUV) : vec3(0.0);
    outVisibility = vec4(irradiance, ssaoUpsample(inputs, fragUV));
}
