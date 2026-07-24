#version 450
#extension GL_GOOGLE_include_directive : require

#include "web_compat.glsl"

DECL_TEX2D(0, 0, 1, sourceInput);

PUSH_QUALIFIER PushConstants {
    vec4 sourceRect; // xy = source UV origin, zw = source UV size
    vec4 params;     // x threshold, y bright-pass first level, z radius, w enabled
} push;

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

const float BLOOM_SOFT_KNEE = 0.35;

vec2 sourceUV(vec2 uv) {
    return push.sourceRect.xy + clamp(uv, vec2(0.0), vec2(1.0)) * push.sourceRect.zw;
}

vec3 brightPass(vec3 color) {
    float threshold = push.params.x;
    float luminance = dot(color, vec3(0.2126, 0.7152, 0.0722));
    float knee = max(threshold * BLOOM_SOFT_KNEE, 0.001);
    float weight = smoothstep(threshold - knee, threshold + knee, luminance);
    return color * weight;
}

void main() {
    if (push.params.w < 0.5) {
        outColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec2 texel = 1.0 / vec2(textureSize(TEX2D(sourceInput), 0));
    vec2 uv = sourceUV(fragUV);

    vec3 color = texture(TEX2D(sourceInput), uv).rgb * 0.40;
    color += texture(TEX2D(sourceInput), uv + texel * vec2( 1.0,  0.0)).rgb * 0.15;
    color += texture(TEX2D(sourceInput), uv + texel * vec2(-1.0,  0.0)).rgb * 0.15;
    color += texture(TEX2D(sourceInput), uv + texel * vec2( 0.0,  1.0)).rgb * 0.15;
    color += texture(TEX2D(sourceInput), uv + texel * vec2( 0.0, -1.0)).rgb * 0.15;

    if (push.params.y > 0.5) color = brightPass(color);
    outColor = vec4(color, 1.0);
}
