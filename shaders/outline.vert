#version 450
#extension GL_GOOGLE_include_directive : require

#ifdef MULTIVIEW
#extension GL_EXT_multiview : require
#endif

#include "web_compat.glsl"

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view[2];
    mat4 proj[2];
} cam;

#include "skinning.glsl"

PUSH_QUALIFIER PushConstants {
    mat4 model;
    vec4 color;
    vec4 params;  // x widthPx, y boneOffset, zw viewport size
    vec4 localCenter;
    vec4 localHalfExtent;
} push;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 6) in ivec4 inBoneIndices;
layout(location = 7) in vec4 inBoneWeights;

void main() {
#ifdef MULTIVIEW
    int viewIndex = gl_ViewIndex;
#else
    int viewIndex = 0;
#endif

    vec3 localPos = inPosition;
    vec3 boxPos = clamp((inPosition - push.localCenter.xyz) /
        max(push.localHalfExtent.xyz, vec3(1e-4)), vec3(-1.0), vec3(1.0));
    vec3 absBox = abs(boxPos);
    float high = max(max(absBox.x, absBox.y), absBox.z);
    float low = min(min(absBox.x, absBox.y), absBox.z);
    float mid = absBox.x + absBox.y + absBox.z - high - low;
    float cornerWeight = smoothstep(0.55, 0.85, mid);
    vec3 cornerNormal = length(boxPos) > 1e-5 ? normalize(boxPos) : inNormal;
    vec3 localNormal = normalize(mix(inNormal, cornerNormal, cornerWeight));

    int boneOffset = int(push.params.y);
    if (boneOffset >= 0) {
        vec4 row0, row1, row2;
        blendBoneRows(boneOffset, inBoneIndices, inBoneWeights, row0, row1, row2);
        localPos = skinPoint(row0, row1, row2, inPosition);
        localNormal = skinDirection(row0, row1, row2, inNormal);
    }

    mat3 normalMatrix = mat3(transpose(inverse(push.model)));
    vec4 worldPos = push.model * vec4(localPos, 1.0);
    vec3 worldNormal = normalize(normalMatrix * localNormal);

    vec4 viewPos = cam.view[viewIndex] * worldPos;
    vec3 viewNormal = normalize(mat3(cam.view[viewIndex]) * worldNormal);

    float viewportH = max(push.params.w, 1.0);
    float projY = max(abs(cam.proj[viewIndex][1][1]), 1e-5);
    float viewUnitsPerPixel = 2.0 * max(abs(viewPos.z), 0.05) / (projY * viewportH);
    viewPos.xyz += viewNormal * (push.params.x * viewUnitsPerPixel);

    gl_Position = cam.proj[viewIndex] * viewPos;
}
