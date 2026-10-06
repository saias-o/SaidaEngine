// Variation across a tiled surface (MaterialDesc::variation), in its texture
// coordinates. A photographed texture repeated over a street or a facade
// reads as wallpaper twice over: every repeat is the same, and every repeat is
// the same brightness. The warp moves texels by a slowly varying amount, so
// no two repeats line up; the macro noise varies brightness and slope over
// several repeats, with a mean of zero, so the material's measured albedo is
// still its average. All terms off cost one branch.
#ifndef SAIDA_SURFACE_VARIATION_GLSL
#define SAIDA_SURFACE_VARIATION_GLSL

#include "noise.glsl"

const float SURFACE_WARP_CELL = 2.0;      // texture repeats per warp cell
const float SURFACE_NORMAL_FADE_START = 0.125; // repeats per pixel: eight pixels per repeat
const float SURFACE_NORMAL_FADE_END = 0.5;     // two pixels per repeat

struct SurfaceCoords {
    vec2 uv;
    vec2 dx;
    vec2 dy;
};

// Texture coordinates displaced by `warp` repeats, with the gradients that
// select mips carried through the warp's analytic Jacobian: a warped texture
// is sampled at the mip its own stretch asks for, never sharper.
SurfaceCoords warpedCoords(vec2 uv, vec2 dx, vec2 dy, float warp) {
    SurfaceCoords c = SurfaceCoords(uv, dx, dy);
    if (warp <= 0.0) return c;
    vec2 p = uv / SURFACE_WARP_CELL;
    vec3 wx = noised(p);
    vec3 wy = noised(p + vec2(19.1, 47.3));
    float k = warp / SURFACE_WARP_CELL;
    c.uv = uv + warp * (vec2(wx.x, wy.x) - 0.5);
    c.dx = dx + k * vec2(dot(wx.yz, dx), dot(wy.yz, dx));
    c.dy = dy + k * vec2(dot(wx.yz, dy), dot(wy.yz, dy));
    return c;
}

// Three octaves of value noise, `scale` cells per coordinate unit:
// x the variation in [-1, 1], yz its gradient per coordinate unit. An octave
// a pixel can no longer show fades to its mean before it aliases.
vec3 macroVariation(vec2 uv, vec2 dx, vec2 dy, float scale) {
    vec2 p = uv * scale;
    float footprint = max(length(dx), length(dy)) * scale;
    vec3 sum = vec3(0.0);
    float amplitude = 1.0, frequency = 1.0;
    for (int octave = 0; octave < 3; ++octave) {
        float shown = 1.0 - smoothstep(0.5, 1.0, footprint * frequency);
        vec3 n = noised(p * frequency + vec2(19.1, 47.3) * float(octave));
        sum += amplitude * shown * vec3(2.0 * n.x - 1.0, n.yz * (2.0 * frequency * scale));
        amplitude *= 0.5;
        frequency *= 2.0;
    }
    return sum / 1.75;
}

// How much of a normal map a pixel can carry: a repeat drawn over fewer than
// a few pixels only aliases, so its relief fades to the surface's own.
float surfaceNormalWeight(vec2 dx, vec2 dy) {
    float footprint = max(length(dx), length(dy));
    return 1.0 - smoothstep(SURFACE_NORMAL_FADE_START, SURFACE_NORMAL_FADE_END, footprint);
}

#endif
