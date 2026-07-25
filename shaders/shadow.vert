#version 450
#extension GL_GOOGLE_include_directive : require

#include "web_compat.glsl"

// Depth-only shadow pass. mvp = lightViewProj * model.
PUSH_QUALIFIER PushConstants {
    mat4 mvp;
    vec4 params;  // y = boneOffset, -1 when the caster is not skinned
} push;

#include "skinning.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;
layout(location = 3) in vec2 inTexCoord;
layout(location = 6) in ivec4 inBoneIndices;
layout(location = 7) in vec4 inBoneWeights;

void main() {
    // A skinned caster has to be posed here too. Transforming the bind pose --
    // which is what this pass used to do -- casts the shadow of a character
    // standing still in his rest pose, wherever the mesh's origin happens to be.
    vec3 localPos = inPosition;
    int boneOffset = int(push.params.y);
    if (boneOffset >= 0) {
        vec4 row0, row1, row2;
        blendBoneRows(boneOffset, inBoneIndices, inBoneWeights, row0, row1, row2);
        localPos = skinPoint(row0, row1, row2, localPos);
    }
    gl_Position = push.mvp * vec4(localPos, 1.0);
}
