#version 450
#extension GL_GOOGLE_include_directive : require

// AmbientOcclusionPass, GI stage, on the AO grid: the diffuse irradiance the
// surface at each grid texel receives (lighting.glsl giIrradianceOrSky), its
// point and geometric normal rebuilt from the depth like the kernel's
// (ssao.glsl). The upsample stage brings it to every texel of the view, where
// the lit shaders the depth pre-pass draws read it back instead of sampling
// the probes per fragment: a quarter of the samples, out of the uber-shader.
// Set 0 is the scene's global set, set 1 the view's depth.

#include "lighting.glsl"

DECL_TEX2D(1, 0, 1, depthInput);

PUSH_QUALIFIER PushConstants {
    vec4 invProjection;   // InverseProjection::scale
    vec4 invProjection2;  // InverseProjection::depth
    vec4 invOffsets;      // InverseProjection::offsets
    vec4 sourceRect;      // the viewport in UV of the depth target
    vec4 aoGrid;          // xy the viewport's origin in grid texels of the targets, zw its size
    vec4 worldFromView0;  // rows of the view's world transform: xyz rotation, w translation
    vec4 worldFromView1;
    vec4 worldFromView2;
} push;

float sampleDepth(vec2 viewportUV) {
    return textureLod(TEX2D(depthInput),
                      push.sourceRect.xy + clamp(viewportUV, vec2(0.0), vec2(1.0)) * push.sourceRect.zw, 0.0).r;
}

#include "ssao.glsl"

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outIrradiance;

void main() {
    SsaoInputs inputs;
    inputs.settings = vec4(1.0);
    inputs.invProjection = push.invProjection;
    inputs.invProjection2 = push.invProjection2;
    inputs.invOffsets = push.invOffsets;
    inputs.pixels = max(vec2(textureSize(TEX2D(depthInput), 0)) * push.sourceRect.zw, vec2(1.0));
    inputs.aoPixels = push.aoGrid.zw;
    ivec2 aoTexel = ivec2(floor(fragUV * push.aoGrid.zw));
    vec2 uv = ssaoRepresentativeUV(aoTexel, inputs.pixels);
    float depth = sampleDepth(uv);
    if (nothingDrawn(depth)) {
        outIrradiance = vec4(0.0);
        return;
    }
    vec3 viewPosition = ssaoViewPosition(inputs, uv, depth);
    vec3 viewNormal = ssaoSurfaceNormal(inputs, uv, viewPosition, depth);
    mat3 rotation = transpose(mat3(push.worldFromView0.xyz, push.worldFromView1.xyz, push.worldFromView2.xyz));
    vec3 eye = vec3(push.worldFromView0.w, push.worldFromView1.w, push.worldFromView2.w);
    vec3 wp = rotation * viewPosition + eye;
    vec3 N = normalize(rotation * viewNormal);
    outIrradiance = vec4(giIrradianceOrSky(wp, N, normalize(eye - wp)), 1.0);
}
