#version 450

layout(location = 0) in vec3 fragColor;
layout(location = 0) out vec4 outColor;

void main() {
    // Bright unlit color written into the HDR target (tonemapped afterwards).
    outColor = vec4(fragColor, 1.0);
}
