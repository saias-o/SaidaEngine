#include "scene/NodeRegistry.hpp"
#include "scene/Node.hpp"
#include "core/Log.hpp"

namespace saida {

NodeRegistry& NodeRegistry::instance() {
    static NodeRegistry registry;
    return registry;
}

void NodeRegistry::registerType(const std::string& typeName, Factory factory) {
    if (factories_.find(typeName) != factories_.end()) {
        Log::warn("NodeRegistry: type '", typeName, "' is already registered, overwriting.");
    }
    factories_[typeName] = std::move(factory);
}

std::unique_ptr<Node> NodeRegistry::create(const std::string& typeName) const {
    auto it = factories_.find(typeName);
    if (it != factories_.end()) {
        return it->second();
    }
    return nullptr;
}

} // namespace saida
