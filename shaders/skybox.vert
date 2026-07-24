#version 450

layout(location = 0) out vec2 outUV;
layout(location = 1) out vec4 clipPos;

void main() {
    // Fullscreen triangle trick (no VBO needed)
    outUV = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(outUV * 2.0f - 1.0f, 1.0f, 1.0f); // Z=1.0 for far plane
    
    // We want the clip-space position (xy are between -1 and 1)
    clipPos = gl_Position;
}
