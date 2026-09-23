#version 450
#extension GL_GOOGLE_include_directive : require

#ifdef MULTIVIEW
#extension GL_EXT_multiview : require
#endif

#include "web_compat.glsl"

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
    vec4 sunColor;      // linear radiance; black draws no disc
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
    vec4 sunColor;      // linear radiance; black draws no disc
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

void main() {
    // Reconstruct world-space direction from clip-space
    vec4 worldPos = INV_VIEW_PROJ * vec4(clipPos.xy, 1.0, 1.0);
    vec3 dir = normalize(worldPos.xyz / worldPos.w);

    vec3 color = texture(TEX2D(skyboxTex), equirect(dir, push.rotation)).rgb;
    // Both skies are sampled only while a fade is under way.
    if (push.blend > 0.0) {
        vec3 other = texture(TEX2D(blendTex), equirect(dir, push.blendRotation)).rgb;
        color = mix(color, other, clamp(push.blend, 0.0, 1.0));
    }
    color *= push.exposure;

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
            color += push.sunColor.rgb * edge * (0.4 + 0.6 * mu);
        }
    }
    
    // Write out HDR color. Tonemapping will handle it later. The target is
    // half-float: anything past its 65 504 would be stored as infinity, and a
    // tonemapper dividing by 1 + x turns that into NaN.
    outColor = vec4(min(color, vec3(65000.0)), 1.0);
}
