#version 450
#extension GL_GOOGLE_include_directive : require

#include "lighting.glsl"
#include "surface_variation.glsl"

#ifdef BINDLESS

#include "material_data.glsl"
#include "parallax.glsl"

#else

DECL_TEX2D(1, 0, 5, texAlbedo);
DECL_TEX2D(1, 1, 6, texNormal);
DECL_TEX2D(1, 2, 7, texMetallicRoughness);
layout(set = 1, binding = 3) uniform MaterialUBO {
    vec4 baseColor;
    float metallic;
    float roughness;
    float ao;
    float alphaCutoff;
    vec4 emissive;
    vec4 variation;  // MaterialDesc::variation
    vec4 detail;     // x normal strength, y environment reflection
    vec4 specular;   // rgb dielectric F0, a dielectric lobe strength
} material;
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

    if (fragBillboardView.z >= 0.0) {
#ifdef BINDLESS
        vec4 adjacent = textureGrad(globalTextures[nonuniformEXT(mat.albedoTexIdx)], fragBillboardView.xy, nextDx, nextDy);
#else
        vec4 adjacent = textureGrad(TEX2D(texAlbedo), fragBillboardView.xy, nextDx, nextDy);
#endif
        // Coverage-weighted colour avoids dark fringes where silhouettes differ.
        float weight = fragBillboardView.z;
        float alpha = mix(albedoSample.a, adjacent.a, weight);
        albedoSample.rgb = mix(albedoSample.rgb * albedoSample.a, adjacent.rgb * adjacent.a, weight) / max(alpha, 0.0001);
        albedoSample.a = alpha;
    }
    // Alpha test. Cut-out art (Mario's moustache, castle windows, foliage) would
    // otherwise show the texture's black backing, since the renderer has no
    // blended pass. A cutoff of 0 disables the test entirely.
    if (albedoSample.a * baseColor.a < matAlphaCutoff) discard;

    vec3 albedo = albedoSample.rgb * fragColor * baseColor.rgb;
    if (fragBillboardView.z >= 0.0) {
        // Distant impostors represent a whole volume, not a camera-facing
        // leaf. A broad directional response uses the authored up axis so
        // turning the camera never changes illumination. Only two albedo
        // fetches: no normal/MR maps, shadows, GGX, IBL or GI evaluation.
        const float canopyResponse = 0.2;
        const float lateralResponse = 0.35;
        vec3 irradiance = lights.ambient.rgb;
        for (int i = 0; i < lights.counts.x; ++i) {
            Light light = lights.lights[i];
            if (light.dirType.w != 0.0 || light.colorInt.w <= 0.0) continue;
            float elevation = max(dot(fragBillboardUp, normalize(-light.dirType.xyz)), 0.0);
            irradiance += light.colorInt.rgb * light.colorInt.w
                        * canopyResponse * mix(lateralResponse, 1.0, elevation);
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
