#include "render/GpuDrivenLayout.hpp"
#include "render/TriangleMetrics.hpp"

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

    saida::MeshTriangleMetrics triangles;
    triangles.addIndexedMesh(36, true);
    triangles.addIndexedMesh(60, false);
    triangles.addIndexedMesh(6, true);
    triangles.addIndexedMesh(36, true); // A second instance of the same mesh.
    if (!require(triangles.frustumTriangles == 26)) return 11;
    if (!require(triangles.sceneTriangles == 46)) return 12;
    if (!require(std::string(saida::kProfilerFrustumTriangles) ==
                 "Renderer/FrustumTriangles")) return 13;
    if (!require(std::string(saida::kProfilerSceneTriangles) ==
                 "Scene/TotalTriangles")) return 14;

    const std::filesystem::path root = SAIDA_PROJECT_ROOT;
    const std::string rendererSource =
        readText(root / "src/render/Renderer.cpp");
    if (!require(!contains(rendererSource, "MeshTriangleMetrics"))) return 15;
    if (!require(!contains(rendererSource, "Renderer/Triangles"))) return 16;

    const std::string rootCmake = readText(root / "CMakeLists.txt");
    const size_t editorTarget =
        rootCmake.find("add_library(saida_editor STATIC");
    const size_t editorCollector =
        rootCmake.find("src/editor/ProfilerTriangleMetrics.cpp");
    const size_t editorTargetEnd =
        rootCmake.find("target_link_libraries(saida_editor", editorTarget);
    if (!require(editorTarget != std::string::npos &&
                 editorCollector > editorTarget &&
                 editorCollector < editorTargetEnd)) return 17;
    if (!require(contains(
            rootCmake,
            "target_link_libraries(SaidaEngineRuntime PRIVATE saida_engine)")))
        return 18;
    const std::string webPlayerCmake =
        readText(root / "web/player/CMakeLists.txt");
    if (!require(!contains(webPlayerCmake, "ProfilerTriangleMetrics")))
        return 19;

    const std::filesystem::path shaders = root / "shaders";
    const std::string culling = readText(shaders / "culling.comp");
    const std::string fragment = readText(shaders / "shader.frag");
    const std::string materialData = readText(shaders / "material_data.glsl");
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
    if (!require(contains(fragment, "#include \"material_data.glsl\"") &&
                 contains(materialData, "uint materialType;"))) return 9;
    if (!require(contains(fragment, "if (mat.materialType == 1u)"))) return 10;

    const std::string vertex = readText(shaders / "shader.vert");
    static_assert(offsetof(saida::gpu_driven::InstanceData, doubleSided) == 88);
    if (!require(contains(vertex, "uint doubleSided;\n    float lodFade;"))) return 20;
    if (!require(contains(vertex, "fragDoubleSided = instances[gl_InstanceIndex].doubleSided;"))) return 21;
    if (!require(contains(fragment, "if (!gl_FrontFacing && fragDoubleSided == 0u) discard;"))) return 22;
    if (!require(contains(fragment, "if (!gl_FrontFacing) N = -N;"))) return 23;
    // The LOD cross-fade travels in the instance's last word, on both paths.
    static_assert(offsetof(saida::gpu_driven::InstanceData, lodFade) == 92);
    if (!require(contains(vertex, "fragLodFade = instances[gl_InstanceIndex].lodFade;"))) return 24;
    if (!require(contains(fragment, "if (lodFadeHides(fragLodFade)) discard;") &&
                 contains(fragment, "if (lodFadeHides(push.params.x)) discard;"))) return 25;
    if (!require(contains(culling, "float lodFade;"))) return 26;

    // Culling writes only the visible commands, compacted into the first
    // slots. Without drawIndirectCount every slot up to the instance count is
    // drawn, so the culled buffer must be zeroed before the dispatch: a slot
    // past the visible count otherwise replays a command from an earlier frame.
    const size_t culledBuffer = rendererSource.find("drawCommandBuffers_.push_back(");
    const size_t culledBufferEnd = rendererSource.find(';', culledBuffer);
    const size_t culledTransferDst =
        rendererSource.find("rhi::BufferUsage::TransferDst", culledBuffer);
    if (!require(culledBuffer != std::string::npos &&
                 culledTransferDst < culledBufferEnd)) return 27;
    const size_t noIndirectCount =
        rendererSource.find("if (!device_.capabilities().drawIndirectCount) {");
    const size_t clearCulled =
        rendererSource.find("encoder.fillBuffer(*drawCommandBuffers_[currentFrame_], 0,");
    const size_t cullDispatch = rendererSource.find("cp.setPipeline(*cullingPipeline_);");
    if (!require(noIndirectCount != std::string::npos &&
                 noIndirectCount < clearCulled &&
                 clearCulled != std::string::npos &&
                 clearCulled < cullDispatch &&
                 cullDispatch != std::string::npos)) return 28;
    return 0;
}
