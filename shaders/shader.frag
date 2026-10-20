#version 450
#extension GL_GOOGLE_include_directive : require

#include "lighting.glsl"
#include "surface_variation.glsl"
#include "rain_ripples.glsl"

#ifdef BINDLESS

#include "material_data.glsl"
#include "parallax.glsl"

#else

DECL_TEX2D(1, 0, 5, texAlbedo);
DECL_TEX2D(1, 1, 6, texNormal);
DECL_TEX2D(1, 2, 7, texMetallicRoughness);
#include "material_params.glsl"
DECL_TEX2D(1, 4, 8, texEmissive);
DECL_TEX2D(1, 9, 11, texSpecularColor);
DECL_TEX2D(1, 10, 12, texSpecularStrength);

#endif

PUSH_QUALIFIER PushConstants {
    mat4 model;
    vec4 params;
} push;

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec3 fragColor;
layout(location = 3) in vec2 fragTexCoord;
layout(location = 5) in vec3 fragTangent;
layout(location = 6) in vec3 fragBitangent;
layout(location = 10) in vec3 fragBillboardView;
layout(location = 11) flat in vec3 fragBillboardUp;
layout(location = 12) flat in vec3 fragBillboardNormalX;
layout(location = 13) flat in vec3 fragBillboardNormalY;
layout(location = 14) flat in vec3 fragBillboardNormalZ;

#ifdef BINDLESS
layout(location = 7) flat in uint fragMaterialIndex;
layout(location = 8) flat in uint fragDoubleSided;
layout(location = 9) flat in float fragLodFade;
#endif

layout(location = 0) out vec4 outColor;

// Screen-door LOD cross-fade (MeshNode::lodFade): a level fading in draws the
// pixels whose threshold is under its share t, the level fading out (-t) the
// others, so each pixel is drawn by exactly one of them -- no blending, no
// sorting, and nothing at all outside a transition. Interleaved gradient noise
// spreads the threshold evenly at every share, without a pattern to read.
bool lodFadeHides(float fade) {
    if (fade >= 1.0 || fade == 0.0) return false;
    float threshold = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    return fade > 0.0 ? threshold >= fade : threshold < -fade;
}

void main() {
    // Gradients first, while every invocation of the quad is still running.
    vec2 uvDx = dFdx(fragTexCoord);
    vec2 uvDy = dFdy(fragTexCoord);
    vec2 nextDx = dFdx(fragBillboardView.xy);
    vec2 nextDy = dFdy(fragBillboardView.xy);
    vec3 rainDx = vec3(0.0), rainDy = vec3(0.0);
    if (max(lights.rainParams.x, lights.rainParams.y) > 0.0) {
        rainDx = dFdx(fragWorldPos);
        rainDy = dFdy(fragWorldPos);
    }
#ifdef BINDLESS
    if (lodFadeHides(fragLodFade)) discard;
#else
    if (lodFadeHides(push.params.x)) discard;
#endif
#ifdef BINDLESS
    // Mixed materials share one indirect pipeline; sidedness remains per draw.
    if (!gl_FrontFacing && fragDoubleSided == 0u) discard;
    MaterialData mat = materials[fragMaterialIndex];
    vec4 baseColor = mat.baseColor;
    float matMetallic = mat.metallic;
    float matRoughness = mat.roughness;
    float matAO = mat.ao;
    vec4 matEmissive = mat.emissive;
    vec4 variation = mat.variation;
    float normalStrength = mat.normalStrength;
    SurfaceCoords st = warpedCoords(fragTexCoord, uvDx, uvDy, variation.x);
    if (mat.parallaxDepth > 0.0) {
        vec3 n = normalize(fragNormal), t = normalize(fragTangent), b = normalize(fragBitangent);
        vec3 toEye = normalize(lights.cameraPos.xyz - fragWorldPos);
        st = parallaxCoords(mat.heightTexIdx, mat.parallaxDepth, st,
                            vec3(dot(toEye, t), dot(toEye, b), dot(toEye, n)));
    }
    vec4 albedoSample = textureGrad(globalTextures[nonuniformEXT(mat.albedoTexIdx)], st.uv, st.dx, st.dy);
    float matAlphaCutoff = mat.alphaCutoff;
#else
    vec4 baseColor = material.baseColor;
    float matMetallic = material.metallic;
    float matRoughness = material.roughness;
    float matAO = material.ao;
    vec4 matEmissive = material.emissive;
    vec4 variation = material.variation;
    float normalStrength = material.detail.x;
    SurfaceCoords st = warpedCoords(fragTexCoord, uvDx, uvDy, variation.x);
    vec4 albedoSample = textureGrad(TEX2D(texAlbedo), st.uv, st.dx, st.dy);
    float matAlphaCutoff = material.alphaCutoff;
#endif

    float billboardNextWeight = 0.0;
    if (fragBillboardView.z >= 0.0) {
#ifdef BINDLESS
        vec4 adjacent = textureGrad(globalTextures[nonuniformEXT(mat.albedoTexIdx)], fragBillboardView.xy, nextDx, nextDy);
#else
        vec4 adjacent = textureGrad(TEX2D(texAlbedo), fragBillboardView.xy, nextDx, nextDy);
#endif
        // Coverage-weighted colour avoids dark fringes where silhouettes differ.
        float weight = fragBillboardView.z;
        float alpha = mix(albedoSample.a, adjacent.a, weight);
        billboardNextWeight = adjacent.a * weight / max(alpha, 0.0001);
        albedoSample.rgb = mix(albedoSample.rgb * albedoSample.a, adjacent.rgb * adjacent.a, weight) / max(alpha, 0.0001);
        albedoSample.a = alpha;
    }
#ifdef BINDLESS
    vec4 foliage = mat.foliage;
#else
    vec4 foliage = material.foliage;
#endif
    if (foliage.w > 0.75) {
        // Grazing depth slices contribute no convincing picture and cause
        // duplicate leaves/overdraw. Their contribution fades with view angle.
        vec3 V = normalize(lights.cameraPos.xyz - fragWorldPos);
        albedoSample.a *= smoothstep(0.08, 0.6, abs(dot(normalize(fragNormal), V)));
#ifdef BINDLESS
        vec2 texels = vec2(textureSize(globalTextures[nonuniformEXT(mat.albedoTexIdx)], 0));
#else
        vec2 texels = vec2(textureSize(TEX2D(texAlbedo), 0));
#endif
        // Preserve fine leaf coverage as a mip averages leaves with empty sky.
        float footprint = max(length(st.dx * texels), length(st.dy * texels));
        matAlphaCutoff *= mix(1.0, 0.4, clamp(log2(max(footprint, 1.0)) / 5.0, 0.0, 1.0));
    }
    // Alpha test. Cut-out art (Mario's moustache, castle windows, foliage) would
    // otherwise show the texture's black backing, since the renderer has no
    // blended pass. A cutoff of 0 disables the test entirely.
    if (albedoSample.a * baseColor.a < matAlphaCutoff) discard;

    vec3 albedo = albedoSample.rgb * fragColor * baseColor.rgb;
    if (foliage.w > 0.0) {
#ifdef BINDLESS
        vec4 surface = textureGrad(globalTextures[nonuniformEXT(mat.normalTexIdx)], st.uv, st.dx, st.dy);
#else
        vec4 surface = textureGrad(TEX2D(texNormal), st.uv, st.dx, st.dy);
#endif
        vec3 authored = surface.rgb * 2.0 - 1.0;
        mat3 frame = foliage.w > 0.75
            ? mat3(fragBillboardNormalX, fragBillboardNormalY, fragBillboardNormalZ)
            : mat3(normalize(fragTangent), normalize(fragBitangent), normalize(fragNormal));
        vec3 mapped = frame * authored;
        vec3 N = dot(mapped,mapped)>0.0001 ? normalize(mapped) : normalize(fragBillboardNormalY);
        if (foliage.w <= 0.75 && !gl_FrontFacing) N = -N;
        float visibility = foliage.w > 0.75 ? clamp(surface.a, 0.15, 1.0) : 1.0;
        vec3 irradiance = lights.ambient.rgb * mix(0.45, 1.0, visibility);
        if (lights.environmentParams.x >= 0.5 && lights.environmentParams.y > 0.0)
            irradiance += environmentIrradiance(N) * lights.environmentParams.y * visibility;
        vec3 V = normalize(lights.cameraPos.xyz - fragWorldPos);
        for (int i = 0; i < lights.counts.x; ++i) {
            Light light = lights.lights[i];
            if (light.colorInt.w <= 0.0) continue;
            vec3 delta = light.posRange.xyz - fragWorldPos;
            bool directional = light.dirType.w == 0.0;
            vec3 L = directional ? normalize(-light.dirType.xyz) : delta/max(length(delta),0.0001);
            float attenuation = directional ? 1.0 : pow(clamp(1.0 - dot(delta,delta) / max(light.posRange.w * light.posRange.w, 0.0001), 0.0, 1.0), 2.0);
            if (light.dirType.w == 2.0)
                attenuation *= smoothstep(light.spotShadow.y, light.spotShadow.x, dot(-L, normalize(light.dirType.xyz)));
            float diffuse = max(dot(N, L) + foliage.x, 0.0) / pow(1.0 + foliage.x, 2.0);
            float transmission = foliage.y * pow(max(dot(-L, V), 0.0), 3.0) * (1.0 - abs(dot(N, L)));
            float occlusion = directional && int(lights.sunOcclusion.x) == i ? sunOcclusionAt(fragWorldPos) : 1.0;
            if (light.spotShadow.z >= 0.0)
                occlusion *= shadowFactor(int(light.spotShadow.z + 0.5), fragWorldPos, dot(N,L));
            irradiance += light.colorInt.rgb * light.colorInt.w * attenuation * occlusion
                          * (diffuse * 0.31830988618 + transmission) * mix(0.65, 1.0, visibility);
        }
        outColor = vec4(albedo * irradiance * matAO, albedoSample.a * baseColor.a);
        return;
    }
    if (fragBillboardView.z >= 0.0) {
        // Optional authored-frame normals and static visibility restore surface
        // contrast without evaluating PBR or sampling runtime shadow maps.
        const float canopyResponse = 0.2;
        const float lateralResponse = 0.35;
        const float diffuseResponse = 0.31830988618;
        vec3 surfaceNormal = fragBillboardUp;
        float visibility = 1.0;
        if (normalStrength > 0.0) {
#ifdef BINDLESS
            vec4 surface = textureGrad(globalTextures[nonuniformEXT(mat.normalTexIdx)], st.uv, st.dx, st.dy);
            vec4 nextSurface = textureGrad(globalTextures[nonuniformEXT(mat.normalTexIdx)], fragBillboardView.xy, nextDx, nextDy);
#else
            vec4 surface = textureGrad(TEX2D(texNormal), st.uv, st.dx, st.dy);
            vec4 nextSurface = textureGrad(TEX2D(texNormal), fragBillboardView.xy, nextDx, nextDy);
#endif
            surface = mix(surface, nextSurface, billboardNextWeight);
            vec3 authoredNormal = surface.rgb * 2.0 - 1.0;
            if (dot(authoredNormal, authoredNormal) > 0.0001)
                surfaceNormal = normalize(mat3(fragBillboardNormalX, fragBillboardNormalY,
                                               fragBillboardNormalZ) * authoredNormal);
            visibility = clamp(surface.a, 0.0, 1.0);
        }
        // Match the mesh path's flat ambient fill. The scene may already use
        // it for scattered sky light instead of IBL; multiplying this fallback
        // by atlas AO would introduce a dark discontinuity at the LOD switch.
        // Hemisphere visibility occludes directional sky SH, never the sun.
        // SH uses the mesh coefficients without environment texture fetches.
        vec3 irradiance = lights.ambient.rgb;
        if (lights.environmentParams.x >= 0.5 && lights.environmentParams.y > 0.0)
            irradiance += environmentIrradiance(surfaceNormal) * lights.environmentParams.y * visibility;
        for (int i = 0; i < lights.counts.x; ++i) {
            Light light = lights.lights[i];
            if (light.dirType.w != 0.0 || light.colorInt.w <= 0.0) continue;
            float elevation = max(dot(fragBillboardUp, normalize(-light.dirType.xyz)), 0.0);
            float response = canopyResponse * mix(lateralResponse, 1.0, elevation);
            if (normalStrength > 0.0) {
                float diffuse = max(dot(surfaceNormal, normalize(-light.dirType.xyz)), 0.0);
                response = mix(response, diffuseResponse * diffuse, clamp(normalStrength, 0.0, 1.0));
            }
            irradiance += light.colorInt.rgb * light.colorInt.w * response;
        }
        outColor = vec4(albedo * irradiance * matAO, albedoSample.a * baseColor.a);
        return;
    }
    // Macro variation: brightness and slope over several repeats, mean zero.
    vec3 macro = vec3(0.0);
    if (variation.y > 0.0 && (variation.z > 0.0 || variation.w > 0.0)) {
        macro = macroVariation(st.uv, st.dx, st.dy, variation.y);
        albedo *= 1.0 + variation.z * macro.x;
    }

#ifdef BINDLESS
    // Preserve the non-PBR path in the shared bindless pipeline.
    if (mat.materialType == 1u) {
        vec3 emissive = textureGrad(globalTextures[nonuniformEXT(mat.emissiveTexIdx)], st.uv, st.dx, st.dy).rgb
                      * matEmissive.rgb;
        outColor = vec4(albedo + emissive, albedoSample.a * baseColor.a);
        return;
    }
#endif

    // GI debug: snap to voxel centers so the grid is visible as blocks.
    if (lights.giAtlas.z == 1) {
        float res = float(lights.giAtlas.w);
        vec3 uvw = giVolumeUVW(fragWorldPos);
        vec3 snapped = (floor(uvw * res) + 0.5) / res;
        outColor = vec4(textureLod(TEX3D(giVoxels), snapped, 0.0).rgb, 1.0);
        return;
    }

    vec3 N = normalize(fragNormal);
    vec3 T = normalize(fragTangent);
    vec3 B = normalize(fragBitangent);
    mat3 TBN = mat3(T, B, N);

#ifdef BINDLESS
    vec4 normalTexel = textureGrad(globalTextures[nonuniformEXT(mat.normalTexIdx)], st.uv, st.dx, st.dy);
#else
    vec4 normalTexel = textureGrad(TEX2D(texNormal), st.uv, st.dx, st.dy);
#endif
    vec3 normalSample = normalTexel.xyz;
    if (abs(normalSample.x - 0.5) > 0.01 || abs(normalSample.y - 0.5) > 0.01) {
        vec3 tangentNormal = normalSample * 2.0 - 1.0;
        // Only a material that asks for variation is touched: every other
        // one draws its normal map exactly as authored.
        if (any(greaterThan(variation, vec4(0.0))))
            tangentNormal.xy *= normalStrength * surfaceNormalWeight(st.dx, st.dy);
        N = normalize(TBN * tangentNormal);
    }
    // The macro slope tilts the normal along the texture's own axes.
    if (variation.w > 0.0) N = normalize(N - variation.w * (macro.y * T + macro.z * B));
    // Reverse the complete mapped normal on the back of a two-sided surface.
    if (!gl_FrontFacing) N = -N;

#ifdef BINDLESS
    vec3 mrSample = textureGrad(globalTextures[nonuniformEXT(mat.metallicRoughnessTexIdx)], st.uv, st.dx, st.dy).rgb;
#else
    vec3 mrSample = textureGrad(TEX2D(texMetallicRoughness), st.uv, st.dx, st.dy).rgb;
#endif
    float roughness = mrSample.g * matRoughness;
    float metallic = mrSample.b * matMetallic;

#ifdef BINDLESS
    vec4 reflectance = mat.specular;
    if (mat.reflection.y >= 0.0)
        reflectance.rgb *= textureGrad(globalTextures[nonuniformEXT(uint(mat.reflection.y))], st.uv, st.dx, st.dy).rgb;
    if (mat.reflection.z >= 0.0)
        reflectance.a *= textureGrad(globalTextures[nonuniformEXT(uint(mat.reflection.z))], st.uv, st.dx, st.dy).a;
#else
    vec4 reflectance = material.specular;
    if (material.detail.z > 0.5)
        reflectance.rgb *= textureGrad(TEX2D(texSpecularColor), st.uv, st.dx, st.dy).rgb;
    if (material.detail.w > 0.5)
        reflectance.a *= textureGrad(TEX2D(texSpecularStrength), st.uv, st.dx, st.dy).a;
#endif
    // Clamp F0 after the color texture, then apply the dielectric lobe weight.
    // This preserves conversions from specular-glossiness with high/zero IOR.
    float dielectricF90 = clamp(reflectance.a, 0.0, 1.0);
    vec3 dielectricF0 = clamp(reflectance.rgb, vec3(0.0), vec3(1.0)) * dielectricF90;
    vec3 geometricNormal = gl_FrontFacing ? normalize(fragNormal) : -normalize(fragNormal);
#ifdef BINDLESS
    vec4 rainReception = mat.rain;
#else
    vec4 rainReception = material.rain;
#endif
    applyRainSurface(fragWorldPos, geometricNormal, rainDx, rainDy, rainReception,
                     N, albedo, roughness, dielectricF0, dielectricF90);

    vec3 V = normalize(lights.cameraPos.xyz - fragWorldPos);

    LightTerms t = accumulateReflectance(N, V, fragWorldPos, albedo, metallic, roughness,
                                         dielectricF0, dielectricF90);
    vec3 lit = t.diffuse + t.specular;
#ifdef BINDLESS
    float reflection = mat.reflection.x;
#else
    float reflection = material.detail.y;
#endif
    // Where the scene's IBL is on, `accumulate` already reflected it.
    if (reflection > 0.0 && lights.environmentParams.x < 0.5)
        lit += environmentReflection(N, V, albedo, metallic, roughness,
                                      dielectricF0, dielectricF90) * reflection;

    lit *= matAO;
    
#ifdef BINDLESS
    vec3 emissive = textureGrad(globalTextures[nonuniformEXT(mat.emissiveTexIdx)], st.uv, st.dx, st.dy).rgb * matEmissive.rgb;
#else
    vec3 emissive = textureGrad(TEX2D(texEmissive), st.uv, st.dx, st.dy).rgb * material.emissive.rgb;
#endif

    outColor = vec4(lit + emissive, albedoSample.a * baseColor.a);
}
