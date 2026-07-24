#include "editor/panels/ProfilerPanel.hpp"

#include "core/Profiler.hpp"
#include "editor/EditorUI.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <numeric>
#include <string>
#include <unordered_map>
#include <vector>

namespace saida {

namespace {

float bytesToMiB(uint64_t bytes) {
    return static_cast<float>(static_cast<double>(bytes) / (1024.0 * 1024.0));
}

const char* safeName(const char* name) {
    return name ? name : "";
}

bool containsNoCase(const char* text, const char* query) {
    if (!query || query[0] == '\0') return true;
    if (!text) return false;
    std::string a(text);
    std::string b(query);
    std::transform(a.begin(), a.end(), a.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(b.begin(), b.end(), b.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return a.find(b) != std::string::npos;
}

const ProfileCounter* findCounter(const ProfileFrame& frame, const char* name) {
    for (const auto& counter : frame.counters)
        if (counter.name && std::strcmp(counter.name, name) == 0) return &counter;
    return nullptr;
}

double scopeDuration(const ProfileFrame& frame, const char* name) {
    double total = 0.0;
    for (const ProfileEvent& event : frame.events) {
        if (event.name && std::strcmp(event.name, name) == 0)
            total += std::max(0.0, event.endMs - event.startMs);
    }
    return total;
}

double gpuDuration(const ProfileFrame& frame, const char* name) {
    double total = 0.0;
    for (const GpuProfileZone& zone : frame.gpuZones) {
        if (zone.name && std::strcmp(zone.name, name) == 0)
            total += std::max(0.0, zone.ms);
    }
    return total;
}

void drawMetric(const char* label, const char* value) {
    ImGui::BeginGroup();
    ImGui::TextDisabled("%s", label);
    ImGui::Text("%s", value);
    ImGui::EndGroup();
}

struct FrameStats {
    double avgCpu = 0.0;
    double maxCpu = 0.0;
    double avgGpu = 0.0;
    double maxGpu = 0.0;
    double avgDraws = 0.0;
    uint64_t worstFrame = 0;
    size_t count = 0;
};

FrameStats computeStats(const std::vector<ProfileFrame>& frames, size_t maxFrames = 120) {
    FrameStats stats{};
    const size_t first = frames.size() > maxFrames ? frames.size() - maxFrames : 0;
    for (size_t i = first; i < frames.size(); ++i) {
        const ProfileFrame& frame = frames[i];
        if (frame.cpuFrameMs <= 0.0) continue;
        stats.avgCpu += frame.cpuFrameMs;
        stats.maxCpu = std::max(stats.maxCpu, frame.cpuFrameMs);
        if (stats.maxCpu == frame.cpuFrameMs) stats.worstFrame = frame.index;
        const double gpu = gpuDuration(frame, "GPU/Frame");
        stats.avgGpu += gpu;
        stats.maxGpu = std::max(stats.maxGpu, gpu);
        if (const ProfileCounter* draws = findCounter(frame, "Renderer/DrawCalls")) {
            stats.avgDraws += draws->value;
        }
        ++stats.count;
    }
    if (stats.count > 0) {
        stats.avgCpu /= static_cast<double>(stats.count);
        stats.avgGpu /= static_cast<double>(stats.count);
        stats.avgDraws /= static_cast<double>(stats.count);
    }
    return stats;
}

void drawAnalysis(const ProfileFrame& frame) {
    struct Alert {
        const char* level = "Info";
        std::string text;
    };

    std::vector<Alert> alerts;
    const double cpu = frame.cpuFrameMs;
    const double gpu = gpuDuration(frame, "GPU/Frame");
    const double throttle = scopeDuration(frame, "Frame/Throttle");
    const double fence = scopeDuration(frame, "Vulkan/WaitForFrameFence");
    const double acquire = scopeDuration(frame, "Vulkan/AcquireNextImage");
    const double present = scopeDuration(frame, "Vulkan/QueuePresent");
    const double physics = scopeDuration(frame, "Physics/SceneStep");
    const double scripting = scopeDuration(frame, "Scripting/Update");
    const double scene = scopeDuration(frame, "Scene/Update");
    const double record = scopeDuration(frame, "Renderer/RecordCommandBuffer");
    const double webUploadBytes = findCounter(frame, "WebCanvas/UploadBytes")
        ? findCounter(frame, "WebCanvas/UploadBytes")->value : 0.0;

    auto add = [&](const char* level, const std::string& text) {
        alerts.push_back({level, text});
    };

    if (cpu <= 0.0) {
        ImGui::TextDisabled("No completed frame to analyze.");
        return;
    }

    if (cpu > 33.3) add("Spike", "CPU frame is above 33 ms.");
    else if (cpu > 16.7) add("Warn", "CPU frame is above 16.7 ms.");

    if (gpu > 0.0) {
        if (gpu > cpu * 1.15) add("GPU", "GPU work is higher than CPU frame time.");
        else if (cpu > gpu * 1.35 && throttle < cpu * 0.25) add("CPU", "CPU frame time dominates GPU time.");
    }

    if (throttle > cpu * 0.30) add("Info", "Frame cap / throttling is a large part of this frame.");
    if (fence > 1.0) add("Sync", "Waiting on the previous GPU frame is visible.");
    if (acquire > 1.0) add("Present", "Swapchain acquire is taking noticeable time.");
    if (present > 1.0) add("Present", "Queue present is taking noticeable time.");
    if (physics > 2.0) add("Physics", "Physics update is a visible cost.");
    if (scripting > 1.0) add("Script", "JavaScript behaviours are a visible cost.");
    if (scene > cpu * 0.35) add("Scene", "Scene update is a large part of the frame.");
    if (record > 2.0) add("Render", "Command recording is a visible CPU cost.");
    if (frame.memory.allocationCountThisFrame > 0 && frame.memory.allocatedBytesThisFrame > 16ull * 1024ull * 1024ull) {
        add("Memory", "Large allocation spike during this frame.");
    }
    if (webUploadBytes > 8.0 * 1024.0 * 1024.0) {
        add("UI", "WebCanvas uploaded more than 8 MiB this frame.");
    }

    if (frame.memory.vramBudgetBytes > 0) {
        const double ratio = static_cast<double>(frame.memory.vramUsedBytes) /
                             static_cast<double>(frame.memory.vramBudgetBytes);
        if (ratio > 0.90) add("Memory", "VRAM budget is above 90%.");
        else if (ratio > 0.75) add("Memory", "VRAM budget is above 75%.");
    }
    if (frame.memory.systemBudgetBytes > 0) {
        const double ratio = static_cast<double>(frame.memory.systemUsedBytes) /
                             static_cast<double>(frame.memory.systemBudgetBytes);
        if (ratio > 0.90) add("Memory", "System RAM usage is above 90%.");
    }

    if (alerts.empty()) {
        add("OK", "No obvious bottleneck in this frame.");
    }

    for (const Alert& alert : alerts) {
        ImVec4 color = ImVec4(0.49f, 0.80f, 0.39f, 1.0f);
        if (std::string(alert.level) == "Spike") color = ImVec4(0.95f, 0.25f, 0.25f, 1.0f);
        else if (std::string(alert.level) == "Warn") color = ImVec4(0.95f, 0.62f, 0.18f, 1.0f);
        else if (std::string(alert.level) == "OK") color = ImVec4(0.35f, 0.85f, 0.45f, 1.0f);
        ImGui::TextColored(color, "%s", alert.level);
        ImGui::SameLine(90.0f);
        ImGui::TextWrapped("%s", alert.text.c_str());
    }
}

bool drawTimeline(const std::vector<ProfileFrame>& frames, uint64_t& selectedFrame) {
    if (frames.empty()) return false;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = 72.0f;
    const int count = static_cast<int>(std::min<size_t>(frames.size(), 160));
    const int first = static_cast<int>(frames.size()) - count;
    const float barW = std::max(2.0f, width / static_cast<float>(std::max(1, count)));
    bool changed = false;

    dl->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), IM_COL32(18, 18, 22, 255), 4.0f);
    for (int i = 0; i < count; ++i) {
        const ProfileFrame& frame = frames[static_cast<size_t>(first + i)];
        const float ms = static_cast<float>(frame.cpuFrameMs);
        const float t = std::min(ms / 33.3f, 1.0f);
        const float h = std::max(2.0f, t * (height - 8.0f));
        const ImU32 color = ms > 33.3f ? IM_COL32(239, 68, 68, 255)
                           : ms > 16.7f ? IM_COL32(245, 158, 11, 255)
                                        : IM_COL32(59, 130, 246, 255);
        float x = origin.x + i * barW;
        dl->AddRectFilled(ImVec2(x, origin.y + height - h - 4.0f),
                          ImVec2(x + std::max(1.0f, barW - 1.0f), origin.y + height - 4.0f),
                          color);
        if (frame.index == selectedFrame) {
            dl->AddRect(ImVec2(x, origin.y + 3.0f),
                        ImVec2(x + std::max(1.0f, barW - 1.0f), origin.y + height - 3.0f),
                        IM_COL32(255, 255, 255, 230), 0.0f, 0, 1.5f);
        }
    }

    ImGui::InvisibleButton("##ProfilerTimeline", ImVec2(width, height));
    if (ImGui::IsItemHovered()) {
        const float localX = ImGui::GetIO().MousePos.x - origin.x;
        const int idx = std::clamp(static_cast<int>(localX / barW), 0, count - 1);
        const ProfileFrame& hovered = frames[static_cast<size_t>(first + idx)];
        ImGui::SetTooltip("Frame %llu\nCPU %.3f ms\nGPU %.3f ms",
            static_cast<unsigned long long>(hovered.index), hovered.cpuFrameMs,
            gpuDuration(hovered, "GPU/Frame"));
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            selectedFrame = hovered.index;
            changed = true;
        }
    }
    return changed;
}

std::vector<double> computeSelfTimes(const ProfileFrame& frame) {
    std::vector<double> self(frame.events.size(), 0.0);
    for (size_t i = 0; i < frame.events.size(); ++i) {
        self[i] = std::max(0.0, frame.events[i].endMs - frame.events[i].startMs);
    }
    for (size_t i = 0; i < frame.events.size(); ++i) {
        const ProfileEvent& child = frame.events[i];
        for (size_t p = i; p-- > 0;) {
            const ProfileEvent& parent = frame.events[p];
            if (parent.depth + 1 == child.depth &&
                parent.startMs <= child.startMs && parent.endMs >= child.endMs) {
                self[p] = std::max(0.0, self[p] - std::max(0.0, child.endMs - child.startMs));
                break;
            }
        }
    }
    return self;
}

void drawEventTree(const ProfileFrame& frame, const char* filter) {
    if (frame.events.empty()) {
        ImGui::TextDisabled("No CPU scopes recorded.");
        return;
    }

    const std::vector<double> selfTimes = computeSelfTimes(frame);
    if (ImGui::BeginTable("ProfilerCpuTree", 5,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Scope", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Thread", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn("Start", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Duration", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Self", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < frame.events.size(); ++i) {
            const ProfileEvent& event = frame.events[i];
            if (!containsNoCase(event.name, filter)) continue;
            const double duration = std::max(0.0, event.endMs - event.startMs);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Indent(static_cast<float>(event.depth) * 14.0f);
            ImGui::Text("%s", safeName(event.name));
            ImGui::Unindent(static_cast<float>(event.depth) * 14.0f);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextDisabled("%s", event.threadName.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%.2f ms", event.startMs);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%.3f ms", duration);
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%.3f ms", selfTimes[i]);
        }
        ImGui::EndTable();
    }
}

void drawCpuHotspots(const ProfileFrame& frame, const char* filter) {
    struct Aggregate {
        double totalMs = 0.0;
        double selfMs = 0.0;
        double maxMs = 0.0;
        int calls = 0;
    };
    const std::vector<double> selfTimes = computeSelfTimes(frame);
    std::unordered_map<std::string, Aggregate> map;
    for (size_t i = 0; i < frame.events.size(); ++i) {
        const ProfileEvent& event = frame.events[i];
        if (!containsNoCase(event.name, filter)) continue;
        const double duration = std::max(0.0, event.endMs - event.startMs);
        auto& aggregate = map[safeName(event.name)];
        aggregate.totalMs += duration;
        aggregate.selfMs += selfTimes[i];
        aggregate.maxMs = std::max(aggregate.maxMs, duration);
        aggregate.calls++;
    }

    std::vector<std::pair<std::string, Aggregate>> rows(map.begin(), map.end());
    std::sort(rows.begin(), rows.end(),
        [](const auto& a, const auto& b) { return a.second.totalMs > b.second.totalMs; });
    if (ImGui::BeginTable("ProfilerCpuHotspots", 6,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable)) {
        ImGui::TableSetupColumn("Scope", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Total", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Self", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Avg", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Max", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Calls", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableHeadersRow();

        if (ImGuiTableSortSpecs* sort = ImGui::TableGetSortSpecs()) {
            if (sort->SpecsCount > 0) {
                const ImGuiTableColumnSortSpecs& spec = sort->Specs[0];
                const bool asc = spec.SortDirection == ImGuiSortDirection_Ascending;
                std::sort(rows.begin(), rows.end(), [&](const auto& a, const auto& b) {
                    double av = 0.0;
                    double bv = 0.0;
                    switch (spec.ColumnIndex) {
                        case 1: av = a.second.totalMs; bv = b.second.totalMs; break;
                        case 2: av = a.second.selfMs; bv = b.second.selfMs; break;
                        case 3: av = a.second.calls ? a.second.totalMs / a.second.calls : 0.0;
                                bv = b.second.calls ? b.second.totalMs / b.second.calls : 0.0; break;
                        case 4: av = a.second.maxMs; bv = b.second.maxMs; break;
                        case 5: av = static_cast<double>(a.second.calls); bv = static_cast<double>(b.second.calls); break;
                        default:
                            return asc ? a.first < b.first : a.first > b.first;
                    }
                    return asc ? av < bv : av > bv;
                });
            }
        }

        for (const auto& [name, aggregate] : rows) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%s", name.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%.3f ms", aggregate.totalMs);
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%.3f ms", aggregate.selfMs);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%.3f ms", aggregate.calls > 0 ? aggregate.totalMs / aggregate.calls : 0.0);
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%.3f ms", aggregate.maxMs);
            ImGui::TableSetColumnIndex(5);
            ImGui::Text("%d", aggregate.calls);
        }
        ImGui::EndTable();
    }
}

void drawGpuZones(const ProfileFrame& frame) {
    if (frame.gpuZones.empty()) {
        ImGui::TextDisabled("No GPU timestamps resolved yet.");
        return;
    }

    std::vector<GpuProfileZone> zones = frame.gpuZones;
    if (ImGui::BeginTable("ProfilerGpuZones", 3,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable)) {
        ImGui::TableSetupColumn("Zone", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Start", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Duration", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableHeadersRow();

        if (ImGuiTableSortSpecs* sort = ImGui::TableGetSortSpecs()) {
            if (sort->SpecsCount > 0) {
                const ImGuiTableColumnSortSpecs& spec = sort->Specs[0];
                const bool asc = spec.SortDirection == ImGuiSortDirection_Ascending;
                std::sort(zones.begin(), zones.end(), [&](const GpuProfileZone& a, const GpuProfileZone& b) {
                    if (spec.ColumnIndex == 1) return asc ? a.startMs < b.startMs : a.startMs > b.startMs;
                    if (spec.ColumnIndex == 2) return asc ? a.ms < b.ms : a.ms > b.ms;
                    return asc ? std::strcmp(safeName(a.name), safeName(b.name)) < 0
                               : std::strcmp(safeName(a.name), safeName(b.name)) > 0;
                });
            }
        }

        for (const GpuProfileZone& zone : zones) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Indent(static_cast<float>(zone.depth) * 14.0f);
            ImGui::Text("%s", safeName(zone.name));
            ImGui::Unindent(static_cast<float>(zone.depth) * 14.0f);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%.3f ms", zone.startMs);
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%.3f ms", zone.ms);
        }
        ImGui::EndTable();
    }
}

void drawCounters(const ProfileFrame& frame) {
    if (frame.counters.empty()) {
        ImGui::TextDisabled("No counters recorded.");
        return;
    }

    std::vector<ProfileCounter> counters = frame.counters;
    if (ImGui::BeginTable("ProfilerCounters", 2,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable)) {
        ImGui::TableSetupColumn("Counter", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableHeadersRow();

        if (ImGuiTableSortSpecs* sort = ImGui::TableGetSortSpecs()) {
            if (sort->SpecsCount > 0) {
                const ImGuiTableColumnSortSpecs& spec = sort->Specs[0];
                const bool asc = spec.SortDirection == ImGuiSortDirection_Ascending;
                std::sort(counters.begin(), counters.end(), [&](const ProfileCounter& a, const ProfileCounter& b) {
                    if (spec.ColumnIndex == 1) return asc ? a.value < b.value : a.value > b.value;
                    return asc ? std::strcmp(safeName(a.name), safeName(b.name)) < 0
                               : std::strcmp(safeName(a.name), safeName(b.name)) > 0;
                });
            }
        }

        for (const ProfileCounter& counter : counters) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%s", safeName(counter.name));
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%.0f", counter.value);
        }
        ImGui::EndTable();
    }
}

void drawMemory(const MemorySnapshot& memory) {
    char buf[64];
    if (memory.processResidentBytes > 0) {
        std::snprintf(buf, sizeof(buf), "%.1f MiB", bytesToMiB(memory.processResidentBytes));
        drawMetric("Process RAM", buf);
        ImGui::SameLine();
    }
    std::snprintf(buf, sizeof(buf), "%.1f / %.1f MiB",
                  bytesToMiB(memory.vramUsedBytes), bytesToMiB(memory.vramBudgetBytes));
    drawMetric("VRAM", buf);
    ImGui::SameLine();
    std::snprintf(buf, sizeof(buf), "%.1f / %.1f MiB",
                  bytesToMiB(memory.hostVisibleUsedBytes), bytesToMiB(memory.hostVisibleBudgetBytes));
    drawMetric("VMA Host", buf);
    if (memory.systemBudgetBytes > 0) {
        ImGui::SameLine();
        std::snprintf(buf, sizeof(buf), "%.1f / %.1f MiB",
                      bytesToMiB(memory.systemUsedBytes), bytesToMiB(memory.systemBudgetBytes));
        drawMetric("System RAM", buf);
    }
    if (memory.allocationCountThisFrame > 0 || memory.freeCountThisFrame > 0) {
        ImGui::Spacing();
        std::snprintf(buf, sizeof(buf), "+%.1f MiB / %u",
                      bytesToMiB(memory.allocatedBytesThisFrame), memory.allocationCountThisFrame);
        drawMetric("Alloc Frame", buf);
        ImGui::SameLine();
        std::snprintf(buf, sizeof(buf), "-%.1f MiB / %u",
                      bytesToMiB(memory.freedBytesThisFrame), memory.freeCountThisFrame);
        drawMetric("Free Frame", buf);
    }

    ImGui::Spacing();
    if (memory.categories.empty()) {
        ImGui::TextDisabled("No tracked memory categories.");
        return;
    }

    std::vector<MemorySnapshot::Category> categories = memory.categories;
    if (ImGui::BeginTable("ProfilerMemoryCategories", 3,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable)) {
        ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableHeadersRow();

        if (ImGuiTableSortSpecs* sort = ImGui::TableGetSortSpecs()) {
            if (sort->SpecsCount > 0) {
                const ImGuiTableColumnSortSpecs& spec = sort->Specs[0];
                const bool asc = spec.SortDirection == ImGuiSortDirection_Ascending;
                std::sort(categories.begin(), categories.end(), [&](const auto& a, const auto& b) {
                    if (spec.ColumnIndex == 1) return asc ? a.bytes < b.bytes : a.bytes > b.bytes;
                    if (spec.ColumnIndex == 2) return asc ? a.count < b.count : a.count > b.count;
                    return asc ? a.name < b.name : a.name > b.name;
                });
            }
        }

        for (const auto& category : categories) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%s", category.name.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%.1f MiB", bytesToMiB(category.bytes));
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%u", category.count);
        }
        ImGui::EndTable();
    }
}

void drawCompactOverlay(const ProfileFrame& frame, const FrameStats& stats) {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 220.0f, 34.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.72f);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("Profiler Overlay", nullptr, flags)) {
        const double gpu = gpuDuration(frame, "GPU/Frame");
        ImGui::Text("CPU %.2f ms  avg %.2f", frame.cpuFrameMs, stats.avgCpu);
        ImGui::Text("GPU %.2f ms  avg %.2f", gpu, stats.avgGpu);
        if (const ProfileCounter* draws = findCounter(frame, "Renderer/DrawCalls"))
            ImGui::Text("Draws %.0f", draws->value);
        if (frame.cpuFrameMs > 33.3)
            ImGui::TextColored(ImVec4(0.95f, 0.25f, 0.25f, 1.0f), "Spike");
    }
    ImGui::End();
}

const ProfileFrame* findFrameByIndex(const std::vector<ProfileFrame>& frames, uint64_t index) {
    for (const ProfileFrame& frame : frames)
        if (frame.index == index) return &frame;
    return nullptr;
}

std::vector<ProfileFrame> selectRecentFramesByDuration(const std::vector<ProfileFrame>& frames,
                                                       double durationMs) {
    std::vector<ProfileFrame> out;
    double accumulated = 0.0;
    for (auto it = frames.rbegin(); it != frames.rend(); ++it) {
        if (it->cpuFrameMs <= 0.0) continue;
        out.push_back(*it);
        accumulated += it->cpuFrameMs;
        if (accumulated >= durationMs) break;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

std::string tracePathForFrame(uint64_t frameIndex) {
    std::filesystem::path path = std::filesystem::path(SAIDA_BINARY_DIR) / "profiling";
    path /= "profile_" + std::to_string(frameIndex) + ".json";
    return path.string();
}

bool exportTrace(const std::vector<ProfileFrame>& frames, std::string& status,
                 std::string* exportedPath = nullptr) {
    if (exportedPath) exportedPath->clear();
    if (frames.empty()) {
        status = "No frames to export.";
        return false;
    }

    std::string error;
    const std::string path = tracePathForFrame(frames.back().index);
    if (!Profiler::instance().exportChromeTrace(path, frames, &error)) {
        status = "Export failed: " + error;
        return false;
    }
    status = "Exported " + path;
    if (exportedPath) *exportedPath = path;
    return true;
}

} // namespace

void ProfilerPanel::draw(EditorUI* editor) {
    if (!editor || !editor->showProfiler_) return;

    static bool frozen = false;
    static bool freezeOnSpike = false;
    static float spikeThresholdMs = 40.0f;
    static ProfileFrame frozenFrame;
    static bool captureActive = false;
    static double captureStart = 0.0;
    static float captureSeconds = 5.0f;
    static std::string status;
    static std::string lastExportPath;
    static bool openedOnce = false;
    static bool compactOverlay = false;
    static bool recordOnlySpikes = false;
    static uint64_t selectedFrame = 0;
    static char scopeFilter[128] = "";

    Profiler& profiler = Profiler::instance();
    if (!openedOnce) {
        profiler.setEnabled(true);
        openedOnce = true;
    }

    auto frames = profiler.recentFrames();
    ProfileFrame liveFrame = frames.empty() ? ProfileFrame{} : frames.back();
    if (selectedFrame == 0 && liveFrame.index != 0) selectedFrame = liveFrame.index;

    if (freezeOnSpike && !frozen && liveFrame.cpuFrameMs >= spikeThresholdMs) {
        frozen = true;
        frozenFrame = liveFrame;
        status = "Frozen on spike.";
    }

    if (captureActive && ImGui::GetTime() - captureStart >= captureSeconds) {
        captureActive = false;
        std::vector<ProfileFrame> captureFrames = selectRecentFramesByDuration(frames, captureSeconds * 1000.0);
        if (recordOnlySpikes) {
            captureFrames.erase(std::remove_if(captureFrames.begin(), captureFrames.end(),
                [&](const ProfileFrame& f) { return f.cpuFrameMs < spikeThresholdMs; }),
                captureFrames.end());
        }
        exportTrace(captureFrames, status, &lastExportPath);
    }

    const ProfileFrame* selected = findFrameByIndex(frames, selectedFrame);
    ProfileFrame frame = frozen ? frozenFrame : (selected ? *selected : liveFrame);
    FrameStats stats = computeStats(frames);
    if (compactOverlay) drawCompactOverlay(liveFrame, stats);

    ImGui::SetNextWindowSize(ImVec2(820, 560), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Profiler", &editor->showProfiler_)) {
        ImGui::End();
        return;
    }

    bool enabled = profiler.enabled();
    if (ImGui::Checkbox("Enabled", &enabled)) {
        profiler.setEnabled(enabled);
    }
    ImGui::SameLine();
    ImGui::Checkbox("Freeze on spike", &freezeOnSpike);
    ImGui::SameLine();
    ImGui::Checkbox("Overlay", &compactOverlay);
    ImGui::SameLine();
    ImGui::Checkbox("Record spikes", &recordOnlySpikes);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(92.0f);
    ImGui::DragFloat("Spike ms", &spikeThresholdMs, 0.5f, 1.0f, 500.0f, "%.1f");
    ImGui::SameLine();
    if (ImGui::Button(frozen ? "Unfreeze" : "Freeze")) {
        frozen = !frozen;
        if (frozen) frozenFrame = liveFrame;
    }
    ImGui::SameLine();
    if (ImGui::Button(captureActive ? "Capturing..." : "Capture")) {
        captureActive = true;
        captureStart = ImGui::GetTime();
        status.clear();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70.0f);
    ImGui::DragFloat("sec", &captureSeconds, 0.25f, 1.0f, 30.0f, "%.1f");
    ImGui::SameLine();
    if (ImGui::Button("Export")) {
        std::vector<ProfileFrame> exportFrames = selectRecentFramesByDuration(frames, captureSeconds * 1000.0);
        if (recordOnlySpikes) {
            exportFrames.erase(std::remove_if(exportFrames.begin(), exportFrames.end(),
                [&](const ProfileFrame& f) { return f.cpuFrameMs < spikeThresholdMs; }),
                exportFrames.end());
        }
        exportTrace(exportFrames, status, &lastExportPath);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Frame %llu", static_cast<unsigned long long>(frame.index));
    if (!status.empty()) {
        const bool exported = status.rfind("Exported ", 0) == 0;
        const bool failed = status.rfind("Export failed", 0) == 0 ||
                            status.rfind("No frames", 0) == 0;
        const ImVec4 color = exported ? ImVec4(0.35f, 0.85f, 0.45f, 1.0f)
                            : failed ? ImVec4(0.95f, 0.35f, 0.30f, 1.0f)
                                     : ImVec4(0.65f, 0.72f, 0.82f, 1.0f);
        ImGui::TextColored(color, "%s", status.c_str());
        if (!lastExportPath.empty()) {
            ImGui::SameLine();
            if (ImGui::Button("Copy path")) {
                ImGui::SetClipboardText(lastExportPath.c_str());
            }
        }
    }

    char value[64];
    std::snprintf(value, sizeof(value), "%.2f ms", frame.cpuFrameMs);
    drawMetric("CPU Frame", value);
    ImGui::SameLine();
    std::snprintf(value, sizeof(value), "%.2f / %.2f ms", stats.avgCpu, stats.maxCpu);
    drawMetric("CPU Avg/Max", value);
    ImGui::SameLine();
    std::snprintf(value, sizeof(value), "%.1f", frame.cpuFrameMs > 0.0 ? 1000.0 / frame.cpuFrameMs : 0.0);
    drawMetric("FPS", value);
    ImGui::SameLine();
    double gpuTotal = 0.0;
    for (const auto& zone : frame.gpuZones)
        if (zone.depth == 0) gpuTotal += zone.ms;
    std::snprintf(value, sizeof(value), "%.2f ms", gpuTotal);
    drawMetric("GPU", value);
    if (const ProfileCounter* draws = findCounter(frame, "Renderer/DrawCalls")) {
        ImGui::SameLine();
        std::snprintf(value, sizeof(value), "%.0f", draws->value);
        drawMetric("Draws", value);
    }
    if (stats.worstFrame != 0) {
        ImGui::SameLine();
        std::snprintf(value, sizeof(value), "%llu", static_cast<unsigned long long>(stats.worstFrame));
        drawMetric("Worst", value);
    }

    ImGui::Spacing();
    drawTimeline(frames, selectedFrame);
    ImGui::SetNextItemWidth(260.0f);
    ImGui::InputTextWithHint("##ScopeFilter", "Search scope/counter", scopeFilter, sizeof(scopeFilter));
    ImGui::SameLine();
    if (ImGui::Button("Live")) selectedFrame = liveFrame.index;

    if (ImGui::BeginTabBar("ProfilerTabs")) {
        if (ImGui::BeginTabItem("Analysis")) {
            drawAnalysis(frame);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("CPU")) {
            if (ImGui::BeginTabBar("ProfilerCpuTabs")) {
                if (ImGui::BeginTabItem("Tree")) {
                    drawEventTree(frame, scopeFilter);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Hotspots")) {
                    drawCpuHotspots(frame, scopeFilter);
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("GPU")) {
            drawGpuZones(frame);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Memory")) {
            drawMemory(frame.memory);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Counters")) {
            drawCounters(frame);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}

} // namespace saida
