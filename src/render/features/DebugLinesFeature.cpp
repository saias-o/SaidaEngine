#include "render/features/DebugLinesFeature.hpp"

#include "core/Paths.hpp"
#include "graphics/Mesh.hpp"
#include "rhi/vulkan/Format.hpp"
#include "scene/Scene.hpp"
#include "scene/Node.hpp"
#include "scene/animation/Animator.hpp"

#include <algorithm>

namespace saida {

void DebugLinesFeature::createPipelines(const RenderContext& ctx) {
    if (ctx.stereo()) return;  // editor-only aid; not drawn in XR

    // Lines into the HDR target; depth test off so the skeleton shows through the
    // mesh. Reuses set 0 (camera) + the standard Vertex layout.
    Pipeline::Desc desc;
    desc.vertPath = shaderPath("debug_line.vert.spv");
    desc.fragPath = shaderPath("debug_line.frag.spv");
    desc.colorFormats = {ctx.colorFormat};
    desc.depthFormat = ctx.depthFormat;
    desc.bindGroupLayouts = {&ctx.globalSetLayout};
    desc.samples = ctx.samples;
    desc.depthTest = false;
    desc.depthWrite = false;
    desc.cullMode = rhi::CullMode::None;
    desc.topology = rhi::Topology::LineList;
    pipeline_ = std::make_unique<Pipeline>(ctx.device, desc);

    buffers_.reserve(ctx.framesInFlight);
    for (uint32_t i = 0; i < ctx.framesInFlight; ++i)
        buffers_.push_back(std::make_unique<Buffer>(ctx.device,
            kMaxVerts * sizeof(Vertex),
            rhi::BufferUsage::Vertex, MemoryUsage::HostVisible));
}

void DebugLinesFeature::record(FrameContext& fc) {
    if (fc.stereo || !pipeline_) return;
    if (!fc.scene.settings().showSkeletons) return;

    // Build bone segments (parent→child) in world space from every Animator.
    std::vector<Vertex> verts;
    const glm::vec3 boneColor(0.1f, 1.0f, 0.2f);
    fc.scene.traverse([&](Node& node, const glm::mat4& world) {
        Animator* anim = node.getBehaviour<Animator>();
        if (!anim || !anim->rig()) return;
        const GlobalPose& gp = anim->globalPose();
        const auto& bones = anim->rig()->bones();
        const size_t n = std::min(bones.size(), gp.globalMatrices.size());
        for (size_t i = 0; i < n && verts.size() < kMaxVerts; ++i) {
            int parent = bones[i].parentIndex;
            if (parent < 0 || static_cast<size_t>(parent) >= n) continue;
            Vertex a{}; a.pos = glm::vec3(world * gp.globalMatrices[parent][3]); a.color = boneColor;
            Vertex b{}; b.pos = glm::vec3(world * gp.globalMatrices[i][3]);      b.color = boneColor;
            verts.push_back(a);
            verts.push_back(b);
        }
    });
    if (verts.empty()) return;

    const uint32_t count = std::min<uint32_t>(static_cast<uint32_t>(verts.size()), kMaxVerts);
    buffers_[fc.frameIndex]->write(verts.data(), count * sizeof(Vertex));

    fc.pass.setPipeline(*pipeline_);
    fc.pass.setBindGroup(0, *fc.globalSet);
    fc.pass.setVertexBuffer(*buffers_[fc.frameIndex]);
    fc.pass.draw(count);
}

} // namespace saida
