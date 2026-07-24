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

struct Particle {
    vec4 positionAge;
    vec4 velocityLifetime;
    vec4 colorA;
    vec4 colorB;
    vec4 sizeRotation;
    vec4 renderParams;
};

layout(std430, set = 1, binding = 0) readonly buffer ParticleBuffer {
    Particle particles[];
};

layout(std430, set = 1, binding = 1) readonly buffer AliveBuffer {
    uint aliveIndices[];
};

PUSH_QUALIFIER PushConstants {
    uint particleOffset;
} push;

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec2 fragUV;

const vec2 CORNERS[6] = vec2[6](
    vec2(-0.5, -0.5), vec2( 0.5, -0.5), vec2( 0.5,  0.5),
    vec2(-0.5, -0.5), vec2( 0.5,  0.5), vec2(-0.5,  0.5)
);

void main() {
    uint aliveSlot = push.particleOffset + uint(gl_VertexIndex / 6);
    uint particleIndex = aliveIndices[aliveSlot];
    uint cornerIndex = uint(gl_VertexIndex % 6);

#ifdef MULTIVIEW
    int viewIndex = gl_ViewIndex;
#else
    int viewIndex = 0;
#endif

    Particle p = particles[particleIndex];
    float t = clamp(p.positionAge.w / max(p.velocityLifetime.w, 0.001), 0.0, 1.0);
    float size = max(p.sizeRotation.x * mix(1.0, max(p.sizeRotation.w, 0.0), t), 0.001);
    vec4 color = mix(p.colorA, p.colorB, t);

    mat3 invViewRot = transpose(mat3(cam.view[viewIndex]));
    vec3 right = normalize(invViewRot[0]);
    vec3 up = normalize(invViewRot[1]);

    vec2 corner = CORNERS[cornerIndex];
    float rotation = p.sizeRotation.y;
    float stretchY = max(p.renderParams.x, 1.0);
    if (p.renderParams.y > 0.5) {
        vec3 fallDir = vec3(0.0, -1.0, 0.0);
        vec2 projectedFall = vec2(dot(fallDir, right), dot(fallDir, up));
        float projectedLen = length(projectedFall);
        if (projectedLen > 0.0001) {
            projectedFall /= projectedLen;
            rotation = atan(-projectedFall.x, projectedFall.y);
        } else {
            rotation = 0.0;
        }
    }

    float c = cos(rotation);
    float s = sin(rotation);
    vec2 local = vec2(corner.x, corner.y * stretchY);
    local = vec2(local.x * c - local.y * s, local.x * s + local.y * c);
    vec3 world = p.positionAge.xyz + (right * local.x + up * local.y) * size;
    gl_Position = cam.proj[viewIndex] * cam.view[viewIndex] * vec4(world, 1.0);

    fragColor = color;
    fragUV = corner + vec2(0.5);
}
