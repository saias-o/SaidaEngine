#pragma once

#include "core/Profiler.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace saida {

class VulkanDevice;

class MemoryProfiler {
public:
    static void registerAllocation(const std::string& category, uint64_t bytes);
    static void unregisterAllocation(const std::string& category, uint64_t bytes);
    static MemorySnapshot sample(VulkanDevice& device);
    static void publish(VulkanDevice& device);
    // Host allocation headroom, without a renderer or profiler capture.
    // Windows: the smaller of free physical RAM and remaining commit.
    // nullopt means the platform could not provide a measurement.
    static std::optional<uint64_t> availableHostBytes();
};

} // namespace saida
