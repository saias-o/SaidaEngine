#include "scene/GltfMeshopt.hpp"
#include "core/Log.hpp"

#include <meshoptimizer.h>

namespace saida {

bool decodeMeshoptBuffers(cgltf_data* data) {
    for (size_t i = 0; i < data->buffer_views_count; ++i) {
        cgltf_buffer_view& view = data->buffer_views[i];
        if (!view.has_meshopt_compression) continue;

        const cgltf_meshopt_compression& mc = view.meshopt_compression;
        if (!mc.buffer || !mc.buffer->data) {
            Log::error("GltfMeshopt: meshopt buffer view ", i, " has no source data");
            return false;
        }
        const unsigned char* src =
            static_cast<const unsigned char*>(mc.buffer->data) + mc.offset;
        const size_t outSize = mc.count * mc.stride;

        // Allocate with cgltf's allocator so cgltf_free(data) releases it (it
        // unconditionally frees buffer_view.data with this same free_func).
        unsigned char* out = static_cast<unsigned char*>(
            data->memory.alloc_func(data->memory.user_data, outSize));
        if (!out) {
            Log::error("GltfMeshopt: out of memory decoding view ", i);
            return false;
        }

        int rc = 0;
        switch (mc.mode) {
            case cgltf_meshopt_compression_mode_attributes:
                rc = meshopt_decodeVertexBuffer(out, mc.count, mc.stride, src, mc.size);
                break;
            case cgltf_meshopt_compression_mode_triangles:
                rc = meshopt_decodeIndexBuffer(out, mc.count, mc.stride, src, mc.size);
                break;
            case cgltf_meshopt_compression_mode_indices:
                rc = meshopt_decodeIndexSequence(out, mc.count, mc.stride, src, mc.size);
                break;
            default:
                data->memory.free_func(data->memory.user_data, out);
                Log::error("GltfMeshopt: invalid meshopt compression mode on view ", i);
                return false;
        }
        if (rc != 0) {
            data->memory.free_func(data->memory.user_data, out);
            Log::error("GltfMeshopt: meshopt decode failed on view ", i, " (rc ", rc, ")");
            return false;
        }

        // Filters are applied in place on the decoded, stride-sized data.
        switch (mc.filter) {
            case cgltf_meshopt_compression_filter_octahedral:
                meshopt_decodeFilterOct(out, mc.count, mc.stride); break;
            case cgltf_meshopt_compression_filter_quaternion:
                meshopt_decodeFilterQuat(out, mc.count, mc.stride); break;
            case cgltf_meshopt_compression_filter_exponential:
                meshopt_decodeFilterExp(out, mc.count, mc.stride); break;
            default: break; // none (color is handled by the stride-aware decode)
        }

        view.data = out;  // released by cgltf_free(data)
    }
    return true;
}

} // namespace saida
