#pragma once

#include <algorithm>
#include <limits>

namespace saida {

// RmlUi reports a delay relative to its last Update(), not a live countdown.
// Keep the deadline so delayed animations and caret blinking also wake an idle UI.
class UiUpdateSchedule {
public:
    bool due(double now, bool dirty) const { return dirty || now >= nextUpdate_; }
    void updated(double now, double delay) {
        nextUpdate_ = now + std::max(0.0, delay);
    }
    void reset() { nextUpdate_ = -std::numeric_limits<double>::infinity(); }
private:
    double nextUpdate_ = -std::numeric_limits<double>::infinity();
};

} // namespace saida
