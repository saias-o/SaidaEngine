#version 450
#extension GL_GOOGLE_include_directive : require
#ifndef WEB
#extension GL_EXT_nonuniform_qualifier : require
#endif

#include "web_compat.glsl"

// Desktop indexes a bindless texture array; web (no bindless) binds the single
// canvas texture per draw.
#ifdef WEB
DECL_TEX2D(0, 0, 1, canvasTexture);
#else
layout(set = 0, binding = 0) uniform sampler2D globalTextures[8192];
#endif

layout(location = 0) in vec2 fragUV;
layout(location = 1) flat in uint fragTextureId;
layout(location = 2) in float fragAlpha;

layout(location = 0) out vec4 outColor;

void main() {
#ifdef WEB
    vec4 color = texture(TEX2D(canvasTexture), fragUV);
#else
    vec4 color = texture(globalTextures[nonuniformEXT(fragTextureId)], fragUV);
#endif
    color.a *= fragAlpha;
    if (color.a <= 0.001) discard;
    outColor = color;
}
