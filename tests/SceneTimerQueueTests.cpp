#include "scene/Behaviour.hpp"
#include "scene/Node.hpp"
#include "scene/SceneTimerQueue.hpp"

#include <cassert>
#include <cmath>
#include <vector>

namespace {

constexpr float kFloatTolerance = 1e-5f;

bool near(float actual, float expected) {
    return std::abs(actual - expected) <= kFloatTolerance;
}

void testOneShotAndRepeatingTimers() {
    saida::SceneTimerQueue timers;
    saida::Node owner("TimerOwner");
    int oneShotCount = 0;
    int repeatingCount = 0;

    const auto oneShot = timers.after(&owner, 0.5f, [&] { ++oneShotCount; });
    const auto repeating = timers.every(&owner, 0.25f, [&] { ++repeatingCount; });
    assert(oneShot != saida::kInvalidTimerId);
    assert(repeating != saida::kInvalidTimerId);
    assert(timers.every(&owner, 0.0f, [] {}) == saida::kInvalidTimerId);

    timers.tick(0.2f);
    assert(oneShotCount == 0 && repeatingCount == 0);
    timers.tick(0.1f);
    assert(oneShotCount == 0 && repeatingCount == 1);
    timers.tick(0.2f);
    assert(oneShotCount == 1 && repeatingCount == 2);
    timers.tick(1.0f);
    assert(oneShotCount == 1 && repeatingCount == 3);

    timers.cancel(repeating);
    timers.tick(1.0f);
    assert(repeatingCount == 3);
}

void testTweenAndOwnerCancellation() {
    saida::SceneTimerQueue timers;
    saida::Node owner("TweenOwner");
    saida::Node cancelledOwner("CancelledOwner");
    std::vector<float> samples;
    bool cancelledCallbackRan = false;

    timers.tween(&owner, 1.0f, saida::Easing::Linear,
                 [&](float value) { samples.push_back(value); });
    timers.after(&cancelledOwner, 0.1f, [&] { cancelledCallbackRan = true; });
    timers.cancelOwnedBy(&cancelledOwner);

    timers.tick(0.25f);
    timers.tick(0.75f);
    timers.tick(1.0f);

    assert(!cancelledCallbackRan);
    assert(samples.size() == 2);
    assert(near(samples[0], 0.25f));
    assert(near(samples[1], 1.0f));
}

void testTimersCreatedByCallbackStartNextTick() {
    saida::SceneTimerQueue timers;
    saida::Behaviour owner;
    int callbackCount = 0;

    timers.after(&owner, 0.0f, [&] {
        ++callbackCount;
        timers.after(&owner, 0.0f, [&] { ++callbackCount; });
    });

    timers.tick(0.1f);
    assert(callbackCount == 1);
    timers.tick(0.1f);
    assert(callbackCount == 2);
}

void testBehaviourOwnerCancellation() {
    saida::SceneTimerQueue timers;
    saida::Behaviour owner;
    saida::Behaviour otherOwner;
    int callbackCount = 0;

    timers.after(&owner, 0.1f, [&] { ++callbackCount; });
    timers.after(&otherOwner, 0.1f, [&] { callbackCount += 10; });
    timers.cancelOwnedBy(&owner);
    timers.tick(0.1f);

    assert(callbackCount == 10);
}

} // namespace

int main() {
    testOneShotAndRepeatingTimers();
    testTweenAndOwnerCancellation();
    testTimersCreatedByCallbackStartNextTick();
    testBehaviourOwnerCancellation();
    return 0;
}
