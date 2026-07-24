// Shared PBR lighting math for the scene shaders.

#include "ddgi_common.glsl"
#include "web_compat.glsl"

const float PI = 3.14159265359;
const int MAX_LIGHTS = 16;
const int MAX_SHADOWS = 4;

// One unified light covers directional / point / spot (mirrors GpuLight in C++).
struct Light {
    vec4 posRange;    // xyz world position (point/spot), w = range
    vec4 colorInt;    // rgb color, w = intensity
    vec4 dirType;     // xyz travel direction (dir/spot), w = type (0 dir,1 point,2 spot)
    vec4 spotShadow;  // x = cosInner, y = cosOuter, z = shadowIndex (-1 none), w unused
};

layout(set = 0, binding = 1) uniform LightingUBO {
    vec4 ambient;       // rgb = ambient color (fallback when GI disabled)
    vec4 cameraPos;     // xyz = camera world position
    vec4 shadowParams;  // x = softness
    ivec4 counts;       // x = light count, y = mode (0 realtime, 1 baked)
    Light lights[MAX_LIGHTS];
    mat4 shadowMatrices[MAX_SHADOWS];
    // DDGI irradiance volume params.
    vec4 giOrigin;      // xyz = volume min corner (world), w = enabled (0/1)
    vec4 giSpacing;     // xyz = probe spacing (world units)
    ivec4 giCounts;     // xyz = probe counts per axis, w = probesPerRow in atlas
    ivec4 giAtlas;      // x = irradiance texels/probe, y = visibility texels/probe
    vec4 environmentParams; // x enabled, y diffuse intensity, z specular intensity, w rotation
} lights;

DECL_SHADOW2DARRAY(0, 2, 8, shadowMap);

// DDGI atlases: one octahedral tile per probe, laid out in a 2D atlas. Irradiance
// holds incident radiance (rgb), visibility holds (mean dist, mean dist^2) for the
// Chebyshev occlusion test. Both are LINEAR + CLAMP sampled.
DECL_TEX2D(0, 4, 9,  giIrradiance);
DECL_TEX2D(0, 5, 10, giVisibility);

// Voxelized scene albedo (the GI ray-march source). Sampled here only for the
// debug visualization; the DDGI update compute pass reads it directly.
DECL_TEX3D(0, 6, 11, giVoxels);

// Canonical equirectangular environment. Reused by skybox, IBL and DDGI misses.
DECL_TEX2D(0, 7, 12, iblEnvironment);

const float INV_TWO_PI = 0.15915494309;
const float INV_PI = 0.31830988618;
const float IBL_DIFFUSE_LOD_BIAS = 2.0;
const float IBL_SPECULAR_MIP_CURVE = 1.5;

// World position -> [0,1] coordinate inside the voxel/probe volume box.
vec3 giVolumeUVW(vec3 wp) {
    vec3 extent = lights.giSpacing.xyz * vec3(lights.giCounts.xyz - 1);
    return (wp - lights.giOrigin.xyz) / extent;
}

struct LightTerms { vec3 diffuse; vec3 specular; };

// Indirect diffuse from the DDGI irradiance volume.

int giProbeIndex(ivec3 c) {
    return c.x + c.y * lights.giCounts.x + c.z * lights.giCounts.x * lights.giCounts.y;
}

// UV of a probe's octahedral tile for direction `dir`, in an atlas of `atlasSize`
// pixels with `texels` interior texels/probe and a 1px border (gutter) per side.
vec2 giProbeUV(int probeIndex, vec3 dir, int texels, vec2 atlasSize) {
    int ppr = lights.giCounts.w;
    ivec2 tile = ivec2(probeIndex % ppr, probeIndex / ppr);
    vec2 oct = octEncode(dir) * 0.5 + 0.5;                       // [0,1]
    vec2 px = vec2(tile * (texels + 2)) + 1.0 + oct * float(texels);
    return px / atlasSize;
}

// Trilinearly interpolate the 8 probes around `wp`, with DDGI's directional
// (back-face) and Chebyshev visibility weights to kill light leaking.
vec3 sampleIrradianceVolume(vec3 wp, vec3 N, vec3 V) {
    ivec3 counts  = lights.giCounts.xyz;
    vec3  spacing = lights.giSpacing.xyz;

    // Bias the sample off the surface before the Chebyshev visibility test.
    float maxSpacing = max(spacing.x, max(spacing.y, spacing.z));
    wp += (N * 0.35 + V * 0.35) * maxSpacing;

    vec3  gridF   = (wp - lights.giOrigin.xyz) / spacing;
    ivec3 base    = ivec3(floor(gridF));
    vec3  frac    = gridF - vec3(base);

    vec2 irrAtlas = vec2(textureSize(TEX2D(giIrradiance), 0));
    vec2 visAtlas = vec2(textureSize(TEX2D(giVisibility), 0));
    int  irrT     = lights.giAtlas.x;
    int  visT     = lights.giAtlas.y;

    vec3  sumIrr = vec3(0.0);
    float sumW   = 0.0;

    for (int i = 0; i < 8; ++i) {
        ivec3 off = ivec3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        ivec3 c   = clamp(base + off, ivec3(0), counts - 1);
        int   idx = giProbeIndex(c);

        vec3  probePos = lights.giOrigin.xyz + vec3(c) * spacing;
        vec3  toProbe  = probePos - wp;
        float dist     = length(toProbe);
        vec3  dir      = dist > 1e-5 ? toProbe / dist : N;

        // 1. Trilinear weight.
        vec3  tw   = mix(1.0 - frac, frac, vec3(off));
        float wTri = tw.x * tw.y * tw.z;

        // 2. Directional weight: prefer probes in front of the surface.
        float wDir = max(0.0001, dot(dir, N) * 0.5 + 0.5);
        wDir *= wDir;

        // 3. Chebyshev visibility from the probe.
        vec2  vis  = texture(TEX2D(giVisibility), giProbeUV(idx, -dir, visT, visAtlas)).rg;
        float mean = vis.x;
        float wVis = 1.0;
        if (dist > mean) {
            float variance = max(0.0, vis.y - mean * mean);
            wVis = variance / (variance + (dist - mean) * (dist - mean));
            wVis = max(0.0, wVis);
            wVis = wVis * wVis;   // soften vs the paper's cube to limit dark over-occlusion on a noisy 16x16 visibility map
        }

        float w   = wTri * wDir * wVis + 1e-4;   // epsilon avoids all-zero weights
        vec3  irr = texture(TEX2D(giIrradiance), giProbeUV(idx, N, irrT, irrAtlas)).rgb;
        sumIrr += irr * w;
        sumW   += w;
    }
    return sumIrr / max(sumW, 1e-4);
}

// Indirect diffuse contribution for a surface. Falls back to the flat ambient
// constant when the volume is disabled (giOrigin.w == 0), so the engine still
// renders without GI.
vec3 giIndirectDiffuse(vec3 wp, vec3 N, vec3 V, vec3 albedo) {
    if (lights.giOrigin.w < 0.5)
        return lights.ambient.rgb * albedo;
    // Probes store cosine-weighted mean incident radiance; pi cancels for Lambert.
    return sampleIrradianceVolume(wp, N, V) * albedo * lights.giSpacing.w;
}

// Hardware-PCF shadow lookup. Returns the lit fraction in [0,1] (1 = fully lit).
float shadowFactor(int idx, vec3 worldPos, float ndotl) {
    vec4 clip = lights.shadowMatrices[idx] * vec4(worldPos, 1.0);
    vec3 proj = clip.xyz / clip.w;
    vec2 uv = proj.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || proj.z > 1.0)
        return 1.0;
    float bias = clamp(0.0015 * tan(acos(clamp(ndotl, 0.0, 1.0))), 0.0, 0.004);
    float depthRef = proj.z - bias;
    vec2 texel = 1.0 / vec2(textureSize(SHADOW2DARRAY(shadowMap), 0).xy);
    float sum = 0.0;
    float softness = lights.shadowParams.x;
    for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y)
            sum += SAMPLE_SHADOW2DARRAY(shadowMap, vec4(uv + vec2(x, y) * texel * softness, float(idx), depthRef));
    return sum / 9.0;
}

// GGX / Trowbridge-Reitz Normal Distribution Function.
// D(h) = alpha^2 / (pi * ((n·h)^2 * (alpha^2 - 1) + 1)^2)
float distributionGGX(float NdotH, float roughness) {
    float a  = roughness * roughness;
    float a2 = a * a;
    float denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * denom * denom);
}

// Smith-Schlick geometry sub-term G1 for a single direction.
// G1(n,x) = (n·x) / ((n·x)(1-k) + k)
// k = (roughness+1)^2 / 8  for direct (analytic) lighting.
float geometrySchlickG1(float NdotX, float k) {
    return NdotX / (NdotX * (1.0 - k) + k);
}

// Combined Smith geometry term: product of masking (view) and shadowing (light).
// G = G1(n,v) * G1(n,l)
float geometrySmith(float NdotV, float NdotL, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;        // k for direct lighting
    return geometrySchlickG1(NdotV, k) * geometrySchlickG1(NdotL, k);
}

// Fresnel-Schlick approximation.
// F = F0 + (1 - F0) * (1 - h·v)^5
vec3 fresnelSchlick(float HdotV, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - HdotV, 0.0, 1.0), 5.0);
}

vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    vec3 grazing = max(vec3(1.0 - roughness), F0);
    return F0 + (grazing - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec3 rotateEnvironmentDir(vec3 dir) {
    float s = sin(lights.environmentParams.w);
    float c = cos(lights.environmentParams.w);
    dir.xz = mat2(c, -s, s, c) * dir.xz;
    return dir;
}

vec2 environmentUV(vec3 dir) {
    dir = rotateEnvironmentDir(normalize(dir));
    vec2 uv = vec2(atan(dir.z, dir.x), asin(-dir.y));
    uv *= vec2(INV_TWO_PI, INV_PI);
    return uv + 0.5;
}

float environmentMaxLod() {
    return max(float(textureQueryLevels(TEX2D(iblEnvironment)) - 1), 0.0);
}

vec3 sampleEnvironmentLod(vec3 dir, float lod) {
    return textureLod(TEX2D(iblEnvironment), environmentUV(dir), lod).rgb;
}

vec3 environmentMissRadiance(vec3 dir) {
    if (lights.environmentParams.x < 0.5)
        return lights.ambient.rgb;
    return sampleEnvironmentLod(dir, 0.0) * lights.environmentParams.y;
}

// Split-sum BRDF approximation for environment specular.
vec3 environmentBRDF(vec3 F0, float roughness, float NdotV) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4( 1.0,  0.0425,  1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NdotV)) * r.x + r.y;
    vec2 ab = vec2(-1.04, 1.04) * a004 + r.zw;
    return F0 * ab.x + ab.y;
}

LightTerms environmentLighting(vec3 N, vec3 V, vec3 albedo, float metallic, float roughness) {
    LightTerms env;
    env.diffuse = vec3(0.0);
    env.specular = vec3(0.0);
    if (lights.environmentParams.x < 0.5)
        return env;

    roughness = clamp(roughness, 0.04, 1.0);
    float maxLod = environmentMaxLod();

    float NdotV = max(dot(N, V), 0.001);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 F = fresnelSchlickRoughness(NdotV, F0, roughness);
    vec3 kd = (vec3(1.0) - F) * (1.0 - metallic);

    float diffuseLod = max(maxLod - IBL_DIFFUSE_LOD_BIAS, 0.0);
    vec3 irradiance = sampleEnvironmentLod(N, diffuseLod) * lights.environmentParams.y;
    env.diffuse = kd * albedo * irradiance;

    vec3 R = reflect(-V, N);
    float specularLod = pow(roughness, IBL_SPECULAR_MIP_CURVE) * maxLod;
    vec3 prefiltered = sampleEnvironmentLod(R, specularLod);
    env.specular = prefiltered * environmentBRDF(F0, roughness, NdotV) * lights.environmentParams.z;
    return env;
}

// ---------------------------------------------------------------------------
// Per-light contribution (Cook-Torrance GGX)
// ---------------------------------------------------------------------------

// Evaluates the full Cook-Torrance specular + Lambertian diffuse for one light,
// with attenuation, spot cone falloff, and shadow already folded in.
LightTerms lightContribution(int i, vec3 N, vec3 V, vec3 wp,
                              vec3 albedo, float metallic, float roughness) {
    // Clamp roughness to avoid NaN from division-by-zero in GGX when alpha→0.
    roughness = max(roughness, 0.04);

    Light lt = lights.lights[i];
    int type = int(lt.dirType.w + 0.5);
    vec3 radiance = lt.colorInt.rgb * lt.colorInt.w;

    // --- light vector & attenuation ---
    vec3 L;
    float atten = 1.0;
    if (type == 0) {
        L = normalize(-lt.dirType.xyz);              // directional
    } else {
        vec3 toLight = lt.posRange.xyz - wp;         // point / spot
        float dist = length(toLight);
        L = toLight / max(dist, 1e-4);
        float range = lt.posRange.w;
        atten = clamp(1.0 - (dist * dist) / (range * range), 0.0, 1.0);
        atten *= atten;
        if (type == 2) {                             // spot cone falloff
            float cosAngle = dot(-L, normalize(lt.dirType.xyz));
            atten *= smoothstep(lt.spotShadow.y, lt.spotShadow.x, cosAngle);
        }
    }

    // --- shadow ---
    float ndotl = dot(N, L);
    float shadow = 1.0;
    if (lt.spotShadow.z >= 0.0)
        shadow = shadowFactor(int(lt.spotShadow.z + 0.5), wp, ndotl);
    float vis = atten * shadow;

    // --- BRDF ---
    float NdotL = max(ndotl, 0.0);
    float NdotV = max(dot(N, V), 0.001);   // avoid zero for G denominator
    vec3 H = normalize(L + V);
    float NdotH = max(dot(N, H), 0.0);
    float HdotV = max(dot(H, V), 0.0);

    // Dielectrics reflect ~4% at normal incidence; metals use the albedo color.
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    // Cook-Torrance specular: D * G * F / (4 * NdotV * NdotL)
    float D = distributionGGX(NdotH, roughness);
    float G = geometrySmith(NdotV, NdotL, roughness);
    vec3  F = fresnelSchlick(HdotV, F0);

    // The denominator can approach zero at grazing angles; clamp to avoid inf.
    vec3 specBRDF = (D * G * F) / max(4.0 * NdotV * NdotL, 0.001);

    // Energy conservation: whatever isn't reflected is refracted (diffuse).
    // Metals have no diffuse because all refracted light is absorbed.
    vec3 kd = (vec3(1.0) - F) * (1.0 - metallic);

    // Lambertian diffuse: albedo / pi  (the pi in the numerator and the
    // NdotL in the rendering equation cancel the ones in the denominator).
    vec3 diffBRDF = kd * albedo / PI;

    LightTerms t;
    t.diffuse  = diffBRDF  * radiance * NdotL * vis;
    t.specular = specBRDF  * radiance * NdotL * vis;
    return t;
}

// ---------------------------------------------------------------------------
// Convenience aggregators
// ---------------------------------------------------------------------------

// Diffuse + ambient + shadows, view-independent.
vec3 diffuseIrradiance(vec3 N, vec3 wp, vec3 albedo, float metallic, float roughness) {
    vec3 sum = giIndirectDiffuse(wp, N, N, albedo);
    sum += environmentLighting(N, N, albedo, metallic, roughness).diffuse;
    // Use the surface normal as a stand-in view dir for the diffuse-only path.
    for (int i = 0; i < lights.counts.x; ++i)
        sum += lightContribution(i, N, N, wp, albedo, metallic, roughness).diffuse;
    return sum;
}

// View-dependent specular only.
vec3 specularRadiance(vec3 N, vec3 V, vec3 wp, vec3 albedo, float metallic, float roughness) {
    vec3 sum = environmentLighting(N, V, albedo, metallic, roughness).specular;
    for (int i = 0; i < lights.counts.x; ++i)
        sum += lightContribution(i, N, V, wp, albedo, metallic, roughness).specular;
    return sum;
}

// Direct lighting only (no indirect/ambient term) — diffuse + specular with
// shadows.
LightTerms directLighting(vec3 N, vec3 V, vec3 wp, vec3 albedo, float metallic, float roughness) {
    LightTerms total;
    total.diffuse = vec3(0.0);
    total.specular = vec3(0.0);
    for (int i = 0; i < lights.counts.x; ++i) {
        LightTerms t = lightContribution(i, N, V, wp, albedo, metallic, roughness);
        total.diffuse += t.diffuse;
        total.specular += t.specular;
    }
    return total;
}

// Full realtime lighting in a single loop (diffuse incl. ambient, + specular).
LightTerms accumulate(vec3 N, vec3 V, vec3 wp, vec3 albedo, float metallic, float roughness) {
    LightTerms total;
    LightTerms env = environmentLighting(N, V, albedo, metallic, roughness);
    total.diffuse = giIndirectDiffuse(wp, N, V, albedo) + env.diffuse;
    total.specular = env.specular;
    for (int i = 0; i < lights.counts.x; ++i) {
        LightTerms t = lightContribution(i, N, V, wp, albedo, metallic, roughness);
        total.diffuse  += t.diffuse;
        total.specular += t.specular;
    }
    return total;
}
