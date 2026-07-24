#pragma once

#include "scene/Behaviour.hpp"
#include "core/Signal.hpp"

#include <vector>

namespace saida {

class XRController;

// Makes its node a touch/poke target (a 3D button, a switch, a proximity zone).
// Attach to a node with a collider; an XRDirectInteractor reports which hands are
// within range. Reacts via the touchEntered/touchExited signals. Supports both
// hands touching at once.
class XRTouchable : public Behaviour {
public:
    void onReady() override;
    void onDestroy() override;

    bool isTouched() const { return !touchers_.empty(); }
    int touchCount() const { return static_cast<int>(touchers_.size()); }

    float touchRadius = 0.05f;  // poke range (m); the interactor may widen it

    Signal<XRController*> touchEntered;
    Signal<XRController*> touchExited;

    // Called by the interactor every frame for each controller it owns: emits the
    // edge signals by diffing against the current toucher set.
    void updateTouch(XRController* by, bool inRange);

    const char* typeName() const override { return "XRTouchable"; }
    void save(nlohmann::json& j) const override;
    void load(const nlohmann::json& j) override;

private:
    std::vector<XRController*> touchers_;  // controllers currently inside
};

} // namespace saida
