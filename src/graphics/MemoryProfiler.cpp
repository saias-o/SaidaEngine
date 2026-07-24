#include "graphics/MemoryProfiler.hpp"

#include "graphics/VulkanDevice.hpp"
#include "vk_mem_alloc.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#endif

namespace saida {

namespace {

struct CategoryStats {
    uint64_t bytes = 0;
    uint32_t count = 0;
};

std::mutex g_categoryMutex;
std::unordered_map<std::string, CategoryStats> g_categories;
std::atomic<uint64_t> g_allocatedBytesThisFrame{0};
std::atomic<uint64_t> g_freedBytesThisFrame{0};
std::atomic<uint32_t> g_allocationCountThisFrame{0};
std::atomic<uint32_t> g_freeCountThisFrame{0};

void sampleSystemMemory(MemorySnapshot& snapshot) {
#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        snapshot.systemBudgetBytes = status.ullTotalPhys;
        snapshot.systemUsedBytes = status.ullTotalPhys - status.ullAvailPhys;
    }

    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        snapshot.processResidentBytes = static_cast<uint64_t>(counters.WorkingSetSize);
    }
#else
    (void)snapshot;
#endif
}

std::vector<MemorySnapshot::Category> sampleCategories() {
    std::lock_guard<std::mutex> lock(g_categoryMutex);
    std::vector<MemorySnapshot::Category> out;
    out.reserve(g_categories.size());
    for (const auto& [name, stats] : g_categories) {
        if (stats.count == 0 && stats.bytes == 0) continue;
        out.push_back({name, stats.bytes, stats.count});
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return a.bytes > b.bytes;
    });
    return out;
}

} // namespace

void MemoryProfiler::registerAllocation(const std::string& category, uint64_t bytes) {
    if (category.empty() || bytes == 0) return;
    g_allocatedBytesThisFrame.fetch_add(bytes, std::memory_order_relaxed);
    g_allocationCountThisFrame.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_categoryMutex);
    auto& stats = g_categories[category];
    stats.bytes += bytes;
    stats.count++;
}

void MemoryProfiler::unregisterAllocation(const std::string& category, uint64_t bytes) {
    if (category.empty() || bytes == 0) return;
    g_freedBytesThisFrame.fetch_add(bytes, std::memory_order_relaxed);
    g_freeCountThisFrame.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_categoryMutex);
    auto it = g_categories.find(category);
    if (it == g_categories.end()) return;
    it->second.bytes = it->second.bytes > bytes ? it->second.bytes - bytes : 0;
    if (it->second.count > 0) it->second.count--;
}

MemorySnapshot MemoryProfiler::sample(VulkanDevice& device) {
    MemorySnapshot snapshot{};
    sampleSystemMemory(snapshot);

    VkPhysicalDeviceMemoryProperties memoryProps{};
    vkGetPhysicalDeviceMemoryProperties(device.physicalDevice(), &memoryProps);

    VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
    vmaGetHeapBudgets(device.allocator(), budgets);

    for (uint32_t i = 0; i < memoryProps.memoryHeapCount; ++i) {
        const uint64_t used = budgets[i].usage;
        const uint64_t budget = budgets[i].budget ? budgets[i].budget
                                                  : memoryProps.memoryHeaps[i].size;
        if (memoryProps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            snapshot.vramUsedBytes += used;
            snapshot.vramBudgetBytes += budget;
        } else {
            snapshot.hostVisibleUsedBytes += used;
            snapshot.hostVisibleBudgetBytes += budget;
        }
    }

    snapshot.allocatedBytesThisFrame =
        g_allocatedBytesThisFrame.exchange(0, std::memory_order_relaxed);
    snapshot.freedBytesThisFrame =
        g_freedBytesThisFrame.exchange(0, std::memory_order_relaxed);
    snapshot.allocationCountThisFrame =
        g_allocationCountThisFrame.exchange(0, std::memory_order_relaxed);
    snapshot.freeCountThisFrame =
        g_freeCountThisFrame.exchange(0, std::memory_order_relaxed);
    snapshot.categories = sampleCategories();
    return snapshot;
}

void MemoryProfiler::publish(VulkanDevice& device) {
    Profiler::instance().setMemorySnapshot(sample(device));
}

} // namespace saida
