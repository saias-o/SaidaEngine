#include "xr/toolkit/XRTouchable.hpp"
#include "xr/toolkit/XRTypes.hpp"

#include "scene/Node.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>

namespace saida {

void XRTouchable::onReady() {
    node()->addToGroup(kXRTouchableGroup);
}

void XRTouchable::updateTouch(XRController* by, bool inRange) {
    if (!by) return;
    auto it = std::find(touchers_.begin(), touchers_.end(), by);
    const bool wasTouching = it != touchers_.end();

    if (inRange && !wasTouching) {
        touchers_.push_back(by);
        touchEntered.emit(by);
    } else if (!inRange && wasTouching) {
        touchers_.erase(it);
        touchExited.emit(by);
    }
}

void XRTouchable::onDestroy() {
    // Notify exit for anything still touching, then drop the records.
    auto snapshot = touchers_;
    touchers_.clear();
    for (XRController* c : snapshot) touchExited.emit(c);
}

void XRTouchable::save(nlohmann::json& j) const {
    j["touchRadius"] = touchRadius;
}

void XRTouchable::load(const nlohmann::json& j) {
    if (j.contains("touchRadius")) touchRadius = j["touchRadius"].get<float>();
}

} // namespace saida
