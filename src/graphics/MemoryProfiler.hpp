#pragma once

#include "core/Profiler.hpp"

#include <cstdint>
#include <string>

namespace saida {

class VulkanDevice;

class MemoryProfiler {
public:
    static void registerAllocation(const std::string& category, uint64_t bytes);
    static void unregisterAllocation(const std::string& category, uint64_t bytes);
    static MemorySnapshot sample(VulkanDevice& device);
    static void publish(VulkanDevice& device);
};

} // namespace saida
