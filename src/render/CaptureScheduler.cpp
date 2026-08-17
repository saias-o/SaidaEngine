#include "render/CaptureScheduler.hpp"

namespace saida {

bool CaptureScheduler::arm(CaptureRequest request, std::string& error) {
    if (armed_) {
        error = "a frame capture is already armed for " + request_.pngPath;
        return false;
    }
    if (request.pngPath.empty()) {
        error = "a frame capture needs an output path";
        return false;
    }
    // Frame 0 is no frame at all. Clamping up rather than accepting it keeps
    // "--after-frames N captures frame N" true for every N a caller can pass.
    if (request.frame == 0) request.frame = 1;

    request_ = std::move(request);
    armed_ = true;
    settled_ = false;
    framesWaited_ = 0;
    framesCounted_ = 0;
    abandonReason_.clear();
    return true;
}

CaptureScheduler::Decision CaptureScheduler::beforeFrame(bool assetsSettled) {
    if (!armed_) return Decision::Wait;

    // The wait is one-way: once the world has settled, a later load (an asset
    // the captured frame itself asks for) must not restart the count, or the
    // requested frame number would stop meaning anything.
    if (request_.waitForAssets && !settled_) {
        if (!assetsSettled) {
            ++framesWaited_;
            if (framesWaited_ >= request_.settleTimeoutFrames) {
                armed_ = false;
                abandonReason_ = "assets were still loading after " +
                                 std::to_string(framesWaited_) +
                                 " frames; the capture would show a partially "
                                 "loaded world";
                return Decision::Abandon;
            }
            return Decision::Wait;
        }
        settled_ = true;
    }

    ++framesCounted_;
    if (framesCounted_ < request_.frame) return Decision::Wait;

    armed_ = false;
    return Decision::Capture;
}

} // namespace saida
