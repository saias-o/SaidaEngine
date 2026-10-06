#version 450
#extension GL_GOOGLE_include_directive : require

#ifdef MULTIVIEW
#extension GL_EXT_multiview : require
#endif

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view[2];
    mat4 proj[2];
} cam;

#include "water_common.glsl"
#include "water_wave.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out float fragCrest;

void main() {
    GpuWater w = waters.items[push.index];
    vec3 base = (w.localToWorld * vec4(inPosition, 1.0)).xyz;
#ifdef MULTIVIEW
    int vi = gl_ViewIndex;
#else
    int vi = 0;
#endif
    // A shaped surface's vertices are where its outline put them, not on a
    // grid: only the pixels they cover bound what it can carry.
    float distance = length(waterCameraPosition(cam.view[vi]) - base);
    float spacing = 3.0 * waterPixelFootprint(cam.proj[vi], distance);
    waterWaveAt(base.xz, base.y, w, push.time, spacing, fragWorldPos, fragNormal, fragCrest);
    gl_Position = cam.proj[vi] * cam.view[vi] * vec4(fragWorldPos, 1.0);
}
