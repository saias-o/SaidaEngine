// Lattice noise shared by the scene shaders: arithmetic only, no texture.
#ifndef SAIDA_NOISE_GLSL
#define SAIDA_NOISE_GLSL

// An integer hash of a lattice point. The float hash it replaces was chaotic
// on purpose, so the last bit a compiler rounds differently -- the same
// corner reached as `p + 1` from one cell and as `floor` from the next --
// gave that corner two values, and the ripples tore along lattice lines.
float hash21(ivec2 q) {
    uvec2 u = uvec2(q);
    uint h = u.x * 1597334673u ^ u.y * 3812015801u;
    h ^= h >> 16;
    h *= 2246822519u;
    h ^= h >> 13;
    h *= 3266489917u;
    h ^= h >> 16;
    return float(h) * (1.0 / 4294967296.0);
}

// Value noise with analytic derivative: returns (value, d/dx, d/dy).
// Quintic, so the derivative -- the ripple's slope, hence the normal -- is
// itself smooth: a cubic's slope creases along every lattice line, and the
// octaves' rotated lattices drew a mesh of triangles across the water.
vec3 noised(vec2 x) {
    vec2 cell = floor(x);
    ivec2 p = ivec2(cell);
    vec2 f = x - cell;
    vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    vec2 du = 30.0 * f * f * (f * (f - 2.0) + 1.0);
    float a = hash21(p);
    float b = hash21(p + ivec2(1, 0));
    float c = hash21(p + ivec2(0, 1));
    float d = hash21(p + ivec2(1, 1));
    float k1 = b - a;
    float k2 = c - a;
    float k3 = a - b - c + d;
    float v = a + k1 * u.x + k2 * u.y + k3 * u.x * u.y;
    vec2 deriv = du * vec2(k1 + k3 * u.y, k2 + k3 * u.x);
    return vec3(v, deriv);
}

#endif
