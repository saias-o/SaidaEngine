// Shared PBR lighting math for the scene shaders.

#include "ddgi_common.glsl"
#include "web_compat.glsl"

const float PI = 3.14159265359;
#include "lighting_data.glsl"

DECL_SHADOW2DARRAY(0, 2, 8, shadowMap);

// DDGI atlases: one octahedral tile per probe, laid out in a 2D atlas. Irradiance
// holds incident radiance (rgb), visibility holds (mean dist, mean dist^2) for the
// Chebyshev occlusion test. Both are LINEAR + CLAMP sampled.
#include "gi_sampling.glsl"

// Voxelized scene albedo (the GI ray-march source). Sampled here only for the
// debug visualization; the DDGI update compute pass reads it directly.
DECL_TEX3D(0, 6, 11, giVoxels);

// Canonical equirectangular environment. Reused by skybox, IBL and DDGI misses.
DECL_TEX2D(0, 7, 12, iblEnvironment);
DECL_TEX2D(0, 15, 16, iblBlendEnvironment);

// Where the terrain hides the light: r the height below which a point is in
// its shadow, g the penumbra there (metres of height), wider the farther away
// what hides it stands.
DECL_TEX2D(0, 13, 14, sunOcclusionMap);

// The narrowest penumbra (m): what hides the light is never a knife edge.
const float MIN_SUN_PENUMBRA = 1.0;

// How much of the occluded light reaches `wp`, 1 outside the map.
float sunOcclusionAt(vec3 wp) {
    vec3 m = (lights.sunOcclusionToMap * vec4(wp, 1.0)).xyz;
    if (any(lessThan(m.xy, vec2(0.0))) || any(greaterThan(m.xy, vec2(1.0)))) return 1.0;
    vec2 hold = textureLod(TEX2D(sunOcclusionMap), m.xy, 0.0).rg;
    return clamp(0.5 + (m.z - hold.x) / max(hold.y, MIN_SUN_PENUMBRA), 0.0, 1.0);
}

const float INV_TWO_PI = 0.15915494309;
const float INV_PI = 0.31830988618;
const float IBL_SPECULAR_MIP_CURVE = 1.5;

struct LightTerms { vec3 diffuse; vec3 specular; };

#ifndef SAIDA_LIGHTING_COMPUTE
vec3 shadowWorldDx;
vec3 shadowWorldDy;

// Call before any discard or material branch, including the billboard path.
// WebGPU requires derivatives in uniform control flow.
void prepareShadowReceiver(vec3 worldPos) {
    shadowWorldDx = dFdx(worldPos);
    shadowWorldDy = dFdy(worldPos);
}
#endif

// Receiver-plane PCF. Returns the lit fraction in [0,1] (1 = fully lit).
float shadowFactor(int idx, vec3 worldPos) {
#ifdef SAIDA_LIGHTING_COMPUTE
    // GI traces voxel visibility; camera shadow maps do not cover its rays.
    return 1.0;
#else
    vec4 clip = lights.shadowMatrices[idx] * vec4(worldPos, 1.0);
    vec3 proj = clip.xyz / clip.w;
    vec2 uv = proj.xy * 0.5 + 0.5;
    // Derive the geometric receiver plane, independently of normal maps. Each
    // PCF tap compares at that plane's depth, rather than subtracting a fixed
    // fraction of the light's entire range (metres at grazing incidence).
    vec4 clipDx = lights.shadowMatrices[idx] * vec4(shadowWorldDx, 0.0);
    vec4 clipDy = lights.shadowMatrices[idx] * vec4(shadowWorldDy, 0.0);
    vec3 dx = (clipDx.xyz - proj * clipDx.w) / clip.w;
    vec3 dy = (clipDy.xyz - proj * clipDy.w) / clip.w;
    dx.xy *= 0.5;
    dy.xy *= 0.5;
    float determinant = dx.x * dy.y - dx.y * dy.x;
    vec2 depthGradient = vec2(0.0);
    if (abs(determinant) > 1e-12)
        depthGradient = vec2(dy.y * dx.z - dx.y * dy.z,
                             dx.x * dy.z - dy.x * dx.z) / determinant;
    if (clip.w <= 0.0 || uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 ||
        proj.z < 0.0 || proj.z > 1.0)
        return 1.0;
    vec2 texel = 1.0 / vec2(textureSize(SHADOW2DARRAY(shadowMap), 0).xy);
    // Adjacent triangles need not have exactly the same plane. Reserve a small
    // fraction of a shadow texel for that discontinuity, rather than exposing
    // every triangulation edge or displacing depth by a fraction of the range.
    const float receiverTexelAllowance = 0.1;
    float bias = lights.shadowParams.y + receiverTexelAllowance * dot(abs(depthGradient), texel);
    float sum = 0.0;
    float softness = lights.shadowParams.x;
    if (softness <= 1.0) {
        // The nine bilinear footprints overlap here: combine their weights and
        // read the sixteen distinct texels once, keeping exactly the same filter.
        vec2 tap = uv / texel - 0.5;
        vec2 base = floor(tap);
        vec2 blend = fract(tap);
        for (int a = -1; a <= 2; ++a)
            for (int b = -1; b <= 2; ++b) {
                vec2 relative = vec2(a, b) - blend;
                vec2 weight = max(1.0 - abs(relative + softness), vec2(0.0)) +
                              max(1.0 - abs(relative), vec2(0.0)) +
                              max(1.0 - abs(relative - softness), vec2(0.0));
                vec2 sampleUV = (base + vec2(a, b) + 0.5) * texel;
                float depthRef = proj.z + dot(depthGradient, sampleUV - uv) - bias;
                sum += weight.x * weight.y * SAMPLE_SHADOW2DARRAY(shadowMap,
                    vec4(sampleUV, float(idx), depthRef));
            }
        return sum / 9.0;
    }
    for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y)
        {
            vec2 tap = (uv + vec2(x, y) * texel * softness) / texel - 0.5;
            vec2 base = floor(tap);
            vec2 blend = fract(tap);
            // Hardware PCF shares one reference across four texels, which still
            // self-shadows a grazing plane. Compare at each texel's centre and
            // interpolate the results with the same bilinear weights instead.
            for (int a = 0; a <= 1; ++a)
                for (int b = 0; b <= 1; ++b) {
                    vec2 sampleUV = (base + vec2(a, b) + 0.5) * texel;
                    float depthRef = proj.z + dot(depthGradient, sampleUV - uv) - bias;
                    vec2 weight = mix(1.0 - blend, blend, vec2(a, b));
                    sum += weight.x * weight.y * SAMPLE_SHADOW2DARRAY(shadowMap,
                        vec4(sampleUV, float(idx), depthRef));
                }
        }
    return sum / 9.0;
#endif
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

vec3 rotateEnvironmentDir(vec3 dir, float rotation) {
    float s = sin(rotation);
    float c = cos(rotation);
    dir.xz = mat2(c, -s, s, c) * dir.xz;
    return dir;
}

vec2 environmentUV(vec3 dir, float rotation) {
    dir = rotateEnvironmentDir(normalize(dir), rotation);
    vec2 uv = vec2(atan(dir.z, dir.x), asin(-dir.y));
    uv *= vec2(INV_TWO_PI, INV_PI);
    return uv + 0.5;
}

float environmentMaxLod() {
    float levels = float(textureQueryLevels(TEX2D(iblEnvironment)) - 1);
    if (lights.environmentBlendParams.x > 0.0)
        levels = max(levels, float(textureQueryLevels(TEX2D(iblBlendEnvironment)) - 1));
    return max(levels, 0.0);
}

vec3 sampleEnvironmentLod(vec3 dir, float lod) {
    // Match the background's linear crossfade and independent rotations.
    // Normalize LOD so skies with different resolutions have the same blur.
    float fraction = lod / max(environmentMaxLod(), 1.0);
    float primaryLod = fraction * float(textureQueryLevels(TEX2D(iblEnvironment)) - 1);
    vec3 color = textureLod(TEX2D(iblEnvironment),
        environmentUV(dir, lights.environmentParams.w), primaryLod).rgb;
    if (lights.environmentBlendParams.x > 0.0) {
        float secondaryLod = fraction * float(textureQueryLevels(TEX2D(iblBlendEnvironment)) - 1);
        vec3 other = textureLod(TEX2D(iblBlendEnvironment),
            environmentUV(dir, lights.environmentBlendParams.y), secondaryLod).rgb;
        color = mix(color, other, lights.environmentBlendParams.x);
    }
    return color;
}

// What a GI ray that escapes every voxel sees: the sky as shown, softened four
// mips (a probe's few rays would turn clouds into noise), plus the flat
// ambient -- the very light the legacy fallback applies, so open ground reads
// the same with GI on or off.
vec3 environmentMissRadiance(vec3 dir) {
    if (lights.environmentParams.x < 0.5 || lights.environmentParams.y <= 0.0)
        return lights.ambient.rgb;
    return lights.ambient.rgb
         + sampleEnvironmentLod(dir, max(environmentMaxLod() - 4.0, 0.0)) * lights.environmentParams.y;
}

// Lambertian irradiance from the environment, reconstructed from its order-2 SH.
//
// The alternative — one sample of a coarse mip of the equirectangular map — is
// close to the environment's average for EVERY normal, so it adds a flat wash
// with no horizon: a floor and a ceiling receive the same light. Nine
// coefficients restore the direction for a few multiply-adds, which is the whole
// reason diffuse IBL is worth having.
vec3 evaluateEnvironmentSH(vec3 n, vec4 coefficients[9]) {
    vec3 e = coefficients[0].rgb
           + coefficients[1].rgb * n.y
           + coefficients[2].rgb * n.z
           + coefficients[3].rgb * n.x
           + coefficients[4].rgb * (n.x * n.y)
           + coefficients[5].rgb * (n.y * n.z)
           + coefficients[6].rgb * (3.0 * n.z * n.z - 1.0)
           + coefficients[7].rgb * (n.x * n.z)
           + coefficients[8].rgb * (n.x * n.x - n.y * n.y);
    // A truncated SH series can ring below zero on high-contrast skies; negative
    // light is not a thing.
    return max(e, vec3(0.0));
}

vec3 environmentIrradiance(vec3 N) {
    vec3 n = normalize(N);
    vec3 color = evaluateEnvironmentSH(rotateEnvironmentDir(n, lights.environmentParams.w),
                                       lights.environmentSH);
    if (lights.environmentBlendParams.x > 0.0) {
        vec3 other = evaluateEnvironmentSH(rotateEnvironmentDir(n, lights.environmentBlendParams.y),
                                           lights.environmentBlendSH);
        color = mix(color, other, lights.environmentBlendParams.x);
    }
    return color;
}

// Split-sum BRDF approximation for environment specular.
vec3 environmentBRDF(vec3 F0, float roughness, float NdotV, float F90) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4( 1.0,  0.0425,  1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NdotV)) * r.x + r.y;
    vec2 ab = vec2(-1.04, 1.04) * a004 + r.zw;
    return F0 * ab.x + F90 * ab.y;
}
vec3 environmentBRDF(vec3 F0, float roughness, float NdotV) {
    return environmentBRDF(F0, roughness, NdotV, 1.0);
}

LightTerms environmentLighting(vec3 N, vec3 V, vec3 albedo, float metallic, float roughness,
                               vec3 dielectricF0, float dielectricF90) {
    LightTerms env;
    env.diffuse = vec3(0.0);
    env.specular = vec3(0.0);
    if (lights.environmentParams.x < 0.5)
        return env;

    roughness = clamp(roughness, 0.04, 1.0);
    float maxLod = environmentMaxLod();

    float NdotV = max(dot(N, V), 0.001);
    vec3 F0 = mix(dielectricF0, albedo, metallic);
    float F90 = mix(dielectricF90, 1.0, metallic);
    vec3 grazing = max(vec3(F90 * (1.0 - roughness)), F0);
    vec3 F = F0 + (grazing - F0) * pow(clamp(1.0 - NdotV, 0.0, 1.0), 5.0);
    vec3 kd = vec3(1.0 - max(F.r, max(F.g, F.b))) * (1.0 - metallic);

    vec3 irradiance = environmentIrradiance(N) * lights.environmentParams.y;
    env.diffuse = kd * albedo * irradiance;

    vec3 R = reflect(-V, N);
    float specularLod = pow(roughness, IBL_SPECULAR_MIP_CURVE) * maxLod;
    vec3 prefiltered = sampleEnvironmentLod(R, specularLod);
    env.specular = prefiltered * environmentBRDF(F0, roughness, NdotV, F90) * lights.environmentParams.z;
    return env;
}
LightTerms environmentLighting(vec3 N, vec3 V, vec3 albedo, float metallic, float roughness) {
    return environmentLighting(N, V, albedo, metallic, roughness, vec3(0.04), 1.0);
}

// Roughness over which the environment's reflection fades out of
// `environmentReflection`: a rough surface's reflection of the sky is a blur
// the ambient term already stands for, so only a surface smooth enough to
// mirror it adds it.
const float MIRROR_ROUGHNESS_FULL = 0.2;
const float MIRROR_ROUGHNESS_NONE = 0.45;

// The environment's specular term alone, at the scene's specular intensity,
// whether or not image-based lighting is on (MaterialDesc::environmentReflection).
vec3 environmentReflection(vec3 N, vec3 V, vec3 albedo, float metallic, float roughness,
                           vec3 dielectricF0, float dielectricF90) {
    float mirror = 1.0 - smoothstep(MIRROR_ROUGHNESS_FULL, MIRROR_ROUGHNESS_NONE, roughness);
    if (mirror <= 0.0) return vec3(0.0);
    roughness = clamp(roughness, 0.04, 1.0);
    float NdotV = max(dot(N, V), 0.001);
    vec3 F0 = mix(dielectricF0, albedo, metallic);
    float F90 = mix(dielectricF90, 1.0, metallic);
    vec3 R = reflect(-V, N);
    float lod = pow(roughness, IBL_SPECULAR_MIP_CURVE) * environmentMaxLod();
    return sampleEnvironmentLod(R, lod) * environmentBRDF(F0, roughness, NdotV, F90) *
           lights.environmentParams.z * mirror;
}
vec3 environmentReflection(vec3 N, vec3 V, vec3 albedo, float metallic, float roughness) {
    return environmentReflection(N, V, albedo, metallic, roughness, vec3(0.04), 1.0);
}

// ---------------------------------------------------------------------------
// Per-light contribution (Cook-Torrance GGX)
// ---------------------------------------------------------------------------

// Evaluates the full Cook-Torrance specular + Lambertian diffuse for one light,
// with attenuation, spot cone falloff, and shadow already folded in.
// `visibility` scales the light on top of its shadow map: what a surface
// knows of its own shadow (TerrainRingsNode). `readSunOcclusion` false skips
// the terrain's occlusion map for a surface that already counted it.
LightTerms lightContributionOccluded(int i, vec3 N, vec3 V, vec3 wp,
                                     vec3 albedo, float metallic, float roughness,
                                     float visibility, bool readSunOcclusion,
                                     vec3 dielectricF0, float dielectricF90) {
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
        shadow = shadowFactor(int(lt.spotShadow.z + 0.5), wp);
    shadow *= visibility;
    if (readSunOcclusion && i == int(lights.sunOcclusion.x)) shadow *= sunOcclusionAt(wp);
    float vis = atten * shadow;

    // --- BRDF ---
    float NdotL = max(ndotl, 0.0);
    float NdotV = max(dot(N, V), 0.001);   // avoid zero for G denominator
    vec3 H = normalize(L + V);
    float NdotH = max(dot(N, H), 0.0);
    float HdotV = max(dot(H, V), 0.0);

    // Dielectrics reflect ~4% at normal incidence; metals use the albedo color.
    vec3 F0 = mix(dielectricF0, albedo, metallic);
    float F90 = mix(dielectricF90, 1.0, metallic);

    // Cook-Torrance specular: D * G * F / (4 * NdotV * NdotL)
    float D = distributionGGX(NdotH, roughness);
    float G = geometrySmith(NdotV, NdotL, roughness);
    vec3 F = F0 + (F90 - F0) * pow(clamp(1.0 - HdotV, 0.0, 1.0), 5.0);

    // The denominator can approach zero at grazing angles; clamp to avoid inf.
    vec3 specBRDF = (D * G * F) / max(4.0 * NdotV * NdotL, 0.001);

    // Energy conservation: whatever isn't reflected is refracted (diffuse).
    // Metals have no diffuse because all refracted light is absorbed.
    vec3 kd = vec3(1.0 - max(F.r, max(F.g, F.b))) * (1.0 - metallic);

    // Lambertian diffuse: albedo / pi  (the pi in the numerator and the
    // NdotL in the rendering equation cancel the ones in the denominator).
    vec3 diffBRDF = kd * albedo / PI;

    LightTerms t;
    t.diffuse  = diffBRDF  * radiance * NdotL * vis;
    t.specular = specBRDF  * radiance * NdotL * vis;
    return t;
}
LightTerms lightContributionOccluded(int i, vec3 N, vec3 V, vec3 wp,
                                     vec3 albedo, float metallic, float roughness,
                                     float visibility, bool readSunOcclusion) {
    return lightContributionOccluded(i, N, V, wp, albedo, metallic, roughness,
                                     visibility, readSunOcclusion, vec3(0.04), 1.0);
}

LightTerms lightContribution(int i, vec3 N, vec3 V, vec3 wp,
                              vec3 albedo, float metallic, float roughness) {
    return lightContributionOccluded(i, N, V, wp, albedo, metallic, roughness, 1.0, true);
}

// ---------------------------------------------------------------------------
// Preserve the legacy disabled path, and replace sky diffuse only inside ready GI.
// `irradiance` is what GI says the surface receives, over `coverage` of it.
void composeGIIrradiance(inout LightTerms env,vec3 irradiance,float coverage,vec3 N,vec3 V,vec3 albedo,float metallic,float roughness,vec3 dielectricF0,float dielectricF90) {
    vec3 F0=mix(dielectricF0,albedo,metallic);
    float F90=mix(dielectricF90,1.0,metallic);
    vec3 grazing=max(vec3(F90*(1.0-clamp(roughness,.04,1.0))),F0);
    vec3 F=F0+(grazing-F0)*pow(clamp(1.0-max(dot(N,V),.001),0.0,1.0),5.0);
    float kd=(1.0-max(F.r,max(F.g,F.b)))*(1.0-metallic);
    vec3 reference=lights.ambient.rgb;
    if(lights.environmentParams.x>.5 && lights.environmentParams.y>0.0) reference=environmentIrradiance(N)*lights.environmentParams.y;
    float openness=clamp(dot(irradiance,vec3(.2126,.7152,.0722))/max(dot(reference,vec3(.2126,.7152,.0722)),.001),0.0,1.0);
    env.diffuse=mix(env.diffuse,irradiance*albedo*kd,coverage);
    env.specular*=mix(1.0,openness,coverage);
}
void composeGISample(inout LightTerms env,vec4 gi,vec3 N,vec3 V,vec3 albedo,float metallic,float roughness,vec3 dielectricF0,float dielectricF90) {
    env.diffuse+=lights.ambient.rgb*albedo;
    if(lights.giOrigin.w<.5) return;
    composeGIIrradiance(env,gi.rgb*lights.giSpacing.w,gi.a,N,V,albedo,metallic,roughness,dielectricF0,dielectricF90);
}

// What the surface receives as diffuse indirect light: the probes' irradiance
// where they cover it, the legacy ambient and diffuse sky past them. The AO
// pass's GI stage evaluates it at half resolution (ambient_occlusion_gi.frag).
vec3 giIrradianceOrSky(vec3 wp,vec3 N,vec3 V) {
    vec3 sky=lights.ambient.rgb;
    if(lights.environmentParams.x>.5 && lights.environmentParams.y>0.0) sky+=environmentIrradiance(N)*lights.environmentParams.y;
    vec4 gi=sampleGI(wp,N,V);
    return mix(sky,gi.rgb*lights.giSpacing.w,gi.a);
}

#ifdef SAIDA_SCREEN_GI
// Shaders whose surfaces the depth pre-pass draws read the AO pass's GI stage
// back instead of sampling the probes per fragment (screen_ambient_occlusion.glsl).
vec3 screenIndirectIrradiance();
#endif

void composeGI(inout LightTerms env,vec3 wp,vec3 N,vec3 V,vec3 albedo,float metallic,float roughness,vec3 dielectricF0,float dielectricF90) {
#ifdef SAIDA_SCREEN_GI
    if(lights.ambientOcclusion.y>.5) {
        env.diffuse+=lights.ambient.rgb*albedo;
        composeGIIrradiance(env,screenIndirectIrradiance(),1.0,N,V,albedo,metallic,roughness,dielectricF0,dielectricF90);
        return;
    }
#endif
    composeGISample(env,sampleGI(wp,N,V),N,V,albedo,metallic,roughness,dielectricF0,dielectricF90);
}

// Convenience aggregators
// ---------------------------------------------------------------------------

// Diffuse + ambient + shadows, view-independent.
vec3 diffuseIrradiance(vec3 N, vec3 wp, vec3 albedo, float metallic, float roughness) {
    LightTerms indirect = environmentLighting(N, N, albedo, metallic, roughness);
    composeGI(indirect,wp,N,N,albedo,metallic,roughness,vec3(.04),1.0);
    vec3 sum = indirect.diffuse;
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
// As `accumulate`, light `occluded` (an index into lights.lights; -1 for
// none) scaled by `visibility`. `indirectOcclusion` is the surface's ambient
// occlusion -- its material's and the screen-space map's -- and scales the
// indirect terms alone (GI, ambient, environment): it describes how much sky
// and bounce light a crease receives, while what blocks a light is its shadow.
LightTerms accumulateOccluded(vec3 N, vec3 V, vec3 wp, vec3 albedo, float metallic,
                              float roughness, int occluded, float visibility,
                              float indirectOcclusion) {
    LightTerms total;
    LightTerms env = environmentLighting(N, V, albedo, metallic, roughness);
    composeGI(env,wp,N,V,albedo,metallic,roughness,vec3(.04),1.0);
    total.diffuse = env.diffuse * indirectOcclusion;
    total.specular = env.specular * indirectOcclusion;
    for (int i = 0; i < lights.counts.x; ++i) {
        LightTerms t = lightContributionOccluded(i, N, V, wp, albedo, metallic, roughness,
                                                 i == occluded ? visibility : 1.0, i != occluded);
        total.diffuse  += t.diffuse;
        total.specular += t.specular;
    }
    return total;
}

LightTerms accumulate(vec3 N, vec3 V, vec3 wp, vec3 albedo, float metallic, float roughness,
                      float indirectOcclusion) {
    return accumulateOccluded(N, V, wp, albedo, metallic, roughness, -1, 1.0, indirectOcclusion);
}

LightTerms accumulateReflectanceOccluded(vec3 N, vec3 V, vec3 wp, vec3 albedo, float metallic,
                                          float roughness, int occluded, float visibility,
                                          vec3 dielectricF0, float dielectricF90,
                                          float indirectOcclusion) {
    LightTerms total = environmentLighting(N, V, albedo, metallic, roughness,
                                           dielectricF0, dielectricF90);
    composeGI(total,wp,N,V,albedo,metallic,roughness,dielectricF0,dielectricF90);
    total.diffuse *= indirectOcclusion;
    total.specular *= indirectOcclusion;
    for (int i = 0; i < lights.counts.x; ++i) {
        LightTerms t = lightContributionOccluded(i, N, V, wp, albedo, metallic, roughness,
                                                 i == occluded ? visibility : 1.0, i != occluded,
                                                 dielectricF0, dielectricF90);
        total.diffuse += t.diffuse;
        total.specular += t.specular;
    }
    return total;
}

LightTerms accumulateReflectance(vec3 N, vec3 V, vec3 wp, vec3 albedo, float metallic,
                                 float roughness, vec3 dielectricF0, float dielectricF90,
                                 float indirectOcclusion) {
    return accumulateReflectanceOccluded(N, V, wp, albedo, metallic, roughness,
                                          -1, 1.0, dielectricF0, dielectricF90, indirectOcclusion);
}

// Material opt-out retains exactly the legacy ambient and diffuse sky path.
LightTerms accumulateReflectanceMaterial(vec3 N,vec3 V,vec3 wp,vec3 albedo,float metallic,float roughness,
                                        vec3 dielectricF0,float dielectricF90,float ao,bool receiveGI) {
    if(receiveGI) return accumulateReflectance(N,V,wp,albedo,metallic,roughness,dielectricF0,dielectricF90,ao);
    LightTerms total=environmentLighting(N,V,albedo,metallic,roughness,dielectricF0,dielectricF90);
    total.diffuse=(total.diffuse+lights.ambient.rgb*albedo)*ao;total.specular*=ao;
    for(int i=0;i<lights.counts.x;++i) {
        LightTerms term=lightContributionOccluded(i,N,V,wp,albedo,metallic,roughness,1.0,true,dielectricF0,dielectricF90);
        total.diffuse+=term.diffuse;total.specular+=term.specular;
    }
    return total;
}
