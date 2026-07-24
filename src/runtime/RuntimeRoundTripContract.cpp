#include "runtime/RuntimeRoundTripContract.hpp"

#include "authoring/SceneSnapshot.hpp"
#include "core/Reflection.hpp"
#include "graphics/ResourceManager.hpp"
#include "scene/Behaviour.hpp"
#include "scene/BehaviourRegistry.hpp"
#include "scene/Node.hpp"
#include "scene/NodeRegistry.hpp"
#include "scene/Scene.hpp"
#ifndef SAIDA_AUTHORING_WEB
#include "scene/SceneSerializer.hpp"
#endif

#include <nlohmann/json.hpp>

#include <exception>
#include <memory>
#include <string>

namespace saida::runtime {
namespace {

using json = nlohmann::json;

json changedValue(const reflect::PropertyDesc& property, const json& current) {
    if (property.kind == "bool") return !current.get<bool>();
    if (property.kind == "float") {
        const double value = current.get<double>();
        if (property.hasRange)
            return value != property.min ? property.min : property.max;
        return value + 1.25;
    }
    if (property.kind == "int") {
        const int value = current.get<int>();
        if (property.hasRange)
            return value != static_cast<int>(property.min)
                ? static_cast<int>(property.min) : static_cast<int>(property.max);
        return value + 3;
    }
    if (property.kind == "enum") {
        const int value = current.get<int>();
        const int count = static_cast<int>(property.enumLabels.size());
        return count > 1 ? (value + 1) % count : value + 1;
    }
    if (property.kind == "string" || property.kind == "asset")
        return "roundtrip/" + property.name;
    if (property.kind == "vec3") return json::array({1.25f, -2.5f, 3.75f});
    if (property.kind == "vec4")
        return json::array({0.125f, 0.375f, 0.625f, 0.875f});
    throw std::runtime_error("unsupported reflected property kind '" +
                             property.kind + "'");
}

std::size_t seedReflectedProperties(const std::string& type, void* object) {
    const reflect::TypeDesc* desc = reflect::TypeRegistry::instance().find(type);
    if (!desc) return 0;
    for (const reflect::PropertyDesc& property : desc->properties) {
        json before;
        property.get(object, before);
        property.set(object, changedValue(property, before));
        json after;
        property.get(object, after);
        if (after == before)
            throw std::runtime_error(type + "." + property.name +
                                     " rejected its non-default contract value");
    }
    return desc->properties.size();
}

bool promisedHere(const RuntimeTypeRow& row, RuntimeTypeTarget target) {
    const RuntimeTypeAvailability availability =
        row.availability[static_cast<std::size_t>(target)];
    if (availability == RuntimeTypeAvailability::Required) return true;
    if (availability != RuntimeTypeAvailability::Optional) return false;
    if (row.category == RuntimeTypeCategory::Node)
        return NodeRegistry::instance().factories().find(row.name) !=
               NodeRegistry::instance().factories().end();
    return BehaviourRegistry::instance().factories().find(row.name) !=
           BehaviourRegistry::instance().factories().end();
}

void seedNode(Node& node, ResourceManager& resources, std::size_t index) {
    json values = {
        {"name", "ContractNode" + std::to_string(index)},
        {"enabled", false},
        {"x", 24.0f}, {"y", 36.0f}, {"width", 320.0f}, {"height", 48.0f},
        {"anchorX", 0.0f}, {"anchorY", 0.0f}, {"pivotX", 0.0f}, {"pivotY", 0.0f},
        {"text", "Runtime contract"}, {"fontSize", 22.0f},
        {"color", {0.25f, 0.5f, 0.75f, 1.0f}},
        {"interactable", false}, {"isOn", true},
        {"normalColor", {0.1f, 0.2f, 0.3f, 1.0f}},
        {"hoverColor", {0.2f, 0.3f, 0.4f, 1.0f}},
        {"pressedColor", {0.3f, 0.4f, 0.5f, 1.0f}},
        {"onColor", {0.2f, 0.8f, 0.2f, 1.0f}},
        {"offColor", {0.8f, 0.2f, 0.2f, 1.0f}},
        {"fovDegrees", 67.0f}, {"nearZ", 0.2f}, {"farZ", 750.0f},
        {"priority", 3}, {"active", false},
        {"friction", 0.7f}, {"restitution", 0.2f}, {"moving", true},
        {"shapeType", 3}, {"halfExtents", {1.0f, 2.0f, 3.0f}},
        {"radius", 0.75f}, {"height", 3.5f}, {"axis", 2},
        {"offset", {0.1f, 0.2f, 0.3f}},
        {"kinematic", true}, {"mass", 12.0f}, {"gravityFactor", 0.8f},
        {"linearDamping", 0.15f}, {"angularDamping", 0.25f},
        {"maxSlopeAngle", 47.0f},
    };
    node.deserialize(values, resources);
    node.transform().position = {1.0f + static_cast<float>(index), 2.0f, -3.0f};
    node.transform().scale = {1.25f, 0.75f, 1.5f};
    node.addToGroup("roundtrip");
}

// Resource-free seeding for the snapshot codec: common fields only, with each
// type's durable surface driven by its reflected properties. No ResourceManager
// is touched, so headless tools can run the full contract.
void seedNodeCommon(Node& node, std::size_t index) {
    node.setName("ContractNode" + std::to_string(index));
    node.setEnabled(false);
    node.transform().position = {1.0f + static_cast<float>(index), 2.0f, -3.0f};
    node.transform().scale = {1.25f, 0.75f, 1.5f};
    node.addToGroup("roundtrip");
}

void seedBehaviour(Behaviour& behaviour) {
    const std::string type = behaviour.typeName() ? behaviour.typeName() : "";
    if (type == "Blackboard") {
        behaviour.load({{"values", {{"score", {{"n", 42.5}}},
                                      {"doorOpen", {{"b", true}}},
                                      {"chapter", {{"s", "contract"}}}}}});
    } else if (type == "StateMachine") {
        behaviour.load({
            {"initialState", "Idle"},
            {"states", {"Idle", "Active"}},
            {"transitions", {{{"from", "Idle"}, {"to", "Active"},
                               {"trigger", "go"}, {"after", 1.5f}}}},
        });
    } else if (type == "ScriptBehaviour") {
        behaviour.load({
            {"script", "scripts/contract.js"},
            {"hotReload", false},
            {"properties", {{"speed", 3.5}, {"enabled", true},
                              {"label", "contract"}}},
        });
    }
}

} // namespace

#ifndef SAIDA_AUTHORING_WEB
bool verifyRuntimeRoundTripContract(RuntimeTypeTarget target,
                                    ResourceManager& resources,
                                    RoundTripContractReport& report,
                                    std::string& error) {
    try {
        report = {};
        Scene source;
        source.setName("RuntimeContract");
        std::size_t nodeIndex = 0;
        for (const RuntimeTypeRow& row : kRuntimeTypeMatrix) {
            if (row.category != RuntimeTypeCategory::Node || !promisedHere(row, target))
                continue;
            ++report.nodes;
            if (std::string(row.name) == "Scene") continue; // represented by source
            std::unique_ptr<Node> node = NodeRegistry::instance().create(row.name);
            if (!node || std::string(node->typeName()) != row.name)
                throw std::runtime_error("factory failed for node '" +
                                         std::string(row.name) + "'");
            seedNode(*node, resources, nodeIndex++);
            report.reflectedProperties += seedReflectedProperties(row.name, node.get());
            source.addChild(std::move(node));
        }

        auto behaviourHost = std::make_unique<Node>("BehaviourContract");
        for (const RuntimeTypeRow& row : kRuntimeTypeMatrix) {
            if (row.category != RuntimeTypeCategory::Behaviour ||
                !promisedHere(row, target))
                continue;
            std::unique_ptr<Behaviour> behaviour =
                BehaviourRegistry::instance().create(row.name);
            if (!behaviour || std::string(behaviour->typeName()) != row.name)
                throw std::runtime_error("factory failed for behaviour '" +
                                         std::string(row.name) + "'");
            behaviour->setEnabled(false);
            report.reflectedProperties +=
                seedReflectedProperties(row.name, behaviour.get());
            seedBehaviour(*behaviour);
            behaviourHost->addBehaviour(std::move(behaviour));
            ++report.behaviours;
        }
        source.addChild(std::move(behaviourHost));

        const json before = json::parse(SceneSerializer::nodeToJson(source, resources));
        std::unique_ptr<Node> reloaded = SceneSerializer::nodeFromJson(
            before.dump(), resources, NodeIdPolicy::Preserve);
        if (!reloaded || std::string(reloaded->typeName()) != "Scene")
            throw std::runtime_error("full serializer did not reconstruct Scene root");
        const json after = json::parse(SceneSerializer::nodeToJson(*reloaded, resources));
        if (after != before)
            throw std::runtime_error("semantic JSON changed after full serializer round-trip");
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}
#endif

bool verifySnapshotRoundTripContract(RuntimeTypeTarget target,
                                     RoundTripContractReport& report,
                                     std::string& error) {
    try {
        report = {};
        Scene source;
        source.setName("SnapshotContract");
        // Initializes and verifies the target-specific headless registries.
        (void)authoring::serializeSceneSnapshot(source, nullptr);

        std::size_t nodeIndex = 0;
        for (const RuntimeTypeRow& row : kRuntimeTypeMatrix) {
            if (row.category != RuntimeTypeCategory::Node || !promisedHere(row, target))
                continue;
            ++report.nodes;
            if (std::string(row.name) == "Scene") continue;
            std::unique_ptr<Node> node = NodeRegistry::instance().create(row.name);
            if (!node)
                throw std::runtime_error("factory returned null for snapshot node '" +
                                         std::string(row.name) + "'");
            if (std::string(node->typeName()) != row.name)
                throw std::runtime_error("factory for snapshot node '" +
                                         std::string(row.name) + "' created type '" +
                                         (node->typeName() ? node->typeName() : "<null>") + "'");
            seedNodeCommon(*node, nodeIndex++);
            report.reflectedProperties += seedReflectedProperties(row.name, node.get());
            source.addChild(std::move(node));
        }

        auto behaviourHost = std::make_unique<Node>("SnapshotBehaviourContract");
        for (const RuntimeTypeRow& row : kRuntimeTypeMatrix) {
            if (row.category != RuntimeTypeCategory::Behaviour ||
                !promisedHere(row, target))
                continue;
            std::unique_ptr<Behaviour> behaviour =
                BehaviourRegistry::instance().create(row.name);
            if (!behaviour || std::string(behaviour->typeName()) != row.name)
                throw std::runtime_error("factory failed for snapshot behaviour '" +
                                         std::string(row.name) + "'");
            behaviour->setEnabled(false);
            report.reflectedProperties +=
                seedReflectedProperties(row.name, behaviour.get());
            seedBehaviour(*behaviour);
            behaviourHost->addBehaviour(std::move(behaviour));
            ++report.behaviours;
        }
        source.addChild(std::move(behaviourHost));

        const json before = json::parse(authoring::serializeSceneSnapshot(source, nullptr));
        Scene reloaded;
        std::string loadError;
        if (!authoring::deserializeSceneSnapshot(before.dump(), reloaded, &loadError))
            throw std::runtime_error("snapshot reconstruction failed: " + loadError);
        const json after =
            json::parse(authoring::serializeSceneSnapshot(reloaded, nullptr));
        if (after != before)
            throw std::runtime_error("semantic JSON changed after snapshot round-trip");
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

} // namespace saida::runtime
