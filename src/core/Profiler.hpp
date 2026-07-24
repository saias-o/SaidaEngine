#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifndef SAIDA_ENABLE_PROFILER
#define SAIDA_ENABLE_PROFILER 1
#endif

namespace saida {

struct ProfileEvent {
    const char* name = "";
    std::string threadName;
    uint64_t threadHash = 0;
    double startMs = 0.0;
    double endMs = 0.0;
    uint32_t depth = 0;
};

struct ProfileCounter {
    const char* name = "";
    double value = 0.0;
};

struct MemorySnapshot {
    struct Category {
        std::string name;
        uint64_t bytes = 0;
        uint32_t count = 0;
    };

    uint64_t processResidentBytes = 0;
    uint64_t systemUsedBytes = 0;
    uint64_t systemBudgetBytes = 0;
    uint64_t vramUsedBytes = 0;
    uint64_t vramBudgetBytes = 0;
    uint64_t hostVisibleUsedBytes = 0;
    uint64_t hostVisibleBudgetBytes = 0;
    uint64_t allocatedBytesThisFrame = 0;
    uint64_t freedBytesThisFrame = 0;
    uint32_t allocationCountThisFrame = 0;
    uint32_t freeCountThisFrame = 0;
    std::vector<Category> categories;
};

struct GpuProfileZone {
    const char* name = "";
    double startMs = 0.0;
    double ms = 0.0;
    uint32_t depth = 0;
};

struct ProfileFrame {
    uint64_t index = 0;
    double cpuFrameMs = 0.0;
    std::vector<ProfileEvent> events;
    std::vector<ProfileCounter> counters;
    std::vector<GpuProfileZone> gpuZones;
    MemorySnapshot memory;
};

class Profiler {
public:
    static Profiler& instance();

    void beginFrame();
    void endFrame();

    uint32_t beginScope(const char* name);
    void endScope(uint32_t handle);

    void setCounter(const char* name, double value);
    void addCounter(const char* name, double value);
    void setGpuZones(std::vector<GpuProfileZone> zones);
    void setMemorySnapshot(const MemorySnapshot& snapshot);

    std::vector<ProfileFrame> recentFrames() const;
    ProfileFrame latestFrame() const;
    bool exportChromeTrace(const std::string& path,
                           const std::vector<ProfileFrame>& frames,
                           std::string* error = nullptr) const;

    void setThreadName(const std::string& name);
    bool enabled() const { return enabled_.load(std::memory_order_relaxed); }
    void setEnabled(bool enabled);

private:
    using Clock = std::chrono::steady_clock;

    Profiler() = default;
    double nowMs() const;

    mutable std::mutex mutex_;
    std::vector<ProfileFrame> frames_;
    size_t currentSlot_ = 0;
    uint64_t nextFrameIndex_ = 1;
    Clock::time_point frameStart_{};
    bool frameActive_ = false;
    std::atomic<bool> enabled_{false};
    static constexpr size_t kFrameHistory = 600;
};

class ProfileScope {
public:
    explicit ProfileScope(const char* name);
    ~ProfileScope();
    ProfileScope(const ProfileScope&) = delete;
    ProfileScope& operator=(const ProfileScope&) = delete;

private:
    uint32_t handle_ = UINT32_MAX;
};

} // namespace saida

#if SAIDA_ENABLE_PROFILER
#define SAIDA_PROFILE_FRAME_BEGIN() ::saida::Profiler::instance().beginFrame()
#define SAIDA_PROFILE_FRAME_END() ::saida::Profiler::instance().endFrame()
#define SAIDA_PROFILE_SCOPE(name) ::saida::ProfileScope SAIDA_PROFILE_CONCAT(_neProfileScope_, __LINE__)(name)
#define SAIDA_PROFILE_FUNCTION() SAIDA_PROFILE_SCOPE(__func__)
#define SAIDA_PROFILE_COUNTER(name, value) ::saida::Profiler::instance().setCounter((name), static_cast<double>(value))
#define SAIDA_PROFILE_COUNTER_ADD(name, value) ::saida::Profiler::instance().addCounter((name), static_cast<double>(value))
#else
#define SAIDA_PROFILE_FRAME_BEGIN() do {} while (false)
#define SAIDA_PROFILE_FRAME_END() do {} while (false)
#define SAIDA_PROFILE_SCOPE(name) do {} while (false)
#define SAIDA_PROFILE_FUNCTION() do {} while (false)
#define SAIDA_PROFILE_COUNTER(name, value) do {} while (false)
#define SAIDA_PROFILE_COUNTER_ADD(name, value) do {} while (false)
#endif

#define SAIDA_PROFILE_CONCAT_INNER(a, b) a##b
#define SAIDA_PROFILE_CONCAT(a, b) SAIDA_PROFILE_CONCAT_INNER(a, b)
