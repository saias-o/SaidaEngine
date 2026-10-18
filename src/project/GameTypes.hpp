#pragma once

#include "core/Reflection.hpp"
#include "scene/BehaviourRegistry.hpp"
#include "scene/ReflectedTypes.hpp"
#include "scene/RuntimeTypeMatrix.hpp"

#include <stdexcept>

namespace saida {

// Called explicitly before loading scenes/autoloads. Project code never patches
// the built-in registry, and cannot silently replace another project's type.
template<class T>
void registerGameBehaviour() {
    registerReflectedTypes();
    const auto* name = T::reflectName();
    auto& registry = BehaviourRegistry::instance();
    for (const auto& builtin : kRuntimeTypeMatrix)
        if (std::string(builtin.name) == name)
            throw std::runtime_error(std::string("Engine type name is reserved: ") + name);
    if (registry.factories().count(name) || reflect::TypeRegistry::instance().find(name))
        throw std::runtime_error(std::string("Behaviour type already registered: ") + name);
    auto& desc = reflect::TypeRegistry::instance().add(name);
    desc = reflect::localDesc<T>();
    desc.name = name;
    desc.category = "behaviour";
    registry.registerType<T>(name);
}

} // namespace saida
