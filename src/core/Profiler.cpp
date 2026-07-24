#include "core/Profiler.hpp"

#include <algorithm>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>

namespace saida {

namespace {
thread_local std::vector<uint32_t> t_scopeStack;
thread_local std::string t_threadName;

uint64_t threadHash() {
    return static_cast<uint64_t>(
        std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

std::string defaultThreadName() {
    return std::this_thread::get_id() == std::thread::id{}
        ? std::string("unknown")
        : std::string("thread ") + std::to_string(threadHash());
}

std::string jsonEscape(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    std::ostringstream ss;
                    ss << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                       << static_cast<int>(static_cast<unsigned char>(c));
                    out += ss.str();
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

void writeTraceEvent(std::ostream& out, bool& first, std::string_view name,
                     std::string_view cat, uint64_t pid, uint64_t tid,
                     double tsUs, double durUs) {
    if (!first) out << ",\n";
    first = false;
    out << "{\"name\":\"" << jsonEscape(name)
        << "\",\"cat\":\"" << jsonEscape(cat)
        << "\",\"ph\":\"X\",\"pid\":" << pid
        << ",\"tid\":" << tid
        << ",\"ts\":" << std::fixed << std::setprecision(3) << tsUs
        << ",\"dur\":" << std::fixed << std::setprecision(3) << std::max(0.0, durUs)
        << "}";
}

bool sameName(const char* a, const char* b) {
    if (!a || !b) return a == b;
    return std::strcmp(a, b) == 0;
}
} // namespace

Profiler& Profiler::instance() {
    static Profiler profiler;
    return profiler;
}

double Profiler::nowMs() const {
    return std::chrono::duration<double, std::milli>(
        Clock::now() - frameStart_).count();
}

void Profiler::beginFrame() {
    if (!enabled()) return;

    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled()) return;
    if (frames_.size() < kFrameHistory) {
        frames_.resize(kFrameHistory);
    }

    currentSlot_ = static_cast<size_t>(nextFrameIndex_ % kFrameHistory);
    ProfileFrame& frame = frames_[currentSlot_];
    frame = {};
    frame.index = nextFrameIndex_++;
    frameStart_ = Clock::now();
    frameActive_ = true;
    t_scopeStack.clear();
}

void Profiler::endFrame() {
    if (!enabled() && !frameActive_) return;

    std::lock_guard<std::mutex> lock(mutex_);
    if (!frameActive_ || frames_.empty()) return;
    frames_[currentSlot_].cpuFrameMs = nowMs();
    frameActive_ = false;
    t_scopeStack.clear();
}

uint32_t Profiler::beginScope(const char* name) {
    if (!enabled() || !frameActive_) return UINT32_MAX;

    std::lock_guard<std::mutex> lock(mutex_);
    if (!frameActive_) return UINT32_MAX;
    ProfileFrame& frame = frames_[currentSlot_];
    ProfileEvent event;
    event.name = name ? name : "(unnamed)";
    event.threadHash = threadHash();
    event.threadName = t_threadName.empty() ? defaultThreadName() : t_threadName;
    event.startMs = nowMs();
    event.endMs = event.startMs;
    event.depth = static_cast<uint32_t>(t_scopeStack.size());
    frame.events.push_back(std::move(event));

    uint32_t handle = static_cast<uint32_t>(frame.events.size() - 1);
    t_scopeStack.push_back(handle);
    return handle;
}

void Profiler::endScope(uint32_t handle) {
    if (!enabled() || handle == UINT32_MAX || !frameActive_) return;

    std::lock_guard<std::mutex> lock(mutex_);
    if (!frameActive_) return;
    ProfileFrame& frame = frames_[currentSlot_];
    if (handle < frame.events.size()) {
        frame.events[handle].endMs = nowMs();
    }
    if (!t_scopeStack.empty()) {
        t_scopeStack.pop_back();
    }
}

void Profiler::setCounter(const char* name, double value) {
    if (!enabled() || !name || !frameActive_) return;

    std::lock_guard<std::mutex> lock(mutex_);
    if (!frameActive_) return;
    auto& counters = frames_[currentSlot_].counters;
    auto it = std::find_if(counters.begin(), counters.end(),
        [&](const ProfileCounter& c) { return sameName(c.name, name); });
    if (it == counters.end()) {
        counters.push_back({name, value});
    } else {
        it->value = value;
    }
}

void Profiler::addCounter(const char* name, double value) {
    if (!enabled() || !name || !frameActive_) return;

    std::lock_guard<std::mutex> lock(mutex_);
    if (!frameActive_) return;
    auto& counters = frames_[currentSlot_].counters;
    auto it = std::find_if(counters.begin(), counters.end(),
        [&](const ProfileCounter& c) { return sameName(c.name, name); });
    if (it == counters.end()) {
        counters.push_back({name, value});
    } else {
        it->value += value;
    }
}

void Profiler::setGpuZones(std::vector<GpuProfileZone> zones) {
    if (!enabled() || !frameActive_) return;

    std::lock_guard<std::mutex> lock(mutex_);
    if (!frameActive_) return;
    frames_[currentSlot_].gpuZones = std::move(zones);
}

void Profiler::setMemorySnapshot(const MemorySnapshot& snapshot) {
    if (!enabled() || !frameActive_) return;

    std::lock_guard<std::mutex> lock(mutex_);
    if (!frameActive_) return;
    frames_[currentSlot_].memory = snapshot;
}

std::vector<ProfileFrame> Profiler::recentFrames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ProfileFrame> out;
    out.reserve(frames_.size());
    for (const ProfileFrame& frame : frames_) {
        if (frame.index != 0 && frame.cpuFrameMs > 0.0) out.push_back(frame);
    }
    std::sort(out.begin(), out.end(),
        [](const ProfileFrame& a, const ProfileFrame& b) {
            return a.index < b.index;
        });
    return out;
}

ProfileFrame Profiler::latestFrame() const {
    std::lock_guard<std::mutex> lock(mutex_);
    ProfileFrame latest;
    for (const ProfileFrame& frame : frames_) {
        if (frame.index > latest.index && frame.cpuFrameMs > 0.0) latest = frame;
    }
    return latest;
}

bool Profiler::exportChromeTrace(const std::string& path,
                                 const std::vector<ProfileFrame>& frames,
                                 std::string* error) const {
    namespace fs = std::filesystem;

    try {
        fs::path outPath(path);
        if (outPath.has_parent_path()) {
            fs::create_directories(outPath.parent_path());
        }

        std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error) *error = "could not open output file";
            return false;
        }

        out << "{\"displayTimeUnit\":\"ms\",\"traceEvents\":[\n";
        bool first = true;
        double frameBaseUs = 0.0;
        for (const ProfileFrame& frame : frames) {
            if (frame.index == 0 || frame.cpuFrameMs <= 0.0) continue;

            writeTraceEvent(out, first,
                "Frame " + std::to_string(frame.index),
                "Frame", 1, 0, frameBaseUs, frame.cpuFrameMs * 1000.0);

            for (const ProfileEvent& event : frame.events) {
                const double startUs = frameBaseUs + event.startMs * 1000.0;
                const double durUs = (event.endMs - event.startMs) * 1000.0;
                writeTraceEvent(out, first, event.name, "CPU",
                                1, event.threadHash, startUs, durUs);
            }

            for (const GpuProfileZone& zone : frame.gpuZones) {
                writeTraceEvent(out, first, zone.name, "GPU",
                                2, zone.depth,
                                frameBaseUs + zone.startMs * 1000.0,
                                zone.ms * 1000.0);
            }

            frameBaseUs += frame.cpuFrameMs * 1000.0;
        }
        out << "\n]}\n";
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

void Profiler::setThreadName(const std::string& name) {
    t_threadName = name;
}

void Profiler::setEnabled(bool enabled) {
    enabled_.store(enabled, std::memory_order_relaxed);
    if (!enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        frameActive_ = false;
    }
}

ProfileScope::ProfileScope(const char* name) {
    handle_ = Profiler::instance().beginScope(name);
}

ProfileScope::~ProfileScope() {
    Profiler::instance().endScope(handle_);
}

} // namespace saida
