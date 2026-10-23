#ifndef SAIDA_LIGHTING_DATA_GLSL
#define SAIDA_LIGHTING_DATA_GLSL
// Shared lighting ABI, accessible to vertex wind and fragment lighting.
const int MAX_LIGHTS = 16;
const int MAX_SHADOWS = 4;

// One unified light covers directional / point / spot (mirrors GpuLight in C++).
struct Light {
    vec4 posRange;    // xyz world position (point/spot), w = range
    vec4 colorInt;    // rgb color, w = intensity
    vec4 dirType;     // xyz travel direction (dir/spot), w = type (0 dir,1 point,2 spot)
    vec4 spotShadow;  // x = cosInner, y = cosOuter, z = shadowIndex (-1 none), w unused
};

layout(set = 0, binding = 1) uniform LightingUBO {
    vec4 ambient;       // rgb = ambient color (fallback when GI disabled)
    vec4 cameraPos;     // xyz = camera world position
    vec4 shadowParams;  // x = softness, y = numerical depth allowance
    ivec4 counts;       // x = light count, y = mode (0 realtime, 1 baked)
    Light lights[MAX_LIGHTS];
    mat4 shadowMatrices[MAX_SHADOWS];
    // DDGI irradiance volume params.
    vec4 giOrigin;      // xyz = volume min corner (world), w = enabled (0/1)
    vec4 giSpacing;     // xyz = probe spacing (world units)
    ivec4 giCounts;     // xyz = probe counts per axis, w = probesPerRow in atlas
    ivec4 giAtlas;      // x = irradiance texels/probe, y = visibility texels/probe
    vec4 environmentParams; // x enabled, y diffuse intensity, z specular intensity, w rotation
    vec4 environmentBlendParams; // x blend, y second sky rotation
    // Order-2 SH of the environment's Lambertian irradiance, already folded with
    // the basis constants and the cosine convolution (see render/EnvironmentSH).
    vec4 environmentSH[9];
    vec4 environmentBlendSH[9];
    // The terrain's hold on one directional light (SunOcclusionMap): world
    // position -> (u, v, height in the map's frame); x the light, -1 none.
    mat4 sunOcclusionToMap;
    vec4 sunOcclusion;
    vec4 rainParams; // intensity, wetness, world seconds, unused
    vec4 rainUp;     // normalized world-space up shared by both eyes
    vec4 ambientOcclusion; // x 1 when this frame's screen-space AO map holds values
    vec4 giCascadeOrigins[6];
    ivec4 giCascadeWrap[6];
    mat4 giWorldToGrid;
    ivec4 giClipmap;
} lights;

#endif
