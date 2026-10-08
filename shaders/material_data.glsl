// Bindless material ABI shared with graphics/BindlessTables.cpp.
#extension GL_EXT_nonuniform_qualifier : require

#ifndef MATERIAL_SET
#define MATERIAL_SET 1
#endif

struct MaterialData {
    vec4 baseColor;
    float metallic;
    float roughness;
    float ao;
    uint albedoTexIdx;
    uint normalTexIdx;
    uint metallicRoughnessTexIdx;
    uint emissiveTexIdx;
    uint materialType;
    float alphaCutoff;
    float normalStrength;
    uint heightTexIdx;
    float parallaxDepth;  // texture-coordinate units; 0 draws the surface flat
    vec4 emissive;
    vec4 variation;  // MaterialDesc::variation: warp, macroScale, macroAlbedo, macroNormal
    vec4 reflection; // x environment reflection, yz specular indices, w billboard columns + 256*rows
    vec4 specular; // rgb dielectric F0, a dielectric lobe strength
};

layout(set = MATERIAL_SET, binding = 0) uniform sampler2D globalTextures[8192];
layout(std430, set = MATERIAL_SET, binding = 1) readonly buffer MaterialBuffer {
    MaterialData materials[];
};
