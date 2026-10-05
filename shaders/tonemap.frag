#version 450
#extension GL_GOOGLE_include_directive : require

#include "web_compat.glsl"

// HDR -> LDR tonemap pass with AO, fog and bloom.

DECL_TEX2D(0, 0, 3, hdrInput);
DECL_TEX2D(0, 1, 4, depthInput);
DECL_TEX2D(0, 2, 5, bloomInput);

PUSH_QUALIFIER PushConstants {
    mat4 invProjection;
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
} push;

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

const int AO_DIR_COUNT = 8;
const int AO_STEP_COUNT = 2;
const float AO_BIAS = 0.03;
const float AO_NORMAL_EPSILON = 1e-4;
vec2 sourceUV(vec2 viewportUV) {
    return push.sourceRect.xy + clamp(viewportUV, vec2(0.0), vec2(1.0)) * push.sourceRect.zw;
}

vec4 sampleHdr(vec2 viewportUV) {
    return texture(TEX2D(hdrInput), sourceUV(viewportUV));
}

float sampleDepth(vec2 viewportUV) {
    // Explicit LOD: depth has no mips (identical result) and the AO/fog loops
    // call this from non-uniform control flow, which WGSL forbids for the
    // implicit-derivative texture() variant.
    return textureLod(TEX2D(depthInput), sourceUV(viewportUV), 0.0).r;
}

vec2 viewportTexel() {
    vec2 sourcePixels = vec2(textureSize(TEX2D(hdrInput), 0)) * max(push.sourceRect.zw, vec2(1e-6));
    return 1.0 / max(sourcePixels, vec2(1.0));
}

// ACES filmic tonemap, Krzysztof Narkowicz 2015 fit.
vec3 acesFilmic(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

// Depth is reversed (rhi/PipelineState.hpp): the clear value 0 is the far
// plane, and only a pixel nothing was drawn on holds it exactly. A threshold
// near it would take distant ground for sky: the conventional test this
// replaces, depth >= 0.9999, left everything past a kilometre unfogged.
bool nothingDrawn(float depth) {
    return depth <= 0.0;
}

vec3 reconstructViewPosition(vec2 uv, float depth) {
    vec2 clipXY = uv * 2.0 - 1.0;
    float viewW = push.projectionParams.z * depth + push.projectionParams.w;
    vec3 view = vec3(clipXY.x * push.projectionParams.x,
                     clipXY.y * push.projectionParams.y,
                     push.projectionParams2.x * depth + push.projectionParams2.y);
    return view / max(viewW, 1e-6);
}

vec3 viewNormalFromDepth(vec2 uv, vec3 center) {
    vec2 texel = viewportTexel();
    float depthX = sampleDepth(uv + vec2(texel.x, 0.0));
    float depthY = sampleDepth(uv + vec2(0.0, texel.y));
    vec3 px = reconstructViewPosition(uv + vec2(texel.x, 0.0), depthX);
    vec3 py = reconstructViewPosition(uv + vec2(0.0, texel.y), depthY);
    vec3 n = normalize(cross(py - center, px - center));
    vec3 viewDir = normalize(-center);
    if (dot(n, viewDir) < 0.0) n = -n;
    if (length(n) < AO_NORMAL_EPSILON) n = vec3(0.0, 0.0, 1.0);
    return n;
}

float ambientOcclusion(vec2 uv) {
    if (push.aoParams.x < 0.5) return 1.0;

    float centerDepth = sampleDepth(uv);
    if (nothingDrawn(centerDepth)) return 1.0;

    vec3 center = reconstructViewPosition(uv, centerDepth);
    vec3 normal = viewNormalFromDepth(uv, center);
    float viewDepth = max(abs(center.z), 0.05);
    vec2 uvRadius = vec2(0.5 * push.aoParams.y / viewDepth);

    float occlusion = 0.0;
    float samples = 0.0;
    for (int dirIndex = 0; dirIndex < AO_DIR_COUNT; ++dirIndex) {
        float angle = (float(dirIndex) + 0.5) * 6.28318530718 / float(AO_DIR_COUNT);
        vec2 dir = vec2(cos(angle), sin(angle));

        for (int stepIndex = 1; stepIndex <= AO_STEP_COUNT; ++stepIndex) {
            float stepT = float(stepIndex) / float(AO_STEP_COUNT);
            vec2 sampleUV = uv + dir * uvRadius * stepT;
            if (any(lessThan(sampleUV, vec2(0.0))) || any(greaterThan(sampleUV, vec2(1.0))))
                continue;

            float depthSample = sampleDepth(sampleUV);
            if (nothingDrawn(depthSample)) continue;

            vec3 samplePos = reconstructViewPosition(sampleUV, depthSample);
            vec3 delta = samplePos - center;
            float dist = length(delta);
            if (dist <= AO_NORMAL_EPSILON || dist > push.aoParams.y) continue;

            float normalTerm = max(dot(normal, delta / dist) - AO_BIAS, 0.0);
            float rangeTerm = 1.0 - smoothstep(push.aoParams.y * 0.25, push.aoParams.y, dist);
            occlusion += normalTerm * rangeTerm;
            samples += 1.0;
        }
    }

    if (samples <= 0.0) return 1.0;
    float ao = 1.0 - push.aoParams.z * (occlusion / samples);
    return pow(clamp(ao, 0.0, 1.0), push.aoParams.w);
}

vec3 bloom(vec2 uv) {
    if (push.bloomParams.x < 0.5 || push.bloomParams.z <= 0.0)
        return vec3(0.0);
    return texture(TEX2D(bloomInput), clamp(uv, vec2(0.0), vec2(1.0))).rgb * push.bloomParams.z;
}

// Altitude of the view-space point `v`: over the sphere when there is one
// (written so that no two numbers near the planet's radius are subtracted),
// along the up vector otherwise.
float fogAltitude(vec3 v) {
    float h0 = push.fogHeight.y;
    float s = dot(v, push.fogUp.xyz);
    float radius = push.fogHeight.z;
    if (radius <= 0.0) return h0 + s;
    float rh = radius + h0;
    float vv = dot(v, v);
    float lifted = 2.0 * radius * h0 + h0 * h0 + 2.0 * rh * s + vv;  // |c + v|^2 - R^2
    return lifted / (sqrt(max(rh * rh + 2.0 * rh * s + vv, 0.0)) + radius);
}

// Points of a ray the layered air's density is averaged over, and the lowest
// altitude it is taken at (a curved world's far edge drops below the datum).
const int FOG_SAMPLES = 8;
const float FOG_LOWEST_ALTITUDE = -1000.0;

// Optical depth per channel from the camera to `viewPos`, past `start`.
// Uniform grey fog unless the air is layered; then the grey and the Rayleigh
// densities are averaged over eight points of the fogged segment.
vec3 fogDepth(vec3 viewPos, float dist) {
    float start = push.fogParams.y;
    float fogged = max(dist - start, 0.0);
    if (push.fogHeight.w < 0.5) return vec3(fogged * push.fogParams.z);
    float greyHeight = push.fogHeight.x;
    float rayleighHeight = push.fogRayleigh.w;
    float from = dist > 0.0 ? min(start / dist, 1.0) : 1.0;
    float grey = 0.0, air = 0.0;
    for (int i = 0; i < FOG_SAMPLES; ++i) {
        float f = from + (1.0 - from) * (float(i) + 0.5) / float(FOG_SAMPLES);
        // Below the datum the air is denser still, but not without bound.
        float a = max(fogAltitude(viewPos * f), FOG_LOWEST_ALTITUDE);
        grey += greyHeight > 0.0 ? exp(-a / greyHeight) : 1.0;
        air += exp(-a / rayleighHeight);
    }
    grey /= float(FOG_SAMPLES);
    air /= float(FOG_SAMPLES);
    return fogged * (push.fogParams.z * grey + push.fogRayleigh.rgb * air);
}

vec3 applyFog(vec3 color, vec2 uv) {
    if (push.fogParams.x < 0.5) return color;

    float depth = sampleDepth(uv);
    if (nothingDrawn(depth)) return color;

    vec3 viewPos = reconstructViewPosition(uv, depth);
    // Per channel: clear air takes blue several times faster than red, and
    // what it takes is replaced by the colour of the air itself.
    vec3 transmitted = exp(-fogDepth(viewPos, length(viewPos)));
    return color * transmitted + push.fogColor.rgb * (1.0 - transmitted);
}

void main() {
    vec4 hdr4 = sampleHdr(fragUV);
    vec3 hdr = hdr4.rgb;

    hdr *= ambientOcclusion(fragUV);
    hdr = applyFog(hdr, fragUV);
    hdr += bloom(fragUV);

    hdr *= push.fogParams.w;

    vec3 mapped = acesFilmic(hdr);

    // Approximate linear -> sRGB conversion.
    vec3 srgb = pow(mapped, vec3(1.0 / 2.2));

    // Preserve scene coverage in alpha so XR passthrough composites correctly
    // (transparent where nothing was drawn). Ignored by the desktop swapchain.
    outColor = vec4(srgb, hdr4.a);
}
