#pragma once
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace saida::traffic {

enum class Light { Red, Amber, Green };
struct SignalTiming {
    static constexpr uint32_t kApproachGroups = 4;
    static constexpr uint32_t kAxisGroups = 2;
    bool opposingTogether = true;
    double green = 12.0;
    double amber = 3.0;
    double clearance = 1.5;
    double pedestrian = 6.0;
    uint32_t phases() const { return opposingTogether ? kAxisGroups : kApproachGroups; }
    double cycle() const { return phases() * (green + amber + clearance) + pedestrian + clearance; }
    bool valid() const {
        return std::isfinite(green) && std::isfinite(amber) && std::isfinite(clearance) && std::isfinite(pedestrian)
            && green > 0.0 && amber > 0.0 && clearance > 0.0 && pedestrian > 0.0;
    }
};

// An intersection owns one program. Equal intersection keys and host time yield
// equal phases even when its incoming lanes are in different streamed regions.
struct Signal {
    uint64_t key = 0;
    SignalTiming timing;
    double offset = 0.0;
    double phase(double time) const {
        if (!timing.valid() || !std::isfinite(time) || !std::isfinite(offset))
            throw std::invalid_argument("invalid traffic signal timing");
        const double cycle = timing.cycle();
        double result = std::fmod(time + offset, cycle);
        return result < 0.0 ? result + cycle : result;
    }
    Light light(uint32_t group, double time) const {
        const double start = (group % timing.phases()) * (timing.green + timing.amber + timing.clearance);
        const double t = phase(time) - start;
        if (t >= 0.0 && t < timing.green) return Light::Green;
        if (t >= timing.green && t < timing.green + timing.amber) return Light::Amber;
        return Light::Red;
    }
    bool pedestriansMayStart(double time) const {
        const double t = phase(time) - timing.phases() * (timing.green + timing.amber + timing.clearance);
        return t >= 0.0 && t < timing.pedestrian;
    }
};

} // namespace saida::traffic
