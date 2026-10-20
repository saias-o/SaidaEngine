#include "ui/RmlUiGpuRenderer.hpp"

#include "core/Paths.hpp"
#include "core/Profiler.hpp"
#include "graphics/VulkanDevice.hpp"
#include "rhi/Rhi.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <unordered_map>

namespace saida {
namespace {
struct Vertex { float x, y, u, v, r, g, b, a; };
struct Push { float width, height; uint32_t wrap, pad = 0; };
struct Batch {
    uint32_t first = 0, count = 0, wrap = 0;
    int32_t x = 0, y = 0;
    uint32_t width = 0, height = 0;
    std::shared_ptr<Texture> texture;
};

Vertex vertexFor(const Rml::Vertex& v, const RmlUiRenderInterface::Draw& draw) {
    Rml::Vector4f p(v.position.x + draw.translation.x, v.position.y + draw.translation.y, 0.f, 1.f);
    if (draw.transformEnabled) p = draw.transform * p;
    const float w = std::abs(p.w) > 1e-6f ? p.w : 1.f;
    return {p.x / w, p.y / w, v.tex_coord.x, v.tex_coord.y,
            v.colour.red / 255.f, v.colour.green / 255.f,
            v.colour.blue / 255.f, v.colour.alpha / 255.f};
}
}

struct RmlUiGpuRenderer::State {
    VulkanDevice& device;
    rhi::BindGroupLayout geometryLayout;
    rhi::BindGroupLayout resolveLayout;
    std::unique_ptr<Pipeline> geometryPipeline, resolvePipeline;
    std::unique_ptr<rhi::RenderTexture> accumulation;
    std::unique_ptr<rhi::BindGroup> resolveGroup;
    std::shared_ptr<Texture> white;
    struct CachedTexture {
        std::weak_ptr<const RmlUiRenderInterface::TextureData> source;
        std::shared_ptr<Texture> texture;
    };
    std::unordered_map<const void*, CachedTexture> textures;
    struct Frame {
        std::unique_ptr<Buffer> vertices;
        std::vector<std::shared_ptr<Texture>> textures;
        std::vector<std::unique_ptr<rhi::BindGroup>> groups;
    };
    std::vector<Frame> frames;
    bool accumulationInitialized = false;

    explicit State(VulkanDevice& d)
        : device(d), geometryLayout(d, {{0, rhi::BindingType::StorageBuffer, rhi::ShaderStages::Vertex},
                                       {1, rhi::BindingType::CombinedImageSampler, rhi::ShaderStages::Fragment}}),
          resolveLayout(d, {{0, rhi::BindingType::CombinedImageSampler, rhi::ShaderStages::Fragment}}) {
        Pipeline::Desc desc;
        desc.vertPath = shaderPath("rml_ui.vert.spv");
        desc.fragPath = shaderPath("rml_ui.frag.spv");
        desc.colorFormats = {rhi::Format::RGBA8Unorm};
        desc.bindGroupLayouts = {geometryLayout};
        desc.vertexInput = desc.depthTest = desc.depthWrite = false;
        desc.cullMode = rhi::CullMode::None;
        desc.blendMode = rhi::BlendMode::PremultipliedAlpha;
        desc.pushConstantSize = sizeof(Push);
        geometryPipeline = std::make_unique<Pipeline>(d, desc);
        desc.vertPath = shaderPath("rml_ui_resolve.vert.spv");
        desc.fragPath = shaderPath("rml_ui_resolve.frag.spv");
        desc.colorFormats = {rhi::Format::RGBA8Srgb};
        desc.bindGroupLayouts = {resolveLayout};
        desc.blendMode = rhi::BlendMode::None;
        resolvePipeline = std::make_unique<Pipeline>(d, desc);
        const uint8_t pixel[4] = {255, 255, 255, 255};
        white = std::make_shared<Texture>(d, pixel, 1, 1, rhi::Format::RGBA8Unorm, false);
    }

    std::shared_ptr<Texture> textureFor(const std::shared_ptr<const RmlUiRenderInterface::TextureData>& data) {
        if (!data) return white;
        auto it = textures.find(data.get());
        if (it != textures.end() && !it->second.source.expired()) return it->second.texture;
        auto texture = std::make_shared<Texture>(device, data->premultipliedRgba.data(),
            data->size.x, data->size.y, rhi::Format::RGBA8Unorm, false, rhi::AddressMode::ClampToEdge);
        textures[data.get()] = {data, texture};
        return texture;
    }

    void ensureTarget(uint32_t width, uint32_t height) {
        if (accumulation) {
            if (accumulation->extent().width != width || accumulation->extent().height != height)
                throw std::logic_error("UI GPU target resized without retiring its renderer");
            return;
        }
        rhi::RenderTextureDesc desc;
        desc.width = width; desc.height = height;
        desc.format = rhi::Format::RGBA8Unorm;
        desc.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::ColorAttachment;
        desc.memoryCategory = "UI/GpuAccumulation";
        accumulation = std::make_unique<rhi::RenderTexture>(device, desc);
        rhi::BindGroupEntry entry;
        entry.view = accumulation->view(); entry.sampler = white->sampler();
        resolveGroup = std::make_unique<rhi::BindGroup>(resolveLayout, std::vector<rhi::BindGroupEntry>{entry});
    }
};

RmlUiGpuRenderer::RmlUiGpuRenderer(VulkanDevice& device) : state_(std::make_unique<State>(device)) {}
RmlUiGpuRenderer::~RmlUiGpuRenderer() = default;

void RmlUiGpuRenderer::render(const RmlUiRenderInterface::DrawList& list,
                             rhi::vulkan::CommandEncoder& encoder, Texture& target, uint32_t frameSlot) {
    SAIDA_PROFILE_SCOPE("WebCanvas/GpuRaster");
    auto& s = *state_;
    s.ensureTarget(list.width, list.height);
    if (s.frames.size() <= frameSlot) s.frames.resize(frameSlot + 1);
    auto& frame = s.frames[frameSlot];
    // The host has waited for this frame slot's fence. Other slots retain every
    // descriptor, buffer and texture referenced by their submitted commands.
    frame.groups.clear(); frame.textures.clear();
    for (auto it = s.textures.begin(); it != s.textures.end();) {
        if (it->second.source.expired()) it = s.textures.erase(it); else ++it;
    }
    std::vector<Vertex> vertices;
    std::vector<Batch> batches;
    for (const auto& draw : list.draws) {
        Batch batch;
        const int left = draw.scissorEnabled ? std::clamp(draw.scissor.Left(), 0, int(list.width)) : 0;
        const int top = draw.scissorEnabled ? std::clamp(draw.scissor.Top(), 0, int(list.height)) : 0;
        const int right = draw.scissorEnabled ? std::clamp(draw.scissor.Right(), left, int(list.width)) : int(list.width);
        const int bottom = draw.scissorEnabled ? std::clamp(draw.scissor.Bottom(), top, int(list.height)) : int(list.height);
        if (left == right || top == bottom) continue;
        batch.x = left; batch.y = top; batch.width = right-left; batch.height = bottom-top;
        batch.first = static_cast<uint32_t>(vertices.size());
        for (size_t i = 0; i + 2 < draw.geometry->indices.size(); i += 3) {
            std::array<Vertex, 3> triangle;
            bool valid = true;
            for (size_t j = 0; j < 3; ++j) {
                const int index = draw.geometry->indices[i+j];
                if (index < 0 || size_t(index) >= draw.geometry->vertices.size()) { valid = false; break; }
                triangle[j] = vertexFor(draw.geometry->vertices[index], draw);
                if (!std::isfinite(triangle[j].x) || !std::isfinite(triangle[j].y)) { valid = false; break; }
            }
            if (valid) vertices.insert(vertices.end(), triangle.begin(), triangle.end());
        }
        batch.count = static_cast<uint32_t>(vertices.size()) - batch.first;
        if (!batch.count) continue;
        batch.wrap = draw.geometry->wrapTexCoords;
        batch.texture = s.textureFor(draw.texture);
        batches.push_back(std::move(batch));
    }
    if (!vertices.empty()) {
        const uint64_t bytes = vertices.size() * sizeof(Vertex);
        if (!frame.vertices || frame.vertices->size() < bytes) {
            const uint64_t capacity = (bytes + 65535) / 65536 * 65536;
            frame.vertices = std::make_unique<Buffer>(s.device, capacity, rhi::BufferUsage::Storage, MemoryUsage::HostVisible);
        }
        frame.vertices->write(vertices.data(), bytes);
        for (const auto& batch : batches) {
            rhi::BindGroupEntry buffer; buffer.binding = 0; buffer.buffer = frame.vertices.get();
            rhi::BindGroupEntry image; image.binding = 1;
            image.view = batch.texture->imageView(); image.sampler = batch.texture->sampler();
            frame.groups.push_back(std::make_unique<rhi::BindGroup>(s.geometryLayout,
                std::vector<rhi::BindGroupEntry>{buffer, image}));
            frame.textures.push_back(batch.texture);
        }
    }
    encoder.transition(s.accumulation->image(), s.accumulationInitialized ? rhi::ResourceState::ShaderRead
                       : rhi::ResourceState::Undefined, rhi::ResourceState::ColorAttachment);
    rhi::RenderPassDesc passDesc;
    passDesc.width = list.width; passDesc.height = list.height; passDesc.colorCount = 1;
    passDesc.colors[0].view = s.accumulation->view();
    passDesc.colors[0].clearColor = {{0, 0, 0, 0}};
    auto pass = encoder.beginRenderPass(passDesc);
    pass.setPipeline(*s.geometryPipeline);
    for (size_t i = 0; i < batches.size(); ++i) {
        const auto& batch = batches[i];
        pass.setBindGroup(0, *frame.groups[i]);
        const Push push{float(list.width), float(list.height), batch.wrap};
        pass.setPushConstants(&push, sizeof(push));
        pass.setScissor(batch.x, batch.y, batch.width, batch.height);
        pass.draw(batch.count, 1, batch.first);
    }
    pass.end();
    encoder.transition(s.accumulation->image(), rhi::ResourceState::ColorAttachment, rhi::ResourceState::ShaderRead);
    s.accumulationInitialized = true;
    encoder.transition(target.image(), rhi::ResourceState::ShaderRead, rhi::ResourceState::ColorAttachment);
    passDesc.colors[0].view = target.imageView();
    auto resolve = encoder.beginRenderPass(passDesc);
    resolve.setPipeline(*s.resolvePipeline); resolve.setBindGroup(0, *s.resolveGroup);
    resolve.draw(3); resolve.end();
    encoder.transition(target.image(), rhi::ResourceState::ColorAttachment, rhi::ResourceState::ShaderRead);
    SAIDA_PROFILE_COUNTER_ADD("WebCanvas/GpuDraws", static_cast<double>(batches.size()));
    SAIDA_PROFILE_COUNTER_ADD("WebCanvas/VertexUploadBytes", static_cast<double>(vertices.size() * sizeof(Vertex)));
}
} // namespace saida
