#version 450

layout(set = 0, binding = 1) uniform sampler2D image;
layout(push_constant) uniform Push { vec2 viewport; uint wrapUV; uint pad; } pc;
layout(location = 0) in vec2 uv;
layout(location = 1) in vec4 color;
layout(location = 0) out vec4 outColor;

// Match the CPU reference's texel-centre and repeated-decorator addressing.
void main() {
    ivec2 size = textureSize(image, 0);
    vec2 p = pc.wrapUV != 0 ? fract(uv) * vec2(size) : clamp(uv, 0.0, 1.0) * vec2(size - 1);
    ivec2 a = ivec2(floor(p));
    ivec2 b = pc.wrapUV != 0 ? (a + 1) % size : min(a + 1, size - 1);
    vec2 f = fract(p);
    vec4 t = mix(mix(texelFetch(image, a, 0), texelFetch(image, ivec2(b.x, a.y), 0), f.x),
                 mix(texelFetch(image, ivec2(a.x, b.y), 0), texelFetch(image, b, 0), f.x), f.y);
    outColor = color * t;
}
