#ifndef TONEMAP_COMMON_GLSL
#define TONEMAP_COMMON_GLSL

// What tonemap.vert and tonemap.frag share: the push block (TonemapPass.hpp),
// the depth input and how a viewport coordinate reaches the targets.

#include "web_compat.glsl"

DECL_TEX2D(0, 1, 4, depthInput);

PUSH_QUALIFIER PushConstants {
    vec4 aoParams;        // x enabled, y radius, z intensity, w power
    vec4 fogColor;        // rgb = linear HDR fog color
    vec4 fogParams;       // x enabled, y start, z density, w exposure
    vec4 bloomParams;     // x enabled, y threshold, z intensity, w radius px
    vec4 sourceRect;      // xy = source UV origin, zw = source UV size
    vec4 projectionParams; // x invP00, y invP11, z invP23, w invP33
    vec4 projectionParams2; // x invP22, y invP32
    vec4 fogRayleigh;     // rgb extinction at altitude 0 (1/m), w its scale height
    vec4 fogHeight;       // x grey scale height (0 uniform), y camera altitude, z planet radius, w layered
    vec4 fogUp;           // xyz the camera's up, view space
    vec4 outputParams;    // x: shader encodes sRGB (UNORM target only)
    vec4 flareSource;     // xy the source in viewport UV, z lens flare, w sun star
    vec4 flareRadiance;   // rgb the source's linear radiance
} push;

vec2 sourceUV(vec2 viewportUV) {
    return push.sourceRect.xy + clamp(viewportUV, vec2(0.0), vec2(1.0)) * push.sourceRect.zw;
}

// The viewport's size in pixels: the editor draws into a part of the targets.
vec2 viewportPixels() {
    return max(vec2(textureSize(TEX2D(depthInput), 0)) * push.sourceRect.zw, vec2(1.0));
}

// Depth is reversed (rhi/PipelineState.hpp): the clear value 0 is the far
// plane, and only a pixel nothing was drawn on holds it exactly. A threshold
// near it would take distant ground for sky: the conventional test this
// replaces, depth >= 0.9999, left everything past a kilometre unfogged.
bool nothingDrawn(float depth) {
    return depth <= 0.0;
}

#endif  // TONEMAP_COMMON_GLSL
