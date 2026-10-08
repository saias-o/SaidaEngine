#version 450
#extension GL_GOOGLE_include_directive : require

#ifdef MULTIVIEW
#extension GL_EXT_multiview : require
#endif

#include "web_compat.glsl"
#include "billboard.glsl"

// Camera is an array of 2 (left/right eye). Mono/desktop uses index 0; the XR
// multiview variant selects per-eye via gl_ViewIndex. One layout for both paths.
layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view[2];
    mat4 proj[2];
} cam;

PUSH_QUALIFIER PushConstants {
    mat4 model;
    vec4 params;  // x = LOD cross-fade, y = boneOffset, zw = billboard atlas
} push;

#ifdef BINDLESS
#include "material_data.glsl"
struct InstanceData {
    mat4 model;
    vec4 boundingSphere;
    uint materialIndex;
    int boneOffset;
    uint doubleSided;
    float lodFade;
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
layout(location = 10) out vec3 fragBillboardView;
layout(location = 11) flat out vec3 fragBillboardUp;

#ifdef BINDLESS
layout(location = 7) flat out uint fragMaterialIndex;
layout(location = 8) flat out uint fragDoubleSided;
layout(location = 9) flat out float fragLodFade;
#endif

void main() {
    mat4 modelMat;
    int boneOffset = -1;

#ifdef BINDLESS
    modelMat = instances[gl_InstanceIndex].model;
    fragMaterialIndex = instances[gl_InstanceIndex].materialIndex;
    fragDoubleSided = instances[gl_InstanceIndex].doubleSided;
    fragLodFade = instances[gl_InstanceIndex].lodFade;
    boneOffset = instances[gl_InstanceIndex].boneOffset;
#else
    modelMat = push.model;
    boneOffset = int(push.params.y);
#endif

    vec3 localPos = inPosition;
    vec3 localNormal = inNormal;
    vec3 localTangent = inTangent.xyz;
    vec2 localUV = inTexCoord;

    if (boneOffset >= 0) {
        vec4 row0, row1, row2;
        blendBoneRows(boneOffset, inBoneIndices, inBoneWeights, row0, row1, row2);
        localPos = skinPoint(row0, row1, row2, inPosition);
        // Strictly speaking, we should use inverse transpose for normals if scale is non-uniform,
        // but for bones, rotation/translation is the norm.
        localNormal = skinDirection(row0, row1, row2, inNormal);
        localTangent = skinDirection(row0, row1, row2, inTangent.xyz);
    }

#ifdef BINDLESS
    uint atlas = uint(materials[fragMaterialIndex].reflection.w);
    vec2 billboardAtlas = vec2(atlas % 256u, atlas / 256u);
#else
    vec2 billboardAtlas = push.params.zw;
#endif
    // Use the same primary eye for both XR views; the billboard is one surface.
    fragBillboardView = vec3(0.0, 0.0, -1.0);
    fragBillboardUp = vec3(0.0);
    if (billboardAtlas.x > 0.0) {
        fragBillboardUp = normalize(modelMat[1].xyz);
        faceBillboard(modelMat, inverse(cam.view[0])[3].xyz, billboardAtlas,
                      localPos, localNormal, localTangent, localUV, fragBillboardView);
    }
    vec4 worldPos = modelMat * vec4(localPos, 1.0);
    fragWorldPos = worldPos.xyz;
    // Normal matrix (handles non-uniform scale). Cheap enough per-vertex.
    mat3 normalMatrix = mat3(transpose(inverse(modelMat)));
    fragNormal = normalMatrix * localNormal;
    fragTangent = normalize(normalMatrix * localTangent);
    fragBitangent = cross(fragNormal, fragTangent) * inTangent.w;
    fragColor = inColor;
    fragTexCoord = localUV;
#ifdef MULTIVIEW
    int viewIndex = gl_ViewIndex;
#else
    int viewIndex = 0;
#endif
    gl_Position = cam.proj[viewIndex] * cam.view[viewIndex] * worldPos;
}
