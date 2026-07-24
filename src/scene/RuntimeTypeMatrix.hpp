#pragma once

#include "scene/BehaviourRegistry.hpp"
#include "scene/NodeRegistry.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <set>
#include <string>

namespace saida {

// Canonical V1 inventory of concrete node/behaviour factories. Every runtime
// verifies its live registries against this table; a type cannot silently be
// added to one target or disappear from another without updating the contract.
enum class RuntimeTypeTarget : std::size_t {
    Native = 0,
    Headless = 1,
    AuthoringWasm = 2,
    PlayerWeb = 3,
};

enum class RuntimeTypeAvailability {
    Absent,
    Required,
    Optional,
};

enum class RuntimeTypeCategory {
    Node,
    Behaviour,
};

struct RuntimeTypeRow {
    const char* name;
    RuntimeTypeCategory category;
    std::array<RuntimeTypeAvailability, 4> availability;
};

constexpr RuntimeTypeAvailability A = RuntimeTypeAvailability::Absent;
constexpr RuntimeTypeAvailability R = RuntimeTypeAvailability::Required;
constexpr RuntimeTypeAvailability O = RuntimeTypeAvailability::Optional;

// Rows are sorted by category then public serialized name. XR factories are
// optional in the native target because they depend on SAIDA_ENABLE_XR.
inline constexpr std::array<RuntimeTypeRow, 48> kRuntimeTypeMatrix{{
    {"Area", RuntimeTypeCategory::Node, {R, R, A, R}},
    {"Camera", RuntimeTypeCategory::Node, {R, R, R, R}},
    {"CharacterBody", RuntimeTypeCategory::Node, {R, R, A, R}},
    {"CollisionShape", RuntimeTypeCategory::Node, {R, R, A, R}},
    {"FixedJoint", RuntimeTypeCategory::Node, {R, R, A, R}},
    {"HingeJoint", RuntimeTypeCategory::Node, {R, R, A, R}},
    {"LightNode", RuntimeTypeCategory::Node, {R, R, R, R}},
    {"MeshNode", RuntimeTypeCategory::Node, {R, R, R, R}},
    {"Node", RuntimeTypeCategory::Node, {R, R, R, R}},
    {"ParticleSystem", RuntimeTypeCategory::Node, {R, R, R, R}},
    {"PointJoint", RuntimeTypeCategory::Node, {R, R, A, R}},
    {"RigidBody", RuntimeTypeCategory::Node, {R, R, A, R}},
    {"Scene", RuntimeTypeCategory::Node, {R, A, A, R}},
    {"StaticBody", RuntimeTypeCategory::Node, {R, R, A, R}},
    {"TeleportArea", RuntimeTypeCategory::Node, {O, A, A, A}},
    {"UIButtonNode", RuntimeTypeCategory::Node, {R, A, A, A}},
    {"UICanvasNode", RuntimeTypeCategory::Node, {R, R, R, R}},
    {"UIColorNode", RuntimeTypeCategory::Node, {R, A, A, A}},
    {"UIImageNode", RuntimeTypeCategory::Node, {R, A, A, A}},
    {"UIInteractableNode", RuntimeTypeCategory::Node, {R, A, A, A}},
    {"UINode", RuntimeTypeCategory::Node, {R, R, R, R}},
    {"UITextNode", RuntimeTypeCategory::Node, {R, R, R, R}},
    {"UIToggleNode", RuntimeTypeCategory::Node, {R, A, A, A}},
    {"Water", RuntimeTypeCategory::Node, {R, R, R, R}},
    {"WebCanvasNode", RuntimeTypeCategory::Node, {R, A, A, A}},
    {"XRAnchor", RuntimeTypeCategory::Node, {O, A, A, A}},
    {"XRController", RuntimeTypeCategory::Node, {O, A, A, A}},
    {"XRHand", RuntimeTypeCategory::Node, {O, A, A, A}},
    {"XROrigin", RuntimeTypeCategory::Node, {O, A, A, A}},

    {"Animator", RuntimeTypeCategory::Behaviour, {R, R, A, R}},
    {"AudioSource", RuntimeTypeCategory::Behaviour, {R, R, A, R}},
    {"Blackboard", RuntimeTypeCategory::Behaviour, {R, R, A, R}},
    {"CameraFollow", RuntimeTypeCategory::Behaviour, {R, R, A, R}},
    {"Character", RuntimeTypeCategory::Behaviour, {R, R, A, R}},
    {"Health", RuntimeTypeCategory::Behaviour, {R, R, A, A}},
    {"LOD Group", RuntimeTypeCategory::Behaviour, {R, R, A, A}},
    {"Rotator", RuntimeTypeCategory::Behaviour, {R, R, A, R}},
    {"ScenarioAnchor", RuntimeTypeCategory::Behaviour, {R, R, A, A}},
    {"ScenarioDirector", RuntimeTypeCategory::Behaviour, {R, R, A, A}},
    {"ScenarioRunner", RuntimeTypeCategory::Behaviour, {R, R, A, A}},
    {"ScriptBehaviour", RuntimeTypeCategory::Behaviour, {R, R, A, R}},
    {"SequenceDirector", RuntimeTypeCategory::Behaviour, {R, R, A, R}},
    {"Spawner", RuntimeTypeCategory::Behaviour, {R, R, A, R}},
    {"StateMachine", RuntimeTypeCategory::Behaviour, {R, R, A, R}},
    {"XRDirectInteractor", RuntimeTypeCategory::Behaviour, {O, A, A, A}},
    {"XRGrabbable", RuntimeTypeCategory::Behaviour, {O, A, A, A}},
    {"XRRayInteractor", RuntimeTypeCategory::Behaviour, {O, A, A, A}},
    {"XRTouchable", RuntimeTypeCategory::Behaviour, {O, A, A, A}},
}};

inline const char* runtimeTypeTargetName(RuntimeTypeTarget target) {
    switch (target) {
        case RuntimeTypeTarget::Native: return "native";
        case RuntimeTypeTarget::Headless: return "headless";
        case RuntimeTypeTarget::AuthoringWasm: return "authoringWasm";
        case RuntimeTypeTarget::PlayerWeb: return "playerWeb";
    }
    return "unknown";
}

inline const char* runtimeTypeAvailabilityName(RuntimeTypeAvailability availability) {
    switch (availability) {
        case RuntimeTypeAvailability::Absent: return "absent";
        case RuntimeTypeAvailability::Required: return "required";
        case RuntimeTypeAvailability::Optional: return "optional";
    }
    return "absent";
}

inline nlohmann::json buildRuntimeTypeMatrixManifest() {
    nlohmann::json types = nlohmann::json::array();
    for (const RuntimeTypeRow& row : kRuntimeTypeMatrix) {
        nlohmann::json support = nlohmann::json::object();
        for (std::size_t i = 0; i < row.availability.size(); ++i) {
            const auto target = static_cast<RuntimeTypeTarget>(i);
            support[runtimeTypeTargetName(target)] =
                runtimeTypeAvailabilityName(row.availability[i]);
        }
        types.push_back({
            {"name", row.name},
            {"category", row.category == RuntimeTypeCategory::Node ? "node" : "behaviour"},
            {"support", std::move(support)},
        });
    }
    return {
        {"schema", 1},
        {"targets", {"native", "headless", "authoringWasm", "playerWeb"}},
        {"types", std::move(types)},
    };
}

inline bool verifyRegisteredRuntimeTypes(RuntimeTypeTarget target, std::string& error,
                                         bool allowAdditional = false) {
    std::set<std::string> requiredNodes;
    std::set<std::string> allowedNodes;
    std::set<std::string> requiredBehaviours;
    std::set<std::string> allowedBehaviours;
    const std::size_t targetIndex = static_cast<std::size_t>(target);

    for (const RuntimeTypeRow& row : kRuntimeTypeMatrix) {
        const RuntimeTypeAvailability availability = row.availability[targetIndex];
        if (availability == RuntimeTypeAvailability::Absent) continue;
        std::set<std::string>& allowed = row.category == RuntimeTypeCategory::Node
            ? allowedNodes : allowedBehaviours;
        allowed.insert(row.name);
        if (availability == RuntimeTypeAvailability::Required) {
            std::set<std::string>& required = row.category == RuntimeTypeCategory::Node
                ? requiredNodes : requiredBehaviours;
            required.insert(row.name);
        }
    }

    auto verifyCategory = [&](const char* category, const auto& factories,
                              const std::set<std::string>& required,
                              const std::set<std::string>& allowed) {
        for (const std::string& name : required) {
            if (factories.find(name) == factories.end()) {
                error = std::string(runtimeTypeTargetName(target)) + " missing required " +
                        category + " type '" + name + "'";
                return false;
            }
        }
        if (!allowAdditional) {
            for (const auto& [name, factory] : factories) {
                (void)factory;
                if (allowed.find(name) == allowed.end()) {
                    error = std::string(runtimeTypeTargetName(target)) + " registered unexpected " +
                            category + " type '" + name + "'";
                    return false;
                }
            }
        }
        return true;
    };

    return verifyCategory("node", NodeRegistry::instance().factories(),
                          requiredNodes, allowedNodes) &&
           verifyCategory("behaviour", BehaviourRegistry::instance().factories(),
                          requiredBehaviours, allowedBehaviours);
}

} // namespace saida
