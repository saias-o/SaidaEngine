#pragma once

#include "Types.hpp"
#include "xr/XrInputExtension.hpp"

namespace saida::xrgesture {

// All vendor names, dependency choices, action semantics and hand mapping live
// in the optional plugin. The engine sees only a generic input description.
class MetaMicrogestureInput final : public xr::InputExtension {
public:
    xr::InputExtensionDescription describe() const override {
        xr::InputExtensionDescription description;
        description.name = "XRGesture native microgestures";
        description.requiredExtensions = {"XR_EXT_hand_interaction", "XR_META_hand_tracking_microgestures"};
        for (const char* name : componentNames_) {
            xr::InputAction action;
            action.name = name;
            for (const char* hand : {"/user/hand/left", "/user/hand/right"})
                action.bindings.push_back({"/interaction_profiles/ext/hand_interaction_ext", hand,
                                           std::string(hand) + "/input/" + name + "/click"});
            description.actions.push_back(std::move(action));
        }
        return description;
    }

    void submit(const xr::InputExtensionFrame& frame) noexcept override {
        RuntimeInputState next;
        next.focused = frame.focused;
        for (auto& hand : next.hands)
            hand.supported = frame.available;
        uint32_t active[2]{};
        for (const auto& value : frame) {
            if (value.action >= componentNames_.size() || !value.active || !frame.available || !frame.focused)
                continue;
            const int hand = value.userPath == "/user/hand/left"    ? 0
                             : value.userPath == "/user/hand/right" ? 1
                                                                    : -1;
            if (hand < 0)
                continue;
            const uint32_t bit = uint32_t{1} << value.action;
            active[hand] |= bit;
            if (value.boolean) {
                next.hands[hand].downMask |= bit;
                if (value.changed)
                    next.hands[hand].pressedMask |= bit;
            }
        }
        for (size_t i = 0; i < next.hands.size(); ++i) {
            // Each action is independently usable. An inactive channel must not
            // discard completions already validated on another channel.
            next.hands[i].active = active[i] != 0;
            if (!next.hands[i].active)
                next.hands[i].downMask = next.hands[i].pressedMask = 0;
        }
        state_ = next;
    }

    const RuntimeInputState& state() const { return state_; }

private:
    inline static constexpr std::array<const char*, static_cast<size_t>(XRMicrogesture::Count)>
        componentNames_{"swipe_left_meta", "swipe_right_meta", "swipe_forward_meta", "swipe_backward_meta",
                        "tap_thumb_meta"};
    RuntimeInputState state_;
};

} // namespace saida::xrgesture
