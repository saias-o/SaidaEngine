#version 450
#extension GL_GOOGLE_include_directive : require

#ifdef MULTIVIEW
#extension GL_EXT_multiview : require
#endif

#include "web_compat.glsl"

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view[2];
    mat4 proj[2];
} cam;

PUSH_QUALIFIER PushConstants {
    mat4 model;
    vec4 params; // x = worldWidth, y = worldHeight, z = textureId, w = alpha
} push;

layout(location = 0) out vec2 fragUV;
layout(location = 1) flat out uint fragTextureId;
layout(location = 2) out float fragAlpha;

void main() {
    vec2 quad[4] = vec2[](
        vec2(-0.5,  0.5),
        vec2( 0.5,  0.5),
        vec2(-0.5, -0.5),
        vec2( 0.5, -0.5)
    );
    vec2 uv[4] = vec2[](
        vec2(0.0, 0.0),
        vec2(1.0, 0.0),
        vec2(0.0, 1.0),
        vec2(1.0, 1.0)
    );

    vec2 local = quad[gl_VertexIndex] * vec2(push.params.x, push.params.y);
    vec4 world = push.model * vec4(local, 0.0, 1.0);

#ifdef MULTIVIEW
    uint viewIndex = gl_ViewIndex;
#else
    uint viewIndex = 0;
#endif

    gl_Position = cam.proj[viewIndex] * cam.view[viewIndex] * world;
    fragUV = uv[gl_VertexIndex];
    fragTextureId = uint(push.params.z + 0.5);
    fragAlpha = push.params.w;
}
