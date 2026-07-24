#include "behaviours/SpawnerBehaviour.hpp"

#include "scene/Node.hpp"
#include "scene/SceneTree.hpp"

#include <nlohmann/json.hpp>
#include <glm/glm.hpp>

namespace saida {

void SpawnerBehaviour::onReady() {
    if (interval > 0.0f)
        every(interval, [this] { spawn(); });
}

void SpawnerBehaviour::spawn() {
    if (scenePath.empty()) return;
    SceneTree* t = tree();
    if (!t) return;

    Node* n = t->instantiate(scenePath);
    if (!n) return;
    n->transform().position = glm::vec3(node()->worldTransform()[3]);  // at this node

    if (lifetime > 0.0f)
        t->after(n, lifetime, [n] { n->queueFree(); });  // owned by n → cancels if it dies first
}

void SpawnerBehaviour::describe(reflect::TypeBuilder<SpawnerBehaviour>& t) {
    t.doc("Instantiates a scene at this node on a timer.");
    t.property("scenePath", &SpawnerBehaviour::scenePath)
        .asset().tooltip("project-relative .scene to instantiate");
    t.property("interval", &SpawnerBehaviour::interval)
        .range(0.0, 60.0).tooltip("seconds between spawns; 0 disables");
    t.property("lifetime", &SpawnerBehaviour::lifetime)
        .range(0.0, 60.0).tooltip("seconds before spawned nodes are freed; 0 keeps them");
}

} // namespace saida
