#pragma once

#include "XRGesture.hpp"

namespace saida::xrgesture {

enum class LateralAction { Step, SnapTurn };
struct LocomotionConfig {
    LateralAction lateral = LateralAction::SnapTurn;
    float stepMetres = .5f;
    float snapTurnDegrees = 30;
    float maximumWristTiltDegrees = 45;
    float extendedIndexCurl = .12f;
    double idleTimeoutSeconds = 8;
    double maximumSampleGapSeconds = .25;
};

struct LocomotionIntent {
    glm::vec3 step{0};     // world-space, horizontal; host resolves collisions
    float turnDegrees = 0; // positive turns toward the left about world +Y
    bool activated = false;
    bool deactivated = false;
};

class LocomotionGate {
public:
    explicit LocomotionGate(LocomotionConfig config = {}) : config_(config) {
        if (!std::isfinite(config.stepMetres) || config.stepMetres <= 0 ||
            !std::isfinite(config.snapTurnDegrees) || config.snapTurnDegrees <= 0 ||
            config.snapTurnDegrees > 180 || !std::isfinite(config.maximumWristTiltDegrees) ||
            config.maximumWristTiltDegrees <= 0 || config.maximumWristTiltDegrees > 90 ||
            !std::isfinite(config.extendedIndexCurl) || config.extendedIndexCurl < 0 ||
            config.extendedIndexCurl >= 1 || !std::isfinite(config.idleTimeoutSeconds) ||
            config.idleTimeoutSeconds <= 0 || !std::isfinite(config.maximumSampleGapSeconds) ||
            config.maximumSampleGapSeconds <= 0)
            throw std::invalid_argument("XRGesture: invalid locomotion configuration");
    }

    bool active() const { return active_; }
    XRHand hand() const { return hand_; }
    void cancel() { active_ = false; }

    LocomotionIntent update(const Frame& frame, EventRange events, bool permitted = true) {
        LocomotionIntent result;
        if (!permitted || !frame.focused || !frame.head.tracked || !finite(frame.head.orientation) ||
            !finite(frame.head.position) || !std::isfinite(frame.time) || frame.time < 0)
            return close();
        if (active_ && !posture(frame, hand_))
            return close();
        if (frame.time == lastSample_)
            return result;
        const bool gap = lastSample_ >= 0 && (frame.time < lastSample_ ||
                                              frame.time - lastSample_ > config_.maximumSampleGapSeconds);
        lastSample_ = std::isfinite(frame.time) ? frame.time : -1;
        if (gap)
            return close();
        if (active_ && (!posture(frame, hand_) || frame.time - lastActivity_ > config_.idleTimeoutSeconds ||
                        frame.time < lastActivity_ || tilted(frame)))
            return close();
        for (const auto& event : events) {
            if (!validHand(event.hand) || event.phase != Phase::Performed || event.time != frame.time ||
                event.gesture < gesture::SwipeLeft || event.gesture > gesture::ThumbTap)
                continue;
            if (event.gesture == gesture::ThumbTap) {
                if (active_) {
                    if (event.hand == hand_)
                        return close();
                    continue;
                }
                if (!posture(frame, event.hand))
                    continue;
                hand_ = event.hand;
                gravity_ = localGravity(frame, hand_);
                active_ = true;
                lastActivity_ = frame.time;
                result.activated = true;
                continue;
            }
            if (!active_ || event.hand != hand_)
                continue;
            lastActivity_ = frame.time;
            if (config_.lateral == LateralAction::SnapTurn &&
                (event.gesture == gesture::SwipeLeft || event.gesture == gesture::SwipeRight)) {
                result.turnDegrees +=
                    event.gesture == gesture::SwipeLeft ? config_.snapTurnDegrees : -config_.snapTurnDegrees;
                continue;
            }
            glm::vec3 forward = glm::normalize(frame.head.orientation) * glm::vec3(0, 0, -1);
            forward.y = 0;
            if (glm::length(forward) < .1f)
                continue; // no arbitrary heading when looking vertically
            forward = glm::normalize(forward);
            const glm::vec3 right = glm::cross(forward, glm::vec3(0, 1, 0));
            if (event.gesture == gesture::SwipeForward)
                result.step += forward * config_.stepMetres;
            if (event.gesture == gesture::SwipeBackward)
                result.step -= forward * config_.stepMetres;
            if (event.gesture == gesture::SwipeLeft)
                result.step -= right * config_.stepMetres;
            if (event.gesture == gesture::SwipeRight)
                result.step += right * config_.stepMetres;
        }
        return result;
    }

private:
    bool posture(const Frame& frame, XRHand hand) const {
        const auto& skeleton = frame.hands[xrHandIndex(hand)].skeleton;
        const auto index = fingerCurl(skeleton, Finger::Index);
        return skeleton.active && valid(skeleton.joint(XRHandJoint::Wrist)) && index &&
               *index > config_.extendedIndexCurl;
    }
    glm::vec3 localGravity(const Frame& frame, XRHand hand) const {
        const auto& wrist = frame.hands[xrHandIndex(hand)].skeleton.joint(XRHandJoint::Wrist);
        return glm::inverse(glm::normalize(wrist.pose.orientation)) * glm::vec3(0, 1, 0);
    }
    bool tilted(const Frame& frame) const {
        // Gravity in wrist space ignores world rebases and heading/snap turns.
        const float cosine = glm::dot(gravity_, localGravity(frame, hand_));
        return cosine < std::cos(glm::radians(config_.maximumWristTiltDegrees));
    }
    LocomotionIntent close() {
        LocomotionIntent result;
        result.deactivated = active_;
        active_ = false;
        return result;
    }
    LocomotionConfig config_;
    bool active_ = false;
    XRHand hand_ = XRHand::Right;
    glm::vec3 gravity_{0, 1, 0};
    double lastActivity_ = 0;
    double lastSample_ = -1;
};

// Finite steps become bounded deltas for the host's existing character solver.
// Collision rejection consumes the request; no deferred movement is replayed.
class StepMotion {
public:
    explicit StepMotion(float speedMetresPerSecond = 2.8f, float maximumQueuedMetres = 1.f)
        : speed_(speedMetresPerSecond), maximum_(maximumQueuedMetres) {
        if (!std::isfinite(speed_) || speed_ <= 0 || !std::isfinite(maximum_) || maximum_ <= 0)
            throw std::invalid_argument("XRGesture: invalid step motion configuration");
    }
    void cancel() { remaining_ = glm::dvec3(0); }
    void request(const glm::vec3& delta) {
        if (!finite(delta))
            throw std::invalid_argument("XRGesture: invalid step");
        remaining_ += glm::dvec3(delta.x, 0, delta.z);
        const double distance = glm::length(remaining_);
        if (distance > maximum_)
            remaining_ *= maximum_ / distance;
    }
    glm::vec3 advance(float seconds) {
        if (!std::isfinite(seconds) || seconds <= 0 || seconds > .25f) {
            cancel();
            return glm::vec3(0);
        }
        const double distance = glm::length(remaining_);
        if (distance < .00001f) {
            cancel();
            return glm::vec3(0);
        }
        const glm::dvec3 delta = remaining_ * (std::min(distance, double(speed_) * seconds) / distance);
        remaining_ -= delta;
        return glm::vec3(delta);
    }

private:
    glm::dvec3 remaining_{0};
    float speed_, maximum_;
};

} // namespace saida::xrgesture
