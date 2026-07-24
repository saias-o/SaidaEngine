#pragma once

// The resource-free mode stays limited to headless to avoid a second format.

#include <string>

#include <nlohmann/json_fwd.hpp>

namespace saida {

class ResourceManager;
class Scene;

namespace authoring {

std::string serializeSceneSnapshot(Scene& scene, ResourceManager& resources);
// Throws std::runtime_error rather than emitting a lossy document when the
// scene contains a type outside the resource-free authoring contract.
std::string serializeSceneSnapshot(Scene& scene, ResourceManager* resources);

// Rebuilds the resource-free authoring contract. Durable mesh/material refs are
// preserved as data; unsupported node or behaviour types fail explicitly so a
// persistent fold can never downgrade a scene silently.
bool deserializeSceneSnapshot(const nlohmann::json& doc, Scene& out,
                              std::string* error = nullptr);
bool deserializeSceneSnapshot(const std::string& text, Scene& out,
                              std::string* error = nullptr);

} // namespace authoring
} // namespace saida
