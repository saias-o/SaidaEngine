#pragma once

#include "Types.hpp"
#include <algorithm>
#include <optional>
#include <glm/gtc/constants.hpp>

namespace saida::xrgesture {

enum class Finger { Index, Middle, Ring, Little };

inline std::optional<float> fingerCurl(const XRHandSkeletonState& hand, Finger finger) {
    constexpr XRHandJoint starts[] = {XRHandJoint::IndexProximal, XRHandJoint::MiddleProximal,
                                      XRHandJoint::RingProximal, XRHandJoint::LittleProximal};
    const int first = static_cast<int>(starts[static_cast<int>(finger)]);
    for (int i = 0; i < 4; ++i)
        if (!valid(hand.joints[first + i]))
            return {};
    const glm::vec3 a = hand.joints[first + 1].pose.position - hand.joints[first].pose.position;
    const glm::vec3 b = hand.joints[first + 3].pose.position - hand.joints[first + 2].pose.position;
    const float lengths = glm::length(a) * glm::length(b);
    if (lengths < 1e-8f)
        return {};
    return std::acos(std::clamp(glm::dot(a, b) / lengths, -1.f, 1.f)) / glm::pi<float>();
}

inline std::optional<float> handScale(const XRHandSkeletonState& hand) {
    const auto& a = hand.joint(XRHandJoint::IndexProximal);
    const auto& b = hand.joint(XRHandJoint::LittleProximal);
    if (!valid(a) || !valid(b))
        return {};
    constexpr float referenceWidth = .055f;
    const float width = glm::distance(a.pose.position, b.pose.position);
    if (width < .02f || width > .13f)
        return {};
    return std::clamp(width / referenceWidth, .5f, 2.f);
}

struct ThumbContact {
    glm::vec2 coordinates{0}; // tipward, across the index's side surface, metres
    float gap = 0;
    float scale = 1;
    bool posture = false;
};

inline std::optional<ThumbContact> thumbContact(const XRHandSkeletonState& hand, XRHand side) {
    if (!hand.active)
        return {};
    const auto& thumb = hand.joint(XRHandJoint::ThumbTip);
    const auto& a = hand.joint(XRHandJoint::IndexIntermediate);
    const auto& b = hand.joint(XRHandJoint::IndexDistal);
    const auto& inner = hand.joint(XRHandJoint::IndexProximal);
    const auto& outer = hand.joint(XRHandJoint::LittleProximal);
    const auto scale = handScale(hand);
    if (!valid(thumb) || !valid(a) || !valid(b) || !scale)
        return {};
    const glm::vec3 segment = b.pose.position - a.pose.position;
    const float length = glm::length(segment);
    if (length < .003f)
        return {};
    const glm::vec3 tipward = segment / length;
    glm::vec3 thumbward = inner.pose.position - outer.pose.position;
    thumbward -= tipward * glm::dot(thumbward, tipward);
    if (glm::length(thumbward) < .003f)
        return {};
    thumbward = glm::normalize(thumbward);
    const glm::vec3 across = glm::cross(thumbward, tipward) * (side == XRHand::Right ? 1.f : -1.f);
    const glm::vec3 relative = thumb.pose.position - a.pose.position;
    const float along = glm::dot(relative, tipward);
    const glm::vec3 nearest = a.pose.position + tipward * std::clamp(along, 0.f, length);
    ThumbContact contact;
    contact.coordinates = {along, glm::dot(relative, across)};
    contact.scale = *scale;
    contact.gap = glm::distance(thumb.pose.position, nearest) -
                  (std::max(thumb.radius, .005f * *scale) + std::max(a.radius, .005f * *scale));
    contact.posture = true;
    for (Finger finger : {Finger::Index, Finger::Middle, Finger::Ring, Finger::Little}) {
        const auto curl = fingerCurl(hand, finger);
        if (!curl)
            return {};
        contact.posture = contact.posture && *curl >= .15f && *curl <= .85f;
    }
    return contact;
}

inline Observation poseObservation(const Frame& frame, XRHand side, GestureId id) {
    const auto& hand = frame.hands[xrHandIndex(side)].skeleton;
    if (!hand.active)
        return {};
    if (id == gesture::Pinch) {
        const auto& a = hand.joint(XRHandJoint::ThumbTip);
        const auto& b = hand.joint(XRHandJoint::IndexTip);
        const auto scale = handScale(hand);
        if (!valid(a) || !valid(b) || !scale)
            return {};
        const float distance = glm::distance(a.pose.position, b.pose.position) / *scale;
        return {true, std::clamp((.045f - distance) / .03f, 0.f, 1.f)};
    }
    float score = 1;
    for (Finger finger : {Finger::Index, Finger::Middle, Finger::Ring, Finger::Little}) {
        const auto curl = fingerCurl(hand, finger);
        if (!curl)
            return {};
        const bool extended = id == gesture::OpenPalm || (id == gesture::Point && finger == Finger::Index);
        score = std::min(score, extended ? std::clamp(1.f - *curl / .3f, 0.f, 1.f)
                                         : std::clamp((*curl - .15f) / .45f, 0.f, 1.f));
    }
    return {true, score};
}

} // namespace saida::xrgesture
