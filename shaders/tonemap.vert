#version 450

// Fullscreen triangle generated without a vertex buffer.

layout(location = 0) out vec2 fragUV;

void main() {
    vec2 pos;
    if (gl_VertexIndex == 0) pos = vec2(0.0, 0.0);
    else if (gl_VertexIndex == 1) pos = vec2(0.0, 2.0);
    else pos = vec2(2.0, 0.0);

    // Map to clip space [-1,1] and compute matching UV.
    // Vulkan Y-flip: NDC Y goes top-to-bottom, UV Y should go 0 (top) to 1 (bottom).
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
    fragUV = vec2(pos.x, pos.y);
}
