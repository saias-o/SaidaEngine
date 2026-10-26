#pragma once

#include "Microgestures.hpp"
#include <vector>

namespace saida::xrgesture {

class GestureSystem {
public:
    static constexpr size_t kMaximumRules = 64;
    static constexpr size_t kMaximumEvents = 256;

    explicit GestureSystem(MicrogestureConfig micro = {}, double maximumSampleGap = .25)
        : micro_(micro), maximumGap_(maximumSampleGap) {
        if (!std::isfinite(maximumGap_) || maximumGap_ <= 0)
            throw std::invalid_argument("XRGesture: maximum sample gap must be positive");
        rules_.reserve(kMaximumRules);
        for (GestureId id : {gesture::Pinch, gesture::Grab, gesture::OpenPalm, gesture::Point}) {
            Rule rule;
            rule.id = id;
            rule.source = Source::Skeleton;
            rule.evaluate = [id](const Frame& frame, XRHand hand) {
                return poseObservation(frame, hand, id);
            };
            addRule(std::move(rule), true);
        }
    }

    void add(Rule rule) { addRule(std::move(rule), false); }

    bool remove(GestureId id) {
        if (updating_)
            throw std::logic_error("XRGesture: cannot mutate rules during evaluation");
        for (auto it = rules_.begin(); it != rules_.end(); ++it) {
            if (it->rule.id != id)
                continue;
            // The caller must cancel outstanding interactions before removing a
            // live rule; silently abandoning Started would break its lifecycle.
            for (const auto& state : it->states)
                if (state.active && it->rule.recognition == Recognition::Continuous)
                    throw std::logic_error("XRGesture: reset before removing an active rule");
            rules_.erase(it);
            return true;
        }
        return false;
    }

    EventRange reset(double time) {
        if (updating_)
            throw std::logic_error("XRGesture: recursive reset");
        count_ = 0;
        cancel(time);
        lastTime_ = -1;
        return events();
    }

    EventRange update(const Frame& frame) {
        if (updating_)
            throw std::logic_error("XRGesture: recursive update");
        struct Guard {
            bool& flag;
            ~Guard() { flag = false; }
        } guard{updating_};
        updating_ = true;
        count_ = 0;
        if (!std::isfinite(frame.time) || frame.time < 0) {
            cancel(lastTime_);
            lastTime_ = -1;
            return events();
        }
        if (!frame.focused) {
            cancel(frame.time);
            lastTime_ = frame.time;
            return events();
        }
        if (frame.time == lastTime_)
            return events();
        const bool reversed = lastTime_ >= 0 && frame.time < lastTime_;
        const bool discontinuity = lastTime_ >= 0 && (reversed || frame.time - lastTime_ > maximumGap_);
        lastTime_ = frame.time;
        if (discontinuity) {
            cancel(frame.time);
            if (reversed)
                return events();
        }
        for (XRHand hand : {XRHand::Left, XRHand::Right}) {
            micro_.update(frame, hand, [this](const Event& event) { push(event); });
            // Fresh runtime completions are valid after a stall; temporal rules
            // restart on the next sample rather than cancel/restart in one batch.
            if (discontinuity)
                continue;
            for (auto& entry : rules_)
                updateRule(entry, frame, hand);
        }
        return events();
    }

    EventRange events() const { return {events_.data(), count_}; }

private:
    struct State {
        bool active = false, armed = false;
        double since = -1, lastEvent = -std::numeric_limits<double>::infinity();
    };
    struct Entry {
        Rule rule;
        std::array<State, kXRHandCount> states{};
    };

    void addRule(Rule rule, bool builtin) {
        if (updating_)
            throw std::logic_error("XRGesture: cannot mutate rules during evaluation");
        if ((!builtin && rule.id < gesture::FirstCustom) || !rule.evaluate || !std::isfinite(rule.enter) ||
            !std::isfinite(rule.leave) || rule.leave < 0 || rule.enter > 1 || rule.enter <= rule.leave ||
            !std::isfinite(rule.holdSeconds) || rule.holdSeconds < 0 ||
            !std::isfinite(rule.cooldownSeconds) || rule.cooldownSeconds < 0)
            throw std::invalid_argument("XRGesture: invalid rule");
        if (rules_.size() == kMaximumRules)
            throw std::length_error("XRGesture: recognizer capacity reached");
        for (const auto& entry : rules_)
            if (entry.rule.id == rule.id)
                throw std::invalid_argument("XRGesture: duplicate gesture id");
        rules_.push_back({std::move(rule), {}});
    }

    void push(Event event) {
        if (count_ == kMaximumEvents)
            throw std::length_error("XRGesture: event capacity reached");
        events_[count_++] = event;
    }

    void cancel(double time) {
        for (auto& entry : rules_) {
            for (XRHand hand : {XRHand::Left, XRHand::Right}) {
                auto& state = entry.states[xrHandIndex(hand)];
                if (state.active && entry.rule.recognition == Recognition::Continuous)
                    push({entry.rule.id, hand, Phase::Cancelled, entry.rule.source, time, 0});
                state = {};
                if (entry.rule.onReset)
                    entry.rule.onReset(hand);
            }
        }
        micro_.reset();
    }

    void updateRule(Entry& entry, const Frame& frame, XRHand hand) {
        const auto& rule = entry.rule;
        auto& state = entry.states[xrHandIndex(hand)];
        const auto observation = rule.evaluate(frame, hand);
        const auto emit = [&](Phase phase) {
            const float strength =
                std::isfinite(observation.strength) ? std::clamp(observation.strength, 0.f, 1.f) : 0.f;
            push({rule.id, hand, phase, rule.source, frame.time, strength});
        };
        if (!observation.valid || !std::isfinite(observation.strength)) {
            if (state.active && rule.recognition == Recognition::Continuous)
                emit(Phase::Cancelled);
            state = {};
            if (rule.onReset)
                rule.onReset(hand);
            return;
        }
        const float value = std::clamp(observation.strength, 0.f, 1.f);
        if (value <= rule.leave) {
            if (state.active && rule.recognition == Recognition::Continuous)
                emit(Phase::Ended);
            state.active = false;
            state.armed = true;
            state.since = -1;
            return;
        }
        if (state.active) {
            if (rule.recognition == Recognition::Continuous)
                emit(Phase::Updated);
            return;
        }
        if (value < rule.enter || (rule.recognition == Recognition::Discrete && !state.armed)) {
            state.since = -1;
            return;
        }
        if (state.since < 0)
            state.since = frame.time;
        if (frame.time - state.since < rule.holdSeconds ||
            frame.time - state.lastEvent < rule.cooldownSeconds)
            return;
        state.active = true;
        state.armed = false;
        state.lastEvent = frame.time;
        emit(rule.recognition == Recognition::Continuous ? Phase::Started : Phase::Performed);
    }

    Microgestures micro_;
    double maximumGap_, lastTime_ = -1;
    bool updating_ = false;
    std::vector<Entry> rules_;
    std::array<Event, kMaximumEvents> events_{};
    size_t count_ = 0;
};

} // namespace saida::xrgesture
