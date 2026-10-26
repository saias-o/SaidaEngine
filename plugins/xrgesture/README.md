# XRGesture

Optional C++17, header-only gesture recognition for SaidaEngine. The engine
runtime has no dependency on this directory. The host explicitly links
`Saida::XRGesture` and owns its recognizers. No scene node, reflected type,
serialization change, vendor SDK or game behavior is registered by this add-on.

## What the upstream code actually does

Research used [MicrogesturesDemos](https://github.com/dilmerv/MicrogesturesDemos/tree/418b046ae48aab7b1b3cfc431565c121e82dbc4b)
and the official Meta XR Core SDK **76.0.0** sources, obtained from the
[Meta package archive](https://npm.developer.oculus.com/com.meta.xr.sdk.core/-/com.meta.xr.sdk.core-76.0.0.tgz).
The demo is an application of the SDK, rather than a source implementation of
the classifier. The inspected call chain is:

| Layer | Responsibility |
| --- | --- |
| `OVRMicrogesturesSample` | Subscribe to hand event sources and display the recognized gesture in sample UI. |
| `OVRMicrogestureEventSource.Update` | Read `OVRHand.GetMicrogestureType()` and dispatch recognized nonempty events. |
| `OVRHand` | Read native hand state through `OVRPlugin`, send the microgesture hint and validate the returned enum. |
| `OVRPlugin` | Call the native `ovrp_GetHandTrackingState` entry point. |
| Native Meta runtime | Perform the actual classification; its implementation is not present in those C# sources. |

This add-on implements the documented OpenXR action contract independently.
No Meta SDK source, binary, trained model or Unity dependency is redistributed.
The exact native classifier is obtained from the runtime, rather than reproduced
by geometric code. References:

- [Meta microgestures documentation](https://developers.meta.com/vr/documentation/unity/unity-microgestures/).
- [Khronos extension specification](https://github.com/KhronosGroup/OpenXR-Docs/blob/main/specification/sources/chapters/extensions/meta/meta_hand_tracking_microgestures.adoc).
- [Meta locomotion guidance](https://developers.meta.com/horizon/documentation/unity/unity-isdk-locomotion-microgestures/).

## Integration and ownership

```cmake
add_subdirectory("${SaidaEngine_SOURCE_DIR}/plugins/xrgesture" xrgesture)
target_link_libraries(MyGame PRIVATE Saida::XRGesture)
```

Register the native provider before creating the engine; OpenXR extension and
action declarations cannot be added after session attachment:

```cpp
#include "Engine.hpp"
#include "core/Time.hpp"
#include "saida/xrgesture/OpenXRInput.hpp"
#include "saida/xrgesture/RuntimeInput.hpp"
#include "saida/xrgesture/XRGesture.hpp"

namespace gestures = saida::xrgesture;
auto native = std::make_shared<gestures::MetaMicrogestureInput>();
saida::Engine engine(nullptr, projectPath, true, {}, {}, {native});
gestures::GestureSystem recognizer;
engine.setOnFrame([&](float) {
    auto frame = gestures::captureInput(saida::Time::unscaledElapsed(), native->state());
    for (const auto& event : recognizer.update(frame)) {
        // Consume immediately; the range is borrowed until update/reset.
        handleGesture(event);
    }
});
engine.run();
```

Keep providers/recognizers at stable
addresses for the callbacks' lifetime. The engine shares ownership of the
provider; game callbacks must not outlive their captured objects. `captureInput`
reads the existing `XRInput` head/world-space joints and the explicit plugin
state. A test or another tracking backend can supply `Frame` directly. Time
must be monotonic, unscaled seconds. Focus is an explicit input; do not infer it
from visible hands or from extension availability.

The engine's `xr::InputExtension` API is generic and OpenXR-header-free. It owns
only dependency negotiation, typed native actions, merged profile binding,
focus snapshots and native lifecycle. All Meta paths/types remain here.
`XRInput` and `XRTypes` retain their existing public contracts.

## Backends

`MetaMicrogestureInput` requests both `XR_EXT_hand_interaction` and
`XR_META_hand_tracking_microgestures` as one dependency bundle. Five Boolean
actions, each with left/right subactions, bind to
`/interaction_profiles/ext/hand_interaction_ext`:

| Gesture | Component under `/user/hand/{left,right}` |
| --- | --- |
| Left | `/input/swipe_left_meta/click` |
| Right | `/input/swipe_right_meta/click` |
| Forward | `/input/swipe_forward_meta/click` |
| Backward | `/input/swipe_backward_meta/click` |
| Tap | `/input/tap_thumb_meta/click` |

These are completed gestures. Native hand mapping is already defined by the
runtime and is never mirrored again. All five channels must be active for a
hand's native sample to be accepted. The recognizer requires a neutral native
sample first, suppresses sticky/replayed edges and applies a configurable
per-gesture cooldown (120 ms by default).

`MicrogestureConfig::mode` selects NativeOnly, NativePreferred (default), or
SkeletonOnly. NativePreferred uses native actions when supported and active;
otherwise it uses an **independent, unqualified geometric heuristic**, requiring
`XR_EXT_hand_tracking` optical joints. It does not infer fingers from controller
buttons. The heuristic requires slightly curled fingers and a raised thumb,
then contact with the middle index segment, motion and release. Coordinates are
hand-local and distances are normalized by hand width. Ambiguous diagonal
swipes, missing joints, invalid samples and excessive contact duration reset it.

A completed event retains `Source::NativeMeta` or `Source::Skeleton`. Availability
and session focus remain separate. The game should show missing tracking and
log backend transitions. Extension advertisement/activation over PC Link depends
on the actual runtime: a successful build is no evidence of native availability
or reliable geometric recognition on a headset.

## Gestures beyond microgestures

Built-ins are thumb tap/four swipes (Performed), pinch, grab, open palm and point
(Started/Updated/Ended/Cancelled). Continuous poses use confidence, hold timing
and hysteresis. Add custom rules with IDs at or above `gesture::FirstCustom`:

```cpp
gestures::Rule rule;
rule.id = gestures::gesture::FirstCustom;
rule.evaluate = [](const gestures::Frame& frame, saida::XRHand hand) {
    // Any spatial/temporal/bimanual classifier can inspect the complete frame.
    return gestures::Observation{sampleIsValid(frame, hand), score(frame, hand)};
};
rule.onReset = [](saida::XRHand hand) { discardHistory(hand); };
rule.enter = .8f;
rule.leave = .5f;
rule.holdSeconds = .06;
recognizer.add(std::move(rule));
```

Set `recognition = Recognition::Discrete` for completion tokens: a neutral
observation is required before each recognition. `SequenceRecognizer` composes
Started/Performed tokens into arbitrary per-hand sequences with bounded gaps
and total duration, for example two taps or grab followed by a swipe. Complex
arbitrary gestures require a custom classifier/rule; no universal classifier is
claimed. Lost/invalid data invokes `onReset`; focus loss, time reversal and a
sample gap over 250 ms cancel active interactions. Reset before removing a live
continuous rule. Do not recursively call or mutate a recognizer from callbacks.

The framework reserves 64 rules and reuses fixed 256-event storage. Sequences
have at most 16 steps. Updates perform no framework heap allocations; custom
callbacks must uphold that property themselves. Consume or copy events before
the next update/reset. Configure/register outside the per-frame path.

## Optional locomotion intents

`LocomotionGate` produces world-space intents; it never moves scene nodes.
A tap arms one hand. Further owner-hand swipes produce head-relative 0.5 m steps
or 30-degree lateral snap turns (default). `LateralAction::Step` selects strafing.
R1World uses 15-degree snap turns and a more sensitive game-specific microgesture
configuration. A second owner tap, extended index, wrist tilt over 45 degrees,
focus/permission/tracking loss, timeout or sample gap cancels the gate. The
wrist test compares gravity in wrist space, avoiding cancellation on world yaw
or reference-space rebases.

`StepMotion` consumes intents at bounded speed (2.8 m/s, at most 1 m queued).
The host applies each delta through its existing character solver and must call
`cancel` on rejection. R1World handles menus, vehicles, water, tile readiness and
physics in game code; no R1World dependency exists in this plugin.

## Headless verification

From the engine directory, with UCRT64 first on Windows PATH:

```powershell
cmake -S plugins/xrgesture -B build-rel/xrgesture-tests -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-rel/xrgesture-tests --parallel 2
ctest --test-dir build-rel/xrgesture-tests --output-on-failure
```

The portable suite uses synthetic native/joint frames for all five gestures,
handedness, rigid transformations, lifecycle, custom rules, temporal sequences,
gating and bounded steps. On Windows the OpenXR suite compiles the real engine
Instance/Actions/InputExtensions against a fake C dispatch surface, covering
extension negotiation, exact component paths, typed channels, shared-profile
merging, rejection with controller fallback, focus, invalid values and cleanup.
Neither suite creates an OS window, Vulkan device, OpenXR loader session or
headset session. R1World's separate `r1_xr_gesture_test` checks its adapter without
creating an Engine. Hardware qualification remains open in `ROADMAP.md`.
