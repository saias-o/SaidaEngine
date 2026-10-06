#version 450
#extension GL_GOOGLE_include_directive : require

// Texture-free water shading: analytic wave normals, procedural ripples,
// Fresnel reflection, a GGX sun glitter, light through the crests, depth tint
// and shore foam.
//
// Nothing here may repeat. Every wave train and ripple octave is filtered by
// the pixel's footprint: what a pixel is too large to show is not drawn as a
// pattern that aliases into tiles, it becomes roughness, so a far sea is a
// broad glitter path under the Sun, as it is from an aircraft. ALU only: no
// texture, no extra binding, the same cost class on desktop, mobile and Web.

#include "lighting.glsl"
#include "water_common.glsl"
#include "water_wave.glsl"

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in float fragCrest;

layout(location = 0) out vec4 outColor;

const mat2 ROT = mat2(0.80, -0.60, 0.60, 0.80);  // non-axis-aligned octave rotation

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

// Gradient (xz) of a scrolled FBM height field whose octaves fade as the
// pixel outgrows them; `lost` gathers the slope variance they no longer draw.
vec2 fbmGrad(vec2 p, vec2 flow, float t, float footprint, int octaves, inout float lost) {
    vec2 grad = vec2(0.0);
    float amp = 1.0, freq = 1.0, norm = 0.0;
    for (int i = 0; i < 4; ++i) {
        if (i >= octaves) break;
        // A feature is one noise cell: shown while the pixel is under a third of it.
        float shown = 1.0 - smoothstep(0.15, 0.4, footprint * freq);
        vec3 n = noised(p * freq + flow * t * freq);
        grad += amp * freq * n.yz * shown;
        // A value-noise slope's variance is about a fifth of its gain squared.
        lost += (1.0 - shown) * 0.2 * (amp * freq) * (amp * freq);
        norm += amp;
        amp *= 0.5;
        freq *= 1.93;
        p = ROT * p;
        flow = ROT * flow;
    }
    float inv = 1.0 / max(norm, 1e-3);
    lost *= inv * inv;
    return grad * inv;
}

vec2 dirFromAngle(float deg) {
    float a = radians(deg);
    return vec2(cos(a), sin(a));
}

// Cheap analytic sky for reflections when no environment map is bound.
vec3 proceduralSky(vec3 dir, vec3 sunDir, vec3 sunCol) {
    float up = clamp(dir.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 horizon = vec3(0.72, 0.82, 0.93);
    vec3 zenith  = vec3(0.17, 0.40, 0.78);
    return mix(horizon, zenith, pow(up, 0.55));
}

void main() {
    // The pixel's footprint on the water, from the angle a pixel subtends,
    // the distance and how grazing the view is. The ray's direction depends
    // on the pixel alone, so its derivative is continuous from one triangle
    // to the next; a position's is not, the waves tilt every triangle, and
    // filtering on it drew the mesh's edges as seams. Derivatives first,
    // while control flow is uniform: WGSL requires it, and the discard is not.
    vec3 ray = fragWorldPos - lights.cameraPos.xyz;
    vec3 rd = normalize(ray);
    float pixelAngle = max(length(dFdx(rd)), length(dFdy(rd)));
    float footprint = length(ray) * pixelAngle / max(abs(rd.y), 0.05);

    GpuWater w = waters.items[push.index];
    int mode = int(w.shoreMode.x + 0.5);

    // Dry land: do not blend or write depth over the beach.
    float depth = waterDepthAt(fragWorldPos.xz, w);
    if (mode != 0 && depth < 0.0) discard;

    vec3 camPos = lights.cameraPos.xyz;
    vec3 V = normalize(camPos - fragWorldPos);
    float dist = length(camPos - fragWorldPos);
    // The old artistic fade: ripples calm with distance as well as footprint.
    float fade = clamp(1.0 - dist / max(w.misc.y, 1.0), 0.0, 1.0);

    // ── the waves, as normals ────────────────────────────────────────────────
    // Every train, including those the mesh could not carry; each fades once
    // a pixel covers a third of it. Evaluated where the fragment is: the
    // undisplaced point, interpolated over displaced triangles, folds the
    // pattern at every edge of the mesh.
    float shallow = (mode == 0) ? 1.0 : smoothstep(0.0, max(w.shoreTune.w, 0.01), depth);
    vec2 baseDir = waveDirAt(fragWorldPos.xz, w);
    vec2 waveGrad = vec2(0.0);
    float variance = 0.0;
    for (int i = 0; i < WATER_TRAINS; ++i) {
        WaterTrain tr = waterTrain(i, w, baseDir);
        float ang = dot(tr.dir, fragWorldPos.xz) * tr.wn + push.time * tr.celerity * tr.wn + tr.phase;
        float slope = tr.amp * tr.wn * shallow;
        // Down to about three pixels a wavelength.
        float shown = waterTrainShown(tr.wn, footprint * 0.5);
        waveGrad += tr.dir * (slope * cos(ang) * shown);
        variance += (1.0 - shown) * 0.5 * slope * slope;
    }

    // ── the ripples ─────────────────────────────────────────────────────────
    // Gusts: patches hundreds of metres across where the wind blows harder,
    // the ripples taller and the sea rougher, between glossy slicks. They are
    // what breaks a sea's uniformity from the air. Two rotated octaves, so
    // the noise's own lattice never shows.
    vec2 wp = fragWorldPos.xz;
    float gust = smoothstep(0.3, 0.7, 0.65 * noised(ROT * wp * 0.0035 + vec2(17.3, 5.1)).x
                                    + 0.35 * noised(wp * 0.0081 + vec2(3.7, 11.9)).x);
    float wind = mix(0.55, 1.15, gust);
    float lostRipple = 0.0;
    vec2 warp = w.misc.x * fbmGrad(wp * 0.03, vec2(0.05, 0.03), push.time * 0.2, footprint * 0.03, 2, lostRipple);
    lostRipple = 0.0;
    vec2 wpW = wp + warp;
    float lost1 = 0.0, lost2 = 0.0;
    vec2 g1 = fbmGrad(wpW * w.detail1.x, dirFromAngle(w.detail1.w) * w.detail1.y, push.time,
                      footprint * w.detail1.x, 3, lost1) * w.detail1.z;
    vec2 g2 = fbmGrad(wpW * w.detail2.x, dirFromAngle(w.detail2.w) * w.detail2.y, push.time,
                      footprint * w.detail2.x, 3, lost2) * w.detail2.z;
    float rippleGain = wind * mix(0.35, 1.0, fade);
    vec2 grad = waveGrad + (g1 + g2) * rippleGain;
    variance += (lost1 * w.detail1.z * w.detail1.z + lost2 * w.detail2.z * w.detail2.z) * rippleGain * rippleGain;

    vec3 N = normalize(vec3(-grad.x, 1.0, -grad.y));
    float NdotV = max(dot(N, V), 1e-3);

    // Primary directional light = the sun (for the glitter / sky disc).
    vec3 sunDir = vec3(0.0, -1.0, 0.0);
    vec3 sunCol = vec3(1.0);
    for (int i = 0; i < lights.counts.x; ++i) {
        if (int(lights.lights[i].dirType.w + 0.5) == 0) {
            sunDir = normalize(lights.lights[i].dirType.xyz);
            sunCol = lights.lights[i].colorInt.rgb * lights.lights[i].colorInt.w;
            break;
        }
    }
    vec3 L = -sunDir;

    // What the pixel cannot show is roughness (the slope variance it averages),
    // and the slicks are glossier than the gusts around them.
    float baseRough = clamp(w.deep.w, 0.02, 0.4) * mix(0.7, 1.1, gust);
    float rough = clamp(sqrt(baseRough * baseRough + variance), 0.02, 0.6);

    bool hasEnv = lights.environmentParams.x > 0.5;

    // ── the body of the water ───────────────────────────────────────────────
    vec3 shallowCol = (mode != 0) ? w.shoreColor.rgb : w.deep.rgb;
    float dt = clamp(depth / max(w.misc.w, 0.01), 0.0, 1.0);
    vec3 bodyTint = mix(shallowCol, w.deep.rgb, dt);
    vec3 ambient = hasEnv
        ? sampleEnvironmentLod(vec3(0.0, 1.0, 0.0), environmentMaxLod()) * lights.environmentParams.y
        : lights.ambient.rgb;
    float sunUp = clamp(L.y, 0.0, 1.0);
    vec3 body = bodyTint * (0.25 + 0.75 * ambient + sunCol * sunUp * 0.12);
    // Light through the crests: looking towards the Sun, a wave's thin top
    // glows with the water's own colour, brighter as it stands taller.
    float toward = pow(clamp(dot(-V, L) * 0.5 + 0.5, 0.0, 1.0), 4.0);
    float thin = smoothstep(0.45, 1.0, fragCrest);
    body += w.shoreColor.rgb * sunCol * (toward * thin * 0.35 * sunUp);

    // ── reflection ──────────────────────────────────────────────────────────
    vec3 R = reflect(-V, N);
    R.y = max(R.y, 0.02);
    vec3 refl = hasEnv
        ? sampleEnvironmentLod(R, clamp(rough * 1.6, 0.0, 1.0) * environmentMaxLod()) * lights.environmentParams.z
        : proceduralSky(R, sunDir, sunCol);
    float F = 0.02 + 0.98 * pow(1.0 - NdotV, w.look.x);
    // Water rougher than a pixel mirrors less of the bright low sky: its
    // facets tilt away from the grazing angle. That is what draws the wind's
    // patches on a sea seen from the air, darker where it blows. Towards
    // the horizon every facet is seen grazing and the sea stays a mirror.
    F *= mix(1.0, 0.55, smoothstep(0.05, 0.35, rough) * smoothstep(0.12, 0.45, abs(rd.y)));
    vec3 color = mix(body, refl, clamp(F * w.foam.w, 0.0, 1.0));

    // ── the Sun's glitter ───────────────────────────────────────────────────
    // GGX on the filtered roughness: a hard sparkle up close, a long glitter
    // path far away. `specularIntensity` keeps its meaning (0.25 = physical).
    // The Sun is a disc, not a point: no glint is sharper than its half
    // degree spread over the ripples a pixel holds, nor brighter than the
    // bloom can carry without flaring.
    float sunRough = max(rough, 0.1);
    vec3 H = normalize(L + V);
    float NdotL = max(dot(N, L), 0.0);
    float NdotH = max(dot(N, H), 0.0);
    float D = distributionGGX(NdotH, sunRough);
    float G = geometrySmith(NdotV, NdotL, sunRough);
    float Fh = 0.02 + 0.98 * pow(1.0 - max(dot(H, V), 0.0), 5.0);
    float spec = min(D * G * Fh / max(4.0 * NdotV, 1e-3), 6.0);
    color += sunCol * spec * (w.look.z * 4.0);

    // Crest foam plus a swash band near the shoreline.
    float foam = smoothstep(w.look.w, 1.0, fragCrest) * w.misc.z * wind;
    if (mode != 0) {
        // Keep phase mostly coherent along the shore so foam moves inland.
        vec2 wd = waveDirAt(fragWorldPos.xz, w);
        vec2 alongShore = vec2(-wd.y, wd.x);
        float alongCoord = dot(fragWorldPos.xz, alongShore);
        float swash = w.shoreTune.z * (0.5 + 0.5 * sin(push.time * w.shoreTune.y
                      + alongCoord * 0.04));
        float band = w.shoreTune.x + swash;                 // current wet reach (depth units)
        float wet  = smoothstep(band, band * 0.2, depth);   // broad foam near the waterline
        float lace = smoothstep(0.06, 0.0, abs(depth - band));  // bright leading edge
        float shoreFoam = clamp(max(wet * 0.7, lace), 0.0, 1.0) * w.shoreMode.y;
        foam = max(foam, shoreFoam);
    }
    color = mix(color, w.foam.rgb, clamp(foam, 0.0, 1.0));

    // Fade alpha at the waterline; deeper water is opaque.
    float alpha = (mode != 0) ? smoothstep(0.0, max(w.shoreColor.w, 0.01), depth) : 1.0;
    outColor = vec4(color, alpha);
}
