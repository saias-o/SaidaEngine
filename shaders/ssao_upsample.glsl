#ifndef SSAO_UPSAMPLE_GLSL
#define SSAO_UPSAMPLE_GLSL

// The last stage (ambient_occlusion_upsample.frag): the visibility the scene
// pass reads at every texel of the view, from the denoised AO grid
// (ssao_denoise.glsl), shaped by the power. Each AO-grid texel stands at the
// full-resolution texel ssaoRepresentativeUV names; a texel of the view takes
// the four nearest by their bilinear weight times the likeness of their depth
// to its own, so that a silhouette keeps the occlusion of its own surface on
// each side. When none of the four is of its surface -- a sliver thinner than
// the grid -- nothing is known of its occlusion and it is unoccluded, as a
// rejected sample is: the nearest of the four would lend it the occlusion of
// what stands behind, a dark rim. The includer first defines sampleDepth (as
// for ssao.glsl) and
//     float sampleDenoisedVisibility(ivec2 aoTexel)
// returning the denoise stage's result at an AO-grid texel of the viewport.
// Like ssao.glsl, this is also compiled as C++ over GLM by the CPU tests.

#include "ssao.glsl"

// The total weight under which the four grid texels say nothing of this one.
const float AO_MIN_UPSAMPLE_WEIGHT = 1e-4f;

float ssaoUpsample(SsaoInputs inputs, vec2 uv) {
    if (inputs.settings.x < 0.5f) return 1.0f;
    vec2 texel = clamp(floor(uv * inputs.pixels), vec2(0.0f), inputs.pixels - 1.0f);
    vec2 texelUV = (texel + 0.5f) / inputs.pixels;
    float depth = sampleDepth(texelUV);
    if (nothingDrawn(depth)) return 1.0f;
    float centreZ = ssaoViewPosition(inputs, texelUV, depth).z;

    // This texel on the AO grid, whose texel i stands at full-resolution
    // texel AO_RESOLUTION_DIVISOR * i: an exact quotient of integers, so that
    // a texel standing for one takes its value alone.
    vec2 grid = texel / float(AO_RESOLUTION_DIVISOR);
    ivec2 corner = ivec2(floor(grid));
    vec2 fraction = grid - floor(grid);
    ivec2 last = ivec2(inputs.aoPixels) - 1;

    float sum = 0.0f;
    float weights = 0.0f;
    for (int j = 0; j < 2; ++j) {
        for (int i = 0; i < 2; ++i) {
            ivec2 aoTexel = clamp(corner + ivec2(i, j), ivec2(0), last);
            vec2 aoUV = ssaoRepresentativeUV(aoTexel, inputs.pixels);
            float aoDepth = sampleDepth(aoUV);
            if (nothingDrawn(aoDepth)) continue;
            float bilinear = (i == 0 ? 1.0f - fraction.x : fraction.x)
                           * (j == 0 ? 1.0f - fraction.y : fraction.y);
            float weight = bilinear * ssaoDepthWeight(centreZ, ssaoViewPosition(inputs, aoUV, aoDepth).z);
            sum += weight * sampleDenoisedVisibility(aoTexel);
            weights += weight;
        }
    }
    float visibility = weights > AO_MIN_UPSAMPLE_WEIGHT ? sum / weights : 1.0f;
    return pow(clamp(visibility, 0.0f, 1.0f), inputs.settings.w);
}

#endif  // SSAO_UPSAMPLE_GLSL
