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

// Concentrate a large ocean patch around the viewer without another mesh,
// allocation or draw. The outermost vertices still lie on the authored bounds.
vec2 oceanGrid(vec2 cell, vec2 focus, vec2 lo, vec2 hi, float nearScale) {
    vec2 u = cell * 2.0 - 1.0;
    vec2 reach = mix(focus-lo,hi-focus,step(vec2(0),u));
    return focus + sign(u)*nearScale*(pow(vec2(1)+reach/nearScale,abs(u))-1.0);
}

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
    vec3 camera = waterCameraPosition(cam.view[vi]);
    float meshSpacing = 2.0*w.area.w/float(RES);
    if (w.area.w > 64.0) {
        vec2 lo = w.area.xz-w.area.w, hi = w.area.xz+w.area.w;
        vec2 focus = clamp(camera.xz,lo+w.area.w*.01,hi-w.area.w*.01);
        float nearScale = max(2.0,abs(camera.y-w.area.y)*.2);
        xz = oceanGrid(cell,focus,lo,hi,nearScale);
        vec2 a = oceanGrid(clamp(cell-1.0/float(RES),0.0,1.0),focus,lo,hi,nearScale);
        vec2 b = oceanGrid(clamp(cell+1.0/float(RES),0.0,1.0),focus,lo,hi,nearScale);
        meshSpacing = max(max(abs(xz.x-a.x),abs(b.x-xz.x)),max(abs(xz.y-a.y),abs(b.y-xz.y)));
    }
    float distance = length(camera - vec3(xz.x,w.area.y,xz.y));
    float spacing = max(meshSpacing, 3.0 * waterPixelFootprint(cam.proj[vi], distance,w.shoreMode.w));
    vec3 pos;
    waterWaveAt(xz, w.area.y, w, push.time, spacing, pos, fragNormal, fragCrest);
    // Independently sized/neighbouring patches need identical boundary heights
    // even when their sampling densities differ. The shading stays continuous.
    vec2 toEdge = vec2(w.area.w)-abs(xz-w.area.xz);
    float boundary = smoothstep(0.0,max(w.waveA.y*2.0,.01),min(toEdge.x,toEdge.y));
    pos = mix(vec3(xz.x,w.area.y,xz.y),pos,boundary);
    fragWorldPos = pos;
    gl_Position = cam.proj[vi] * cam.view[vi] * vec4(pos, 1.0);
}
