#pragma once

// Applies edit operations without depending on the UI or the network.

#include <string>

namespace saida {

class Scene;
class ResourceManager;

namespace authoring {

// Returns:
//   {"ok":true,"applied":"set_transform","diff":{...}}
//   {"ok":false,"error":"unknown node 'Foo'"}
// An invalid operation does not modify the scene.
std::string applyOpJson(Scene& scene, ResourceManager& resources,
                        const std::string& opJson);

// Without a GPU resource manager, operations that depend on it fail explicitly.
std::string applyOpJson(Scene& scene, ResourceManager* resources,
                        const std::string& opJson);

} // namespace authoring
} // namespace saida
