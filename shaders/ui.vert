#version 450
#extension GL_GOOGLE_include_directive : require

#include "web_compat.glsl"

// Fullscreen-style UI quad generated without a VBO (triangle strip).
// Indices: 0, 1, 2, 3
// Positions: (0,0), (1,0), (0,1), (1,1)

PUSH_QUALIFIER PushConstants {
    vec2 position;    // Top-left position in pixels
    vec2 size;        // Width and height in pixels
    vec2 screenSize;  // Framebuffer size in pixels
    uint textureId;   // Index bindless
    uint hasTexture;  // 1 when textured, 0 for solid color
    vec4 color;       // RGBA tint
    vec2 corners[4];  // Optional pre-projected quad corners in pixels
    uint useCorners;  // 1 for world-space projected quads
    uint _pad;
} push;

layout(location = 0) out vec2 fragUV;
layout(location = 1) out vec4 fragColor;
layout(location = 2) flat out uint fragTextureId;
layout(location = 3) flat out uint fragHasTexture;

void main() {
    // Generate a quad from gl_VertexIndex.
    vec2 quad[4] = vec2[](
        vec2(0.0, 0.0), // Top-Left
        vec2(1.0, 0.0), // Top-Right
        vec2(0.0, 1.0), // Bottom-Left
        vec2(1.0, 1.0)  // Bottom-Right
    );
    
    vec2 localPos = quad[gl_VertexIndex];
    
    // UVs
    fragUV = localPos;
    
    // Pixel position on screen.
    vec2 pixelPos = push.position + localPos * push.size;
    if (push.useCorners != 0) {
        pixelPos = push.corners[gl_VertexIndex];
    }
    
    // Orthographic projection from pixels to NDC.
    // X: 0 -> width becomes -1 -> 1.
    // Vulkan viewport coordinates use a top-left framebuffer origin here:
    // pixel y=0 must map to NDC -1, not +1.
    vec2 ndc = (pixelPos / push.screenSize) * 2.0 - 1.0;
    
    gl_Position = vec4(ndc.x, ndc.y, 0.0, 1.0);
    
    fragColor = push.color;
    fragTextureId = push.textureId;
    fragHasTexture = push.hasTexture;
}
