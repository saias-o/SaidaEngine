#version 450
#extension GL_GOOGLE_include_directive : require

#include "web_compat.glsl"

PUSH_QUALIFIER PushConstants {
    mat4 model;
    vec4 color;
    vec4 params;
    vec4 localCenter;
    vec4 localHalfExtent;
} push;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = push.color;
}
