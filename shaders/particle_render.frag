#version 450

layout(location = 0) in vec4 fragColor;
layout(location = 1) in vec2 fragUV;

layout(location = 0) out vec4 outColor;

void main() {
    vec2 d = fragUV * 2.0 - 1.0;
    float r2 = dot(d, d);
    if (r2 > 1.0) discard;

    float feather = 1.0 - smoothstep(0.64, 1.0, r2);
    float core = 1.0 - smoothstep(0.0, 0.18, r2);
    vec3 color = fragColor.rgb * (0.85 + core * 0.35);
    outColor = vec4(color, fragColor.a * feather);
}
