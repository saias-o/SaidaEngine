#version 450
#extension GL_GOOGLE_include_directive : require

// Procedural water grid with analytic normals. Multiview selects the eye via
// gl_ViewIndex, sharing the same camera UBO as the scene pass.

#ifdef MULTIVIEW
#extension GL_EXT_multiview : require
#endif

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view[2];
    mat4 proj[2];
} cam;

#include "water_common.glsl"
#include "water_wave.glsl"

layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out float fragCrest;

const int RES = 128;  // grid resolution per side; must match kGridRes in the renderer

void main() {
    GpuWater w = waters.items[push.index];

    // Procedural grid: 6 vertices per quad, RES*RES quads, two triangles each.
    int quad = gl_VertexIndex / 6;
    int corner = gl_VertexIndex % 6;
    int qx = quad % RES;
    int qz = quad / RES;
    const vec2 OFF[6] = vec2[6](
        vec2(0, 0), vec2(1, 0), vec2(1, 1),
        vec2(0, 0), vec2(1, 1), vec2(0, 1));
    vec2 cell = (vec2(float(qx), float(qz)) + OFF[corner]) / float(RES);  // [0,1]
    vec2 xz = w.area.xz + (cell - 0.5) * 2.0 * w.area.w;

#ifdef MULTIVIEW
    int vi = gl_ViewIndex;
#else
    int vi = 0;
#endif
    // The grid carries the trains its vertices, and the pixels they cover,
    // can draw; the fragment shader draws the rest.
    float distance = length(waterCameraPosition(cam.view[vi]) - vec3(xz.x, w.area.y, xz.y));
    float spacing = max(2.0 * w.area.w / float(RES), 3.0 * waterPixelFootprint(cam.proj[vi], distance));
    vec3 pos;
    waterWaveAt(xz, w.area.y, w, push.time, spacing, pos, fragNormal, fragCrest);
    fragWorldPos = pos;
    gl_Position = cam.proj[vi] * cam.view[vi] * vec4(pos, 1.0);
}
