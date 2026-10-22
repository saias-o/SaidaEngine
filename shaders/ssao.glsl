#ifndef SSAO_GLSL
#define SSAO_GLSL

// Screen-space ambient occlusion from the depth buffer alone: the raw kernel
// (ambient_occlusion.frag) whose result ssao_denoise.glsl turns into the
// visibility the scene pass's lit shaders put on their indirect light. The
// includer first defines
//     float sampleDepth(vec2 viewportUV)
// returning the reversed depth of the texel at that viewport coordinate.
//
// Like depth_reconstruction.glsl, this file is also compiled as C++ over GLM
// (tests/SsaoTests.cpp), so the CPU tests run this very code. Keep to what
// both languages accept: float literals carry `f`, no swizzles, no out
// parameters, no arrays, and no GLSL keyword or built-in (`sample`, `step`)
// as a name.

#include "depth_reconstruction.glsl"

// AO_DIR_COUNT directions, AO_STEP_COUNT samples along each. One kernel for
// every pixel would print a copy of each occluder per sample, in hard-edged
// bands; instead each pixel of an AO_PATTERN x AO_PATTERN block turns the
// directions and slides the steps by its own share (ssaoPatternIndex), and
// ssao_denoise.glsl averages exactly such a block, so that every pixel sees
// all AO_PATTERN^2 kernels and the bands become noise, then smoothness.
const int AO_DIR_COUNT = 8;
const int AO_STEP_COUNT = 2;
const int AO_PATTERN = 4;
const float AO_TWO_PI = 6.28318530718f;
// Elevation cosine every sample gives up, so that a flat surface does not
// occlude itself through depth quantization.
const float AO_BIAS = 0.03f;
// Share of the radius within which a sample counts fully. Its weight then
// falls to exactly zero at the radius, where rejection starts.
const float AO_FULL_WEIGHT_SHARE = 0.25f;
// Tangents closer to parallel than this sine span no plane.
const float AO_MIN_TANGENT_SINE = 1e-3f;

struct SsaoInputs {
    vec4 settings;         // AmbientOcclusionPass: x enabled, y radius (m), z intensity, w power
    vec4 invProjection;    // InverseProjection::scale
    vec4 invProjection2;   // InverseProjection::depth
    vec4 invOffsets;       // InverseProjection::offsets
    vec2 pixels;           // the viewport's size in depth texels
};

// The texel's place in its AO_PATTERN x AO_PATTERN block, 0 to AO_PATTERN^2 - 1,
// as a 4 x 4 ordered-dither (Bayer) matrix lays it out: neighbours far apart
// in the sequence. `transposed` reads the matrix the other way, a second
// sequence little correlated with the first.
int ssaoPatternIndex(vec2 texelUV, vec2 pixels, bool transposed) {
    int x = int(floor(texelUV.x * pixels.x));
    int y = int(floor(texelUV.y * pixels.y));
    if (transposed) {
        int swap = x;
        x = y;
        y = swap;
    }
    int fine = 2 * ((x ^ y) & 1) + (y & 1);
    int coarse = 2 * (((x >> 1) ^ (y >> 1)) & 1) + ((y >> 1) & 1);
    return 4 * fine + coarse;
}

vec3 ssaoViewPosition(SsaoInputs inputs, vec2 uv, float depth) {
    return viewPositionFromDepth(uv, depth, inputs.invProjection, inputs.invProjection2,
                                 inputs.invOffsets);
}

// The centre of the texel that holds viewport coordinate `uv`. A point is
// rebuilt where its depth was measured: rebuilt at the sample's own
// coordinate with its texel's depth, it leaves the surface by up to half a
// texel of depth slope -- more than AO_BIAS on a floor at a grazing angle --
// and that error moves whenever the view does.
vec2 ssaoTexelCentre(vec2 uv, vec2 pixels) {
    vec2 texel = clamp(floor(uv * pixels), vec2(0.0f), pixels - 1.0f);
    return (texel + 0.5f) / pixels;
}

// The reversed depth of the texel at `uv`; 0 (nothing drawn) outside the viewport.
float ssaoDepthAt(SsaoInputs inputs, vec2 uv) {
    if (any(lessThan(uv, vec2(0.0f))) || any(greaterThan(uv, vec2(1.0f)))) return 0.0f;
    return sampleDepth(ssaoTexelCentre(uv, inputs.pixels));
}

// The view-space surface point of the texel at `uv` with w = 1, or w = 0
// where there is none: outside the viewport, or nothing drawn.
vec4 ssaoSurfaceAt(SsaoInputs inputs, vec2 uv) {
    float depth = ssaoDepthAt(inputs, uv);
    if (nothingDrawn(depth)) return vec4(0.0f);
    return vec4(ssaoViewPosition(inputs, ssaoTexelCentre(uv, inputs.pixels), depth), 1.0f);
}

// How unlikely it is that one side's near neighbour lies on the centre's
// surface: the smaller of its depth step and of how far the depth it and the
// far neighbour extrapolate to misses the centre's. Reversed depth is affine
// in screen space across a plane, under perspective and orthographic
// projections alike, so on the centre's own plane that miss is zero -- at a
// crease, where the face across it can show the smaller step, and on a floor
// at a grazing angle, which moves metres between rows. The step answers where
// the far neighbour is already off the surface: a strip two texels wide.
float ssaoSideCost(float centreDepth, float nearDepth, float farDepth) {
    float nearStep = abs(nearDepth - centreDepth);
    if (nothingDrawn(farDepth)) return nearStep;
    return min(nearStep, abs(2.0f * nearDepth - farDepth - centreDepth));
}

// Whether the neighbour after the centre on a screen axis, rather than the
// one before it, lies on the centre's own surface. A side without its near
// neighbour never does.
bool ssaoAfterIsSameSurface(float centreDepth, float before1, float before2, float after1,
                            float after2) {
    if (nothingDrawn(after1)) return false;
    if (nothingDrawn(before1)) return true;
    return ssaoSideCost(centreDepth, after1, after2) <= ssaoSideCost(centreDepth, before1, before2);
}

// One axis of the surface's tangent frame at `centre` (the texel at `texelUV`,
// of depth `centreDepth`): the step to the neighbour along `axisStep` that
// lies on the same surface, oriented along `axisStep`. A one-sided
// difference crosses every silhouette on its side and joins two surfaces.
// Zero when the centre has neither neighbour.
vec3 ssaoTangent(SsaoInputs inputs, vec2 texelUV, vec2 axisStep, vec3 centre, float centreDepth) {
    float before1 = ssaoDepthAt(inputs, texelUV - axisStep);
    float after1 = ssaoDepthAt(inputs, texelUV + axisStep);
    if (nothingDrawn(before1) && nothingDrawn(after1)) return vec3(0.0f);
    float before2 = ssaoDepthAt(inputs, texelUV - 2.0f * axisStep);
    float after2 = ssaoDepthAt(inputs, texelUV + 2.0f * axisStep);
    if (ssaoAfterIsSameSurface(centreDepth, before1, before2, after1, after2))
        return ssaoViewPosition(inputs, texelUV + axisStep, after1) - centre;
    return centre - ssaoViewPosition(inputs, texelUV - axisStep, before1);
}

// The unit direction from `centre` back toward the viewer, along the pixel's
// ray to the near plane (reversed depth 1): toward the eye in perspective,
// +z in an orthographic view.
vec3 ssaoTowardViewer(SsaoInputs inputs, vec2 uv, vec3 centre) {
    vec3 toNear = ssaoViewPosition(inputs, uv, 1.0f) - centre;
    float length2 = dot(toNear, toNear);
    return length2 > 0.0f ? toNear * inversesqrt(length2) : vec3(0.0f, 0.0f, 1.0f);
}

// The normal of the plane two tangents span, turned toward the viewer. When
// they are missing or too close to parallel to span one, the viewer's own
// direction: normalizing a vanishing cross product would give NaN.
vec3 ssaoNormal(vec3 tangentX, vec3 tangentY, vec3 towardViewer) {
    vec3 normal = cross(tangentY, tangentX);
    float normal2 = dot(normal, normal);
    float span2 = AO_MIN_TANGENT_SINE * AO_MIN_TANGENT_SINE
                * dot(tangentX, tangentX) * dot(tangentY, tangentY);
    if (!(normal2 > span2)) return towardViewer;
    normal *= inversesqrt(normal2);
    return dot(normal, towardViewer) < 0.0f ? -normal : normal;
}

// The view-space normal of the surface at the texel centre `texelUV`.
vec3 ssaoSurfaceNormal(SsaoInputs inputs, vec2 texelUV, vec3 centre, float centreDepth) {
    vec2 texel = 1.0f / inputs.pixels;
    vec3 tangentX = ssaoTangent(inputs, texelUV, vec2(texel.x, 0.0f), centre, centreDepth);
    vec3 tangentY = ssaoTangent(inputs, texelUV, vec2(0.0f, texel.y), centre, centreDepth);
    return ssaoNormal(tangentX, tangentY, ssaoTowardViewer(inputs, texelUV, centre));
}

// How much the surface point `occluder` hides the hemisphere over `centre`:
// the cosine of its elevation above the tangent plane, less AO_BIAS, weighted
// down to exactly zero at `radius`, past which it is rejected.
float ssaoOcclusion(vec3 centre, vec3 normal, vec3 occluder, float radius) {
    vec3 delta = occluder - centre;
    float dist = length(delta);
    if (!(dist > 0.0f) || dist >= radius) return 0.0f;
    float elevation = max(dot(normal, delta) / dist - AO_BIAS, 0.0f);
    float weight = 1.0f - smoothstep(AO_FULL_WEIGHT_SHARE * radius, radius, dist);
    return elevation * weight;
}

// The AO radius at reversed `depth`, in viewport UV per axis. A view-space
// length projects to 0.5 * length * w / invP00 (invP11 vertically), where
// w = invP23 * depth + invP33 is the reciprocal of clip w: one expression for
// perspective, off-centre and orthographic projections.
vec2 ssaoScreenRadius(SsaoInputs inputs, float depth) {
    float viewW = inputs.invProjection.z * depth + inputs.invProjection.w;
    vec2 halfExtent = abs(vec2(inputs.invProjection.x, inputs.invProjection.y));
    return (0.5f * inputs.settings.y * viewW) / max(halfExtent, vec2(1e-6f));
}

// The linear visibility at viewport `uv` that a kernel of `directions`
// directions, turned by `turn` of one direction's sector, and `steps` samples
// along each, slid by `slide` of one step's share of the radius, measures:
// 1 unoccluded, 0 fully occluded. Every attempted sample has the same weight,
// and a rejected one (outside the viewport, nothing drawn, beyond the radius)
// counts as unoccluded. Dividing by the surviving samples instead let a
// single survivor darken a pixel entirely, and how many survive changes as
// the camera moves.
float ssaoKernelVisibility(SsaoInputs inputs, vec2 uv, int directions, int steps, float turn,
                           float slide) {
    float radius = inputs.settings.y;
    if (inputs.settings.x < 0.5f || !(radius > 0.0f) || !(inputs.settings.z > 0.0f)) return 1.0f;

    vec2 texelUV = ssaoTexelCentre(uv, inputs.pixels);
    float depth = sampleDepth(texelUV);
    if (nothingDrawn(depth)) return 1.0f;
    vec3 centre = ssaoViewPosition(inputs, texelUV, depth);
    vec3 normal = ssaoSurfaceNormal(inputs, texelUV, centre, depth);
    vec2 uvRadius = ssaoScreenRadius(inputs, depth);

    float occlusion = 0.0f;
    for (int dirIndex = 0; dirIndex < directions; ++dirIndex) {
        float angle = (float(dirIndex) + turn) * AO_TWO_PI / float(directions);
        vec2 offset = vec2(cos(angle), sin(angle)) * uvRadius;
        for (int stepIndex = 0; stepIndex < steps; ++stepIndex) {
            float along = (float(stepIndex) + slide) / float(steps);
            vec4 occluder = ssaoSurfaceAt(inputs, texelUV + offset * along);
            if (occluder.w > 0.5f)
                occlusion += ssaoOcclusion(centre, normal, vec3(occluder), radius);
        }
    }
    float attempted = float(directions * steps);
    return clamp(1.0f - inputs.settings.z * occlusion / attempted, 0.0f, 1.0f);
}

// The kernel stage's output at viewport `uv`, before denoising and before the
// power: this texel's own turn and slide of the AO_DIR_COUNT x AO_STEP_COUNT kernel.
float ssaoRawVisibility(SsaoInputs inputs, vec2 uv) {
    vec2 texelUV = ssaoTexelCentre(uv, inputs.pixels);
    float patternCells = float(AO_PATTERN * AO_PATTERN);
    float turn = (float(ssaoPatternIndex(texelUV, inputs.pixels, false)) + 0.5f) / patternCells;
    float slide = (float(ssaoPatternIndex(texelUV, inputs.pixels, true)) + 0.5f) / patternCells;
    return ssaoKernelVisibility(inputs, uv, AO_DIR_COUNT, AO_STEP_COUNT, turn, slide);
}

#endif  // SSAO_GLSL
