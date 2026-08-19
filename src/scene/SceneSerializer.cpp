#include "scene/SceneSerializer.hpp"

#include "core/FormatVersions.hpp"
#include "core/Log.hpp"
#include "core/Paths.hpp"
#include "graphics/Material.hpp"
#include "graphics/ResourceManager.hpp"
#include "scene/Behaviour.hpp"
#include "scene/BehaviourRegistry.hpp"
#include "scene/GLTFLoader.hpp"
#include "nodes/LightNode.hpp"
#include "nodes/MeshNode.hpp"
#include "scene/Node.hpp"
#include "scene/NodeRegistry.hpp"
#include "scene/Scene.hpp"
#include "scene/SceneSettingsSerialization.hpp"
#include "scene/SerializationHelpers.hpp"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <unordered_set>

namespace saida {

namespace {
using json = nlohmann::json;

bool isModelPath(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext == ".gltf" || ext == ".glb";
}

bool rejectTypeContract(const std::string& message, std::string* error) {
    if (error) *error = message;
    return false;
}

bool validateTypeContract(const json& node, const std::string& path,
                          std::string* error) {
    if (!node.is_object())
        return rejectTypeContract(path + " must be an object", error);

    std::string type = "Node";
    if (auto it = node.find("type"); it != node.end()) {
        if (!it->is_string())
            return rejectTypeContract(path + ".type must be a string", error);
        type = it->get<std::string>();
    }
    if (NodeRegistry::instance().factories().find(type) ==
        NodeRegistry::instance().factories().end()) {
        return rejectTypeContract(path + ": unsupported node type '" + type + "'", error);
    }

    if (auto it = node.find("behaviours"); it != node.end()) {
        if (!it->is_array())
            return rejectTypeContract(path + ".behaviours must be an array", error);
        for (std::size_t i = 0; i < it->size(); ++i) {
            const json& behaviour = (*it)[i];
            const std::string behaviourPath =
                path + ".behaviours[" + std::to_string(i) + "]";
            if (!behaviour.is_object() || !behaviour.contains("type") ||
                !behaviour["type"].is_string()) {
                return rejectTypeContract(behaviourPath + ".type must be a string", error);
            }
            const std::string behaviourType = behaviour["type"].get<std::string>();
            if (BehaviourRegistry::instance().factories().find(behaviourType) ==
                BehaviourRegistry::instance().factories().end()) {
                return rejectTypeContract(behaviourPath + ": unsupported behaviour type '" +
                                              behaviourType + "'",
                                          error);
            }
        }
    }

    if (auto it = node.find("children"); it != node.end()) {
        if (!it->is_array())
            return rejectTypeContract(path + ".children must be an array", error);
        for (std::size_t i = 0; i < it->size(); ++i) {
            if (!validateTypeContract((*it)[i],
                                      path + ".children[" + std::to_string(i) + "]",
                                      error))
                return false;
        }
    }
    return true;
}

bool reloadImportedModel(Node& target, const std::string& path, ResourceManager& resources) {
    if (!isModelPath(path)) return false;

    // Relative path = relative to the loaded project's root (like scripts).
    std::string resolved = path;
    if (std::filesystem::path p(path); p.is_relative() && !activeProjectRoot().empty()) {
        std::filesystem::path abs = std::filesystem::path(activeProjectRoot()) / p;
        if (std::filesystem::exists(abs)) resolved = abs.string();
    }

    Node temp("ImportRoot");
    GLTFLoadOptions opts;
    if (!GLTFLoader::load(resolved, temp, resources, opts))
        return false;

    if (temp.children().empty())
        return false;

    Node* loadedContainer = temp.children().front().get();
    while (!loadedContainer->children().empty()) {
        target.addChild(loadedContainer->detachChild(loadedContainer->children().front().get()));
    }
    return true;
}

json serializeNode(Node& node, ResourceManager& resources) {
    json j;
    node.serialize(j, resources);
    return j;
}

// `ancestry` is the chain of node names from the document root down to this
// node's parent. It exists only so a refusal is actionable: "unsupported node
// type" carries nowhere to look in a scene of several hundred nodes.
std::unique_ptr<Node> deserializeNode(const json& j, ResourceManager& resources,
                                      NodeIdPolicy idPolicy,
                                      const std::string& ancestry = {}) {
    const std::string type = j.value("type", "Node");
    const std::string here =
        ancestry.empty() ? j.value("name", std::string("<root>"))
                         : ancestry + "/" + j.value("name", std::string("<unnamed>"));

    // Special case for scene prefabs
    if (type == "Scene" && j.contains("prefabAssetId")) {
        AssetID prefabId = j["prefabAssetId"].get<AssetID>();
        if (prefabId != kAssetInvalid && resources.getRegistry()) {
            std::string absPath = resources.getRegistry()->getAbsolutePath(prefabId);
            if (!absPath.empty()) {
                // Load the nested scene from file instead of reading children
                std::unique_ptr<Node> loaded = SceneSerializer::loadNodeFromSceneFile(
                    absPath, resources, NodeIdPolicy::Regenerate);
                if (loaded) {
                    loaded->deserialize(j, resources); // apply settings overrides on top of the prefab
                    if (idPolicy == NodeIdPolicy::Preserve && j.contains("id"))
                        loaded->assignSerializedId(j["id"].get<NodeId>());
                    return loaded;
                }
            }
        }
    }

    std::unique_ptr<Node> node = NodeRegistry::instance().create(type);
    if (!node) {
        Log::error("SceneSerializer: unsupported node type '", type, "' at ", here, ".");
        return nullptr;
    }

    node->deserialize(j, resources);
    if (idPolicy == NodeIdPolicy::Preserve && j.contains("id"))
        node->assignSerializedId(j["id"].get<NodeId>());
    else if (idPolicy == NodeIdPolicy::Regenerate)
        node->regenerateId();
    bool childrenLoadedFromImport = false;
    if (!node->importedFromPath().empty()) {
        childrenLoadedFromImport = reloadImportedModel(*node, node->importedFromPath(), resources);
        if (!childrenLoadedFromImport) {
            Log::warn("SceneSerializer: failed to reload imported model ",
                      node->importedFromPath(), ", falling back to serialized children.");
        }
    }
    
    // We handle children deserialization here because the parent node
    // shouldn't manually instantiate children, the SceneSerializer does that using NodeRegistry.
    if (!childrenLoadedFromImport) {
        if (auto it = j.find("children"); it != j.end() && it->is_array()) {
            for (const json& cj : *it) {
                auto child = deserializeNode(cj, resources, idPolicy, here);
                if (!child) {
                    // Skipping the child would take its entire subtree with it
                    // and still report success -- a typo in a "type" deleting
                    // content while the game boots normally.
                    //
                    // Every public entry point validates the type contract over
                    // the whole tree first, so this is not the load's only
                    // defence and should be unreachable through them. It is
                    // here so that the guarantee belongs to this function rather
                    // than to every caller remembering to validate: the next
                    // caller is the one that forgets.
                    Log::error("SceneSerializer: refusing the load rather than "
                               "dropping the subtree under ", here, ".");
                    return nullptr;
                }
                node->addChild(std::move(child));
            }
        }
    }

    // If it's a prefab instance, load its children from the original file
    if (Scene* s = dynamic_cast<Scene*>(node.get())) {
        if (s->prefabAssetId() != kAssetInvalid && resources.getRegistry()) {
            std::string path = resources.getRegistry()->getAbsolutePath(s->prefabAssetId());
            if (!path.empty()) {
                if (auto prefabNode = SceneSerializer::loadNodeFromSceneFile(
                        path, resources, NodeIdPolicy::Regenerate)) {
                    // Move all children from the prefab file into this instance
                    // We only want the children, not the root settings of the prefab
                    while (!prefabNode->children().empty()) {
                        node->addChild(prefabNode->detachChild(prefabNode->children().front().get()));
                    }
                }
            }
        }
    }

    return node;
}

void ensureUniqueIds(Node& root) {
    std::unordered_set<NodeId> seen;
    root.traverse([&](Node& node, const glm::mat4&) {
        while (node.id() == kNodeInvalid || !seen.insert(node.id()).second)
            node.regenerateId();
        seen.insert(node.id());
    });
}

bool acceptSceneDocumentVersion(const json& doc, const std::string& context,
                                const std::string& path) {
    if (!doc.is_object()) {
        Log::error(context, ": scene document root must be an object: ", path);
        return false;
    }
    if (const std::string envelope =
            format::schemaEnvelopeError(doc, format::kSceneVersion, "scene");
        !envelope.empty()) {
        Log::error(context, ": ", envelope, ": ", path);
        return false;
    }

    return true;
}

} // namespace

std::string SceneSerializer::nodeToJson(Node& node, ResourceManager& resources) {
    return serializeNode(node, resources).dump(2);
}

std::unique_ptr<Node> SceneSerializer::nodeFromJson(const std::string& text,
                                                    ResourceManager& resources,
                                                    NodeIdPolicy idPolicy) {
    try {
        const json document = json::parse(text);
        std::string contractError;
        if (!validateTypeContract(document, "node", &contractError)) {
            Log::error("nodeFromJson: ", contractError);
            return nullptr;
        }
        auto node = deserializeNode(document, resources, idPolicy);
        if (node) ensureUniqueIds(*node);
        return node;
    } catch (const std::exception& e) {
        Log::error("nodeFromJson: ", e.what());
        return nullptr;
    }
}

std::unique_ptr<Node> SceneSerializer::loadNodeFromSceneFile(const std::string& path,
                                                             ResourceManager& resources,
                                                             NodeIdPolicy idPolicy) {
    std::ifstream file(path);
    if (!file.is_open()) {
        Log::error("loadNodeFromSceneFile: cannot open ", path);
        return nullptr;
    }
    try {
        json doc = json::parse(file);
        if (!acceptSceneDocumentVersion(doc, "loadNodeFromSceneFile", path))
            return nullptr;
        json root = doc.at("scene");
        root["type"] = "Scene"; // Force the root node to be instantiated as a Scene
        std::string contractError;
        if (!validateTypeContract(root, "scene", &contractError)) {
            Log::error("loadNodeFromSceneFile: ", contractError, ": ", path);
            return nullptr;
        }
        auto node = deserializeNode(root, resources, idPolicy);
        if (node) ensureUniqueIds(*node);
        return node;
    } catch (const std::exception& e) {
        Log::error("loadNodeFromSceneFile: ", e.what());
        return nullptr;
    }
}

bool SceneSerializer::validateSceneDocumentFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        Log::error("validateSceneDocumentFile: cannot open ", path);
        return false;
    }
    try {
        json doc = json::parse(file);
        if (!acceptSceneDocumentVersion(doc, "validateSceneDocumentFile", path))
            return false;
        if (!doc.contains("scene") || !doc["scene"].is_object()) {
            Log::error("validateSceneDocumentFile: missing 'scene' object: ", path);
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        Log::error("validateSceneDocumentFile: ", e.what());
        return false;
    }
}

bool SceneSerializer::validateTypeContractJson(const std::string& text,
                                               std::string* error) {
    try {
        return validateTypeContract(json::parse(text), "node", error);
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

bool SceneSerializer::saveToFile(Node& sceneRoot, ResourceManager& resources,
                                 const std::string& path) {
    json doc;
    format::writeSchema(doc, format::kSceneVersion);
    doc["scene"] = serializeNode(sceneRoot, resources);

    std::ofstream file(path);
    if (!file.is_open()) {
        Log::error("saveToFile: cannot write ", path);
        return false;
    }
    file << doc.dump(2) << "\n";
    Log::info("saved scene to ", path);
    return true;
}

bool SceneSerializer::loadIntoScene(Scene& scene, ResourceManager& resources,
                                    const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        Log::error("loadIntoScene: cannot open ", path);
        return false;
    }

    try {
        json doc = json::parse(file);
        if (!acceptSceneDocumentVersion(doc, "loadIntoScene", path))
            return false;
        const json& root = doc.at("scene");

        std::string contractError;
        if (!validateTypeContract(root, "scene", &contractError)) {
            Log::error("loadIntoScene: ", contractError, ": ", path);
            return false;
        }

        scene.clearChildren();
        if (root.contains("id")) scene.assignSerializedId(root["id"].get<NodeId>());
        else scene.regenerateId();
        
        // A scene file states its rendering settings in full: whatever it omits
        // takes the engine default, never what the previous scene happened to
        // leave behind.
        scene.settings() = SceneSettings{};
        if (auto it = root.find("settings"); it != root.end())
            applySceneSettings(*it, scene.settings(), resources);

        if (auto it = root.find("children"); it != root.end() && it->is_array()) {
            for (const json& cj : *it) {
                auto child = deserializeNode(cj, resources, NodeIdPolicy::Preserve);
                if (!child) {
                    Log::error("loadIntoScene: failed to deserialize a validated child: ", path);
                    return false;
                }
                scene.addChild(std::move(child));
            }
        }

        // Scene-level data-driven signal connections (loadIntoScene parses the
        // root manually, so read them here in addition to Scene::deserialize).
        scene.readConnections(root);

        ensureUniqueIds(scene);

        Log::info("loaded scene from ", path);
        return true;
    } catch (const std::exception& e) {
        Log::error("loadIntoScene: ", e.what());
        return false;
    }
}

} // namespace saida
