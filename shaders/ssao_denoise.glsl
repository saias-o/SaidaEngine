#ifndef SSAO_DENOISE_GLSL
#define SSAO_DENOISE_GLSL

// The visibility the scene pass reads (ambient_occlusion_denoise.frag): the
// raw kernel's (ssao.glsl), averaged over one AO_PATTERN x AO_PATTERN block
// around the texel -- every kernel of the pattern once, whatever the block's
// phase -- among the texels of the same surface, then shaped by the power.
// The includer first defines sampleDepth (as for ssao.glsl) and
//     float sampleRawVisibility(vec2 viewportUV)
// returning the raw kernel's result at the texel at that coordinate. Like
// ssao.glsl, this is also compiled as C++ over GLM by the CPU tests.

#include "ssao.glsl"

// Relative difference of view depth at which a neighbour stops counting: the
// average must not carry the occlusion of one surface onto another across a
// silhouette, where it would draw a halo.
const float AO_DENOISE_DEPTH_TOLERANCE = 0.05f;

float ssaoDenoise(SsaoInputs inputs, vec2 uv) {
    if (inputs.settings.x < 0.5f) return 1.0f;
    vec2 texelUV = ssaoTexelCentre(uv, inputs.pixels);
    float depth = sampleDepth(texelUV);
    if (nothingDrawn(depth)) return 1.0f;
    float centreZ = ssaoViewPosition(inputs, texelUV, depth).z;
    vec2 texel = 1.0f / inputs.pixels;

    float sum = 0.0f;
    float weights = 0.0f;
    // The block [-AO_PATTERN/2, AO_PATTERN/2 - 1]: half a texel off centre, the
    // price of covering each pattern cell exactly once.
    for (int dy = -AO_PATTERN / 2; dy < AO_PATTERN / 2; ++dy) {
        for (int dx = -AO_PATTERN / 2; dx < AO_PATTERN / 2; ++dx) {
            vec2 neighbourUV = texelUV + vec2(float(dx), float(dy)) * texel;
            if (any(lessThan(neighbourUV, vec2(0.0f))) || any(greaterThan(neighbourUV, vec2(1.0f))))
                continue;
            float neighbourDepth = sampleDepth(neighbourUV);
            if (nothingDrawn(neighbourDepth)) continue;
            float neighbourZ = ssaoViewPosition(inputs, neighbourUV, neighbourDepth).z;
            float weight = max(1.0f - abs(neighbourZ - centreZ)
                                   / (AO_DENOISE_DEPTH_TOLERANCE * abs(centreZ)), 0.0f);
            sum += weight * sampleRawVisibility(neighbourUV);
            weights += weight;
        }
    }
    // The texel itself always counts fully.
    return pow(clamp(sum / weights, 0.0f, 1.0f), inputs.settings.w);
}

#endif  // SSAO_DENOISE_GLSL
