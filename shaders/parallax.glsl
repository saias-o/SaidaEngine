// Parallax occlusion mapping (MaterialDesc::heightId, parallaxDepth): the
// texture coordinates where the view ray first meets the height field, so a
// window drawn into a facade sits behind the wall and a sill in front of it,
// without a vertex more. Bindless scene path only: the height map is read
// through the global texture array.
//
// It is a near-field detail with a cost per pixel, so it fades out as the
// pixel's footprint grows, and steps more finely only where the view grazes.
#ifndef SAIDA_PARALLAX_GLSL
#define SAIDA_PARALLAX_GLSL

const float PARALLAX_FADE_START = 0.004; // texture-coordinate units per pixel
const float PARALLAX_FADE_END = 0.012;
const int PARALLAX_MIN_STEPS = 6;
const int PARALLAX_MAX_STEPS = 24;
const float PARALLAX_MIN_COSINE = 0.15;  // a ray this grazing is clamped, not stretched to infinity

float parallaxHeight(uint heightIdx, vec2 uv, vec2 dx, vec2 dy) {
    return textureGrad(globalTextures[nonuniformEXT(heightIdx)], uv, dx, dy).r;
}

// `viewTS` is the direction to the eye in the surface's tangent frame (x along
// u, y along v, z out of the surface), unit length.
SurfaceCoords parallaxCoords(uint heightIdx, float depth, SurfaceCoords c, vec3 viewTS) {
    if (depth <= 0.0 || viewTS.z <= 0.0) return c;
    float footprint = max(length(c.dx), length(c.dy));
    // No texture coordinates to speak of (a face mapped to one point): no
    // direction to march along, the surface stays flat.
    if (footprint <= 0.0) return c;
    float fade = 1.0 - smoothstep(PARALLAX_FADE_START, PARALLAX_FADE_END, footprint);
    if (fade <= 0.0) return c;

    int steps = int(mix(float(PARALLAX_MAX_STEPS), float(PARALLAX_MIN_STEPS), viewTS.z));
    float layer = 1.0 / float(steps);
    vec2 shift = viewTS.xy / max(viewTS.z, PARALLAX_MIN_COSINE) * depth * fade * layer;

    vec2 uv = c.uv;
    float ray = 1.0;
    float height = parallaxHeight(heightIdx, uv, c.dx, c.dy);
    vec2 previousUv = uv;
    float previousGap = ray - height;
    for (int i = 0; i < PARALLAX_MAX_STEPS; ++i) {
        if (i >= steps || height >= ray) break;
        previousUv = uv;
        previousGap = ray - height;
        uv -= shift;
        ray -= layer;
        height = parallaxHeight(heightIdx, uv, c.dx, c.dy);
    }
    // Between the last step above the field and the first below it, where
    // the ray crosses it: linear in both.
    float gap = ray - height;
    float weight = previousGap - gap > 1e-5 ? gap / (gap - previousGap) : 0.0;
    c.uv = mix(uv, previousUv, clamp(weight, 0.0, 1.0));
    return c;
}

#endif
