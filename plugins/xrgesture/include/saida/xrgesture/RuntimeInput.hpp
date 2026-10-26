#pragma once

#include "Types.hpp"
#include "xr/toolkit/XRInput.hpp"

namespace saida::xrgesture {

// This adapter alone requires an XR-enabled Saida runtime. Recognizers and
// synthetic-frame tests do not link OpenXR or initialize an engine.
inline Frame captureInput(double monotonicSeconds, const RuntimeInputState& runtime) {
    Frame frame;
    frame.time = monotonicSeconds;
    frame.focused = runtime.focused;
    frame.head = XRInput::head();
    for (XRHand hand : {XRHand::Left, XRHand::Right}) {
        auto& sample = frame.hands[xrHandIndex(hand)];
        sample.skeleton = XRInput::skeleton(hand);
        sample.native = runtime.hands[xrHandIndex(hand)];
    }
    return frame;
}

} // namespace saida::xrgesture
