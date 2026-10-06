#include "render/features/GrassFeature.hpp"

#include "core/Camera.hpp"
#include "core/Log.hpp"
#include "core/Paths.hpp"
#include "scene/Scene.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace saida {

namespace {
constexpr uint64_t kGroundBytes = uint64_t(GrassNode::kMaxGroundSamples) * GrassNode::kMaxGroundSamples * sizeof(float);
constexpr uint64_t kCoverBytes = uint64_t(GrassNode::kMaxCoverSize) * GrassNode::kMaxCoverSize * sizeof(uint32_t);

// The node-local box of a field: the unit square of (u, v) carried back
// through the inverse of its affine map.
void fieldBox(const glm::mat3& uvFromLocal, glm::vec2& lo, glm::vec2& hi) {
    const glm::mat3 localFromUv = glm::inverse(uvFromLocal);
    lo = glm::vec2(std::numeric_limits<float>::max());
    hi = glm::vec2(std::numeric_limits<float>::lowest());
    for (const glm::vec2 c : {glm::vec2(0, 0), glm::vec2(1, 0), glm::vec2(0, 1), glm::vec2(1, 1)}) {
        const glm::vec3 p = localFromUv * glm::vec3(c, 1.0f);
        lo = glm::min(lo, glm::vec2(p));
        hi = glm::max(hi, glm::vec2(p));
    }
}
}  // namespace

GrassFeature::~GrassFeature() = default;

GrassFeature::RingDraw GrassFeature::ringDraw(int ring, float density, float radius, glm::vec2 eye,
                                              glm::vec2 lo, glm::vec2 hi) {
    RingDraw d;
    const float nearest = 1.0f / std::sqrt(std::max(density, 1e-3f));
    d.spacing = nearest * float(1 << ring);
    // Every ring centred on one point, snapped to twice the coarsest spacing,
    // so each ring's square is whole cells of the next and the hole between
    // them is exact.
    const float snap = 2.0f * nearest * float(1 << (kRings - 1));
    const glm::vec2 centre = glm::round(eye / snap) * snap;
    const float half = 0.5f * float(kRingCells) * d.spacing;
    d.origin = centre - glm::vec2(half);
    if (ring > 0) d.hole = glm::vec2(0.5f * half);
    // Cells within the field, the ring and the camera's reach.
    const glm::vec2 from = glm::max(glm::max(lo, eye - glm::vec2(radius)), d.origin);
    const glm::vec2 to = glm::min(glm::min(hi, eye + glm::vec2(radius)), d.origin + glm::vec2(2.0f * half));
    if (from.x >= to.x || from.y >= to.y) return d;
    d.cellMin = glm::clamp(glm::ivec2(glm::floor((from - d.origin) / d.spacing)), glm::ivec2(0),
                           glm::ivec2(kRingCells - 1));
    const glm::ivec2 cellMax = glm::clamp(glm::ivec2(glm::ceil((to - d.origin) / d.spacing)), glm::ivec2(1),
                                          glm::ivec2(kRingCells));
    d.cells = glm::max(cellMax - d.cellMin, glm::ivec2(0));
    return d;
}

void GrassFeature::createPipelines(const RenderContext& ctx) {
    device_ = &ctx.device;
    const uint32_t frames = std::max(1u, ctx.framesInFlight);
    setLayout_ = std::make_unique<rhi::BindGroupLayout>(*device_,
        std::vector<rhi::BindGroupLayoutEntry>{
            {0, rhi::BindingType::UniformBuffer, rhi::ShaderStages::Vertex | rhi::ShaderStages::Fragment},
            {1, rhi::BindingType::StorageBuffer, rhi::ShaderStages::Vertex},
            {2, rhi::BindingType::StorageBuffer, rhi::ShaderStages::Vertex},
        });
    const uint64_t uboBytes = sizeof(GpuGrass) * kMaxFields;
    frames_.resize(frames);
    for (FrameBuffers& f : frames_) {
        f.ubo = std::make_unique<Buffer>(*device_, uboBytes, rhi::BufferUsage::Uniform, MemoryUsage::HostVisible);
        f.ground = std::make_unique<Buffer>(*device_, kGroundBytes * kMaxFields, rhi::BufferUsage::Storage,
                                            MemoryUsage::HostVisible);
        f.cover = std::make_unique<Buffer>(*device_, kCoverBytes * kMaxFields, rhi::BufferUsage::Storage,
                                           MemoryUsage::HostVisible);
        std::vector<rhi::BindGroupEntry> entries(3);
        entries[0].binding = 0; entries[0].buffer = f.ubo.get(); entries[0].range = uboBytes;
        entries[1].binding = 1; entries[1].buffer = f.ground.get(); entries[1].range = kGroundBytes * kMaxFields;
        entries[2].binding = 2; entries[2].buffer = f.cover.get(); entries[2].range = kCoverBytes * kMaxFields;
        f.set = std::make_unique<rhi::BindGroup>(*setLayout_, entries);
    }

    Pipeline::Desc desc;
    desc.vertPath = shaderPath(ctx.stereo() ? "multiview.grass.vert.spv" : "grass.vert.spv");
    desc.fragPath = shaderPath("grass.frag.spv");
    desc.colorFormats = {ctx.colorFormat};
    desc.depthFormat = ctx.depthFormat;
    desc.bindGroupLayouts = {&ctx.globalSetLayout, setLayout_.get()};
    desc.samples = ctx.samples;
    desc.vertexInput = false;
    desc.cullMode = rhi::CullMode::None;  // a blade is seen from both sides
    desc.pushConstantSize = sizeof(Push);
    desc.viewMask = ctx.viewMask;
    pipeline_ = std::make_unique<Pipeline>(ctx.device, desc);
}

void GrassFeature::record(FrameContext& fc) {
    glm::vec3 eyeWorld(0.0f);
    if (fc.eyes && !fc.eyes->empty()) eyeWorld = fc.eyes->front().eyePosition;
    else if (fc.camera) eyeWorld = fc.camera->position;
    else return;

    FrameBuffers& f = frames_[std::min<size_t>(fc.frameIndex, frames_.size() - 1)];
    drawn_.clear();
    uint32_t count = 0;
    for (GrassNode* node : fc.scene.grassFields()) {
        if (!node->isActiveInHierarchy() || !node->isVisibleInHierarchy()) continue;
        const GrassNode::Field* field = node->field();
        if (!field) continue;
        const glm::mat4 toWorld = node->worldTransform();
        const glm::vec3 eye = glm::vec3(glm::inverse(toWorld) * glm::vec4(eyeWorld, 1.0f));
        glm::vec2 lo, hi;
        fieldBox(field->uvFromLocal, lo, hi);
        const glm::vec2 xz(eye.x, eye.z);
        if (glm::any(glm::lessThan(hi, xz - glm::vec2(node->radius))) ||
            glm::any(glm::greaterThan(lo, xz + glm::vec2(node->radius))))
            continue;
        if (count == kMaxFields) {
            if (!overflowReported_) {
                Log::warn("[Grass] more than ", kMaxFields, " fields within reach: the farthest are not drawn");
                overflowReported_ = true;
            }
            break;
        }
        const uint32_t slot = count++;
        GpuGrass& g = packed_[slot];
        g.localToWorld = toWorld;
        const glm::mat3& m = field->uvFromLocal;  // columns: x, z, 1
        g.uvFromLocalU = glm::vec4(m[0][0], m[1][0], m[2][0], 0.0f);
        g.uvFromLocalV = glm::vec4(m[0][1], m[1][1], m[2][1], 0.0f);
        g.blades = glm::vec4(node->density, node->bladeHeight, node->bladeWidth, node->radius);
        const glm::vec2 wind = glm::length(node->windDirection) > 0.0f ? glm::normalize(node->windDirection)
                                                                       : glm::vec2(1.0f, 0.0f);
        g.wind = glm::vec4(wind, node->wind, fc.time);
        g.camera = glm::vec4(eye, 0.0f);
        g.sizes = glm::ivec4(field->groundSamples, field->coverSize, 0, 0);
        for (int i = 0; i < GrassNode::kMaxBenders; ++i) g.benders[i] = node->benders[size_t(i)];
        if (f.nodes[slot] != node || f.revisions[slot] != node->revision()) {
            f.ground->write(field->heights.data(), field->heights.size() * sizeof(float), slot * kGroundBytes);
            f.cover->write(field->cover.data(), field->cover.size() * sizeof(uint32_t), slot * kCoverBytes);
            f.nodes[slot] = node;
            f.revisions[slot] = node->revision();
        }
        drawn_.push_back({slot, node->density, node->radius, xz, lo, hi});
    }
    if (count == 0) return;
    f.ubo->write(packed_.data(), sizeof(GpuGrass) * count);

    fc.pass.setPipeline(*pipeline_);
    fc.pass.setBindGroup(0, *fc.globalSet);
    fc.pass.setBindGroup(1, *f.set);
    for (const Drawn& d : drawn_)
        for (int ring = 0; ring < kRings; ++ring) {
            const RingDraw r = ringDraw(ring, d.density, d.radius, d.eye, d.lo, d.hi);
            if (r.cells.x <= 0 || r.cells.y <= 0) continue;
            const uint32_t vertices = kBladeVertices[size_t(ring)];
            Push push{r.origin, r.spacing, d.slot, r.cellMin, uint32_t(r.cells.x), uint32_t(ring), r.hole,
                      vertices, 0u};
            fc.pass.setPushConstants(&push, sizeof(Push));
            fc.pass.draw(uint32_t(r.cells.x) * uint32_t(r.cells.y) * vertices);
        }
}

} // namespace saida
