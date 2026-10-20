#version 450

struct Vertex { vec4 positionUV; vec4 color; };
layout(set = 0, binding = 0, std430) readonly buffer Vertices { Vertex vertices[]; };
layout(push_constant) uniform Push { vec2 viewport; uint wrapUV; uint pad; } pc;
layout(location = 0) out vec2 uv;
layout(location = 1) out vec4 color;

void main() {
    Vertex v = vertices[gl_VertexIndex];
    gl_Position = vec4(v.positionUV.xy / pc.viewport * 2.0 - 1.0, 0.0, 1.0);
    uv = v.positionUV.zw;
    color = v.color;
}
