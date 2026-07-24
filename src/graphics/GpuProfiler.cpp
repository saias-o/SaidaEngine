#include "graphics/GpuProfiler.hpp"

#include "graphics/VulkanDevice.hpp"

#include <algorithm>
#include <stdexcept>

namespace saida {

GpuProfiler::GpuProfiler(VulkanDevice& device, uint32_t framesInFlight, uint32_t maxTimestampsPerFrame)
    : device_(device),
      framesInFlight_(framesInFlight),
      maxTimestampsPerFrame_(maxTimestampsPerFrame),
      frames_(framesInFlight) {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(device_.physicalDevice(), &props);
    timestampPeriod_ = props.limits.timestampPeriod;

    VkQueryPoolCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    ci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    ci.queryCount = framesInFlight_ * maxTimestampsPerFrame_;
    if (vkCreateQueryPool(device_.device(), &ci, nullptr, &queryPool_) != VK_SUCCESS) {
        throw std::runtime_error("failed to create GPU profiler query pool");
    }
}

GpuProfiler::~GpuProfiler() {
    if (queryPool_) vkDestroyQueryPool(device_.device(), queryPool_, nullptr);
}

void GpuProfiler::beginFrame(uint32_t frameIndex) {
    currentFrame_ = frameIndex % framesInFlight_;
    resolveFrame(currentFrame_);
    frames_[currentFrame_].zones.clear();
    frames_[currentFrame_].nextQuery = 0;
    activeStack_.clear();
}

void GpuProfiler::resetQueries(VkCommandBuffer cmd) {
    const uint32_t first = currentFrame_ * maxTimestampsPerFrame_;
    vkCmdResetQueryPool(cmd, queryPool_, first, maxTimestampsPerFrame_);
}

uint32_t GpuProfiler::beginZone(VkCommandBuffer cmd, const char* name) {
    FrameState& frame = frames_[currentFrame_];
    if (frame.nextQuery + 2 > maxTimestampsPerFrame_) return UINT32_MAX;

    Zone zone;
    zone.name = name ? name : "(unnamed)";
    zone.beginQuery = currentFrame_ * maxTimestampsPerFrame_ + frame.nextQuery++;
    zone.endQuery = currentFrame_ * maxTimestampsPerFrame_ + frame.nextQuery++;
    zone.depth = static_cast<uint32_t>(activeStack_.size());

    const uint32_t handle = static_cast<uint32_t>(frame.zones.size());
    frame.zones.push_back(std::move(zone));
    activeStack_.push_back(handle);

    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPool_,
                        frame.zones.back().beginQuery);
    return handle;
}

void GpuProfiler::endZone(VkCommandBuffer cmd, uint32_t zoneHandle) {
    FrameState& frame = frames_[currentFrame_];
    if (zoneHandle == UINT32_MAX || zoneHandle >= frame.zones.size()) return;

    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool_,
                        frame.zones[zoneHandle].endQuery);
    if (!activeStack_.empty()) activeStack_.pop_back();
}

void GpuProfiler::publishLatest() {
    Profiler::instance().setGpuZones(latestZones_);
}

void GpuProfiler::resolveFrame(uint32_t frameIndex) {
    latestZones_.clear();
    if (!queryPool_ || frameIndex >= frames_.size()) return;

    const FrameState& frame = frames_[frameIndex];
    if (frame.nextQuery == 0 || frame.zones.empty()) return;

    struct TimestampResult {
        uint64_t value = 0;
        uint64_t available = 0;
    };
    std::vector<TimestampResult> results(frame.nextQuery);
    VkResult result = vkGetQueryPoolResults(device_.device(), queryPool_,
        frameIndex * maxTimestampsPerFrame_, frame.nextQuery,
        sizeof(TimestampResult) * results.size(), results.data(),
        sizeof(TimestampResult),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);

    if (result != VK_SUCCESS && result != VK_NOT_READY) return;

    uint64_t firstTimestamp = UINT64_MAX;
    for (const Zone& zone : frame.zones) {
        const uint32_t beginLocal = zone.beginQuery - frameIndex * maxTimestampsPerFrame_;
        if (beginLocal < results.size() && results[beginLocal].available) {
            firstTimestamp = std::min(firstTimestamp, results[beginLocal].value);
        }
    }
    if (firstTimestamp == UINT64_MAX) return;

    latestZones_.reserve(frame.zones.size());
    for (const Zone& zone : frame.zones) {
        const uint32_t beginLocal = zone.beginQuery - frameIndex * maxTimestampsPerFrame_;
        const uint32_t endLocal = zone.endQuery - frameIndex * maxTimestampsPerFrame_;
        if (beginLocal >= results.size() || endLocal >= results.size()) continue;
        if (!results[beginLocal].available || !results[endLocal].available) continue;
        if (results[endLocal].value < results[beginLocal].value) continue;

        const uint64_t ticks = results[endLocal].value - results[beginLocal].value;
        GpuProfileZone out;
        out.name = zone.name;
        out.depth = zone.depth;
        out.startMs = static_cast<double>(results[beginLocal].value - firstTimestamp) *
                      static_cast<double>(timestampPeriod_) / 1000000.0;
        out.ms = static_cast<double>(ticks) * static_cast<double>(timestampPeriod_) / 1000000.0;
        latestZones_.push_back(std::move(out));
    }
}

GpuProfileScope::GpuProfileScope(GpuProfiler* profiler, VkCommandBuffer cmd, const char* name)
    : profiler_(profiler), cmd_(cmd) {
    if (profiler_) handle_ = profiler_->beginZone(cmd_, name);
}

GpuProfileScope::~GpuProfileScope() {
    if (profiler_) profiler_->endZone(cmd_, handle_);
}

} // namespace saida
