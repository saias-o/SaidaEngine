#version 450
#extension GL_GOOGLE_include_directive : require

#ifdef MULTIVIEW
#extension GL_EXT_multiview : require
#endif

#include "web_compat.glsl"
#include "storm_clouds.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 1) in vec4 clipPos;

layout(location = 0) out vec4 outColor;

DECL_TEX2D(0, 0, 1, skyboxTex);
// The sky `skyboxTex` fades into. When no second sky is set the feature binds
// the first one here too, so the shader never branches on it.
DECL_TEX2D(0, 2, 3, blendTex);

// Mono uses a single inverse view-proj; the XR multiview variant carries one per
// eye and picks it with gl_ViewIndex.
#ifdef MULTIVIEW
layout(push_constant) uniform PushConsts {
    mat4 invViewProj[2];
    float exposure;
    float rotation;
    float blend;
    float blendRotation;
    vec4 sunDirection;  // xyz toward the Sun, w angular radius in radians
    vec4 sunColor;      // rgb radiance; w runtime storm cloud cover
    vec4 fogColor;      // rgb linear HDR fog color; w atmosphere enabled
    vec4 fogUp;         // xyz world-space local vertical
} push;
#define INV_VIEW_PROJ push.invViewProj[gl_ViewIndex]
#else
PUSH_QUALIFIER PushConsts {
    mat4 invViewProj;
    float exposure;
    float rotation;
    float blend;
    float blendRotation;
    vec4 sunDirection;  // xyz toward the Sun, w angular radius in radians
    vec4 sunColor;      // rgb radiance; w runtime storm cloud cover
    vec4 fogColor;      // rgb linear HDR fog color; w atmosphere enabled
    vec4 fogUp;         // xyz world-space local vertical
} push;
#define INV_VIEW_PROJ push.invViewProj
#endif

const float INV_TWO_PI = 0.15915494309;
const float INV_PI = 0.31830988618;

// Equirectangular UVs of a direction turned about Y by `rotation` radians.
vec2 equirect(vec3 dir, float rotation) {
    float s = sin(rotation);
    float c = cos(rotation);
    dir.xz = mat2(c, -s, s, c) * dir.xz;
    // Invert dir.y so the sky (positive Y) maps to the top of the texture (V=0)
    vec2 uv = vec2(atan(dir.z, dir.x), asin(-dir.y));
    return uv * vec2(INV_TWO_PI, INV_PI) + 0.5;
}

// Longitude wraps from 1 back to 0. Correct that discontinuity in the screen
// derivatives before sampling, or the seam selects an unnecessarily coarse mip.
vec2 equirectDx(vec2 uv) {
    vec2 dx = dFdx(uv);
    dx.x -= round(dx.x);
    return dx;
}

vec2 equirectDy(vec2 uv) {
    vec2 dy = dFdy(uv);
    dy.x -= round(dy.x);
    return dy;
}

// The sky's own radiance in `dir` with its storm-cloud cover, before the Sun
// disc. `color` is the sampled sky (crossfade included); `sunTransmission`
// what the clouds leave of a disc behind them.
vec3 cloudedSky(vec3 dir, vec3 color, out float sunTransmission) {
    color *= push.exposure;
    sunTransmission = 1.0;
    if (push.sunColor.w > 0.0) {
        // Low-frequency sky light keeps stars and HDR highlights from painting
        // isolated bright speckles onto the cloud ceiling at night.
        vec3 cloudLight = textureLod(TEX2D(skyboxTex),equirect(dir,push.rotation),4.0).rgb;
        if (push.blend > 0.0) {
            vec3 otherLight = textureLod(TEX2D(blendTex),equirect(dir,push.blendRotation),4.0).rgb;
            cloudLight = mix(cloudLight,otherLight,clamp(push.blend,0.0,1.0));
        }
        vec4 cloud = stormCloudLayer(dir,normalize(push.sunDirection.xyz),push.sunColor.w,
                                    cloudLight * push.exposure);
        color = mix(color,cloud.rgb,cloud.a);
        // A thick rain cloud hides the high-radiance Sun disc completely. Its
        // opacity cannot simply multiply the disc: one percent of a Sun still
        // burns a white hole through the darkest cloud after tonemapping.
        sunTransmission = exp(-28.0 * cloud.a);
    }
    return color;
}

#ifdef SKY_PROBE
// The sky probe (RenderFeature.hpp, SkyProbe): the same sky into a 64 x 32
// equirectangular image of VIEW space, which the tonemap reads back by view
// direction to colour the fog. INV_VIEW_PROJ carries the camera's rotation
// (world from view). Rays below the horizon take the horizon: fog over far
// ground is the haze of the horizon, not the ground of a sky photograph. Fog
// scatters light from a wide cone, so each texel averages a 5 x 3 grid of
// directions about 28 x 17 degrees across: one dark cloud on the horizon must
// not hang a dark curtain down the fogged land beneath it. No Sun disc: a
// fraction of one texel wide, it would only smear a hot spot.
const vec2 PROBE_SIZE = vec2(64.0, 32.0);
const float PROBE_PI = 3.14159265359;
vec3 probeDirection(vec2 texel) {
    vec2 uv = texel / PROBE_SIZE;
    float phi = (uv.x - 0.5) * 2.0 * PROBE_PI;
    float theta = clamp((0.5 - uv.y) * PROBE_PI, -0.5 * PROBE_PI, 0.5 * PROBE_PI);
    vec3 view = vec3(sin(phi) * cos(theta), sin(theta), -cos(phi) * cos(theta));
    vec3 dir = normalize(mat3(INV_VIEW_PROJ) * view);
    dir.y = max(dir.y, 0.0);
    return normalize(dir + vec3(0.0, 1e-4, 0.0));
}
// The mip whose texels match the probe's: the blur starts in the sky itself.
float probeLod(vec2 size) {
    return max(log2(size.x / PROBE_SIZE.x), 0.0);
}

void main() {
    float skyLod = probeLod(vec2(textureSize(TEX2D(skyboxTex), 0)));
    float blendLod = probeLod(vec2(textureSize(TEX2D(blendTex), 0)));
    vec3 sum = vec3(0.0);
    for (int j = -1; j <= 1; ++j) {
        for (int i = -2; i <= 2; ++i) {
            vec3 dir = probeDirection(gl_FragCoord.xy + vec2(float(i), float(j)) * 0.85);
            vec3 color = textureLod(TEX2D(skyboxTex), equirect(dir, push.rotation), skyLod).rgb;
            if (push.blend > 0.0) {
                vec3 other = textureLod(TEX2D(blendTex), equirect(dir, push.blendRotation), blendLod).rgb;
                color = mix(color, other, clamp(push.blend, 0.0, 1.0));
            }
            float unused;
            sum += cloudedSky(dir, color, unused);
        }
    }
    outColor = vec4(min(sum / 15.0, vec3(65000.0)), 1.0);
}
#else
void main() {
    // Reconstruct world-space direction from clip-space
    vec4 worldPos = INV_VIEW_PROJ * vec4(clipPos.xy, 1.0, 1.0);
    vec3 dir = normalize(worldPos.xyz / worldPos.w);

    vec2 skyUv = equirect(dir, push.rotation);
    vec3 color = textureGrad(TEX2D(skyboxTex), skyUv,
                             equirectDx(skyUv), equirectDy(skyUv)).rgb;
    // Both skies are sampled only while a fade is under way.
    if (push.blend > 0.0) {
        vec2 blendUv = equirect(dir, push.blendRotation);
        vec3 other = textureGrad(TEX2D(blendTex), blendUv,
                                 equirectDx(blendUv), equirectDy(blendUv)).rgb;
        color = mix(color, other, clamp(push.blend, 0.0, 1.0));
    }
    float sunTransmission;
    color = cloudedSky(dir, color, sunTransmission);

    if (push.fogColor.w > 0.5) {
        // Match the distant scene's scattering color in linear HDR, after
        // clouds and exposure. Keep the upper photograph and the Sun intact.
        const float HORIZON_FULL = 0.017452406;  // sin(1 degree)
        const float HORIZON_CLEAR = 0.104528463; // sin(6 degrees)
        float elevation = dot(dir, push.fogUp.xyz);
        float haze = 1.0 - smoothstep(HORIZON_FULL, HORIZON_CLEAR, elevation);
        color = mix(color, push.fogColor.rgb, haze);
    }

    // The Sun disc, in world space and outside the sky's exposure: the exposure
    // normalises photographs, the disc is a radiance of its own.
    float radius = push.sunDirection.w;
    if (radius > 0.0 && dot(push.sunColor.rgb, vec3(1.0)) > 0.0) {
        float angle = acos(clamp(dot(dir, normalize(push.sunDirection.xyz)), -1.0, 1.0));
        float r = angle / radius;
        if (r < 1.1) {
            // A soft edge a tenth of the radius wide, and the limb darkening a
            // real photosphere shows: brightest at the centre, dimmer at the rim.
            float edge = 1.0 - smoothstep(0.95, 1.05, r);
            float mu = sqrt(max(1.0 - r * r, 0.0));
            color += push.sunColor.rgb * edge * (0.4 + 0.6 * mu) * sunTransmission;
        }
    }

    // Write out HDR color. Tonemapping will handle it later. The target is
    // half-float: anything past its 65 504 would be stored as infinity, and a
    // tonemapper dividing by 1 + x turns that into NaN.
    outColor = vec4(min(color, vec3(65000.0)), 1.0);
}
#endif
