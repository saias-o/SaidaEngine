#include "render/CaptureScheduler.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

// A golden image is only worth its name if the frame it shows is the same one
// every run. Two things decide that, and neither is visible in the resulting
// PNG: the capture must wait for the world to finish loading, and it must then
// count frames from that point. Getting either wrong produces a plausible
// image of the wrong moment — the failure mode a pixel comparison cannot
// diagnose, because both runs look like valid pictures.
//
// The policy therefore lives in CaptureScheduler, away from Vulkan, and is
// proven here with no device, no window and no renderer.

using namespace saida;

namespace {

int gChecks = 0;

void require(bool condition, const char* what) {
    ++gChecks;
    if (!condition) {
        std::cerr << "[capture-scheduler] FAIL: " << what << "\n";
        std::abort();
    }
}

CaptureScheduler armed(CaptureRequest request) {
    CaptureScheduler scheduler;
    std::string error;
    const bool ok = scheduler.arm(std::move(request), error);
    require(ok, "arm accepted a well-formed request");
    return scheduler;
}

void testCountsFramesOnceSettled() {
    CaptureRequest request;
    request.pngPath = "out.png";
    request.frame = 3;
    CaptureScheduler scheduler = armed(request);

    // Three frames with assets still in flight must not consume the count.
    for (int i = 0; i < 3; ++i)
        require(scheduler.beforeFrame(false) == CaptureScheduler::Decision::Wait,
                "an unsettled frame is not counted");
    require(scheduler.framesWaited() == 3, "the wait is reported");
    require(scheduler.framesCounted() == 0, "nothing was counted while loading");

    require(scheduler.beforeFrame(true) == CaptureScheduler::Decision::Wait, "frame 1 of 3");
    require(scheduler.beforeFrame(true) == CaptureScheduler::Decision::Wait, "frame 2 of 3");
    require(scheduler.beforeFrame(true) == CaptureScheduler::Decision::Capture,
            "frame 3 is the one captured");
    require(scheduler.framesCounted() == 3, "the captured frame is the third counted");
    require(!scheduler.armed(), "the scheduler is single-shot");
}

// The whole point of waiting: an asset requested BY the settled frames (a
// streamed texture a newly visible mesh asks for) must not restart the count,
// or --after-frames N would mean a different frame in every run — exactly the
// nondeterminism the wait exists to remove.
void testSettleIsOneWay() {
    CaptureRequest request;
    request.pngPath = "out.png";
    request.frame = 3;
    CaptureScheduler scheduler = armed(request);

    require(scheduler.beforeFrame(true) == CaptureScheduler::Decision::Wait, "frame 1");
    require(scheduler.beforeFrame(false) == CaptureScheduler::Decision::Wait,
            "a late load does not un-settle the run");
    require(scheduler.beforeFrame(false) == CaptureScheduler::Decision::Capture,
            "frame 3 is still frame 3");
    require(scheduler.framesWaited() == 0, "the late load is not counted as a wait");
}

// A world that never finishes loading must produce a diagnostic, not a
// photograph of a half-loaded scene that would pass a comparison against
// another half-loaded scene.
void testAbandonsRatherThanCaptureUnsettled() {
    CaptureRequest request;
    request.pngPath = "out.png";
    request.settleTimeoutFrames = 4;
    CaptureScheduler scheduler = armed(request);

    for (int i = 0; i < 3; ++i)
        require(scheduler.beforeFrame(false) == CaptureScheduler::Decision::Wait,
                "still within the timeout");
    require(scheduler.beforeFrame(false) == CaptureScheduler::Decision::Abandon,
            "the timeout abandons the capture");
    require(!scheduler.armed(), "an abandoned capture disarms");
    require(scheduler.abandonReason().find("still loading") != std::string::npos,
            "the reason names what went wrong");
    require(scheduler.beforeFrame(true) == CaptureScheduler::Decision::Wait,
            "an abandoned scheduler never fires later");
}

// An interactive screenshot of a scene that streams forever is still useful;
// it is simply not a golden image. Opting out must be explicit.
void testWaitCanBeDisabled() {
    CaptureRequest request;
    request.pngPath = "out.png";
    request.frame = 2;
    request.waitForAssets = false;
    CaptureScheduler scheduler = armed(request);

    require(scheduler.beforeFrame(false) == CaptureScheduler::Decision::Wait, "frame 1");
    require(scheduler.beforeFrame(false) == CaptureScheduler::Decision::Capture,
            "frame 2 is captured regardless of loading");
    require(scheduler.framesWaited() == 0, "no wait was performed");
}

void testRefusals() {
    CaptureScheduler scheduler;
    std::string error;

    CaptureRequest empty;
    require(!scheduler.arm(empty, error), "a request without a path is refused");
    require(!error.empty(), "the refusal is explained");
    require(!scheduler.armed(), "a refused request arms nothing");

    CaptureRequest first;
    first.pngPath = "a.png";
    require(scheduler.arm(first, error), "the first request arms");

    CaptureRequest second;
    second.pngPath = "b.png";
    require(!scheduler.arm(second, error), "a second request is refused while one is armed");
    require(scheduler.request().pngPath == "a.png", "the armed request is untouched");
}

// Frame 0 does not exist. Clamping it up keeps "--after-frames N captures
// frame N" true for every N a caller can type, rather than silently capturing
// nothing.
void testFrameZeroBecomesFrameOne() {
    CaptureRequest request;
    request.pngPath = "out.png";
    request.frame = 0;
    CaptureScheduler scheduler = armed(request);

    require(scheduler.request().frame == 1, "frame 0 is clamped to 1");
    require(scheduler.beforeFrame(true) == CaptureScheduler::Decision::Capture,
            "the first settled frame is captured");
}

} // namespace

int main() {
    testCountsFramesOnceSettled();
    testSettleIsOneWay();
    testAbandonsRatherThanCaptureUnsettled();
    testWaitCanBeDisabled();
    testRefusals();
    testFrameZeroBecomesFrameOne();

    std::cout << "[capture-scheduler] OK (" << gChecks << " checks)\n";
    return 0;
}
