#include "behaviours/RotatorBehaviour.hpp"

#include "scene/Node.hpp"

#include <glm/gtc/quaternion.hpp>
#include <cmath>

namespace saida {

void RotatorBehaviour::describe(reflect::TypeBuilder<RotatorBehaviour>& t) {
    t.doc("Spins its node continuously around a local axis.");
    t.property("axis", &RotatorBehaviour::axis).tooltip("local rotation axis");
    t.property("speed", &RotatorBehaviour::speed).range(-720.0, 720.0).tooltip("degrees per second");
    t.signal("fullRotation", &RotatorBehaviour::fullRotation);
    t.slot("reset", &RotatorBehaviour::reset);
}

void RotatorBehaviour::onUpdate(float dt) {
    if (speed == 0.0f) return;
    if (glm::length(axis) < 1e-6f) return;

    const glm::vec3 a = glm::normalize(axis);
    const glm::quat delta = glm::angleAxis(glm::radians(speed) * dt, a);
    Transform& t = node()->transform();
    t.rotation = glm::normalize(delta * t.rotation);

    accumDegrees_ += std::abs(speed) * dt;
    if (accumDegrees_ >= 360.0f) {
        accumDegrees_ -= 360.0f;
        fullRotation.emit();
    }
}

void RotatorBehaviour::reset() {
    if (Node* n = node()) n->transform().rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    accumDegrees_ = 0.0f;
}

} // namespace saida
