#include "xr/toolkit/XRInput.hpp"

namespace saida {

namespace {
// Double-buffered hand state: `current` is this frame, `previous` last frame.
// Edges are current-vs-previous so the producer only ever submits raw values.
XRHandState g_current[kXRHandCount];
XRHandState g_previous[kXRHandCount];
XRHandSkeletonState g_skeletons[kXRHandCount];
XRPose g_head;
float g_pressThreshold = 0.5f;

bool down(float v) { return v >= g_pressThreshold; }
} // namespace

const XRHandState& XRInput::hand(XRHand h) { return g_current[xrHandIndex(h)]; }
const XRHandSkeletonState& XRInput::skeleton(XRHand h) { return g_skeletons[xrHandIndex(h)]; }
const XRPose& XRInput::head() { return g_head; }

bool XRInput::handTrackingActive(XRHand h) {
    return g_skeletons[xrHandIndex(h)].active;
}

bool XRInput::anyActive() {
    for (const auto& s : g_current) if (s.active) return true;
    return false;
}

bool XRInput::squeezeDown(XRHand h) { return g_current[xrHandIndex(h)].squeeze >= g_pressThreshold; }
bool XRInput::squeezePressed(XRHand h) {
    int i = xrHandIndex(h);
    return down(g_current[i].squeeze) && !down(g_previous[i].squeeze);
}
bool XRInput::squeezeReleased(XRHand h) {
    int i = xrHandIndex(h);
    return !down(g_current[i].squeeze) && down(g_previous[i].squeeze);
}

bool XRInput::triggerDown(XRHand h) { return g_current[xrHandIndex(h)].trigger >= g_pressThreshold; }
bool XRInput::triggerPressed(XRHand h) {
    int i = xrHandIndex(h);
    return down(g_current[i].trigger) && !down(g_previous[i].trigger);
}
bool XRInput::triggerReleased(XRHand h) {
    int i = xrHandIndex(h);
    return !down(g_current[i].trigger) && down(g_previous[i].trigger);
}

void XRInput::setPressThreshold(float t) { g_pressThreshold = t; }
float XRInput::pressThreshold() { return g_pressThreshold; }

void XRInput::beginFrame() {
    for (int i = 0; i < kXRHandCount; ++i) g_previous[i] = g_current[i];
    // Default this frame to "absent"; submitHand() restores presence for tracked
    // controllers. This way a hand that stops reporting reads back inactive.
    for (auto& s : g_current) s.active = false;
    for (auto& s : g_skeletons) s.active = false;
}

void XRInput::submitHand(XRHand h, const XRHandState& state) {
    g_current[xrHandIndex(h)] = state;
}

void XRInput::submitSkeleton(XRHand h, const XRHandSkeletonState& state) {
    g_skeletons[xrHandIndex(h)] = state;
}

void XRInput::clearHand(XRHand h) {
    g_current[xrHandIndex(h)] = XRHandState{};
}

void XRInput::submitHead(const XRPose& pose) { g_head = pose; }

} // namespace saida
