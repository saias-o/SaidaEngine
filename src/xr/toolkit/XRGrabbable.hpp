#pragma once

#include "scene/Behaviour.hpp"
#include "core/Signal.hpp"

#include <glm/glm.hpp>

namespace saida {

class XRController;

// Makes its node grabbable by an XR hand. RigidBodies become kinematic while held.
class XRGrabbable : public Behaviour {
public:
    void onReady() override;
    void onUpdate(float dt) override;
    void onDestroy() override;

    // Driven by the interactor (or by code). grab() snaps/offsets to the hand.
    void grab(XRController* by);
    void release();
    bool isGrabbed() const { return holder_ != nullptr; }
    XRController* holder() const { return holder_; }

    float grabRadius = 0.15f;  // selection range (m); the interactor may widen it
    bool snapToHand = false;   // true: object origin snaps to the grip pose

    Signal<XRController*> grabbed;
    Signal<XRController*> released;

    const char* typeName() const override { return "XRGrabbable"; }
    void save(nlohmann::json& j) const override;
    void load(const nlohmann::json& j) override;

private:
    XRController* holder_ = nullptr;
    glm::mat4 grabOffset_{1.0f};   // object-in-grip frame captured at grab time
    glm::vec3 lastPos_{0.0f};      // world position last frame (for throw velocity)
    glm::vec3 throwVelocity_{0.0f};
    bool restoreDynamic_ = false;  // body was dynamic before grab → restore on release
    bool pendingThrow_ = false;    // apply throwVelocity_ once dynamic again (next frame)
};

} // namespace saida
