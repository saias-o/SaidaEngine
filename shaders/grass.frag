#version 450
#extension GL_GOOGLE_include_directive : require

// A grass blade under the scene's lights, the same terms as any lit surface:
// darker toward the root, where the blades around shade it, and lit a little
// through from behind, as a leaf is. All of that is near the camera
// (`fragDetail`); away from it a blade is shaded as the ground it stands on
// -- its albedo, its normal, a matt surface -- so that where the blades end
// the ground carries on in the same light.

#include "lighting.glsl"
#include "grass_blade.glsl"

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec3 fragColor;
layout(location = 3) in float fragHeight;
layout(location = 4) in float fragGust;
layout(location = 5) in float fragDetail;

layout(location = 0) out vec4 outColor;

const float ROUGHNESS = 0.8;
const float GROUND_ROUGHNESS = 1.0; // a photographed ground's, seen from where blades give way to it
const float ROOT_SHADE = 0.35;      // light reaching the root, of the tip's
const float TRANSMISSION = 0.25;    // share of the light behind a blade that comes through it
const float TIP_LIGHT = 0.3;        // a tip is this much lighter than the blade's middle: thinner, sunlit
const float GUST_SHEEN = 0.45;      // a blade laid down by a gust shows its paler side
// How deep into a meadow the eye sees, in blade heights per unit of the
// tangent of the line of sight's elevation: looking down into it, whole
// blades, roots and all; along it, only the tips. Measured against the
// ground a meadow stands on, under the same light, the blades drawn and not.
const float VIEW_DEPTH = 5.0;
const float LEAST_SEEN = 0.05;      // of a blade, from the tip down, at the most grazing view

// The albedo a field is given is a meadow's, its own shade included, as the
// ground under it is drawn. The shading above moves light along the blade --
// dark root, bright tip -- and must take none away from what is seen of it:
// this is its mean over the part of the blades the eye reaches, t in
// [from, 1], weighted by their width (1 - BLADE_TAPER t). The shading is
// (ROOT_SHADE + (1 - ROOT_SHADE) t) (1 + TIP_LIGHT t^2); times the width it
// is the quartic sum of E[i] t^i.
float seenShade(float from) {
    const float r = ROOT_SHADE, k = TIP_LIGHT, a = BLADE_TAPER;
    const float E[5] = float[5](r, (1.0 - r) - a * r, r * k - a * (1.0 - r), (1.0 - r) * k - a * r * k,
                                -a * (1.0 - r) * k);
    float shade = 0.0, power = from;
    for (int i = 0; i < 5; ++i) {
        shade += E[i] * (1.0 - power) / float(i + 1);
        power *= from;
    }
    float width = (1.0 - from) - 0.5 * a * (1.0 - from * from);
    return shade / max(width, 1e-4);
}

void main() {
    vec3 N = normalize(fragNormal);
    vec3 V = normalize(lights.cameraPos.xyz - fragWorldPos);
    // Seen from its back, a blade is lit on the side facing the eye.
    if (dot(N, V) < 0.0) N = normalize(N - 2.0 * dot(N, V) * V);
    float occlusion = mix(ROOT_SHADE, 1.0, fragHeight);
    float light = (1.0 + TIP_LIGHT * fragHeight * fragHeight) * (1.0 + GUST_SHEEN * fragGust * fragHeight);
    float elevation = V.y / max(length(V.xz), 1e-3);
    float seen = clamp(VIEW_DEPTH * elevation, LEAST_SEEN, 1.0);
    vec3 albedo = fragColor * mix(1.0, occlusion * light / seenShade(1.0 - seen), fragDetail);
    LightTerms t = accumulate(N, V, fragWorldPos, albedo, 0.0, mix(GROUND_ROUGHNESS, ROUGHNESS, fragDetail));
    vec3 lit = t.diffuse + t.specular;
    // Through the blade: the directional lights from behind, as diffuse.
    for (int i = 0; i < lights.counts.x; ++i) {
        Light l = lights.lights[i];
        if (int(l.dirType.w + 0.5) != 0) continue;
        vec3 L = normalize(-l.dirType.xyz);
        float behind = max(dot(-N, L), 0.0);
        lit += albedo * l.colorInt.rgb * l.colorInt.w * behind * TRANSMISSION * fragDetail / PI;
    }
    outColor = vec4(lit, 1.0);
}
