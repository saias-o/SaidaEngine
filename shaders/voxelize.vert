#version 450
#extension GL_GOOGLE_include_directive : require

#include "web_compat.glsl"

// Scene voxelization for DDGI: render once per dominant axis into a 3D albedo grid.

layout(set = 0, binding = 1) uniform VoxelUBO {
    vec4 origin;    // xyz = volume min corner (world)
    vec4 extent;    // xyz = volume size (world units)
    ivec4 res;      // xyz = voxel resolution
    mat4 axisVP[3]; // orthographic view-proj per dominant axis
} vox;

PUSH_QUALIFIER Push {
    mat4 model;
    uint axis;
} push;

layout(location = 0) in vec3 inPosition;
layout(location = 2) in vec3 inColor;
layout(location = 3) in vec2 inTexCoord;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vColor;
layout(location = 2) out vec2 vUV;

void main() {
    vec4 wp = push.model * vec4(inPosition, 1.0);
    vWorldPos = wp.xyz;
    vColor = inColor;
    vUV = inTexCoord;
    gl_Position = vox.axisVP[push.axis] * wp;
}
