#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace saida {

class Behaviour;

// Maps a behaviour type name to a factory, so the serializer can rebuild
// behaviours by name when loading a scene. Engine built-ins and the game both
// register their behaviours here at startup. Single global instance.
class BehaviourRegistry {
public:
    using Factory = std::function<std::unique_ptr<Behaviour>()>;

    static BehaviourRegistry& instance();

    void registerType(const std::string& typeName, Factory factory);

    // Creates a default-constructed behaviour of the given type, or nullptr if
    // the type was never registered. Caller then calls load() to fill it.
    std::unique_ptr<Behaviour> create(const std::string& typeName) const;

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
