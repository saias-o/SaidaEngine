#version 450
#extension GL_GOOGLE_include_directive : require

// A grass blade under the scene's lights, the same terms as any lit surface:
// darker toward the root, where the blades around shade it, and lit a little
// through from behind, as a leaf is.

#include "lighting.glsl"

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec3 fragColor;
layout(location = 3) in float fragHeight;

layout(location = 0) out vec4 outColor;

const float ROUGHNESS = 0.8;
const float ROOT_SHADE = 0.35;      // light reaching the root, of the tip's
const float TRANSMISSION = 0.25;    // share of the light behind a blade that comes through it

void main() {
    vec3 N = normalize(fragNormal);
    vec3 V = normalize(lights.cameraPos.xyz - fragWorldPos);
    // Seen from its back, a blade is lit on the side facing the eye.
    if (dot(N, V) < 0.0) N = normalize(N - 2.0 * dot(N, V) * V);
    float occlusion = mix(ROOT_SHADE, 1.0, fragHeight);
    vec3 albedo = fragColor * occlusion;
    LightTerms t = accumulate(N, V, fragWorldPos, albedo, 0.0, ROUGHNESS);
    vec3 lit = t.diffuse + t.specular;
    // Through the blade: the directional lights from behind, as diffuse.
    for (int i = 0; i < lights.counts.x; ++i) {
        Light l = lights.lights[i];
        if (int(l.dirType.w + 0.5) != 0) continue;
        vec3 L = normalize(-l.dirType.xyz);
        float behind = max(dot(-N, L), 0.0);
        lit += albedo * l.colorInt.rgb * l.colorInt.w * behind * TRANSMISSION / PI;
    }
    outColor = vec4(lit, 1.0);
}
