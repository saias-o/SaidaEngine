#version 450
#extension GL_GOOGLE_include_directive : require
#include "web_compat.glsl"
#include "gi_voxel_data.glsl"
PUSH_QUALIFIER Push { mat4 model; vec4 params; } push;
layout(location=0) in vec3 inPosition;
layout(location=1) in vec3 inNormal;
layout(location=2) in vec3 inColor;
layout(location=3) in vec2 inTexCoord;
layout(location=0) out vec3 vWorldPos;
layout(location=1) out vec3 vColor;
layout(location=2) out vec2 vUV;
layout(location=3) out vec3 vNormal;
void main() {
    vec4 wp=push.model*vec4(inPosition,1);
    vWorldPos=wp.xyz;vColor=inColor;vUV=inTexCoord;
    vNormal=normalize(transpose(inverse(mat3(push.model)))*inNormal);
    gl_Position=vox.axisVP[int(push.params.x)]*wp;
}
