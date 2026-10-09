#version 450

layout(location = 0) in vec4 fragColor;
layout(location = 1) in vec2 fragUV;
layout(location = 2) flat in float fragRain;

layout(location = 0) out vec4 outColor;

void main() {
    vec2 d = fragUV * 2.0 - 1.0;
    if (fragRain > 0.5) {
        // Positive Y follows velocity: a broad leading drop and a thin tail.
        float halfWidth = mix(0.14, 0.62, smoothstep(0.0, 1.0, fragUV.y));
        float feather = 1.0 - smoothstep(0.35, 1.0, abs(d.x) / halfWidth);
        float ends = smoothstep(0.0, 0.18, fragUV.y) * (1.0 - smoothstep(0.88, 1.0, fragUV.y));
        float alpha = fragColor.a * feather * ends;
        if (alpha <= 0.001) discard;
        outColor = vec4(fragColor.rgb, alpha);
        return;
    }
    float r2 = dot(d, d);
    if (r2 > 1.0) discard;

    float feather = 1.0 - smoothstep(0.64, 1.0, r2);
    float core = 1.0 - smoothstep(0.0, 0.18, r2);
    vec3 color = fragColor.rgb * (0.85 + core * 0.35);
    outColor = vec4(color, fragColor.a * feather);
}
