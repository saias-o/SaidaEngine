#pragma once

#include "Features.hpp"
#include <limits>
#include <stdexcept>

namespace saida::xrgesture {

struct MicrogestureConfig {
    MicrogestureMode mode = MicrogestureMode::NativePreferred;
    double neutralSeconds = .08;
    double minimumContactSeconds = .035;
    double maximumContactSeconds = .85;
    double cooldownSeconds = .12;
    float contactMargin = .008f;
    float releaseMargin = .016f;
    float swipeDistance = .008f;
    float tapTravel = .006f;
    float directionalDominance = 1.35f;
};

class Microgestures {
public:
    explicit Microgestures(MicrogestureConfig config = {}) : config_(config) {
        const bool timing = std::isfinite(config.neutralSeconds) && config.neutralSeconds >= 0 &&
                            std::isfinite(config.minimumContactSeconds) &&
                            config.minimumContactSeconds >= 0 &&
                            std::isfinite(config.maximumContactSeconds) &&
                            config.maximumContactSeconds > config.minimumContactSeconds &&
                            std::isfinite(config.cooldownSeconds) && config.cooldownSeconds >= 0;
        const bool distances = std::isfinite(config.contactMargin) && config.contactMargin >= 0 &&
                               std::isfinite(config.releaseMargin) &&
                               config.releaseMargin > config.contactMargin &&
                               std::isfinite(config.swipeDistance) && config.swipeDistance > 0 &&
                               std::isfinite(config.tapTravel) && config.tapTravel >= 0 &&
                               config.tapTravel < config.swipeDistance &&
                               std::isfinite(config.directionalDominance) && config.directionalDominance > 1;
        if (!timing || !distances)
            throw std::invalid_argument("XRGesture: invalid microgesture configuration");
    }

    void reset() { states_ = {}; }

    template <class Emit> void update(const Frame& frame, XRHand hand, Emit&& emit) {
        if (!validHand(hand))
            throw std::invalid_argument("XRGesture: invalid hand");
        auto& state = states_[xrHandIndex(hand)];
        const auto& sample = frame.hands[xrHandIndex(hand)];
        if (!frame.focused) {
            state = {};
            return;
        }
        const bool native =
            config_.mode != MicrogestureMode::SkeletonOnly && sample.native.supported && sample.native.active;
        if (native) {
            if (!state.native)
                state = {};
            state.native = true;
            if (sample.native.downMask == 0)
                state.nativeArmed = true;
            if (state.nativeArmed) {
                const uint32_t pressed =
                    sample.native.pressedMask & sample.native.downMask & ~state.previousNative;
                for (uint32_t i = 0; i < static_cast<uint32_t>(XRMicrogesture::Count); ++i)
                    if (pressed & (uint32_t{1} << i))
                        publish(state, frame, hand, i + 1, Source::NativeMeta, emit);
            }
            state.previousNative = sample.native.downMask;
            return;
        }
        if (state.native)
            state = {};
        if (config_.mode == MicrogestureMode::NativeOnly) {
            state = {};
            return;
        }
        updateSkeleton(state, frame, hand, emit);
    }

private:
    struct State {
        bool native = false, nativeArmed = false, armed = false, touching = false;
        uint32_t previousNative = 0;
        double neutralSince = -1, contactSince = 0;
        glm::vec2 start{0}, last{0};
        float maximumTravel = 0;
        std::array<double, 5> lastEvent{
            -std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
            -std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
            -std::numeric_limits<double>::infinity()};
    };

    template <class Emit>
    void publish(State& state, const Frame& frame, XRHand hand, GestureId id, Source source, Emit& emit) {
        if (frame.time - state.lastEvent[id - 1] < config_.cooldownSeconds)
            return;
        state.lastEvent[id - 1] = frame.time;
        emit(Event{id, hand, Phase::Performed, source, frame.time, 1});
    }

    template <class Emit> void updateSkeleton(State& state, const Frame& frame, XRHand hand, Emit& emit) {
        const auto contact = thumbContact(frame.hands[xrHandIndex(hand)].skeleton, hand);
        if (!contact || !contact->posture) {
            state = {};
            return;
        }
        const float enter = config_.contactMargin * contact->scale;
        const float leave = config_.releaseMargin * contact->scale;
        if (!state.touching) {
            if (contact->gap > leave) {
                if (state.neutralSince < 0)
                    state.neutralSince = frame.time;
                if (frame.time - state.neutralSince >= config_.neutralSeconds)
                    state.armed = true;
            } else if (state.armed && contact->gap <= enter) {
                state.touching = true;
                state.armed = false;
                state.contactSince = frame.time;
                state.start = state.last = contact->coordinates;
                state.maximumTravel = 0;
                state.neutralSince = -1;
            } else if (!state.armed)
                state.neutralSince = -1;
            return;
        }
        const double duration = frame.time - state.contactSince;
        if (duration > config_.maximumContactSeconds) {
            state = {};
            return;
        }
        if (contact->gap <= leave) {
            state.last = contact->coordinates;
            state.maximumTravel = std::max(state.maximumTravel, glm::length(state.last - state.start));
            return;
        }
        state.touching = false;
        state.neutralSince = frame.time;
        if (duration < config_.minimumContactSeconds)
            return;
        const glm::vec2 delta = state.last - state.start;
        const glm::vec2 travel = glm::abs(delta);
        const float minimum = config_.swipeDistance * contact->scale;
        GestureId id = 0;
        if (travel.x >= minimum && travel.x >= travel.y * config_.directionalDominance) {
            const bool left = (delta.x > 0) == (hand == XRHand::Right);
            id = left ? gesture::SwipeLeft : gesture::SwipeRight;
        } else if (travel.y >= minimum && travel.y >= travel.x * config_.directionalDominance) {
            id = delta.y > 0 ? gesture::SwipeForward : gesture::SwipeBackward;
        } else if (state.maximumTravel <= config_.tapTravel * contact->scale)
            id = gesture::ThumbTap;
        if (id)
            publish(state, frame, hand, id, Source::Skeleton, emit);
    }

    MicrogestureConfig config_;
    std::array<State, kXRHandCount> states_{};
};

} // namespace saida::xrgesture
