// Classic/Web material ABI; mirrors graphics/Material.cpp.
layout(set = 1, binding = 3) uniform MaterialUBO {
    vec4 baseColor;
    float metallic;
    float roughness;
    float ao;
    float alphaCutoff;
    vec4 emissive;
    vec4 variation;
    vec4 detail;
    vec4 specular;
    vec4 rain;
    vec4 foliage;
} material;
