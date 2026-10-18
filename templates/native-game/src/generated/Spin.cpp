#include "Spin.hpp"
#include "core/Log.hpp"
#include "scene/Node.hpp"

namespace game {
void Spin::describe(saida::reflect::TypeBuilder<Spin>& t) {
    t.property("speed", &Spin::speed).tooltip("Rotation speed in radians per second");
}
void Spin::onReady() {
    saida::Log::info("[MyGame] project-native Spin ready");
}
void Spin::onUpdate(float dt) {
    auto& pose = node()->transform();
    pose.rotation = glm::normalize(glm::angleAxis(speed * dt, glm::vec3(0, 1, 0)) * pose.rotation);
}
} // namespace game
