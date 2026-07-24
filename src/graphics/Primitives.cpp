#include "graphics/Primitives.hpp"

namespace saida {

// Unit cube with per-face normals, texture UVs, and non-overlapping lightmap UVs.
const std::vector<Vertex>& cubeVertices() {
    static const std::vector<Vertex> verts = [] {
        struct Corner { glm::vec3 pos; glm::vec2 uv; };
        struct Face { glm::vec3 normal; Corner corners[4]; };
        // Per-corner texture UVs, shared by every face.
        const glm::vec2 c0{0, 1}, c1{1, 1}, c2{1, 0}, c3{0, 0};
        const Face faces[6] = {
            {{0, 0, 1},  {{{-0.5f,-0.5f, 0.5f},c0},{{ 0.5f,-0.5f, 0.5f},c1},{{ 0.5f, 0.5f, 0.5f},c2},{{-0.5f, 0.5f, 0.5f},c3}}},
            {{0, 0,-1},  {{{ 0.5f,-0.5f,-0.5f},c0},{{-0.5f,-0.5f,-0.5f},c1},{{-0.5f, 0.5f,-0.5f},c2},{{ 0.5f, 0.5f,-0.5f},c3}}},
            {{0, 1, 0},  {{{-0.5f, 0.5f, 0.5f},c0},{{ 0.5f, 0.5f, 0.5f},c1},{{ 0.5f, 0.5f,-0.5f},c2},{{-0.5f, 0.5f,-0.5f},c3}}},
            {{0,-1, 0},  {{{-0.5f,-0.5f,-0.5f},c0},{{ 0.5f,-0.5f,-0.5f},c1},{{ 0.5f,-0.5f, 0.5f},c2},{{-0.5f,-0.5f, 0.5f},c3}}},
            {{1, 0, 0},  {{{ 0.5f,-0.5f, 0.5f},c0},{{ 0.5f,-0.5f,-0.5f},c1},{{ 0.5f, 0.5f,-0.5f},c2},{{ 0.5f, 0.5f, 0.5f},c3}}},
            {{-1,0, 0},  {{{-0.5f,-0.5f,-0.5f},c0},{{-0.5f,-0.5f, 0.5f},c1},{{-0.5f, 0.5f, 0.5f},c2},{{-0.5f, 0.5f,-0.5f},c3}}},
        };

        constexpr float kInset = 0.06f;  // fraction of a cell kept as padding
        std::vector<Vertex> out;
        out.reserve(24);
        for (int f = 0; f < 6; ++f) {
            const int col = f % 3, row = f / 3;
            glm::vec4 tangent;
            if (faces[f].normal.z > 0.5f) tangent = glm::vec4(1, 0, 0, 1);
            else if (faces[f].normal.z < -0.5f) tangent = glm::vec4(-1, 0, 0, 1);
            else if (faces[f].normal.y > 0.5f) tangent = glm::vec4(1, 0, 0, 1);
            else if (faces[f].normal.y < -0.5f) tangent = glm::vec4(1, 0, 0, 1);
            else if (faces[f].normal.x > 0.5f) tangent = glm::vec4(0, 0, -1, 1);
            else if (faces[f].normal.x < -0.5f) tangent = glm::vec4(0, 0, 1, 1);
            else tangent = glm::vec4(1, 0, 0, 1);

            for (const Corner& c : faces[f].corners) {
                // Map the corner's 0/1 UV into this face's grid cell, inset.
                float lu = (col + kInset + c.uv.x * (1.0f - 2.0f * kInset)) / 3.0f;
                float lv = (row + kInset + c.uv.y * (1.0f - 2.0f * kInset)) / 2.0f;
                out.push_back(Vertex{c.pos, faces[f].normal, {1, 1, 1}, c.uv, {lu, lv}, tangent});
            }
        }
        return out;
    }();
    return verts;
}

const std::vector<uint32_t>& cubeIndices() {
    static const std::vector<uint32_t> idx = {
         0,  1,  2,  2,  3,  0,   4,  5,  6,  6,  7,  4,
         8,  9, 10, 10, 11,  8,  12, 13, 14, 14, 15, 12,
        16, 17, 18, 18, 19, 16,  20, 21, 22, 22, 23, 20,
    };
    return idx;
}

} // namespace saida
