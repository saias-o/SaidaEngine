#version 450
#extension GL_GOOGLE_include_directive : require

#ifdef MULTIVIEW
#extension GL_EXT_multiview : require
#endif

#include "web_compat.glsl"
#include "billboard.glsl"
#include "lighting_data.glsl"

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

#ifndef BINDLESS
#include "material_params.glsl"
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
layout(location = 4) flat out vec3 fragEyePosition;
layout(location = 5) out vec3 fragTangent;
layout(location = 6) out vec3 fragBitangent;
layout(location = 10) out vec3 fragBillboardView;
layout(location = 11) flat out vec3 fragBillboardUp;
layout(location = 12) flat out vec3 fragBillboardNormalX;
layout(location = 13) flat out vec3 fragBillboardNormalY;
layout(location = 14) flat out vec3 fragBillboardNormalZ;

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
    fragBillboardNormalX = normalMatrix[0];
    fragBillboardNormalY = normalMatrix[1];
    fragBillboardNormalZ = normalMatrix[2];
    fragNormal = normalMatrix * localNormal;
    fragTangent = normalize(normalMatrix * localTangent);
    fragBitangent = cross(fragNormal, fragTangent) * inTangent.w;
    fragColor = inColor;
    // Root-anchored sway in the authored Y-up frame. Phase is per instance;
    // neighbouring plants share a slow gust but never move in lockstep.
#ifdef BINDLESS
    vec4 foliage = materials[fragMaterialIndex].foliage;
#else
    vec4 foliage = material.foliage;
#endif
    if (foliage.w > 0.0) {
        // Rotation/scale survive floating-origin translation. A translation
        // hash would change every plant's colour and wind phase on rebasing.
        float phase = dot(normalize(modelMat[0].xyz), vec3(3.17,1.27,2.71)) + length(modelMat[1].xyz)*1.93;
        float time = lights.rainParams.z;
        float gust = sin(time * 1.35 + phase) + 0.35 * sin(time * 3.1 + phase * 1.7);
        float weight = pow(clamp(localPos.y, 0.0, 1.2), 2.0);
        worldPos.xyz += mat3(modelMat) * vec3(gust * weight * foliage.z, 0.0,
                                              gust * weight * foliage.z * 0.43);
        fragWorldPos = worldPos.xyz;
        float tint = 0.5 + 0.5 * sin(phase * 2.17);
        fragColor *= mix(vec3(0.82, 0.84, 0.71), vec3(1.0, 1.0, 0.98), tint);
    }
    fragTexCoord = localUV;
#ifdef MULTIVIEW
    int viewIndex = gl_ViewIndex;
    // A rigid view's inverse translation is the eye's world position. Keep
    // specular, parallax and view-biased GI in the same eye as the geometry.
    fragEyePosition = -transpose(mat3(cam.view[viewIndex])) * cam.view[viewIndex][3].xyz;
#else
    int viewIndex = 0;
    fragEyePosition = lights.cameraPos.xyz;
#endif
    gl_Position = cam.proj[viewIndex] * cam.view[viewIndex] * worldPos;
}
