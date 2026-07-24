#include "render/features/OutlineFeature.hpp"

#include "core/Paths.hpp"
#include "graphics/Mesh.hpp"
#include "rhi/vulkan/Format.hpp"
#include "nodes/MeshNode.hpp"

#include <algorithm>
#include <vector>

namespace saida {

void OutlineFeature::createPipelines(const RenderContext& ctx) {
    const char* vert = ctx.stereo() ? "multiview.outline.vert.spv" : "outline.vert.spv";

    Pipeline::Desc desc;
    desc.vertPath = shaderPath(vert);
    desc.fragPath = shaderPath("outline.frag.spv");
    desc.colorFormats = {ctx.colorFormat};
    desc.depthFormat = ctx.depthFormat;
    desc.bindGroupLayouts = {&ctx.globalSetLayout};
    desc.samples = ctx.samples;
    desc.depthWrite = false;
    desc.depthCompare = rhi::CompareOp::LessOrEqual;
    desc.cullMode = rhi::CullMode::Front;
    desc.blendMode = rhi::BlendMode::Alpha;
    desc.pushConstantSize = sizeof(Push);
    desc.viewMask = ctx.viewMask;
    pipeline_ = std::make_unique<Pipeline>(ctx.device, desc);
}

void OutlineFeature::record(FrameContext& fc) {
    if (!pipeline_ || !fc.draws || fc.drawCount == 0) return;

    const float viewportW = static_cast<float>(std::max(1u, fc.extent.width));
    const float viewportH = static_cast<float>(std::max(1u, fc.extent.height));
    bool bound = false;

    for (uint32_t i = 0; i < fc.drawCount; ++i) {
        const SceneDraw& draw = fc.draws[i];
        MeshNode* node = draw.node;
        if (!node || !draw.mesh || !node->outlineEnabled()) continue;

        const float width = node->outlineWidth();
        const glm::vec4 color = node->outlineColor();
        if (width <= 0.0f || color.a <= 0.0f) continue;

        if (!bound) {
            fc.pass.setPipeline(*pipeline_);
            fc.pass.setBindGroup(0, *fc.globalSet);
            bound = true;
        }

        Push pc{};
        pc.model = draw.world;
        pc.color = color;
        pc.params = glm::vec4(width, static_cast<float>(draw.boneOffset),
                              viewportW, viewportH);
        const Aabb& bounds = draw.mesh->bounds();
        pc.localCenter = glm::vec4(bounds.center(), 0.0f);
        pc.localHalfExtent = glm::vec4(glm::max(bounds.extent() * 0.5f, glm::vec3(1e-4f)), 0.0f);
        fc.pass.setPushConstants(&pc, sizeof(Push));

        draw.mesh->bind(fc.pass);
        draw.mesh->draw(fc.pass);
    }
}

} // namespace saida
