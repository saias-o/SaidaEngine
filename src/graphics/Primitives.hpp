#pragma once

#include "graphics/Mesh.hpp"

#include <vector>

namespace saida {

// Built-in primitive geometry, referenced by the mesh id "builtin:cube" etc.
// so scenes can describe primitive meshes without a file on disk.
const std::vector<Vertex>& cubeVertices();
const std::vector<uint32_t>& cubeIndices();

} // namespace saida
