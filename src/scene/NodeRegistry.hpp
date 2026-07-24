#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace saida {

class Node;

// Maps a node type name to a factory, allowing the SceneSerializer to rebuild
// nodes by name when loading a scene. Both engine built-ins and custom nodes
// are registered here. Single global instance.
class NodeRegistry {
public:
    using Factory = std::function<std::unique_ptr<Node>()>;

    static NodeRegistry& instance();

    void registerType(const std::string& typeName, Factory factory);

    // Creates a default-constructed node of the given type, or nullptr if
    // the type was never registered. Caller then calls deserialize() to fill it.
    std::unique_ptr<Node> create(const std::string& typeName) const;

    const std::unordered_map<std::string, Factory>& factories() const { return factories_; }

    // Convenience for the common case where T is default-constructible.
    template <typename T>
    void registerType(const std::string& typeName) {
        registerType(typeName, [] { return std::make_unique<T>(); });
    }

private:
    std::unordered_map<std::string, Factory> factories_;
};

} // namespace saida
