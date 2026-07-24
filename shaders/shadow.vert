#version 450
#extension GL_GOOGLE_include_directive : require

#include "web_compat.glsl"

// Depth-only shadow pass. mvp = lightViewProj * model.
PUSH_QUALIFIER PushConstants {
    mat4 mvp;
} push;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;
layout(location = 3) in vec2 inTexCoord;

void main() {
    gl_Position = push.mvp * vec4(inPosition, 1.0);
}
