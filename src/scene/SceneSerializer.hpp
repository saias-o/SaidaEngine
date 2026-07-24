#pragma once

#include <memory>
#include <string>

namespace saida {

class Node;
class Scene;
class ResourceManager;

enum class NodeIdPolicy {
    Preserve,
    Regenerate,
};

// Reads/writes a scene (node hierarchy, transforms, lights, mesh/material
// references by key, and registered behaviours) as JSON. Also converts a single
// node subtree to/from a JSON string, used for the editor's copy/paste/duplicate.
class SceneSerializer {
public:
    static bool saveToFile(Node& sceneRoot, ResourceManager& resources, const std::string& path);
    static bool loadIntoScene(Scene& scene, ResourceManager& resources, const std::string& path);

    static std::string nodeToJson(Node& node, ResourceManager& resources);
    static std::unique_ptr<Node> nodeFromJson(
        const std::string& json, ResourceManager& resources,
        NodeIdPolicy idPolicy = NodeIdPolicy::Regenerate);

    // Load a .scene file as a single node subtree (for instancing a scene as a
    // child of another). Returns nullptr on failure.
    static std::unique_ptr<Node> loadNodeFromSceneFile(const std::string& path,
                                                       ResourceManager& resources,
                                                       NodeIdPolicy idPolicy = NodeIdPolicy::Regenerate);

    // Headless check that a .scene document is parseable and uses the current
    // schema. No resources are touched.
    static bool validateSceneDocumentFile(const std::string& path);

    // Validates that every serialized node and behaviour can be constructed by
    // the registries currently installed in this runtime. Player builds use
    // this as a fail-closed contract boundary: unsupported content is
    // rejected instead of being silently replaced by a generic Node.
    static bool validateTypeContractJson(const std::string& json,
                                         std::string* error = nullptr);
};

} // namespace saida
