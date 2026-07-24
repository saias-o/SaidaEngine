#version 450

#include "water_common.glsl"

layout(location = 0) in vec3 fragWorldPos;
layout(location = 0) out vec4 outColor;

vec2 cartoonDirection(float degrees) {
    float angle = radians(degrees);
    return vec2(cos(angle), sin(angle));
}

float crestBand(float wave, float width) {
    float start = 1.0 - clamp(width, 0.0, 0.5) * 2.0;
    float aa = max(fwidth(wave), 0.002);
    return smoothstep(start - aa, start + aa, wave);
}

float shoreLine(float depth, float center, float width) {
    float aa = max(fwidth(depth), 0.002);
    float distanceToLine = abs(depth - center);
    return 1.0 - smoothstep(width, width + aa, distanceToLine);
}

void main() {
    GpuWater w = waters.items[push.index];
    int mode = int(w.shoreMode.x + 0.5);
    float depth = waterDepthAt(fragWorldPos.xz, w);
    if (mode != 0 && depth < 0.0) discard;

    vec2 primaryDir = cartoonDirection(w.cartoonWave.z);
    vec2 detailDir = cartoonDirection(w.cartoonDetail.z);
    float primaryPhase = dot(fragWorldPos.xz, primaryDir) * w.cartoonWave.x
        + push.time * w.cartoonWave.y;
    float detailPhase = dot(fragWorldPos.xz, detailDir) * w.cartoonDetail.x
        - push.time * w.cartoonDetail.y;

    float primary = sin(primaryPhase);
    float detail = sin(detailPhase);
    float detailStrength = clamp(w.cartoonDetail.w, 0.0, 1.0);
    float wave = (primary + detail * detailStrength) / (1.0 + detailStrength);
    float normalized = wave * 0.5 + 0.5;
    float edge = clamp(w.cartoonWave.w, 0.0, 0.98) * 0.45;
    normalized = smoothstep(edge, 1.0 - edge, normalized);

    float steps = max(2.0, floor(w.cartoonLook.x + 0.5));
    float stepped = floor(normalized * (steps - 1.0) + 0.5) / (steps - 1.0);
    float depthMix = mode == 0
        ? 1.0
        : clamp(depth / max(w.misc.w, 0.01), 0.0, 1.0);
    vec3 baseColor = mix(w.shoreColor.rgb, w.deep.rgb, depthMix);
    vec3 bandColor = mix(baseColor, w.foam.rgb, clamp(w.cartoonLook.y, 0.0, 1.0));
    vec3 color = mix(baseColor, bandColor, stepped);

    float crest = crestBand(primary, w.cartoonLook.z);
    float breakup = smoothstep(-0.35, 0.25, detail);
    crest *= mix(1.0, breakup, detailStrength);
    color = mix(color, w.foam.rgb, crest * clamp(w.cartoonLook.w, 0.0, 1.0));

    if (mode != 0) {
        vec2 shoreDir = waveDirAt(fragWorldPos.xz, w);
        vec2 tangent = vec2(-shoreDir.y, shoreDir.x);
        float along = dot(fragWorldPos.xz, tangent);
        float irregularity = sin(along * w.cartoonShore.x + detailPhase)
            * w.cartoonShore.y;
        float swash = 0.5 + 0.5 * sin(push.time * w.shoreTune.y + irregularity);
        float reach = w.shoreTune.x + w.shoreTune.z * swash;
        float lineWidth = max(w.cartoonShore.z, 0.005);
        float foam = shoreLine(depth, reach, lineWidth);
        float bands = floor(w.cartoonShore.w + 0.5);
        if (bands >= 2.0) foam = max(foam, shoreLine(depth, reach * 0.55, lineWidth) * 0.65);
        if (bands >= 3.0) foam = max(foam, shoreLine(depth, reach * 0.25, lineWidth) * 0.35);
        color = mix(color, w.foam.rgb, clamp(foam * w.shoreMode.y, 0.0, 1.0));
    }

    float alpha = mode == 0
        ? 1.0
        : smoothstep(0.0, max(w.shoreColor.w, 0.01), depth);
    outColor = vec4(color, alpha);
}
