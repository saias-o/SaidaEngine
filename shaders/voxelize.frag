#version 450
#extension GL_GOOGLE_include_directive : require

#include "web_compat.glsl"

// Writes surface albedo into the 3D voxel grid; overlapping writes are harmless.

layout(set = 0, binding = 0, rgba16f) uniform writeonly image3D voxelAlbedo;

layout(set = 0, binding = 1) uniform VoxelUBO {
    vec4 origin;
    vec4 extent;
    ivec4 res;
    mat4 axisVP[3];
} vox;

// Set 1 = material (same layout as the main pass; we only read albedo + baseColor).
DECL_TEX2D(1, 0, 5, texAlbedo);
layout(set = 1, binding = 3) uniform MaterialUBO {
    vec4 baseColor;
    float metallic;
    float roughness;
    float ao;
    float _pad;
    vec4 emissive;
} material;

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vColor;
layout(location = 2) in vec2 vUV;

void main() {
    // Sampled before the bounds check: the early return is per-fragment
    // (non-uniform) control flow, and WGSL forbids implicit-derivative
    // sampling after it.
    vec3 albedo = texture(TEX2D(texAlbedo), vUV).rgb * vColor * material.baseColor.rgb;

    vec3 g = (vWorldPos - vox.origin.xyz) / vox.extent.xyz;   // [0,1] inside volume
    if (any(lessThan(g, vec3(0.0))) || any(greaterThan(g, vec3(1.0))))
        return;
    ivec3 c = clamp(ivec3(g * vec3(vox.res.xyz)), ivec3(0), vox.res.xyz - 1);

    imageStore(voxelAlbedo, c, vec4(albedo, 1.0));
}
