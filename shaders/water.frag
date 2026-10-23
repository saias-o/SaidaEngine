#version 450
#extension GL_GOOGLE_include_directive : require
#include "lighting.glsl"
#include "water_common.glsl"
#include "water_shading.glsl"

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in float fragCrest;
layout(location = 3) in float fragDepth;
layout(location = 0) out vec4 outColor;

void main() {
    prepareShadowReceiver(fragWorldPos);
    // Evaluate derivatives before shore discard / any non-uniform branch (WGSL).
    float footprint = waterFootprint(fragWorldPos, vec3(0,1,0));
    GpuWater w = waters.items[push.index];
    float depth = w.optics.y > .5 ? waterSwashDepth(fragDepth,fragWorldPos.xz,w,push.time) : waterDepthAt(fragWorldPos.xz, w);
    // A depth-field shore keeps fragments down to the swash reach: backwash
    // foam stays stranded on the drained apron. Where none is left, discard.
    float reach = w.optics.y > .5 ? -max(w.shoreTune.z,0.0) - .01 : 0.0;
    if (int(w.shoreMode.x + .5) != 0 && depth < reach) discard;
    outColor = shadeWaterDepth(fragWorldPos, vec3(0,1,0), w, push.time, footprint, depth);
    if (depth < 0.0 && outColor.a < .004) discard;
}
