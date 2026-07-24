#pragma once

#include "scene/Behaviour.hpp"

namespace saida {

class XRController;

// Near-field interactor for grabbing and touching. It rescans groups every frame,
// so interactables can be freed safely.
class XRDirectInteractor : public Behaviour {
public:
    void onReady() override;
    void onUpdate(float dt) override;
    void onDestroy() override;

    float reach = 0.0f;  // extra range added to each interactable's radius (m)

    const char* typeName() const override { return "XRDirectInteractor"; }
    void save(nlohmann::json& j) const override;
    void load(const nlohmann::json& j) override;

private:
    XRController* controller_ = nullptr;  // cached: this interactor's hand
};

} // namespace saida
