// Palette de skinning commune Vulkan/WebGPU : 3 lignes vec4 par os (matrice
// affine 3x4, 48 octets), remplie par le Renderer depuis les GlobalPose des
// personnages visibles. L'indexation se fait en os, pas en lignes.

layout(std140, set = 0, binding = 3) readonly buffer BoneBuffer {
    vec4 boneRows[];
};

// Lignes de la matrice affine blendée des 4 influences d'un vertex.
void blendBoneRows(int boneOffset, ivec4 indices, vec4 weights,
                   out vec4 row0, out vec4 row1, out vec4 row2) {
    int i0 = (boneOffset + indices.x) * 3;
    int i1 = (boneOffset + indices.y) * 3;
    int i2 = (boneOffset + indices.z) * 3;
    int i3 = (boneOffset + indices.w) * 3;
    row0 = weights.x * boneRows[i0] + weights.y * boneRows[i1] +
           weights.z * boneRows[i2] + weights.w * boneRows[i3];
    row1 = weights.x * boneRows[i0 + 1] + weights.y * boneRows[i1 + 1] +
           weights.z * boneRows[i2 + 1] + weights.w * boneRows[i3 + 1];
    row2 = weights.x * boneRows[i0 + 2] + weights.y * boneRows[i1 + 2] +
           weights.z * boneRows[i2 + 2] + weights.w * boneRows[i3 + 2];
}

vec3 skinPoint(vec4 row0, vec4 row1, vec4 row2, vec3 p) {
    vec4 h = vec4(p, 1.0);
    return vec3(dot(row0, h), dot(row1, h), dot(row2, h));
}

vec3 skinDirection(vec4 row0, vec4 row1, vec4 row2, vec3 d) {
    return vec3(dot(row0.xyz, d), dot(row1.xyz, d), dot(row2.xyz, d));
}
