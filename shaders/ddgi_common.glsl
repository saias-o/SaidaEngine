// Shared DDGI octahedral direction encoding.
//
// Keep dependency-free: volume sampling lives in lighting.glsl.

#ifndef DDGI_COMMON_GLSL
#define DDGI_COMMON_GLSL

vec2 octSignNotZero(vec2 v) {
    return vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}

// Encode a normalized direction -> [-1,1]^2.
vec2 octEncode(vec3 d) {
    float l1 = abs(d.x) + abs(d.y) + abs(d.z);
    vec2 p = d.xy * (1.0 / l1);
    // Fold the lower hemisphere (z<0) outward. The yx swap + abs + sign is exact;
    // any other arrangement gives the classic "cross artifact" near the equator.
    return (d.z < 0.0) ? ((1.0 - abs(p.yx)) * octSignNotZero(p)) : p;
}

// Decode [-1,1]^2 -> normalized direction. Must use the same fold as octEncode.
vec3 octDecode(vec2 e) {
    vec3 v = vec3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
    if (v.z < 0.0) v.xy = (1.0 - abs(v.yx)) * octSignNotZero(v.xy);
    return normalize(v);
}

// Direction i of n uniformly distributed on the sphere (spherical Fibonacci).
// Used to generate per-probe ray directions for the DDGI update.
vec3 sphericalFibonacci(float i, float n) {
    const float PHI = 1.6180339887498949;
    const float TWO_PI = 6.28318530718;
    float phi = TWO_PI * fract(i * (PHI - 1.0));
    float cosTheta = 1.0 - (2.0 * i + 1.0) / n;
    float sinTheta = sqrt(clamp(1.0 - cosTheta * cosTheta, 0.0, 1.0));
    return vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

#endif // DDGI_COMMON_GLSL
