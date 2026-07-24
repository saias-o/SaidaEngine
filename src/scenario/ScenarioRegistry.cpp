#include "scenario/ScenarioRegistry.hpp"

#include <algorithm>
#include <array>

namespace saida {
namespace {

constexpr std::array<const char*, 17> kActions = {
    "audio.play",
    "audio.stop",
    "blackboard.set",
    "camera.setTarget",
    "input.lock",
    "node.disable",
    "node.enable",
    "node.setTransform",
    "objective.complete",
    "objective.fail",
    "objective.show",
    "scene.freeOwned",
    "scene.instantiate",
    "signal.emit",
    "slot.call",
    "timeline.play",
    "timeline.stop",
};

constexpr std::array<const char*, 12> kConditions = {
    "all",
    "any",
    "area.entered",
    "area.exited",
    "blackboard.equals",
    "group.count",
    "input.pressed",
    "node.enabled",
    "node.exists",
    "not",
    "signal.received",
    "time.elapsed",
};

constexpr std::array<const char*, 1> kTimelineConditions = {
    "timeline.finished",
};

template <typename Array>
bool contains(const Array& values, const std::string& name) {
    return std::find(values.begin(), values.end(), name) != values.end();
}

} // namespace

bool ScenarioActionRegistry::isKnown(const std::string& name) {
    return contains(kActions, name);
}

std::vector<std::string> ScenarioActionRegistry::names() {
    std::vector<std::string> out(kActions.begin(), kActions.end());
    std::sort(out.begin(), out.end());
    return out;
}

bool ScenarioConditionRegistry::isKnown(const std::string& name) {
    return contains(kConditions, name) || contains(kTimelineConditions, name);
}

bool ScenarioConditionRegistry::isComposite(const std::string& name) {
    return name == "all" || name == "any" || name == "not";
}

std::vector<std::string> ScenarioConditionRegistry::names() {
    std::vector<std::string> out(kConditions.begin(), kConditions.end());
    out.insert(out.end(), kTimelineConditions.begin(), kTimelineConditions.end());
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace saida
