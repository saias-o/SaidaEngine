#version 450
#extension GL_GOOGLE_include_directive : require
#ifndef WEB
#extension GL_EXT_nonuniform_qualifier : require
#endif

#include "web_compat.glsl"

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec4 fragColor;
layout(location = 2) flat in uint fragTextureId;
layout(location = 3) flat in uint fragHasTexture;

layout(location = 0) out vec4 outColor;

// Desktop uses a bindless texture array indexed per-draw; web (no bindless) binds
// a single UI texture per draw.
#ifdef WEB
DECL_TEX2D(0, 0, 1, uiTexture);
#else
layout(set = 0, binding = 0) uniform sampler2D bindlessTextures[];
#endif

void main() {
#ifdef WEB
    // Sample unconditionally (fragHasTexture is a flat varying, so branching on
    // it is non-uniform control flow — forbidden around texture() in WGSL),
    // then select. A dummy white texture is always bound.
    vec4 sampled = texture(TEX2D(uiTexture), fragUV);
    vec4 texColor = fragHasTexture != 0 ? sampled : vec4(1.0);
#else
    vec4 texColor = vec4(1.0);
    if (fragHasTexture != 0)
        texColor = texture(bindlessTextures[nonuniformEXT(fragTextureId)], fragUV);
#endif

    outColor = fragColor * texColor;
}
