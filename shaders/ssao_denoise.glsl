#ifndef SSAO_DENOISE_GLSL
#define SSAO_DENOISE_GLSL

// The second stage on the AO grid (ambient_occlusion_denoise.frag): the raw
// kernel's visibility (ssao.glsl), averaged over the 3 x 3 grid texels around
// the texel with the tent [1 2 1] each way -- centred, so the occlusion does
// not shift, and giving each cell of the 2 x 2 pattern the same total weight,
// 4 of 16 -- among the texels of the same surface. Still linear:
// ssao_upsample.glsl applies the power. The includer first defines
// sampleDepth (as for ssao.glsl) and
//     float sampleRawVisibility(ivec2 aoTexel)
// returning the kernel stage's result at an AO-grid texel of the viewport.
// Like ssao.glsl, this is also compiled as C++ over GLM by the CPU tests.

#include "ssao.glsl"

float ssaoDenoise(SsaoInputs inputs, ivec2 aoTexel) {
    if (inputs.settings.x < 0.5f) return 1.0f;
    vec2 uv = ssaoRepresentativeUV(aoTexel, inputs.pixels);
    float depth = sampleDepth(uv);
    if (nothingDrawn(depth)) return 1.0f;
    float centreZ = ssaoViewPosition(inputs, uv, depth).z;
    ivec2 gridSize = ivec2(inputs.aoPixels);

    float sum = 0.0f;
    float weights = 0.0f;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            ivec2 neighbour = aoTexel + ivec2(dx, dy);
            if (any(lessThan(neighbour, ivec2(0))) || any(greaterThanEqual(neighbour, gridSize)))
                continue;
            vec2 neighbourUV = ssaoRepresentativeUV(neighbour, inputs.pixels);
            float neighbourDepth = sampleDepth(neighbourUV);
            if (nothingDrawn(neighbourDepth)) continue;
            float tent = float((2 - abs(dx)) * (2 - abs(dy)));
            float weight = tent * ssaoDepthWeight(centreZ,
                                                  ssaoViewPosition(inputs, neighbourUV, neighbourDepth).z);
            sum += weight * sampleRawVisibility(neighbour);
            weights += weight;
        }
    }
    // The texel itself always counts fully.
    return clamp(sum / weights, 0.0f, 1.0f);
}

#endif  // SSAO_DENOISE_GLSL
