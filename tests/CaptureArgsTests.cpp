#include "runtime/CaptureArgs.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

// The capture flags are parsed once and obeyed by two executables, so a
// divergence here is a golden image compared against a differently-configured
// run — a wrong answer that still looks like a picture. The refusals matter as
// much as the parsing: every value this rejects would otherwise produce a
// plausible frame of the wrong thing.

using namespace saida;
using saida::runtime::CaptureViewpoint;

namespace {

int gChecks = 0;

void require(bool condition, const char* what) {
    ++gChecks;
    if (!condition) {
        std::cerr << "[capture-args] FAIL: " << what << "\n";
        std::abort();
    }
}

struct Parsed {
    bool ok = false;
    CaptureRequest request;
    CaptureViewpoint viewpoint;
    std::string error;
};

Parsed parse(std::vector<std::string> args) {
    args.insert(args.begin(), "SaidaEngine.exe");
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (std::string& arg : args) argv.push_back(arg.data());

    Parsed out;
    out.ok = runtime::parseCaptureArgs(static_cast<int>(argv.size()), argv.data(),
                                       out.request, out.viewpoint, out.error);
    return out;
}

// A screenshot is asked for in order to be compared with another one, so the
// reproducible policy is what an unadorned --screenshot must give.
void testDefaultsAreTheReproducibleOnes() {
    const Parsed p = parse({"--screenshot", "out.png"});
    require(p.ok, "a bare --screenshot parses");
    require(p.request.pngPath == "out.png", "the path is taken");
    require(p.request.frame == 2, "frame 2 by default");
    require(p.request.fixedStep > 0.0f, "the clock is pinned by default");
    require(p.request.waitForAssets, "the asset wait is on by default");
    require(!p.viewpoint.set, "no viewpoint override unless asked");
}

void testNoCaptureIsNotAnError() {
    const Parsed p = parse({"--project", "Game/Game.saidaproj"});
    require(p.ok, "unrelated arguments parse");
    require(p.request.pngPath.empty(), "no capture was requested");
}

void testOptOuts() {
    const Parsed p = parse({"--screenshot", "o.png", "--no-wait-assets",
                            "--fixed-step", "0", "--after-frames", "7",
                            "--settle-timeout", "12"});
    require(p.ok, "the opt-outs parse");
    require(!p.request.waitForAssets, "--no-wait-assets is honoured");
    require(p.request.fixedStep == 0.0f, "--fixed-step 0 restores the real clock");
    require(p.request.frame == 7, "--after-frames is honoured");
    require(p.request.settleTimeoutFrames == 12, "--settle-timeout is honoured");
}

void testRejectsValuesThatWouldSilentlyMisbehave() {
    require(!parse({"--after-frames", "0"}).ok, "frame 0 is refused");
    require(!parse({"--after-frames", "abc"}).ok, "a non-number is refused");
    require(!parse({"--after-frames", "3x"}).ok, "a trailing suffix is refused");
    require(!parse({"--fixed-step", "-1"}).ok, "a negative step is refused");
    require(!parse({"--settle-timeout", "0"}).ok, "a zero timeout is refused");

    const Parsed p = parse({"--after-frames", "abc"});
    require(p.error.find("--after-frames") != std::string::npos,
            "the refusal names the flag");
}

void testViewpoint() {
    const Parsed p = parse({"--screenshot", "o.png", "--camera-pos", "1,2.5,-3",
                            "--camera-look", "0,0,0"});
    require(p.ok, "a full viewpoint parses");
    require(p.viewpoint.set, "the viewpoint is marked set");
    require(p.viewpoint.position[0] == 1.0f, "x");
    require(p.viewpoint.position[1] == 2.5f, "y keeps its fraction");
    require(p.viewpoint.position[2] == -3.0f, "z keeps its sign");
    require(p.viewpoint.target[0] == 0.0f && p.viewpoint.target[2] == 0.0f, "target");
}

// Half a viewpoint would aim a placed camera wherever the scene happened to
// look: an image nobody asked for, indistinguishable from a broken scene.
void testHalfAViewpointIsRefused() {
    require(!parse({"--camera-pos", "1,2,3"}).ok, "position alone is refused");
    require(!parse({"--camera-look", "1,2,3"}).ok, "target alone is refused");

    const Parsed p = parse({"--camera-pos", "1,2,3"});
    require(p.error.find("together") != std::string::npos,
            "the refusal says they go together");
}

// A vector that parses to something shorter than three components would place
// the camera at a partly-default position — inside the floor, most likely, which
// reads as a rendering bug rather than a bad flag.
void testMalformedVectorsAreRefused() {
    require(!parse({"--camera-pos", "1,2", "--camera-look", "0,0,0"}).ok,
            "two components are refused");
    require(!parse({"--camera-pos", "1,2,3,4", "--camera-look", "0,0,0"}).ok,
            "four components are refused");
    require(!parse({"--camera-pos", "1,2,z", "--camera-look", "0,0,0"}).ok,
            "a non-numeric component is refused");
    require(!parse({"--camera-pos", "1 2 3", "--camera-look", "0,0,0"}).ok,
            "spaces are not separators");
    require(!parse({"--camera-pos", "1,,3", "--camera-look", "0,0,0"}).ok,
            "an empty component is refused");
}

} // namespace

int main() {
    testDefaultsAreTheReproducibleOnes();
    testNoCaptureIsNotAnError();
    testOptOuts();
    testRejectsValuesThatWouldSilentlyMisbehave();
    testViewpoint();
    testHalfAViewpointIsRefused();
    testMalformedVectorsAreRefused();

    std::cout << "[capture-args] OK (" << gChecks << " checks)\n";
    return 0;
}
