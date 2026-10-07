#version 450
#extension GL_GOOGLE_include_directive : require
#include "lighting.glsl"
#include "water_common.glsl"
#include "water_shading.glsl"

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in float fragCrest;
layout(location = 0) out vec4 outColor;

void main() {
    // Evaluate derivatives before shore discard / any non-uniform branch (WGSL).
    float footprint = waterFootprint(fragWorldPos, vec3(0,1,0));
    GpuWater w = waters.items[push.index];
    float depth = waterDepthAt(fragWorldPos.xz, w);
    if (int(w.shoreMode.x + .5) != 0 && depth < 0.0) discard;
    outColor = shadeWater(fragWorldPos, vec3(0,1,0), w, push.time, footprint);
}
