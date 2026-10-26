#pragma once

#include "Types.hpp"
#include <algorithm>
#include <stdexcept>
#include <vector>

namespace saida::xrgesture {

// Compose poses, custom recognizers and microgestures into temporal gestures.
// Started and Performed count as tokens; level updates never count as repeats.
class SequenceRecognizer {
public:
    static constexpr size_t kMaximumSteps = 16;
    static constexpr size_t kMaximumEvents = 256;

    SequenceRecognizer(GestureId id, std::vector<GestureId> steps, double maximumGapSeconds = .6,
                       double maximumDurationSeconds = 2)
        : id_(id), steps_(std::move(steps)), gap_(maximumGapSeconds), duration_(maximumDurationSeconds) {
        if (id < gesture::FirstCustom || steps_.empty() || steps_.size() > kMaximumSteps ||
            std::any_of(steps_.begin(), steps_.end(), [id](GestureId step) { return !step || step == id; }) ||
            !std::isfinite(gap_) || gap_ <= 0 || !std::isfinite(duration_) || duration_ < gap_)
            throw std::invalid_argument("XRGesture: invalid gesture sequence");
    }

    void reset() {
        states_ = {};
        lastTime_ = -1;
        count_ = 0;
    }

    EventRange update(const Frame& frame, EventRange input) {
        count_ = 0;
        if (!frame.focused || !std::isfinite(frame.time) || frame.time < 0 || frame.time < lastTime_) {
            reset();
            return {};
        }
        if (frame.time == lastTime_)
            return {};
        lastTime_ = frame.time;
        for (XRHand hand : {XRHand::Left, XRHand::Right}) {
            auto& state = states_[xrHandIndex(hand)];
            const auto& sample = frame.hands[xrHandIndex(hand)];
            if ((!sample.skeleton.active && !sample.native.active) ||
                (state.next && (frame.time - state.last > gap_ || frame.time - state.start > duration_)))
                state = {};
        }
        for (const auto& event : input) {
            if (event.time != frame.time || !validHand(event.hand))
                continue;
            const auto& sample = frame.hands[xrHandIndex(event.hand)];
            if (!sample.skeleton.active && !sample.native.active)
                continue;
            auto& state = states_[xrHandIndex(event.hand)];
            if (event.phase == Phase::Cancelled) {
                state = {};
                continue;
            }
            if (event.phase != Phase::Started && event.phase != Phase::Performed)
                continue;
            if (std::find(steps_.begin(), steps_.end(), event.gesture) == steps_.end())
                continue;
            if (event.gesture != steps_[state.next]) {
                state = {};
                if (event.gesture != steps_[0])
                    continue;
            }
            if (state.next == 0)
                state.start = frame.time;
            state.last = frame.time;
            if (++state.next != steps_.size())
                continue;
            if (count_ == kMaximumEvents)
                throw std::length_error("XRGesture: sequence event capacity reached");
            events_[count_++] = {id_, event.hand, Phase::Performed, Source::Custom, frame.time, 1};
            state = {};
        }
        return {events_.data(), count_};
    }

private:
    struct State {
        size_t next = 0;
        double start = 0, last = 0;
    };
    GestureId id_;
    std::vector<GestureId> steps_;
    double gap_, duration_, lastTime_ = -1;
    std::array<State, kXRHandCount> states_{};
    std::array<Event, kMaximumEvents> events_{};
    size_t count_ = 0;
};

} // namespace saida::xrgesture
