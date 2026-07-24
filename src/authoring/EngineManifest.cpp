#include "authoring/EngineManifest.hpp"

#include "authoring/SaidaOp.hpp"
#include "core/FormatVersions.hpp"
#include "core/Reflection.hpp"
#include "scenario/ScenarioRegistry.hpp"
#include "nodes/LightNode.hpp"
#include "nodes/ParticleSystemNode.hpp"
#include "scene/RuntimeTypeMatrix.hpp"
#include "nodes/WaterNode.hpp"

#ifndef __EMSCRIPTEN__
#include "scene/ReflectedTypes.hpp"
#include "scene/animation/AnimGraphAsset.hpp"
#include "scene/animation/AnimationSequence.hpp"
#include "scene/animation/ClipView.hpp"
#include "scene/animation/RetargetProfile.hpp"
#include "scene/animation/RigAsset.hpp"
#endif

#include <utility>

namespace saida::authoring {
namespace {

template <typename T>
nlohmann::json reflectedNodeManifest() {
    nlohmann::json out = reflect::localDesc<T>().manifest();
    out["name"] = T::reflectName();
    out["category"] = "node";
    return out;
}

nlohmann::json scenarioManifest() {
    nlohmann::json actions = nlohmann::json::array();
    for (const auto& name : ScenarioActionRegistry::names())
        actions.push_back(nlohmann::json{{"name", name}});

    nlohmann::json conditions = nlohmann::json::array();
    for (const auto& name : ScenarioConditionRegistry::names()) {
        nlohmann::json condition{{"name", name}};
        if (ScenarioConditionRegistry::isComposite(name))
            condition["composite"] = true;
        conditions.push_back(std::move(condition));
    }

    return nlohmann::json{{"actions", std::move(actions)},
                          {"conditions", std::move(conditions)}};
}

nlohmann::json nodePropertiesByType(const nlohmann::json& nodes) {
    nlohmann::json out;
    out["*"] = nlohmann::json::array({"name", "enabled"});
    for (const auto& node : nodes) {
        if (!node.contains("name") || !node["name"].is_string()) continue;
        out[node["name"].get<std::string>()] =
            node.value("properties", nlohmann::json::array());
    }
    return out;
}

} // namespace

nlohmann::json buildEngineManifest() {
    nlohmann::json m;
    m["engineVersion"] = kEngineVersion;
    m["opVersion"] = kOpVersion;

    // Single source of truth for every durable format's schema version. A
    // release manifest pins these so the platform refuses a bundle whose tool,
    // players and fixtures do not share one contract. Animation asset formats
    // are native-only; the web authoring surface does not link them.
    m["formats"] = {
        {"opVersion", kOpVersion},
        {"scene", format::kSceneVersion},
        {"project", format::kProjectVersion},
        {"assetRegistry", format::kAssetRegistryVersion},
        {"scenario", format::kScenarioVersion},
        {"bootManifest", format::kBootManifestVersion},
    };
#ifndef __EMSCRIPTEN__
    m["formats"]["rig"] = kRigAssetSchema;
    m["formats"]["clipView"] = kClipViewSchema;
    m["formats"]["animGraph"] = kAnimGraphSchema;
    m["formats"]["sequence"] = kAnimationSequenceSchema;
    m["formats"]["retargetProfile"] = kRetargetProfileSchema;
#endif
    m["sceneSnapshot"] = {
        {"schema", format::kSceneVersion},
        {"version", format::kSceneVersion},
        {"unsupportedTypePolicy", "reject"},
    };
#ifdef __EMSCRIPTEN__
    m["sceneSnapshot"]["atomicLoad"] = true;
    m["sceneSnapshot"]["failureState"] = "previous-scene-preserved";
#else
    m["sceneSnapshot"]["atomicLoad"] = false;
    m["sceneSnapshot"]["failureState"] = "empty-scene";
#endif

#ifndef __EMSCRIPTEN__
    registerReflectedTypes();
    nlohmann::json reflected = reflect::TypeRegistry::instance().manifest();
    nlohmann::json nodes = nlohmann::json::array({
        nlohmann::json{{"name", "Node"}, {"category", "node"}},
        nlohmann::json{{"name", "MeshNode"}, {"category", "node"}},
        nlohmann::json{{"name", "Camera"}, {"category", "node"}},
    });
    for (const auto& node : reflected.value("nodes", nlohmann::json::array()))
        nodes.push_back(node);
    m["nodes"] = std::move(nodes);
    m["behaviours"] = reflected.value("behaviours", nlohmann::json::array());
#else
    m["nodes"] = nlohmann::json::array({
        nlohmann::json{{"name", "Node"}, {"category", "node"}},
        nlohmann::json{{"name", "MeshNode"}, {"category", "node"}},
        nlohmann::json{{"name", "Camera"}, {"category", "node"}},
        reflectedNodeManifest<LightNode>(),
        reflectedNodeManifest<ParticleSystemNode>(),
        reflectedNodeManifest<WaterNode>(),
    });
    m["behaviours"] = nlohmann::json::array();
#endif

    m["ops"] = knownOpTypes();
    m["opAddressing"] = {
        {"kind", "stable-node-id"},
        {"nodeIdJsonType", "decimal-string"},
        {"snapshotNodeIdJsonType", "decimal-string"},
        {"rootParent", "omitted-parentId"},
        {"createNodeId", "optional-client-supplied"},
    };
    m["properties"] = nodePropertiesByType(m["nodes"]);
    m["scenario"] = scenarioManifest();
    m["runtimeTypeMatrix"] = buildRuntimeTypeMatrixManifest();

    return m;
}

} // namespace saida::authoring
