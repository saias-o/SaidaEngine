// Run the real instance/action bridge against a fake OpenXR dispatch surface.
// No loader, headset, Vulkan device, engine, window or OS input is initialized.
#include "xr/XrPlatform.hpp"
#include "xr/XrInstance.hpp"
#include "xr/XrActions.hpp"
#include "xr/toolkit/XRInput.hpp"
#include "xr/XrInputExtensions.hpp"
#include "saida/xrgesture/OpenXRInput.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <limits>
#include <array>
#include <stdexcept>
#include <set>
#include <string>
#include <vector>
#include <new>

static size_t allocations = 0;
void* operator new(std::size_t size) {
    ++allocations;
    if (void* result = std::malloc(size ? size : 1))
        return result;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) {
    return ::operator new(size);
}
void operator delete(void* memory) noexcept {
    std::free(memory);
}
void operator delete[](void* memory) noexcept {
    std::free(memory);
}
void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}
void operator delete[](void* memory, std::size_t) noexcept {
    std::free(memory);
}

namespace mock {
static bool dependency = true, meta = true, reject = false, queryFailure = false;
static bool rejectTouch = false, invalidValues = false;
static std::map<std::string, std::vector<size_t>> profileCalls;
static int destroyedActions = 0, destroyedSpaces = 0;
static XrResult syncResult = XR_SUCCESS;
static std::set<std::string> enabled, bound;
static std::map<XrPath, std::string> paths;
static std::map<XrAction, std::string> actions;
static std::map<XrAction, int> nativeActions;
static uint64_t next = 1;
static size_t actionsAtAttach = 0;
static size_t failActionAt = 0;
static int destroyedSets = 0;
static uint32_t pressed[2]{}, down[2]{};
template <class T> T handle() {
    return reinterpret_cast<T>(++next);
}
static void reset() {
    dependency = meta = true;
    reject = queryFailure = rejectTouch = invalidValues = false;
    syncResult = XR_SUCCESS;
    profileCalls.clear();
    destroyedActions = destroyedSpaces = 0;
    enabled.clear();
    bound.clear();
    paths.clear();
    actions.clear();
    nativeActions.clear();
    next = 1;
    actionsAtAttach = failActionAt = 0;
    pressed[0] = pressed[1] = down[0] = down[1] = 0;
    saida::XRInput::beginFrame();
}
static int gesture(const std::string& name) {
    const char* names[] = {"swipe_left_meta", "swipe_right_meta", "swipe_forward_meta", "swipe_backward_meta",
                           "tap_thumb_meta"};
    for (int i = 0; i < 5; ++i)
        if (name.find(std::string("/input/") + names[i] + "/click") != std::string::npos)
            return i;
    return -1;
}
} // namespace mock

extern "C" {
XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateInstanceExtensionProperties(const char*, uint32_t capacity,
                                                                      uint32_t* count,
                                                                      XrExtensionProperties* out) {
    std::vector<const char*> names = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME,
                                      XR_EXT_HAND_TRACKING_EXTENSION_NAME};
    if (mock::dependency)
        names.push_back(XR_EXT_HAND_INTERACTION_EXTENSION_NAME);
    if (mock::meta)
        names.push_back(XR_META_HAND_TRACKING_MICROGESTURES_EXTENSION_NAME);
    *count = static_cast<uint32_t>(names.size());
    if (capacity)
        for (size_t i = 0; i < names.size(); ++i)
            std::strcpy(out[i].extensionName, names[i]);
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrCreateInstance(const XrInstanceCreateInfo* info, XrInstance* out) {
    for (uint32_t i = 0; i < info->enabledExtensionCount; ++i)
        mock::enabled.insert(info->enabledExtensionNames[i]);
    *out = mock::handle<XrInstance>();
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrDestroyInstance(XrInstance) {
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrGetSystem(XrInstance, const XrSystemGetInfo*, XrSystemId* out) {
    *out = 1;
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrGetInstanceProperties(XrInstance, XrInstanceProperties* out) {
    std::strcpy(out->runtimeName, "XRGesture fake runtime");
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrGetSystemProperties(XrInstance, XrSystemId, XrSystemProperties* out) {
    std::strcpy(out->systemName, "XRGesture test system");
    if (out->next)
        static_cast<XrSystemHandTrackingPropertiesEXT*>(out->next)->supportsHandTracking = XR_TRUE;
    return XR_SUCCESS;
}
static void XRAPI_CALL unusedVulkanProc() {}
XRAPI_ATTR XrResult XRAPI_CALL xrGetInstanceProcAddr(XrInstance, const char*, PFN_xrVoidFunction* out) {
    *out = &unusedVulkanProc;
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrResultToString(XrInstance, XrResult, char out[XR_MAX_RESULT_STRING_SIZE]) {
    std::strcpy(out, "mock error");
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrStringToPath(XrInstance, const char* text, XrPath* out) {
    for (const auto& entry : mock::paths)
        if (entry.second == text) {
            *out = entry.first;
            return XR_SUCCESS;
        }
    *out = ++mock::next;
    mock::paths[*out] = text;
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrCreateActionSet(XrInstance, const XrActionSetCreateInfo*, XrActionSet* out) {
    *out = mock::handle<XrActionSet>();
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrDestroyActionSet(XrActionSet) {
    ++mock::destroyedSets;
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrCreateAction(XrActionSet, const XrActionCreateInfo* info, XrAction* out) {
    if (mock::failActionAt && mock::actions.size() + 1 == mock::failActionAt)
        return XR_ERROR_OUT_OF_MEMORY;
    *out = mock::handle<XrAction>();
    mock::actions[*out] = info->actionName;
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrDestroyAction(XrAction) {
    ++mock::destroyedActions;
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL
xrSuggestInteractionProfileBindings(XrInstance, const XrInteractionProfileSuggestedBinding* info) {
    const auto& profile = mock::paths[info->interactionProfile];
    mock::profileCalls[profile].push_back(info->countSuggestedBindings);
    if (profile == "/interaction_profiles/ext/hand_interaction_ext" && mock::reject)
        return XR_ERROR_PATH_UNSUPPORTED;
    if (profile != "/interaction_profiles/ext/hand_interaction_ext") {
        if (mock::rejectTouch && profile == "/interaction_profiles/oculus/touch_controller")
            for (uint32_t i = 0; i < info->countSuggestedBindings; ++i)
                if (mock::actions[info->suggestedBindings[i].action].rfind("extension_", 0) == 0)
                    return XR_ERROR_PATH_UNSUPPORTED;
        return XR_SUCCESS;
    }
    for (uint32_t i = 0; i < info->countSuggestedBindings; ++i) {
        const auto& binding = info->suggestedBindings[i];
        mock::bound.insert(mock::paths[binding.binding]);
        mock::nativeActions[binding.action] = mock::gesture(mock::paths[binding.binding]);
    }
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrCreateActionSpace(XrSession, const XrActionSpaceCreateInfo*, XrSpace* out) {
    *out = mock::handle<XrSpace>();
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrDestroySpace(XrSpace) {
    ++mock::destroyedSpaces;
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrAttachSessionActionSets(XrSession, const XrSessionActionSetsAttachInfo*) {
    mock::actionsAtAttach = mock::actions.size();
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrSyncActions(XrSession, const XrActionsSyncInfo*) {
    return mock::syncResult;
}
XRAPI_ATTR XrResult XRAPI_CALL xrLocateSpace(XrSpace, XrSpace, XrTime, XrSpaceLocation* out) {
    out->locationFlags = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    out->pose.orientation.w = 1;
    out->pose.position = {1, 2, 3};
    if (mock::invalidValues)
        out->pose.orientation.w = std::numeric_limits<float>::quiet_NaN();
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStatePose(XrSession, const XrActionStateGetInfo*,
                                                    XrActionStatePose* out) {
    out->isActive = XR_TRUE;
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateFloat(XrSession, const XrActionStateGetInfo*,
                                                     XrActionStateFloat* out) {
    out->isActive = XR_TRUE;
    out->currentState = mock::invalidValues ? std::numeric_limits<float>::quiet_NaN() : .375f;
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateVector2f(XrSession, const XrActionStateGetInfo*,
                                                        XrActionStateVector2f* out) {
    out->isActive = XR_TRUE;
    out->currentState = {mock::invalidValues ? std::numeric_limits<float>::quiet_NaN() : .25f, -.75f};
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateBoolean(XrSession, const XrActionStateGetInfo* info,
                                                       XrActionStateBoolean* out) {
    const auto found = mock::nativeActions.find(info->action);
    const int index = found == mock::nativeActions.end() ? -1 : found->second;
    if (index < 0) {
        out->isActive = XR_TRUE;
        out->currentState = XR_FALSE;
        return XR_SUCCESS;
    }
    if (mock::queryFailure)
        return XR_ERROR_RUNTIME_FAILURE;
    const int hand = mock::paths[info->subactionPath] == "/user/hand/right" ? 1 : 0;
    out->isActive = XR_TRUE;
    out->currentState = (mock::down[hand] & (1u << index)) ? XR_TRUE : XR_FALSE;
    out->changedSinceLastSync = (mock::pressed[hand] & (1u << index)) ? XR_TRUE : XR_FALSE;
    return XR_SUCCESS;
}
}

static int checks = 0, failures = 0;
static void check(bool ok, const char* label) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", label);
    }
}

class GenericInput final : public saida::xr::InputExtension {
public:
    saida::xr::InputExtensionDescription description;
    bool available = false, focused = false;
    double time = 0;
    size_t count = 0;
    std::array<saida::xr::InputValue, 4> values{};
    GenericInput() {
        using namespace saida::xr;
        description.name = "generic test input";
        const InputType types[] = {InputType::Boolean, InputType::Float, InputType::Vector2, InputType::Pose};
        const char* components[] = {"a/click", "trigger/value", "thumbstick", "aim/pose"};
        for (size_t i = 0; i < 4; ++i)
            description.actions.push_back(
                {"test " + std::to_string(i),
                 types[i],
                 {{"/interaction_profiles/oculus/touch_controller", "/user/hand/right",
                   std::string("/user/hand/right/input/") + components[i]}}});
    }
    saida::xr::InputExtensionDescription describe() const override { return description; }
    void submit(const saida::xr::InputExtensionFrame& frame) noexcept override {
        available = frame.available;
        focused = frame.focused;
        time = frame.time;
        count = frame.count;
        values = {};
        for (size_t i = 0; i < std::min(frame.count, values.size()); ++i)
            values[i] = frame.values[i];
        // Copies of string_view are used only during this live session in tests.
    }
};

static void genericContracts() {
    using namespace saida;
    mock::reset();
    auto source = std::make_shared<GenericInput>();
    {
        xr::Instance instance({source});
        xr::Actions actions(instance, mock::handle<XrSession>());
        const auto session = mock::handle<XrSession>();
        const auto space = mock::handle<XrSpace>();
        const size_t beforeSync = allocations;
        actions.sync(session, space, 2000000000);
        check(allocations == beforeSync, "typed input sampling performs no heap allocations");
        check(mock::actionsAtAttach == 11, "generic actions are attached together with controller actions");
        const auto& calls = mock::profileCalls["/interaction_profiles/oculus/touch_controller"];
        check(calls.size() == 1 && calls[0] == 18,
              "base and extension bindings sharing a profile are merged once");
        check(source->available && source->focused && source->count == 4 && source->time == 2,
              "generic provider receives borrowed typed snapshot and XR timestamp");
        check(source->values[0].active && !source->values[0].boolean, "generic boolean channel");
        check(source->values[1].active && source->values[1].scalar == .375f, "generic scalar channel");
        check(source->values[2].active && source->values[2].vector == glm::vec2(.25f, -.75f),
              "generic vector channel");
        check(source->values[3].active && source->values[3].pose.tracked &&
                  source->values[3].pose.position == glm::vec3(1, 2, 3),
              "generic pose channel owns an action space");
        mock::invalidValues = true;
        actions.sync(session, space, 2010000000);
        check(!source->values[1].active && !source->values[2].active && !source->values[3].pose.tracked,
              "nonfinite runtime values cannot escape the bridge");
        actions.deactivate(2020000000);
        check(!source->focused && !source->values[0].active, "focus suspension clears all typed values");
    }
    check(!source->available && !source->focused && source->count == 0,
          "session teardown resets the provider before destroying native resources");
    check(mock::destroyedActions == 4 && mock::destroyedSpaces == 5,
          "generic actions and pose spaces are each released once");
    mock::reset();
    mock::rejectTouch = true;
    {
        xr::Instance instance({source});
        xr::Actions actions(instance, mock::handle<XrSession>());
        actions.sync(mock::handle<XrSession>(), mock::handle<XrSpace>(), 1);
        const auto& calls = mock::profileCalls["/interaction_profiles/oculus/touch_controller"];
        check(calls.size() == 2 && calls[0] == 18 && calls[1] == 14,
              "rejected extension retries the original controller bindings");
        check(!source->available && XRInput::hand(XRHand::Right).active,
              "rejected extension does not break controller input");
    }
    auto invalid = [&](xr::InputExtensionList providers) {
        try {
            xr::InputExtensions registry(std::move(providers));
            return false;
        } catch (const std::invalid_argument&) {
            return true;
        }
    };
    check(invalid({nullptr}), "null input provider rejected before native initialization");
    check(invalid({source, source}), "duplicate input provider rejected");
    source->description.actions[0].bindings[0].componentPath = "/user/hand/left/input/a/click";
    check(invalid({source}), "binding user and component must match");
    source = std::make_shared<GenericInput>();
    source->description.actions[1].name = source->description.actions[0].name;
    check(invalid({source}), "duplicate action names rejected");
    source = std::make_shared<GenericInput>();
    source->description.actions[0].type = static_cast<xr::InputType>(999);
    check(invalid({source}), "invalid input type rejected");
    mock::reset();
    {
        xr::Instance instance;
        xr::Actions actions(instance, mock::handle<XrSession>());
        check(mock::actionsAtAttach == 7 &&
                  !mock::enabled.count(XR_META_HAND_TRACKING_MICROGESTURES_EXTENSION_NAME),
              "engine without providers preserves its original seven actions and extension set");
    }
}

int main() {
    using namespace saida;
    using namespace saida::xrgesture;
    auto source = std::make_shared<MetaMicrogestureInput>();
    mock::reset();
    mock::dependency = false;
    {
        xr::Instance instance({source});
        check(!mock::enabled.count(XR_META_HAND_TRACKING_MICROGESTURES_EXTENSION_NAME),
              "META cannot be enabled without EXT dependency");
        check(!mock::enabled.count(XR_META_HAND_TRACKING_MICROGESTURES_EXTENSION_NAME),
              "missing dependency omitted from create info");
    }
    mock::reset();
    mock::meta = false;
    {
        xr::Instance instance({source});
        check(!mock::enabled.count(XR_META_HAND_TRACKING_MICROGESTURES_EXTENSION_NAME),
              "ordinary runtime remains supported");
    }
    mock::reset();
    {
        xr::Instance instance({source});
        xr::Actions actions(instance, mock::handle<XrSession>());
        check(mock::enabled.count(XR_META_HAND_TRACKING_MICROGESTURES_EXTENSION_NAME),
              "both advertised extensions enable native gestures");
        check(mock::enabled.count(XR_EXT_HAND_INTERACTION_EXTENSION_NAME) &&
                  mock::enabled.count(XR_META_HAND_TRACKING_MICROGESTURES_EXTENSION_NAME),
              "both dependencies reach xrCreateInstance");
        check(mock::actionsAtAttach == 12, "all five gesture actions exist before the single attach");
        check(mock::bound.size() == 10, "five bindings per hand");
        for (const char* hand : {"left", "right"})
            for (const char* action : {"swipe_left_meta", "swipe_right_meta", "swipe_forward_meta",
                                       "swipe_backward_meta", "tap_thumb_meta"})
                check(mock::bound.count(std::string("/user/hand/") + hand + "/input/" + action + "/click"),
                      "exact Khronos component path");
        const auto session = mock::handle<XrSession>();
        const auto space = mock::handle<XrSpace>();
        mock::down[1] = mock::pressed[1] = xrMicrogestureBit(XRMicrogesture::SwipeForward);
        XRInput::beginFrame();
        actions.sync(session, space, 1);
        const auto first = source->state().hands[1];
        check(first.supported && first.active, "native capability and per-hand activity exposed");
        check(first.pressedMask == mock::pressed[1], "boolean rising edge reaches input service");
        check(source->state().hands[0].pressedMask == 0, "no cross-hand event leakage");
        mock::pressed[1] = 0;
        XRInput::beginFrame();
        actions.sync(session, space, 2);
        check(source->state().hands[1].pressedMask == 0, "held action is not another completion");
        mock::syncResult = XR_SESSION_NOT_FOCUSED;
        XRInput::beginFrame();
        check(!actions.sync(session, space, 3), "nonfocused sync rejects skeleton fallback for the session");
        check(!source->state().hands[1].active, "positive nonfocused result cannot publish stale gestures");
        mock::syncResult = XR_SUCCESS;
        mock::queryFailure = true;
        XRInput::beginFrame();
        actions.sync(session, space, 4);
        check(!source->state().hands[1].active, "failed action query invalidates native sample");
    }
    mock::reset();
    mock::reject = true;
    {
        xr::Instance instance({source});
        xr::Actions actions(instance, mock::handle<XrSession>());
        actions.sync(mock::handle<XrSession>(), mock::handle<XrSpace>(), 1);
        check(!source->state().hands[1].supported, "rejected profile is an explicit unavailable capability");
        check(XRInput::hand(XRHand::Right).active,
              "rejected microgesture bindings do not disable controllers");
    }
    check(mock::destroyedSets == 2, "owned action sets destroyed without a runtime leak");
    mock::reset();
    mock::failActionAt = 10;
    {
        xr::Instance instance({source});
        bool failed = false;
        try {
            xr::Actions actions(instance, mock::handle<XrSession>());
        } catch (const std::exception&) {
            failed = true;
        }
        check(failed && mock::destroyedSets == 3 && mock::destroyedActions == 2,
              "partial native action initialization releases actions and their owning set");
    }
    genericContracts();
    std::printf("%s XRGesture OpenXR bridge: %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks,
                failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
