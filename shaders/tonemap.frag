#version 450
#extension GL_GOOGLE_include_directive : require

#include "tonemap_common.glsl"

// HDR -> LDR tonemap pass with AO, fog, bloom and lens effects.

DECL_TEX2D(0, 0, 3, hdrInput);
DECL_TEX2D(0, 2, 5, bloomInput);

layout(location = 0) in vec2 fragUV;
layout(location = 1) flat in float flareVisibility;  // tonemap.vert
layout(location = 0) out vec4 outColor;

const int AO_DIR_COUNT = 8;
const int AO_STEP_COUNT = 2;
const float AO_BIAS = 0.03;
const float AO_NORMAL_EPSILON = 1e-4;

vec4 sampleHdr(vec2 viewportUV) {
    return texture(TEX2D(hdrInput), sourceUV(viewportUV));
}

float sampleDepth(vec2 viewportUV) {
    // Explicit LOD: depth has no mips (identical result) and the AO/fog loops
    // call this from non-uniform control flow, which WGSL forbids for the
    // implicit-derivative texture() variant.
    return textureLod(TEX2D(depthInput), sourceUV(viewportUV), 0.0).r;
}

vec2 viewportTexel() {
    vec2 sourcePixels = vec2(textureSize(TEX2D(hdrInput), 0)) * max(push.sourceRect.zw, vec2(1e-6));
    return 1.0 / max(sourcePixels, vec2(1.0));
}

// ACES filmic tonemap, Krzysztof Narkowicz 2015 fit.
vec3 acesFilmic(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

vec3 reconstructViewPosition(vec2 uv, float depth) {
    vec2 clipXY = uv * 2.0 - 1.0;
    float viewW = push.projectionParams.z * depth + push.projectionParams.w;
    vec3 view = vec3(clipXY.x * push.projectionParams.x,
                     clipXY.y * push.projectionParams.y,
                     push.projectionParams2.x * depth + push.projectionParams2.y);
    return view / max(viewW, 1e-6);
}

vec3 viewNormalFromDepth(vec2 uv, vec3 center) {
    vec2 texel = viewportTexel();
    float depthX = sampleDepth(uv + vec2(texel.x, 0.0));
    float depthY = sampleDepth(uv + vec2(0.0, texel.y));
    vec3 px = reconstructViewPosition(uv + vec2(texel.x, 0.0), depthX);
    vec3 py = reconstructViewPosition(uv + vec2(0.0, texel.y), depthY);
    vec3 n = normalize(cross(py - center, px - center));
    vec3 viewDir = normalize(-center);
    if (dot(n, viewDir) < 0.0) n = -n;
    if (length(n) < AO_NORMAL_EPSILON) n = vec3(0.0, 0.0, 1.0);
    return n;
}

float ambientOcclusion(vec2 uv) {
    if (push.aoParams.x < 0.5) return 1.0;

    float centerDepth = sampleDepth(uv);
    if (nothingDrawn(centerDepth)) return 1.0;

    vec3 center = reconstructViewPosition(uv, centerDepth);
    vec3 normal = viewNormalFromDepth(uv, center);
    float viewDepth = max(abs(center.z), 0.05);
    vec2 uvRadius = vec2(0.5 * push.aoParams.y / viewDepth);

    float occlusion = 0.0;
    float samples = 0.0;
    for (int dirIndex = 0; dirIndex < AO_DIR_COUNT; ++dirIndex) {
        float angle = (float(dirIndex) + 0.5) * 6.28318530718 / float(AO_DIR_COUNT);
        vec2 dir = vec2(cos(angle), sin(angle));

        for (int stepIndex = 1; stepIndex <= AO_STEP_COUNT; ++stepIndex) {
            float stepT = float(stepIndex) / float(AO_STEP_COUNT);
            vec2 sampleUV = uv + dir * uvRadius * stepT;
            if (any(lessThan(sampleUV, vec2(0.0))) || any(greaterThan(sampleUV, vec2(1.0))))
                continue;

            float depthSample = sampleDepth(sampleUV);
            if (nothingDrawn(depthSample)) continue;

            vec3 samplePos = reconstructViewPosition(sampleUV, depthSample);
            vec3 delta = samplePos - center;
            float dist = length(delta);
            if (dist <= AO_NORMAL_EPSILON || dist > push.aoParams.y) continue;

            float normalTerm = max(dot(normal, delta / dist) - AO_BIAS, 0.0);
            float rangeTerm = 1.0 - smoothstep(push.aoParams.y * 0.25, push.aoParams.y, dist);
            occlusion += normalTerm * rangeTerm;
            samples += 1.0;
        }
    }

    if (samples <= 0.0) return 1.0;
    float ao = 1.0 - push.aoParams.z * (occlusion / samples);
    return pow(clamp(ao, 0.0, 1.0), push.aoParams.w);
}

vec3 bloom(vec2 uv) {
    if (push.bloomParams.x < 0.5 || push.bloomParams.z <= 0.0)
        return vec3(0.0);
    return texture(TEX2D(bloomInput), clamp(uv, vec2(0.0), vec2(1.0))).rgb * push.bloomParams.z;
}

// Altitude of the view-space point `v`: over the sphere when there is one
// (written so that no two numbers near the planet's radius are subtracted),
// along the up vector otherwise.
float fogAltitude(vec3 v) {
    float h0 = push.fogHeight.y;
    float s = dot(v, push.fogUp.xyz);
    float radius = push.fogHeight.z;
    if (radius <= 0.0) return h0 + s;
    float rh = radius + h0;
    float vv = dot(v, v);
    float lifted = 2.0 * radius * h0 + h0 * h0 + 2.0 * rh * s + vv;  // |c + v|^2 - R^2
    return lifted / (sqrt(max(rh * rh + 2.0 * rh * s + vv, 0.0)) + radius);
}

// Points of a ray the layered air's density is averaged over, and the lowest
// altitude it is taken at (a curved world's far edge drops below the datum).
const int FOG_SAMPLES = 8;
const float FOG_LOWEST_ALTITUDE = -1000.0;

// Optical depth per channel from the camera to `viewPos`, past `start`.
// Uniform grey fog unless the air is layered; then the grey and the Rayleigh
// densities are averaged over eight points of the fogged segment.
vec3 fogDepth(vec3 viewPos, float dist) {
    float start = push.fogParams.y;
    float fogged = max(dist - start, 0.0);
    if (push.fogHeight.w < 0.5) return vec3(fogged * push.fogParams.z);
    float greyHeight = push.fogHeight.x;
    float rayleighHeight = push.fogRayleigh.w;
    float from = dist > 0.0 ? min(start / dist, 1.0) : 1.0;
    float grey = 0.0, air = 0.0;
    for (int i = 0; i < FOG_SAMPLES; ++i) {
        float f = from + (1.0 - from) * (float(i) + 0.5) / float(FOG_SAMPLES);
        // Below the datum the air is denser still, but not without bound.
        float a = max(fogAltitude(viewPos * f), FOG_LOWEST_ALTITUDE);
        grey += greyHeight > 0.0 ? exp(-a / greyHeight) : 1.0;
        air += exp(-a / rayleighHeight);
    }
    grey /= float(FOG_SAMPLES);
    air /= float(FOG_SAMPLES);
    return fogged * (push.fogParams.z * grey + push.fogRayleigh.rgb * air);
}

vec3 applyFog(vec3 color, vec2 uv) {
    if (push.fogParams.x < 0.5) return color;

    float depth = sampleDepth(uv);
    if (nothingDrawn(depth)) return color;

    vec3 viewPos = reconstructViewPosition(uv, depth);
    // Per channel: clear air takes blue several times faster than red, and
    // what it takes is replaced by the colour of the air itself.
    vec3 transmitted = exp(-fogDepth(viewPos, length(viewPos)));
    return color * transmitted + push.fogColor.rgb * (1.0 - transmitted);
}

// Lens effects of one directional light (LightNode lensFlare / sunStar), in
// scene radiance before exposure. Lengths are in viewport heights so that
// circles stay round; every gain is a fraction of the source's radiance.
const float TWO_PI = 6.28318530718;
const float LENS_EPSILON = 1e-4;

// Sun star: the diffraction of an aperture of STAR_POINTS / 2 straight blades,
// two spikes per blade, alternate spikes shorter, red spreading wider than blue.
const float STAR_POINTS = 8.0;
const float STAR_ROTATION = 0.2618;    // radians: no spike lies along a screen axis
const float STAR_LENGTH = 0.30;
const float STAR_SHORT = 0.55;         // the short spikes' share of STAR_LENGTH
const float STAR_WIDTH = 0.0025;       // a spike's half-width at its root
const vec3 STAR_SPREAD = vec3(1.12, 1.0, 0.88);
const float STAR_GAIN = 0.5;
const float STAR_GLOW_RADIUS = 0.03;
const float STAR_GLOW_GAIN = 0.3;

vec3 sunStar(vec2 q) {
    float r = length(q);
    float sector = TWO_PI / STAR_POINTS;
    float angle = r > LENS_EPSILON ? atan(q.y, q.x) - STAR_ROTATION : 0.0;
    float spike = floor(angle / sector + 0.5);
    float offset = angle - spike * sector;
    float along = r * cos(offset);
    float across = r * sin(offset);
    float reach = fract(spike * 0.5) < 0.25 ? STAR_LENGTH : STAR_LENGTH * STAR_SHORT;
    float rest = clamp(1.0 - along / reach, 0.0, 1.0);
    vec3 width = STAR_WIDTH * (0.3 + 0.7 * rest) * STAR_SPREAD;
    vec3 spikes = exp(-(across * across) / (width * width)) * (rest * rest);
    return STAR_GAIN * spikes + vec3(STAR_GLOW_GAIN * exp(-r / STAR_GLOW_RADIUS));
}

// Lens flare: ghosts, the source re-imaged by reflections between lens
// elements on the line through the optical centre (the viewport's), each at
// `scale` times the source's offset from it -- negative across the centre --
// and tinted by the coatings; and a halo ringing the centre on the source's side.
const float GHOST_GAIN = 0.05;
const float HALO_RADIUS = 0.45;
const float HALO_WIDTH = 0.035;
const vec3 HALO_SPREAD = vec3(1.0, 0.97, 0.94);  // the ring's radius per channel
const float HALO_GAIN = 0.04;
const float HALO_FULL_OFFSET = 0.2;              // source offset at which the halo is whole

// A ghost is brightest at its rim, where the iris that clips it is in focus.
float ghostProfile(float d) {
    return (1.0 - smoothstep(0.8, 1.0, d)) * (0.4 + 0.6 * d * d);
}

// The ghosts that pass the aperture take its hexagonal shape.
float hexagon(vec2 q) {
    q = abs(q);
    return max(q.x * 0.8660254 + q.y * 0.5, q.y);
}

vec3 discGhost(vec2 p, vec2 source, float scale, float radius, vec3 tint) {
    return tint * ghostProfile(length(p - source * scale) / radius);
}

vec3 hexGhost(vec2 p, vec2 source, float scale, float radius, vec3 tint) {
    return tint * ghostProfile(hexagon(p - source * scale) / radius);
}

vec3 lensFlare(vec2 p, vec2 source) {
    vec3 ghosts = discGhost(p, source, 0.55, 0.030, vec3(1.00, 0.70, 0.40))
                + hexGhost(p, source, 0.22, 0.055, vec3(0.45, 0.85, 0.55))
                + discGhost(p, source, -0.18, 0.022, vec3(0.65, 0.55, 1.00))
                + hexGhost(p, source, -0.42, 0.085, vec3(0.40, 0.65, 1.00))
                + discGhost(p, source, -0.70, 0.045, vec3(1.00, 0.55, 0.75))
                + hexGhost(p, source, -1.10, 0.120, vec3(0.50, 0.90, 0.70));
    float fromCentre = length(p);
    float offset = length(source);
    float facing = max(dot(p / max(fromCentre, LENS_EPSILON), source / max(offset, LENS_EPSILON)), 0.0);
    vec3 ring = (vec3(fromCentre) - HALO_RADIUS * HALO_SPREAD) / HALO_WIDTH;
    vec3 halo = exp(-ring * ring) * (facing * facing * min(offset / HALO_FULL_OFFSET, 1.0));
    return GHOST_GAIN * ghosts + HALO_GAIN * halo;
}

vec3 lensEffects(vec2 uv) {
    if (flareVisibility <= 0.0) return vec3(0.0);
    vec2 pixels = viewportPixels();
    vec2 toHeights = vec2(pixels.x / pixels.y, 1.0);
    vec2 p = (uv - 0.5) * toHeights;
    vec2 source = (push.flareSource.xy - 0.5) * toHeights;
    vec3 effects = vec3(0.0);
    if (push.flareSource.z > 0.5) effects += lensFlare(p, source);
    if (push.flareSource.w > 0.5) effects += sunStar(p - source);
    return effects * push.flareRadiance.rgb * flareVisibility;
}

void main() {
    vec4 hdr4 = sampleHdr(fragUV);
    vec3 hdr = hdr4.rgb;

    hdr *= ambientOcclusion(fragUV);
    hdr = applyFog(hdr, fragUV);
    hdr += bloom(fragUV);
    hdr += lensEffects(fragUV);

    hdr *= push.fogParams.w;

    vec3 mapped = acesFilmic(hdr);

    // Exactly one display transfer: sRGB attachments encode on store; an
    // UNORM surface (WebGPU) needs the IEC sRGB transfer here, including its toe.
    vec3 outputRGB = mapped;
    if (push.outputParams.x > 0.5) {
        outputRGB = mix(1.055 * pow(mapped, vec3(1.0 / 2.4)) - 0.055,
                        12.92 * mapped, lessThanEqual(mapped, vec3(0.0031308)));
    }

    // Preserve scene coverage in alpha so XR passthrough composites correctly
    // (transparent where nothing was drawn). Ignored by the desktop swapchain.
    outColor = vec4(outputRGB, hdr4.a);
}
