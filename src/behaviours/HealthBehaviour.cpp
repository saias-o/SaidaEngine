#include "behaviours/HealthBehaviour.hpp"

#include "scene/Node.hpp"
#include "physics/CharacterBodyNode.hpp"
#include "core/Log.hpp"

#include <glm/gtc/quaternion.hpp>

namespace saida {

void HealthBehaviour::onReady() {
    health_ = maxHealth;
    dead_ = false;
}

void HealthBehaviour::damage(float amount) {
    if (dead_ || amount <= 0.0f) return;
    health_ -= amount;
    if (health_ <= 0.0f) die();
}

void HealthBehaviour::kill() {
    if (!dead_) die();
}

void HealthBehaviour::die() {
    dead_ = true;
    health_ = 0.0f;

    // Stop sliding and stop every other behaviour on this node.
    if (CharacterBodyNode* body = node()->asCharacterBody()) body->velocity = glm::vec3(0.0f);
    for (const auto& b : node()->behaviours())
        if (b.get() != this) b->setEnabled(false);

    if (tipOverOnDeath) {
        node()->transform().rotation =
            glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f)) *
            node()->transform().rotation;
    }

    died.emit();
    Log::info("'", node()->name(), "' died");

    if (deathDelay > 0.0f)
        wait(deathDelay, [n = node()] { n->queueFree(); });
    else
        node()->queueFree();
}

bool applyDamage(Node* target, float amount) {
    if (!target) return false;
    auto* h = target->getBehaviour<HealthBehaviour>();
    if (!h || h->dead()) return false;
    h->damage(amount);
    return true;
}

void HealthBehaviour::describe(reflect::TypeBuilder<HealthBehaviour>& t) {
    t.doc("Hit points + death. Use the 'damage'/'kill' slots; emits 'died' on death, "
          "then frees the node after deathDelay.");
    t.property("maxHealth", &HealthBehaviour::maxHealth).range(1.0, 1000.0);
    t.property("deathDelay", &HealthBehaviour::deathDelay).range(0.0, 30.0)
        .tooltip("seconds the corpse lingers before despawning");
    t.property("tipOverOnDeath", &HealthBehaviour::tipOverOnDeath);
    t.signal("died", &HealthBehaviour::died);
    t.slot("damage", &HealthBehaviour::damage);
    t.slot("kill", &HealthBehaviour::kill);
}

} // namespace saida
