#version 450
layout(set = 0, binding = 0) uniform sampler2D image;
layout(location = 0) out vec4 outColor;
void main() {
    vec4 p = texelFetch(image, ivec2(gl_FragCoord.xy), 0);
    vec3 srgb = p.a > 0.0 ? clamp(p.rgb / p.a, 0.0, 1.0) : vec3(0.0);
    // CSS blends in its encoded colour space. The sRGB attachment applies the
    // forward transfer on store; invert it here to retain the CPU pixel contract.
    vec3 linear = mix(pow((srgb + 0.055) / 1.055, vec3(2.4)), srgb / 12.92,
                      lessThanEqual(srgb, vec3(0.04045)));
    outColor = vec4(linear, p.a);
}
