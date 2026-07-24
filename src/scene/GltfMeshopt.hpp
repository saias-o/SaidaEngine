#pragma once

#include <cgltf.h>

namespace saida {

// Decoded bytes use cgltf's allocator so cgltf_free(data) owns their lifetime.
bool decodeMeshoptBuffers(cgltf_data* data);

} // namespace saida
