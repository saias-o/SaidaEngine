#include "render/GpuDrivenLayout.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

std::string readText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

bool contains(const std::string& text, const char* needle) {
    return text.find(needle) != std::string::npos;
}

bool require(bool condition) {
    return condition;
}

} // namespace

int main() {
    using saida::gpu_driven::CullingBinding;
    using saida::gpu_driven::binding;

    static_assert(binding(CullingBinding::Instances) == 0);
    static_assert(binding(CullingBinding::OriginalDrawCommands) == 1);
    static_assert(binding(CullingBinding::DrawCount) == 2);
    static_assert(binding(CullingBinding::CulledDrawCommands) == 3);

    const std::filesystem::path shaders = std::filesystem::path(SAIDA_PROJECT_ROOT) / "shaders";
    const std::string culling = readText(shaders / "culling.comp");
    const std::string fragment = readText(shaders / "shader.frag");
    if (!require(!culling.empty())) return 1;
    if (!require(!fragment.empty())) return 2;

    // Descriptor types alone cannot detect a swapped storage-buffer binding:
    // both sides still compile. Keep the culling data-flow ABI explicit here.
    if (!require(contains(culling, "binding = 0, std430) readonly buffer InstanceBuffer"))) return 3;
    if (!require(contains(culling, "binding = 1, std430) readonly buffer OriginalDrawCommandBuffer"))) return 4;
    if (!require(contains(culling, "binding = 2) buffer CountBuffer"))) return 5;
    if (!require(contains(culling, "binding = 3, std430) writeonly buffer CulledDrawCommandBuffer"))) return 6;
    if (!require(contains(culling, "VkDrawIndexedIndirectCommand cmd = originalDraws[idx];"))) return 7;
    if (!require(contains(culling, "culledDraws[drawIdx] = cmd;"))) return 8;

    // One bindless MDI pipeline must preserve the classic unlit equation rather
    // than accidentally routing MaterialType::Unlit through PBR lighting.
    if (!require(contains(fragment, "uint materialType;"))) return 9;
    if (!require(contains(fragment, "if (mat.materialType == 1u)"))) return 10;

    return 0;
}
