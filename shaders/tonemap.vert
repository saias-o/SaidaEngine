#version 450
#extension GL_GOOGLE_include_directive : require

#include "tonemap_common.glsl"

// Fullscreen triangle generated without a vertex buffer.

layout(location = 0) out vec2 fragUV;
// How much of the lens-effect source is visible, the same for the whole draw.
layout(location = 1) flat out float flareVisibility;

// The visibility probe: a disc of FLARE_PROBE_RADIUS viewport heights around
// the source, sampled on a FLARE_PROBE_TAPS x FLARE_PROBE_TAPS grid. Measured
// here, three times a draw, rather than once for every pixel the effects
// cover. A little wider than the Sun's own disc so that a branch or a roof
// edge crossing it dims the effects gradually instead of switching them.
const int FLARE_PROBE_TAPS = 8;
const float FLARE_PROBE_RADIUS = 0.012;

// Share of the probe where nothing was drawn: the sky the source sits in. A
// tap outside the viewport sees nothing, so the effects fade as the source
// leaves the screen.
float measureFlareVisibility() {
    if (push.flareSource.z < 0.5 && push.flareSource.w < 0.5) return 0.0;
    vec2 pixels = viewportPixels();
    vec2 radius = vec2(FLARE_PROBE_RADIUS * pixels.y) / pixels;
    float seen = 0.0;
    float taps = 0.0;
    for (int y = 0; y < FLARE_PROBE_TAPS; ++y) {
        for (int x = 0; x < FLARE_PROBE_TAPS; ++x) {
            vec2 offset = (vec2(float(x), float(y)) + 0.5) * (2.0 / float(FLARE_PROBE_TAPS)) - 1.0;
            if (dot(offset, offset) > 1.0) continue;
            taps += 1.0;
            vec2 uv = push.flareSource.xy + offset * radius;
            if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) continue;
            if (nothingDrawn(textureLod(TEX2D(depthInput), sourceUV(uv), 0.0).r)) seen += 1.0;
        }
    }
    return seen / max(taps, 1.0);
}

void main() {
    vec2 pos;
    if (gl_VertexIndex == 0) pos = vec2(0.0, 0.0);
    else if (gl_VertexIndex == 1) pos = vec2(0.0, 2.0);
    else pos = vec2(2.0, 0.0);

    // Map to clip space [-1,1] and compute matching UV.
    // Vulkan Y-flip: NDC Y goes top-to-bottom, UV Y should go 0 (top) to 1 (bottom).
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
    fragUV = vec2(pos.x, pos.y);
    flareVisibility = measureFlareVisibility();
}
