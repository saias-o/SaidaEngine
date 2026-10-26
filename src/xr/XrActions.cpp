#include "xr/XrPlatform.hpp" // must precede the XR headers (include order)
#include "xr/XrActions.hpp"
#include "xr/XrInstance.hpp"
#include "xr/XrInputExtensions.hpp"
#include "xr/XrMath.hpp"

#include "xr/toolkit/XRInput.hpp"
#include "xr/toolkit/XRTypes.hpp"
#include "core/Log.hpp"

#include <cstring>
#include <string>

namespace saida::xr {

XrPath Actions::path(const char* str) const {
    XrPath p = XR_NULL_PATH;
    check(xrStringToPath(instance_, str, &p), "xrStringToPath");
    return p;
}

XrAction Actions::makeAction(const char* name, XrActionType type) {
    XrActionCreateInfo ci{};
    ci.type = XR_TYPE_ACTION_CREATE_INFO;
    std::strncpy(ci.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(ci.localizedActionName, name, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    ci.actionType = type;
    ci.countSubactionPaths = 2; // every action is per-hand (left/right)
    ci.subactionPaths = handPaths_;
    XrAction a = XR_NULL_HANDLE;
    check(xrCreateAction(actionSet_, &ci, &a), "xrCreateAction");
    return a;
}

void Actions::suggest(const char* profile, const std::vector<XrActionSuggestedBinding>& bindings) {
    auto& target = bindings_[profile].base;
    target.insert(target.end(), bindings.begin(), bindings.end());
}

void Actions::bindProfiles() {
    for (auto& item : bindings_) {
        auto merged = item.second.base;
        merged.insert(merged.end(), item.second.extensions.begin(), item.second.extensions.end());
        XrInteractionProfileSuggestedBinding info{};
        info.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING;
        info.interactionProfile = path(item.first.c_str());
        info.countSuggestedBindings = static_cast<uint32_t>(merged.size());
        info.suggestedBindings = merged.data();
        const bool accepted = XR_SUCCEEDED(xrSuggestInteractionProfileBindings(instance_, &info));
        inputExtensions_->profileResult(info.interactionProfile, accepted);
        if (accepted)
            continue;
        Log::warn("XR: suggested bindings rejected for ", item.first);
        if (!item.second.extensions.empty() && !item.second.base.empty()) {
            info.countSuggestedBindings = static_cast<uint32_t>(item.second.base.size());
            info.suggestedBindings = item.second.base.data();
            if (XR_FAILED(xrSuggestInteractionProfileBindings(instance_, &info)))
                Log::warn("XR: base bindings also rejected for ", item.first);
        }
    }
    bindings_.clear();
}

Actions::Actions(Instance& instance, XrSession session)
    : instance_(instance.handle()), inputExtensions_(&instance.inputExtensions()) {
    try {
        initialize(session);
    } catch (...) {
        release();
        throw;
    }
}

void Actions::initialize(XrSession session) {
    XrActionSetCreateInfo asci{};
    asci.type = XR_TYPE_ACTION_SET_CREATE_INFO;
    std::strncpy(asci.actionSetName, "nexrtk", XR_MAX_ACTION_SET_NAME_SIZE - 1);
    std::strncpy(asci.localizedActionSetName, "SaidaXRTK", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
    check(xrCreateActionSet(instance_, &asci, &actionSet_), "xrCreateActionSet");

    handPaths_[0] = path("/user/hand/left");
    handPaths_[1] = path("/user/hand/right");

    gripPose_ = makeAction("grip_pose", XR_ACTION_TYPE_POSE_INPUT);
    aimPose_ = makeAction("aim_pose", XR_ACTION_TYPE_POSE_INPUT);
    squeeze_ = makeAction("squeeze", XR_ACTION_TYPE_FLOAT_INPUT);
    trigger_ = makeAction("trigger", XR_ACTION_TYPE_FLOAT_INPUT);
    thumbstick_ = makeAction("thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT);
    primary_ = makeAction("primary", XR_ACTION_TYPE_BOOLEAN_INPUT);
    secondary_ = makeAction("secondary", XR_ACTION_TYPE_BOOLEAN_INPUT);

    auto bind = [&](XrAction a, const char* p) { return XrActionSuggestedBinding{a, path(p)}; };

    // Oculus Touch (Quest / Rift — the PCVR target).
    suggest("/interaction_profiles/oculus/touch_controller",
            {
                bind(gripPose_, "/user/hand/left/input/grip/pose"),
                bind(gripPose_, "/user/hand/right/input/grip/pose"),
                bind(aimPose_, "/user/hand/left/input/aim/pose"),
                bind(aimPose_, "/user/hand/right/input/aim/pose"),
                bind(squeeze_, "/user/hand/left/input/squeeze/value"),
                bind(squeeze_, "/user/hand/right/input/squeeze/value"),
                bind(trigger_, "/user/hand/left/input/trigger/value"),
                bind(trigger_, "/user/hand/right/input/trigger/value"),
                bind(thumbstick_, "/user/hand/left/input/thumbstick"),
                bind(thumbstick_, "/user/hand/right/input/thumbstick"),
                bind(primary_, "/user/hand/left/input/x/click"),
                bind(primary_, "/user/hand/right/input/a/click"),
                bind(secondary_, "/user/hand/left/input/y/click"),
                bind(secondary_, "/user/hand/right/input/b/click"),
            });

    // Khronos simple controller (universal fallback: pose + select only).
    suggest("/interaction_profiles/khr/simple_controller",
            {
                bind(gripPose_, "/user/hand/left/input/grip/pose"),
                bind(gripPose_, "/user/hand/right/input/grip/pose"),
                bind(aimPose_, "/user/hand/left/input/aim/pose"),
                bind(aimPose_, "/user/hand/right/input/aim/pose"),
                bind(trigger_, "/user/hand/left/input/select/click"),
                bind(trigger_, "/user/hand/right/input/select/click"),
                bind(squeeze_, "/user/hand/left/input/select/click"),
                bind(squeeze_, "/user/hand/right/input/select/click"),
            });

    inputExtensions_->createActions(instance_, actionSet_, session,
                                    [this](const std::string& profile, XrActionSuggestedBinding binding) {
                                        bindings_[profile].extensions.push_back(binding);
                                    });
    ownsInputSession_ = true;
    bindProfiles();

    for (int i = 0; i < 2; ++i) {
        XrActionSpaceCreateInfo si{};
        si.type = XR_TYPE_ACTION_SPACE_CREATE_INFO;
        si.subactionPath = handPaths_[i];
        si.poseInActionSpace.orientation.w = 1.0f; // identity offset
        si.action = gripPose_;
        check(xrCreateActionSpace(session, &si, &gripSpace_[i]), "xrCreateActionSpace(grip)");
        si.action = aimPose_;
        check(xrCreateActionSpace(session, &si, &aimSpace_[i]), "xrCreateActionSpace(aim)");
    }

    XrSessionActionSetsAttachInfo attach{};
    attach.type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO;
    attach.countActionSets = 1;
    attach.actionSets = &actionSet_;
    check(xrAttachSessionActionSets(session, &attach), "xrAttachSessionActionSets");
}

Actions::~Actions() {
    release();
}

void Actions::release() noexcept {
    if (ownsInputSession_)
        inputExtensions_->releaseSession();
    ownsInputSession_ = false;
    for (int i = 0; i < 2; ++i) {
        if (gripSpace_[i])
            xrDestroySpace(gripSpace_[i]);
        gripSpace_[i] = XR_NULL_HANDLE;
        if (aimSpace_[i])
            xrDestroySpace(aimSpace_[i]);
        aimSpace_[i] = XR_NULL_HANDLE;
    }
    if (actionSet_)
        xrDestroyActionSet(actionSet_);
    actionSet_ = XR_NULL_HANDLE;
}

void Actions::deactivate(XrTime time) {
    inputExtensions_->sync(XR_NULL_HANDLE, XR_NULL_HANDLE, time, false);
}

bool Actions::sync(XrSession session, XrSpace space, XrTime time) {
    XrActiveActionSet active{actionSet_, XR_NULL_PATH};
    XrActionsSyncInfo si{};
    si.type = XR_TYPE_ACTIONS_SYNC_INFO;
    si.countActiveActionSets = 1;
    si.activeActionSets = &active;
    // XR_SESSION_NOT_FOCUSED is a positive result, not XR_FAILED.
    if (xrSyncActions(session, &si) != XR_SUCCESS) {
        deactivate(time);
        return false;
    }

    for (int i = 0; i < 2; ++i) {
        const XrPath sub = handPaths_[i];
        const saida::XRHand hand = i == 0 ? saida::XRHand::Left : saida::XRHand::Right;
        saida::XRHandState st;

        auto locate = [&](XrSpace as, saida::XRPose& out) {
            XrSpaceLocation loc{};
            loc.type = XR_TYPE_SPACE_LOCATION;
            if (XR_FAILED(xrLocateSpace(as, space, time, &loc)))
                return;
            const bool pos = loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT;
            const bool ori = loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
            out.tracked = pos && ori;
            if (pos)
                out.position = toGlm(loc.pose.position);
            if (ori)
                out.orientation = toGlm(loc.pose.orientation);
        };
        locate(gripSpace_[i], st.grip);
        locate(aimSpace_[i], st.aim);

        XrActionStateGetInfo gi{};
        gi.type = XR_TYPE_ACTION_STATE_GET_INFO;
        gi.subactionPath = sub;
        gi.action = gripPose_;
        XrActionStatePose poseState{};
        poseState.type = XR_TYPE_ACTION_STATE_POSE;
        xrGetActionStatePose(session, &gi, &poseState);
        st.active = poseState.isActive;

        auto getFloat = [&](XrAction a) -> float {
            XrActionStateGetInfo g{};
            g.type = XR_TYPE_ACTION_STATE_GET_INFO;
            g.action = a;
            g.subactionPath = sub;
            XrActionStateFloat s{};
            s.type = XR_TYPE_ACTION_STATE_FLOAT;
            if (XR_SUCCEEDED(xrGetActionStateFloat(session, &g, &s)) && s.isActive)
                return s.currentState;
            return 0.0f;
        };
        st.squeeze = getFloat(squeeze_);
        st.trigger = getFloat(trigger_);

        {
            XrActionStateGetInfo g{};
            g.type = XR_TYPE_ACTION_STATE_GET_INFO;
            g.action = thumbstick_;
            g.subactionPath = sub;
            XrActionStateVector2f s{};
            s.type = XR_TYPE_ACTION_STATE_VECTOR2F;
            if (XR_SUCCEEDED(xrGetActionStateVector2f(session, &g, &s)) && s.isActive)
                st.thumbstick = {s.currentState.x, s.currentState.y};
        }

        auto getBool = [&](XrAction a) -> bool {
            XrActionStateGetInfo g{};
            g.type = XR_TYPE_ACTION_STATE_GET_INFO;
            g.action = a;
            g.subactionPath = sub;
            XrActionStateBoolean s{};
            s.type = XR_TYPE_ACTION_STATE_BOOLEAN;
            if (XR_SUCCEEDED(xrGetActionStateBoolean(session, &g, &s)) && s.isActive)
                return s.currentState == XR_TRUE;
            return false;
        };
        st.primaryButton = getBool(primary_);
        st.secondaryButton = getBool(secondary_);

        if (st.active)
            saida::XRInput::submitHand(hand, st);
        else
            saida::XRInput::clearHand(hand);
    }
    inputExtensions_->sync(session, space, time, true);
    return true;
}

} // namespace saida::xr
