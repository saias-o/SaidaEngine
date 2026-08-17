#pragma once

#include <cstdint>
#include <string>

namespace saida {

// What a caller asks for when it wants one frame written to a PNG.
//
// `frame` alone is not enough to make two runs produce the same image. A scene
// streams its meshes and textures asynchronously, so a load that lands on frame
// 3 in one run and on frame 4 in the next changes what frame 3 contains for a
// reason that has nothing to do with the change under test; and a frame clock
// read from the wall clock advances animation and physics by a different amount
// every run. `waitForAssets` and `fixedStep` remove those two, which is what
// turns a capture into a golden image.
struct CaptureRequest {
    std::string pngPath;
    // 1-based index into the counted sequence: frame 1 is the first frame drawn
    // after the world settled, not the first frame of the process.
    uint32_t frame = 1;
    // Seconds handed to the frame clock instead of the measured frame time.
    // 0 keeps the real clock (an interactive screenshot, not a golden image).
    float fixedStep = 0.0f;
    bool waitForAssets = true;
    // Bound on the wait. Reaching it abandons the capture with a diagnostic
    // rather than photographing a half-loaded world, which would be a silent
    // wrong answer.
    uint32_t settleTimeoutFrames = 600;
};

// Decides which frame a capture lands on. It owns the policy and nothing else:
// no Vulkan, no renderer, no window — so the rule that makes a capture
// reproducible is provable headlessly, while FrameCapture owns the GPU copy.
//
// One instance serves one capture: it disarms itself once it has named a frame.
class CaptureScheduler {
public:
    enum class Decision {
        Wait,     // not this frame — still settling, or still counting
        Capture,  // capture THIS frame, before it is drawn
        Abandon,  // the world never settled; see abandonReason()
    };

    // Refuses a second request while one is armed: two captures sharing one
    // frame counter would each get the other's frame.
    bool arm(CaptureRequest request, std::string& error);
    bool armed() const { return armed_; }

    // Called once per frame, before the frame is drawn. `assetsSettled` is the
    // world's answer to "is anything still loading".
    Decision beforeFrame(bool assetsSettled);

    const CaptureRequest& request() const { return request_; }
    const std::string& abandonReason() const { return abandonReason_; }
    uint32_t framesWaited() const { return framesWaited_; }
    // Frames counted since the world settled, including the captured one.
    uint32_t framesCounted() const { return framesCounted_; }

private:
    CaptureRequest request_;
    bool armed_ = false;
    bool settled_ = false;
    uint32_t framesWaited_ = 0;
    uint32_t framesCounted_ = 0;
    std::string abandonReason_;
};

} // namespace saida
