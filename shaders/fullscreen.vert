#version 450

// One triangle covering the viewport, without a vertex buffer: fragUV runs
// from (0, 0) at the top left to (1, 1) at the bottom right (Vulkan's Y-down
// clip space), the convention depth_reconstruction.glsl inverts.

layout(location = 0) out vec2 fragUV;

void main() {
    vec2 pos = vec2(float(gl_VertexIndex == 2) * 2.0, float(gl_VertexIndex == 1) * 2.0);
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
    fragUV = pos;
}
