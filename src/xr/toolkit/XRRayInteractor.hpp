#pragma once

#include "scene/Behaviour.hpp"
#include "core/Signal.hpp"

#include <glm/glm.hpp>

namespace saida {

class XRController;
class TeleportArea;
class WebCanvasNode;

// Far-field interactor for teleport locomotion and UI ray presses.
class XRRayInteractor : public Behaviour {
public:
    void onReady() override;
    void onUpdate(float dt) override;

    float maxDistance = 10.0f;   // ray length (m)
    float aimThreshold = 0.5f;   // thumbstick.y past this starts aiming

    bool isAiming() const { return aiming_; }
    bool hasTarget() const { return hasTarget_; }
    glm::vec3 target() const { return target_; }      // valid when hasTarget()
    glm::vec3 rayOrigin() const { return rayOrigin_; }
    glm::vec3 rayDirection() const { return rayDir_; }

    Signal<glm::vec3> teleported;  // emitted on a committed teleport (world point)

    const char* typeName() const override { return "XRRayInteractor"; }
    void save(nlohmann::json& j) const override;
    void load(const nlohmann::json& j) override;

private:
    void commitTeleport();

    XRController* controller_ = nullptr;
    bool aiming_ = false;
    bool hasTarget_ = false;
    glm::vec3 target_{0.0f};
    glm::vec3 rayOrigin_{0.0f};
    glm::vec3 rayDir_{0.0f, 0.0f, -1.0f};
    TeleportArea* targetArea_ = nullptr;  // re-validated against the live group on commit
    WebCanvasNode* uiPressedTarget_ = nullptr;
    glm::vec2 uiPressedLocal_{0.0f};
};

} // namespace saida
