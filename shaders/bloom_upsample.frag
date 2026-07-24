#version 450
#extension GL_GOOGLE_include_directive : require

#include "web_compat.glsl"

DECL_TEX2D(0, 0, 1, sourceInput);

PUSH_QUALIFIER PushConstants {
    vec4 sourceRect;
    vec4 params; // z radius
} push;

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

void main() {
    vec2 texel = 1.0 / vec2(textureSize(TEX2D(sourceInput), 0));
    float radius = clamp(push.params.z, 0.5, 8.0) * 0.35;
    vec2 r = texel * radius;

    vec3 color = texture(TEX2D(sourceInput), fragUV).rgb * 0.36;
    color += texture(TEX2D(sourceInput), fragUV + r * vec2( 1.0,  0.0)).rgb * 0.12;
    color += texture(TEX2D(sourceInput), fragUV + r * vec2(-1.0,  0.0)).rgb * 0.12;
    color += texture(TEX2D(sourceInput), fragUV + r * vec2( 0.0,  1.0)).rgb * 0.12;
    color += texture(TEX2D(sourceInput), fragUV + r * vec2( 0.0, -1.0)).rgb * 0.12;
    color += texture(TEX2D(sourceInput), fragUV + r * vec2( 1.0,  1.0)).rgb * 0.04;
    color += texture(TEX2D(sourceInput), fragUV + r * vec2(-1.0,  1.0)).rgb * 0.04;
    color += texture(TEX2D(sourceInput), fragUV + r * vec2( 1.0, -1.0)).rgb * 0.04;
    color += texture(TEX2D(sourceInput), fragUV + r * vec2(-1.0, -1.0)).rgb * 0.04;

    outColor = vec4(color, 1.0);
}
