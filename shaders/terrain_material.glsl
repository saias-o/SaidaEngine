// Physical-scale triplanar materials: cliffs retain vertical detail, and
// every LOD samples the same coordinates. Explicit gradients keep sampling
// defined when neighbouring pixels choose different materials.
#define MATERIAL_SET 2
#include "material_data.glsl"

const float PROJECTION_SHARPNESS = 4.0;
const float NORMAL_STRENGTH = 0.5;
const float NORMAL_FADE_START = 0.125; // repeats per pixel: eight pixels per repeat
const float NORMAL_FADE_END = 0.5;     // two pixels per repeat
const float TEXTURE_WARP_SIZE = 2.0;   // repeats per slowly varying warp cell
const float TEXTURE_WARP_AMOUNT = 1.5; // displacement range, in repeats

float terrainHash(vec3 p) {
    p = fract(p * vec3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return fract((p.x + p.y) * p.z);
}

// Value and analytic gradient of smooth value noise: the same eight corners
// give both colour variation and a normal, without extra texture reads.
vec4 terrainNoiseGradient(vec3 p) {
    vec3 at = floor(p), f = fract(p);
    vec3 derivative = 6.0 * f * (1.0 - f);
    vec3 u = f * f * (3.0 - 2.0 * f);
    float a = terrainHash(at), b = terrainHash(at + vec3(1, 0, 0));
    float c = terrainHash(at + vec3(0, 1, 0)), d = terrainHash(at + vec3(1, 1, 0));
    float e = terrainHash(at + vec3(0, 0, 1)), g = terrainHash(at + vec3(1, 0, 1));
    float h = terrainHash(at + vec3(0, 1, 1)), j = terrainHash(at + vec3(1, 1, 1));
    float low = mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
    float high = mix(mix(e, g, u.x), mix(h, j, u.x), u.y);
    vec3 gradient = vec3(mix(mix(b-a, d-c, u.y), mix(g-e, j-h, u.y), u.z),
                         mix(mix(c-a, d-b, u.x), mix(h-e, j-g, u.x), u.z), high-low);
    return vec4(mix(low, high, u.z), gradient * derivative);
}

vec4 filteredNoise(vec3 p, float footprint) {
    return mix(terrainNoiseGradient(p), vec4(0.5, 0, 0, 0), smoothstep(0.5, 1.0, footprint));
}

vec3 projectedColor(uint index, vec3 p, vec3 dx, vec3 dy, vec3 weights) {
    return textureGrad(globalTextures[nonuniformEXT(index)], p.zy, dx.zy, dy.zy).rgb * weights.x +
           textureGrad(globalTextures[nonuniformEXT(index)], p.xz, dx.xz, dy.xz).rgb * weights.y +
           textureGrad(globalTextures[nonuniformEXT(index)], p.xy, dx.xy, dy.xy).rgb * weights.z;
}

struct TerrainSurface {
    vec3 albedo;
    vec3 normal;
    float roughness;
    float metallic;
    vec4 rain;
};

TerrainSurface terrainMaterial(GpuTerrain t, uint layer, vec3 p, vec3 n, vec3 dx, vec3 dy) {
    vec4 fallback = t.layers[layer], settings = t.textures[layer];
    TerrainSurface surface = TerrainSurface(fallback.rgb, n, fallback.a, 0.0, vec4(0.0));
    if (settings.x > 0.5) {
        MaterialData material = materials[int(settings.x + 0.5) - 1];
        surface.rain = material.rain;
        vec3 weights = pow(abs(n), vec3(PROJECTION_SHARPNESS));
        weights /= max(dot(weights, vec3(1.0)), 0.0001);
        vec3 uv = p * settings.y, ux = dx * settings.y, uy = dy * settings.y;
        // Continuous domain warping breaks repeated cracks without seams,
        // rotated normal maps, or a second set of texture reads. Its analytic
        // Jacobian transforms the gradients used to select texture mips.
        vec3 warpAt = uv / TEXTURE_WARP_SIZE;
        vec4 wx = terrainNoiseGradient(warpAt), wy = terrainNoiseGradient(warpAt + vec3(19.1)),
             wz = terrainNoiseGradient(warpAt + vec3(47.3));
        vec3 warp = vec3(wx.x, wy.x, wz.x) - 0.5;
        float warpDerivative = TEXTURE_WARP_AMOUNT / TEXTURE_WARP_SIZE;
        ux += warpDerivative * vec3(dot(wx.yzw, ux), dot(wy.yzw, ux), dot(wz.yzw, ux));
        uy += warpDerivative * vec3(dot(wx.yzw, uy), dot(wy.yzw, uy), dot(wz.yzw, uy));
        uv += TEXTURE_WARP_AMOUNT * warp;
        surface.albedo = material.baseColor.rgb * projectedColor(material.albedoTexIdx, uv, ux, uy, weights);
        // Roughness has no directional basis; one blended triplanar sample
        // also keeps it continuous at a projection boundary.
        vec3 mr = projectedColor(material.metallicRoughnessTexIdx, uv, ux, uy, weights);
        surface.roughness = material.roughness * mr.g;
        surface.metallic = material.metallic * mr.b;
        float footprint = max(length(ux), length(uy));
        float normalWeight = NORMAL_STRENGTH * (1.0 - smoothstep(NORMAL_FADE_START, NORMAL_FADE_END, footprint));
        if (normalWeight > 0.0 && material.normalTexIdx != 0u) {
            vec2 nx = textureGrad(globalTextures[nonuniformEXT(material.normalTexIdx)], uv.zy, ux.zy, uy.zy).xy * 2.0 - 1.0;
            vec2 ny = textureGrad(globalTextures[nonuniformEXT(material.normalTexIdx)], uv.xz, ux.xz, uy.xz).xy * 2.0 - 1.0;
            vec2 nz = textureGrad(globalTextures[nonuniformEXT(material.normalTexIdx)], uv.xy, ux.xy, uy.xy).xy * 2.0 - 1.0;
            vec3 detail = vec3(0, nx.y, nx.x) * weights.x + vec3(ny.x, 0, ny.y) * weights.y +
                          vec3(nz.x, nz.y, 0) * weights.z;
            detail -= n * dot(n, detail);
            surface.normal = normalize(n + detail * normalWeight);
        }
    }
    vec4 macroSettings = t.macros[layer];
    if (macroSettings.y > 0.0 || macroSettings.z > 0.0) {
        vec3 macro = p * macroSettings.x;
        float footprint = max(length(dx), length(dy)) * macroSettings.x;
        // Zero-centred, bounded variation changes neither mean albedo nor
        // relief. Fine octaves vanish before they become smaller than a pixel.
        vec4 variation = (filteredNoise(macro, footprint) +
                           vec4(0.5, 1, 1, 1) * filteredNoise(macro * 2.0 + vec3(19.1), footprint * 2.0) +
                           vec4(0.25, 1, 1, 1) * filteredNoise(macro * 4.0 + vec3(47.3), footprint * 4.0)) / 1.75;
        surface.albedo *= 1.0 + macroSettings.y * (2.0 * variation.x - 1.0);
        vec3 gradient = variation.yzw;
        gradient -= n * dot(n, gradient);
        surface.normal = normalize(surface.normal - macroSettings.z * gradient);
    }
    return surface;
}

TerrainSurface terrainSurface(GpuTerrain t, int base, vec2 grid, vec3 p, vec3 n, vec3 dx, vec3 dy) {
    // Blend neighbouring cell centres, merging equal IDs before sampling.
    // A uniform area costs one material, rather than four copies of it.
    vec2 at = grid - 0.5;
    ivec2 low = ivec2(floor(at));
    vec2 f = smoothstep(vec2(0.0), vec2(1.0), fract(at));
    uint ids[4] = uint[4](layerOf(base, low.x, low.y), layerOf(base, low.x + 1, low.y),
                         layerOf(base, low.x, low.y + 1), layerOf(base, low.x + 1, low.y + 1));
    float weights[4] = float[4]((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y),
                               (1.0 - f.x) * f.y, f.x * f.y);
    TerrainSurface sum = TerrainSurface(vec3(0), vec3(0), 0.0, 0.0, vec4(0.0));
    for (int i = 0; i < 4; ++i) {
        for (int j = i + 1; j < 4; ++j)
            if (ids[i] == ids[j]) { weights[i] += weights[j]; weights[j] = 0.0; }
        if (weights[i] <= 0.0) continue;
        TerrainSurface value = terrainMaterial(t, ids[i], p, n, dx, dy);
        sum.albedo += value.albedo * weights[i];
        sum.normal += value.normal * weights[i];
        sum.roughness += value.roughness * weights[i];
        sum.metallic += value.metallic * weights[i];
        sum.rain += value.rain * weights[i];
    }
    sum.normal = normalize(sum.normal);
    return sum;
}
