#version 450
#extension GL_GOOGLE_include_directive : require
#include "web_compat.glsl"
#include "gi_voxel_storage.glsl"
PUSH_QUALIFIER Push { mat4 model; vec4 params; } push;
DECL_TEX2D(1,0,5,texAlbedo);
DECL_TEX2D(1,4,8,texEmissive);
layout(set=1,binding=3) uniform MaterialUBO {
    vec4 baseColor;float metallic;float roughness;float ao;float alphaCutoff;vec4 emissive;
} material;
layout(location=0) in vec3 vWorldPos;
layout(location=1) in vec3 vColor;
layout(location=2) in vec2 vUV;
layout(location=3) in vec3 vNormal;
void main() {
    vec4 base=texture(TEX2D(texAlbedo),vUV)*material.baseColor;
    vec3 emission=texture(TEX2D(texEmissive),vUV).rgb*material.emissive.rgb;
    if(base.a<material.alphaCutoff) discard;
    vec3 g=(mat3(vox.worldToGrid)*(vWorldPos-vox.origin.xyz))/vox.extent.xyz;
    if(any(lessThan(g,vec3(0))) || any(greaterThanEqual(g,vec3(1)))) return;
    ivec3 c=(ivec3(floor(g*vec3(vox.res.xyz)+vec3(.0001)))+vox.wrap.xyz)&(vox.res.xyz-1);
    ivec3 bricks=vox.res.xyz/8;ivec3 b=c/8;
    int job=b.x+bricks.x*(b.y+bricks.y*(b.z+bricks.z*vox.res.w));
    if(jobs[job]==0u) return;
    c.z+=vox.res.w*vox.res.z;
    // Sqrt encoding retains dark surface colors in the compact UNORM volume.
    imageStore(voxelAlbedo,c,vec4(sqrt(clamp(base.rgb*vColor*(1.0-material.metallic),vec3(0),vec3(1))),push.params.y));
    imageStore(voxelNormal,c,vec4(normalize(vNormal)*.5+.5,push.params.z));
    imageStore(voxelEmission,c,vec4(emission,1));
}
