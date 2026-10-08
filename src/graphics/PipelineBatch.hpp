#pragma once

#include "core/Log.hpp"
#include "graphics/Pipeline.hpp"
#include "rhi/Rhi.hpp"

#include <chrono>
#include <exception>
#include <future>
#include <memory>
#include <vector>

namespace saida {

// Layouts and the device remain borrowed until both lanes have joined. Only
// independent pipeline objects are built here; render queues and pools stay on
// their owning thread. Vulkan's default pipeline cache is internally synchronized.
inline std::vector<std::unique_ptr<Pipeline>> buildGraphicsPipelines(
    rhi::Device& device, const std::vector<Pipeline::Desc>& descriptions) {
    const auto start = std::chrono::steady_clock::now();
    std::vector<std::unique_ptr<Pipeline>> result(descriptions.size());
#ifdef SAIDA_RHI_WEBGPU
    for (size_t i = 0; i < descriptions.size(); ++i)
        result[i] = std::make_unique<Pipeline>(device, descriptions[i]);
#else
    if (descriptions.size() < 2) {
        for (size_t i = 0; i < descriptions.size(); ++i)
            result[i] = std::make_unique<Pipeline>(device, descriptions[i]);
    } else {
        auto buildLane = [&](size_t first) {
            for (size_t i = first; i < descriptions.size(); i += 2)
                result[i] = std::make_unique<Pipeline>(device, descriptions[i]);
        };
        auto worker = std::async(std::launch::async, buildLane, 0);
        std::exception_ptr error;
        try { buildLane(1); } catch (...) { error = std::current_exception(); }
        try { worker.get(); } catch (...) { if (!error) error = std::current_exception(); }
        if (error) std::rethrow_exception(error);
    }
#endif
    Log::info("Pipeline batch: ", result.size(), " pipelines, build_ms=",
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    return result;
}

} // namespace saida
