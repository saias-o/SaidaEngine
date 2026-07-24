#version 450

#ifdef MULTIVIEW
#extension GL_EXT_multiview : require
#endif

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view[2];
    mat4 proj[2];
} cam;

#include "water_common.glsl"

layout(location = 0) out vec3 fragWorldPos;

void main() {
    GpuWater w = waters.items[push.index];
    const vec2 corners[6] = vec2[6](
        vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
        vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));

    vec2 xz = w.area.xz + corners[gl_VertexIndex] * w.area.w;
    vec3 position = vec3(xz.x, w.area.y, xz.y);
    fragWorldPos = position;

#ifdef MULTIVIEW
    int viewIndex = gl_ViewIndex;
#else
    int viewIndex = 0;
#endif
    gl_Position = cam.proj[viewIndex] * cam.view[viewIndex] * vec4(position, 1.0);
}
