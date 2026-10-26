#include "saida/xrgesture/Locomotion.hpp"
#include "saida/xrgesture/RuntimeInput.hpp"
#include "saida/xrgesture/OpenXRInput.hpp"
#include "saida/xrgesture/Sequence.hpp"
#include <cstdio>
#include <cstdlib>
#include <limits>

using namespace saida;
using namespace saida::xrgesture;
static int checks = 0, failures = 0;
static void check(bool ok, const char* label) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", label);
    }
}
template <class Fn> static bool throws(Fn&& fn) {
    try {
        fn();
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

static XRHandSkeletonState curled(XRHand hand = XRHand::Right, float curl = .5f) {
    XRHandSkeletonState result;
    result.active = true;
    for (auto& joint : result.joints) {
        joint.valid = joint.pose.tracked = true;
        joint.radius = .004f;
    }
    const float mirror = hand == XRHand::Right ? 1.f : -1.f;
    result.joints[0].pose.position = {0, .025f, 0};
    for (int finger = 0; finger < 4; ++finger) {
        const int start = static_cast<int>(XRHandJoint::IndexProximal) + finger * 5;
        const glm::vec3 root((.0275f - finger * .018333333f) * mirror, .06f, 0);
        const glm::vec3 end(0, std::cos(curl * glm::pi<float>()), std::sin(curl * glm::pi<float>()));
        result.joints[start].pose.position = root;
        result.joints[start + 1].pose.position = root + glm::vec3(0, .025f, 0);
        result.joints[start + 2].pose.position = result.joints[start + 1].pose.position + end * .018f;
        result.joints[start + 3].pose.position = result.joints[start + 2].pose.position + end * .015f;
    }
    result.joints[static_cast<int>(XRHandJoint::ThumbTip)].pose.position = {.0725f * mirror, .085f, .009f};
    return result;
}

static Frame frame(double time = 0) {
    Frame result;
    result.time = time;
    result.focused = true;
    result.head.tracked = true;
    result.head.position = {0, 1.65f, 0};
    result.hands[0].skeleton = curled(XRHand::Left);
    result.hands[1].skeleton = curled();
    return result;
}

static int microCount(EventRange events, GestureId id = 0) {
    int count = 0;
    for (const auto& event : events)
        if (event.phase == Phase::Performed && event.gesture <= gesture::ThumbTap &&
            (!id || event.gesture == id))
            ++count;
    return count;
}

static void nativeContracts() {
    GestureSystem system;
    auto f = frame();
    auto& native = f.hands[1].native;
    native.supported = native.active = true;
    native.downMask = xrMicrogestureBit(XRMicrogesture::ThumbTap);
    native.pressedMask = 0;
    check(microCount(system.update(f)) == 0, "native held level without changed flag is not a completion");
    f.time = .02;
    native.downMask = native.pressedMask = 0;
    system.update(f);
    f.time = .04;
    native.downMask = native.pressedMask = xrMicrogestureBit(XRMicrogesture::ThumbTap);
    auto events = system.update(f);
    check(microCount(events, gesture::ThumbTap) == 1, "native completion becomes one performed event");
    bool nativeSource = false;
    for (const auto& e : events)
        if (e.gesture == gesture::ThumbTap)
            nativeSource = e.source == Source::NativeMeta;
    check(nativeSource, "native source remains distinct from heuristic");
    f.time = .06;
    native.pressedMask = 0;
    check(microCount(system.update(f)) == 0, "held native boolean without another change never repeats");
    check(system.update(f).empty(), "duplicate timestamp cannot replay a sample");
    f.time = .07;
    native.pressedMask = native.downMask;
    check(microCount(system.update(f), gesture::ThumbTap) == 1,
          "fresh native completion bypasses local cooldown and remembered down level");
    f.time = .08;
    native.downMask = native.pressedMask = 0;
    system.update(f);
    f.time = .20;
    native.downMask = native.pressedMask = xrMicrogestureBit(XRMicrogesture::ThumbTap);
    check(microCount(system.update(f), gesture::ThumbTap) == 1, "next real edge is accepted");
    f.time = .22;
    f.focused = false;
    check(microCount(system.update(f)) == 0, "unfocused gestures suppressed");
    f.time = .24;
    native.pressedMask = 0;
    f.focused = true;
    check(microCount(system.update(f)) == 0, "held input after focus regain suppressed");
    f.time = 2;
    check(microCount(system.update(f)) == 0, "hitch cannot turn a held native level into a completion");
    f.time = 4;
    native.pressedMask = native.downMask;
    check(microCount(system.update(f), gesture::ThumbTap) == 1,
          "fresh native completion after a hitch is delivered like Meta's sample");
    f.time = 1;
    check(microCount(system.update(f)) == 0, "clock reversal cancels recognition");
    f.time = std::numeric_limits<double>::quiet_NaN();
    check(microCount(system.update(f)) == 0, "invalid clock cannot recognize");
}

static std::vector<Event> stroke(XRHand side, glm::vec2 travel, glm::quat rotation = glm::quat(1, 0, 0, 0),
                                 bool lift = true) {
    MicrogestureConfig config;
    config.mode = MicrogestureMode::SkeletonOnly;
    GestureSystem system(config);
    auto f = frame();
    f.hands[1 - xrHandIndex(side)].skeleton.active = false;
    const float mirror = side == XRHand::Right ? 1.f : -1.f;
    std::vector<Event> result;
    auto submit = [&](double time, float outward, glm::vec2 motion) {
        auto hand = curled(side);
        hand.joints[static_cast<int>(XRHandJoint::ThumbTip)].pose.position =
            glm::vec3((.0275f + outward) * mirror, .085f - motion.y, .009f + motion.x);
        for (auto& joint : hand.joints) {
            joint.pose.position = rotation * joint.pose.position + glm::vec3(2, -.3f, 4);
            joint.pose.orientation = rotation;
        }
        f.hands[xrHandIndex(side)].skeleton = hand;
        f.time = time;
        for (const auto& event : system.update(f))
            result.push_back(event);
    };
    submit(0, .045f, {0, 0});
    submit(.10, .045f, {0, 0});
    submit(.13, .013f, {0, 0});
    submit(.17, .013f, travel * .5f);
    submit(.21, .013f, travel);
    if (lift)
        submit(.25, .045f, travel);
    return result;
}

static void skeletonContracts() {
    struct Case {
        glm::vec2 motion;
        GestureId id;
    };
    const Case cases[] = {{{0, 0}, gesture::ThumbTap},
                          {{.012f, 0}, gesture::SwipeLeft},
                          {{-.012f, 0}, gesture::SwipeRight},
                          {{0, .012f}, gesture::SwipeForward},
                          {{0, -.012f}, gesture::SwipeBackward}};
    for (const auto& c : cases) {
        auto events = stroke(XRHand::Right, c.motion);
        check(microCount({events.data(), events.size()}, c.id) == 1,
              "right-hand tap and four swipes recognized on release");
        auto beforeLift = stroke(XRHand::Right, c.motion, glm::quat(1, 0, 0, 0), false);
        check(microCount({beforeLift.data(), beforeLift.size()}) == 0, "no microgesture before release");
        auto rotated =
            stroke(XRHand::Right, c.motion, glm::angleAxis(.8f, glm::normalize(glm::vec3(1, 2, 3))));
        check(microCount({rotated.data(), rotated.size()}, c.id) == 1,
              "recognition is invariant under rigid hand motion");
    }
    auto left = stroke(XRHand::Left, {-.012f, 0});
    check(microCount({left.data(), left.size()}, gesture::SwipeLeft) == 1,
          "left-hand lateral mapping is mirrored");
    auto diagonal = stroke(XRHand::Right, {.012f, .012f});
    check(microCount({diagonal.data(), diagonal.size()}) == 0, "ambiguous diagonal rejected");
    auto tiny = stroke(XRHand::Right, {.003f, .001f});
    check(microCount({tiny.data(), tiny.size()}, gesture::ThumbTap) == 1,
          "small tracking noise remains a tap");
    auto h = curled();
    h.joints[static_cast<int>(XRHandJoint::IndexDistal)].valid = false;
    check(!thumbContact(h, XRHand::Right), "missing required finger joint is not inferred");
    h = curled();
    h.joints[5].pose.position.x = std::numeric_limits<float>::quiet_NaN();
    check(!thumbContact(h, XRHand::Right), "nonfinite hand sample rejected");
}

static void customContracts() {
    GestureSystem system;
    float value = 0;
    bool validSample = true;
    Rule rule;
    rule.id = gesture::FirstCustom;
    int rightResets = 0;
    rule.onReset = [&](XRHand hand) {
        if (hand == XRHand::Right)
            ++rightResets;
    };
    rule.evaluate = [&](const Frame&, XRHand hand) {
        return Observation{validSample && hand == XRHand::Right, value};
    };
    system.add(rule);
    check(throws([&] { system.add(rule); }), "duplicate custom id rejected");
    auto f = frame();
    system.update(f);
    value = 1;
    f.time = .02;
    system.update(f);
    f.time = .10;
    auto events = system.update(f);
    bool started = false;
    for (auto e : events)
        if (e.gesture == rule.id && e.phase == Phase::Started)
            started = true;
    check(started, "custom recognizer obeys hold interval");
    check(throws([&] { system.remove(rule.id); }), "live continuous gesture cannot be abandoned");
    value = .65f;
    f.time = .12;
    events = system.update(f);
    bool updated = false;
    for (auto e : events)
        if (e.gesture == rule.id && e.phase == Phase::Updated)
            updated = true;
    check(updated, "hysteresis prevents chatter between enter and leave");
    f.focused = false;
    events = system.update(f);
    bool cancelled = false;
    for (auto e : events)
        if (e.gesture == rule.id && e.phase == Phase::Cancelled)
            cancelled = true;
    check(cancelled, "focus loss cancels active interaction even at the same timestamp");
    check(rightResets == 1, "custom temporal history receives a reset on focus loss");
    f.focused = true;
    check(system.remove(rule.id), "inactive custom recognizer can be removed");
    Rule bad = rule;
    bad.id = 1;
    check(throws([&] { system.add(bad); }), "built-in id namespace reserved");
    bad.id = gesture::FirstCustom;
    bad.enter = .2f;
    bad.leave = .8f;
    check(throws([&] { system.add(bad); }), "invalid hysteresis rejected");
    MicrogestureConfig invalid;
    invalid.releaseMargin = invalid.contactMargin;
    check(throws([&] { GestureSystem badSystem(invalid); }), "invalid contact hysteresis rejected");
    Rule discrete = rule;
    validSample = true;
    value = 1;
    discrete.recognition = Recognition::Discrete;
    discrete.holdSeconds = 0;
    system.add(discrete);
    f.time = .16;
    check(microCount(system.update(f)) == 0, "custom discrete recognition is not a reserved microgesture");
    bool performed = false;
    value = 0;
    f.time = .18;
    system.update(f);
    value = 1;
    f.time = .20;
    for (auto e : system.update(f))
        if (e.gesture == discrete.id && e.phase == Phase::Performed)
            performed = true;
    check(performed, "custom discrete event requires a neutral-to-active transition");
}

static void poseContracts() {
    auto f = frame();
    f.hands[0].skeleton.active = false;
    for (GestureId id : {gesture::Pinch, gesture::Grab, gesture::OpenPalm, gesture::Point}) {
        auto& hand = f.hands[1].skeleton;
        hand = curled(XRHand::Right, id == gesture::OpenPalm ? 0.f : .8f);
        if (id == gesture::Pinch)
            hand.joints[static_cast<int>(XRHandJoint::ThumbTip)].pose.position =
                hand.joint(XRHandJoint::IndexTip).pose.position;
        if (id == gesture::Point) {
            const auto straight = curled(XRHand::Right, 0);
            for (int i = static_cast<int>(XRHandJoint::IndexProximal);
                 i <= static_cast<int>(XRHandJoint::IndexTip); ++i)
                hand.joints[i] = straight.joints[i];
        }
        GestureSystem system;
        f.time = 0;
        system.update(f);
        f.time = .08;
        bool started = false;
        for (const auto& event : system.update(f))
            if (event.gesture == id && event.phase == Phase::Started)
                started = true;
        check(started, "built-in pinch/grab/open/point uses the continuous recognition pipeline");
        hand.active = false;
        f.time = .10;
        bool cancelled = false;
        for (const auto& event : system.update(f))
            if (event.gesture == id && event.phase == Phase::Cancelled)
                cancelled = true;
        check(cancelled, "built-in pose cancels when optical tracking is lost");
    }
}

static void locomotionContracts() {
    LocomotionConfig config;
    config.lateral = LateralAction::Step;
    LocomotionGate gate(config);
    auto f = frame();
    Event event{gesture::SwipeForward, XRHand::Right, Phase::Performed, Source::NativeMeta, 0, 1};
    auto send = [&](GestureId id, double time, XRHand hand = XRHand::Right) {
        f.time = time;
        event.gesture = id;
        event.time = time;
        event.hand = hand;
        return gate.update(f, {&event, 1});
    };
    check(glm::length(send(gesture::SwipeForward, 0).step) == 0, "movement requires explicit activation");
    check(send(gesture::ThumbTap, .02).activated && gate.active(), "thumb tap opens locomotion gate");
    auto command = send(gesture::SwipeForward, .04);
    check(glm::length(command.step - glm::vec3(0, 0, -.5f)) < 1e-5f, "forward step follows headset heading");
    check(glm::length(gate.update(f, {&event, 1}).step) == 0, "duplicate locomotion sample ignored");
    check(glm::length(send(gesture::SwipeRight, .06, XRHand::Left).step) == 0,
          "other hand cannot drive an armed gate");
    f.head.orientation = glm::angleAxis(glm::half_pi<float>(), glm::vec3(0, 1, 0));
    command = send(gesture::SwipeForward, .08);
    check(glm::length(command.step - glm::vec3(-.5f, 0, 0)) < 1e-5f,
          "stepping is world-space and head-relative");
    check(send(gesture::ThumbTap, .10).deactivated && !gate.active(), "second tap closes gate");
    send(gesture::ThumbTap, .12);
    f.focused = false;
    check(gate.update(f, {}).deactivated, "focus loss cancels locomotion even at the same timestamp");
    f.focused = true;
    send(gesture::ThumbTap, .16);
    f.hands[1].skeleton = curled(XRHand::Right, 0);
    f.time = .18;
    check(gate.update(f, {}).deactivated, "extending index cancels locomotion");
    f.hands[1].skeleton = curled();
    send(gesture::ThumbTap, .20);
    f.hands[1].skeleton.joints[1].pose.orientation = glm::angleAxis(glm::radians(55.f), glm::vec3(0, 0, 1));
    f.time = .22;
    check(gate.update(f, {}).deactivated, "wrist tilt cancels locomotion");
    f.hands[1].skeleton = curled();
    send(gesture::ThumbTap, .24);
    f.time = 1;
    check(gate.update(f, {}).deactivated, "long sample gap cancels locomotion");
    StepMotion motion;
    motion.request({0, 0, -.5f});
    glm::vec3 sum(0);
    for (int i = 0; i < 100; ++i)
        sum += motion.advance(.01f);
    check(glm::length(sum - glm::vec3(0, 0, -.5f)) < 1e-5f, "step distance does not depend on frame rate");
    motion.request({0, 0, -100});
    check(glm::length(motion.advance(.1f)) <= .281f, "queued step size and velocity bounded");
    check(glm::length(motion.advance(.5f)) == 0, "a stalled frame cannot replay a long stride");
    check(throws([&] { motion.request({std::numeric_limits<float>::quiet_NaN(), 0, 0}); }),
          "invalid motion cannot reach character solver");
}

static void inputContracts() {
    XRInput::beginFrame();
    MetaMicrogestureInput input;
    xr::InputValue values[5];
    for (size_t i = 0; i < 5; ++i) {
        values[i].action = i;
        values[i].userPath = "/user/hand/left";
        values[i].active = true;
    }
    values[0].boolean = values[0].changed = true;
    input.submit({true, true, 3, values, 5});
    XRInput::submitSkeleton(XRHand::Left, curled(XRHand::Left));
    auto captured = captureInput(3, input.state());
    check(captured.focused && captured.hands[0].native.pressedMask == 1,
          "runtime adapter preserves native snapshot");
    values[4].active = false;
    values[4].boolean = values[4].changed = true;
    input.submit({true, true, 3.1, values, 5});
    check(input.state().hands[0].active && input.state().hands[0].pressedMask == 1,
          "inactive tap channel cannot suppress a valid swipe or publish its stale tap");
    for (auto& value : values)
        value.active = false;
    input.submit({true, true, 3.2, values, 5});
    check(!input.state().hands[0].active && input.state().hands[0].downMask == 0 &&
              input.state().hands[0].pressedMask == 0,
          "all inactive channels clear native activity and stale completions");
    values[0].active = true;
    input.submit({true, true, 3.3, values, 5});
    check(input.state().hands[0].active && input.state().hands[0].pressedMask == 1,
          "a single recovered channel immediately delivers fresh completions");
    XRInput::beginFrame();
    input.submit({});
    captured = captureInput(4, input.state());
    check(!captured.focused && !captured.hands[0].native.active && captured.hands[0].native.pressedMask == 0,
          "missing producer cannot leave stale native events or focus");
}

static void sequenceContracts() {
    SequenceRecognizer twice(gesture::FirstCustom, {gesture::ThumbTap, gesture::ThumbTap}, .4, .8);
    auto f = frame();
    Event event{gesture::ThumbTap, XRHand::Right, Phase::Performed, Source::NativeMeta, 0, 1};
    auto send = [&](double time, XRHand hand = XRHand::Right) {
        f.time = event.time = time;
        event.hand = hand;
        return twice.update(f, {&event, 1});
    };
    check(send(0).empty(), "sequence waits for the second completed token");
    auto output = send(.2);
    check(output.size() == 1 && output[0].gesture == gesture::FirstCustom,
          "two taps form a separate custom gesture");
    check(twice.update(f, {&event, 1}).empty(), "same-frame sequence replay suppressed");
    send(.3);
    check(send(.4, XRHand::Left).empty(), "sequences do not combine different hands");
    check(send(.5).size() == 1, "other hand does not erase owner sequence progress");
    send(.6);
    check(send(1.1).empty(), "sequence timeout discards stale progress");
    f.focused = false;
    f.time = 1.2;
    twice.update(f, {});
    f.focused = true;
    check(send(1.3).empty(), "sequence cannot bridge focus loss");
    check(throws([&] { SequenceRecognizer bad(2, {gesture::ThumbTap}); }),
          "sequence id cannot shadow built-ins");
}

int main() {
    nativeContracts();
    skeletonContracts();
    customContracts();
    poseContracts();
    locomotionContracts();
    inputContracts();
    sequenceContracts();
    std::printf("%s XRGesture: %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
