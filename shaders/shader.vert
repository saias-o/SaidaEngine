#version 450
#extension GL_GOOGLE_include_directive : require

#ifdef MULTIVIEW
#extension GL_EXT_multiview : require
#endif

#include "web_compat.glsl"

// Camera is an array of 2 (left/right eye). Mono/desktop uses index 0; the XR
// multiview variant selects per-eye via gl_ViewIndex. One layout for both paths.
layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view[2];
    mat4 proj[2];
} cam;

PUSH_QUALIFIER PushConstants {
    mat4 model;
    vec4 params;  // y = boneOffset
} push;

#ifdef BINDLESS
struct InstanceData {
    mat4 model;
    vec4 boundingSphere;
    uint materialIndex;
    int boneOffset;
    uint pad2, pad3;
};

layout(std140, set = 2, binding = 0) readonly buffer InstanceBuffer {
    InstanceData instances[];
};
#endif

#include "skinning.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;
layout(location = 3) in vec2 inTexCoord;
layout(location = 5) in vec4 inTangent;
layout(location = 6) in ivec4 inBoneIndices;
layout(location = 7) in vec4 inBoneWeights;

layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec3 fragColor;
layout(location = 3) out vec2 fragTexCoord;
layout(location = 5) out vec3 fragTangent;
layout(location = 6) out vec3 fragBitangent;

#ifdef BINDLESS
layout(location = 7) flat out uint fragMaterialIndex;
#endif

void main() {
    mat4 modelMat;
    int boneOffset = -1;

#ifdef BINDLESS
    modelMat = instances[gl_InstanceIndex].model;
    fragMaterialIndex = instances[gl_InstanceIndex].materialIndex;
    boneOffset = instances[gl_InstanceIndex].boneOffset;
#else
    modelMat = push.model;
    boneOffset = int(push.params.y);
#endif

    vec3 localPos = inPosition;
    vec3 localNormal = inNormal;
    vec3 localTangent = inTangent.xyz;

    if (boneOffset >= 0) {
        vec4 row0, row1, row2;
        blendBoneRows(boneOffset, inBoneIndices, inBoneWeights, row0, row1, row2);
        localPos = skinPoint(row0, row1, row2, inPosition);
        // Strictly speaking, we should use inverse transpose for normals if scale is non-uniform,
        // but for bones, rotation/translation is the norm.
        localNormal = skinDirection(row0, row1, row2, inNormal);
        localTangent = skinDirection(row0, row1, row2, inTangent.xyz);
    }

    vec4 worldPos = modelMat * vec4(localPos, 1.0);
    fragWorldPos = worldPos.xyz;
    // Normal matrix (handles non-uniform scale). Cheap enough per-vertex.
    mat3 normalMatrix = mat3(transpose(inverse(modelMat)));
    fragNormal = normalMatrix * localNormal;
    fragTangent = normalize(normalMatrix * localTangent);
    fragBitangent = cross(fragNormal, fragTangent) * inTangent.w;
    fragColor = inColor;
    fragTexCoord = inTexCoord;
#ifdef MULTIVIEW
    int viewIndex = gl_ViewIndex;
#else
    int viewIndex = 0;
#endif
    gl_Position = cam.proj[viewIndex] * cam.view[viewIndex] * worldPos;
}
