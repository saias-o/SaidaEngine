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
    waterWaveAt(base.xz, base.y, w, push.time, fragWorldPos, fragNormal, fragCrest);
#ifdef MULTIVIEW
    int vi = gl_ViewIndex;
#else
    int vi = 0;
#endif
    gl_Position = cam.proj[vi] * cam.view[vi] * vec4(fragWorldPos, 1.0);
}
