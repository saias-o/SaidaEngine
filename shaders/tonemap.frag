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
    if (centerDepth >= 0.9999) return 1.0;

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
            if (depthSample >= 0.9999) continue;

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

vec3 applyFog(vec3 color, vec2 uv) {
    if (push.fogParams.x < 0.5) return color;

    float depth = sampleDepth(uv);
    if (depth >= 0.9999) return color;

    vec3 viewPos = reconstructViewPosition(uv, depth);
    float distanceFog = max(length(viewPos) - push.fogParams.y, 0.0);
    float fogAmount = 1.0 - exp(-distanceFog * push.fogParams.z);
    return mix(color, push.fogColor.rgb, clamp(fogAmount, 0.0, 1.0));
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
