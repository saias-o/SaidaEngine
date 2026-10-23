#ifndef DEPTH_RECONSTRUCTION_GLSL
#define DEPTH_RECONSTRUCTION_GLSL

// What a reversed depth sample means: nothing drawn, or a view-space point.
// Shared by ambient occlusion, fog and lens-flare visibility, and compiled as
// C++ over GLM by the CPU tests, so it keeps to what both languages accept:
// float literals carry `f`, no swizzles, no out parameters.

// Depth is reversed (rhi/PipelineState.hpp): the clear value 0 is the far
// plane, and only a pixel nothing was drawn on holds it exactly. A threshold
// near it would take distant ground for sky: the conventional test this
// replaces, depth >= 0.9999, left everything past a kilometre unfogged.
bool nothingDrawn(float depth) {
    return depth <= 0.0f;
}

// The view-space point at viewport `uv` and reversed `depth`, through the
// inverse projection InverseProjection.hpp packs: invProjection = (invP00, invP11,
// invP23, invP33), invProjection2.xy = (invP22, invP32) and invOffsets =
// (invP30, invP31, invP20, invP21), the terms of an off-centre (OpenXR) eye.
vec3 viewPositionFromDepth(vec2 uv, float depth, vec4 invProjection, vec4 invProjection2,
                           vec4 invOffsets) {
    vec2 clipXY = uv * 2.0f - 1.0f;
    vec2 viewXY = clipXY * vec2(invProjection.x, invProjection.y)
                + vec2(invOffsets.x, invOffsets.y) + vec2(invOffsets.z, invOffsets.w) * depth;
    float viewZ = invProjection2.x * depth + invProjection2.y;
    float viewW = invProjection.z * depth + invProjection.w;
    return vec3(viewXY, viewZ) / max(viewW, 1e-6f);
}

#endif  // DEPTH_RECONSTRUCTION_GLSL
