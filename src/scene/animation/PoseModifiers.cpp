#include "scene/animation/PoseModifiers.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>

namespace saida {

namespace {

constexpr float kEpsilon = 1e-6f;
// Yaw (radians from forward) past which a target counts as behind the body.
constexpr float kBehind = 2.6f;

// A bone's rotation out of its global matrix, whatever scale it carries.
glm::quat rotationOf(const glm::mat4& m) {
    glm::mat3 r(m);
    for (int c = 0; c < 3; ++c) {
        const float length = glm::length(r[c]);
        if (length > kEpsilon) r[c] /= length;
    }
    return glm::normalize(glm::quat_cast(r));
}

// One bone's global matrix, composed up its parents from the pose as it is
// now: a modifier walks a chain it is itself changing, so the GlobalPose the
// Animator computed before it is already stale for the bones after the first.
glm::mat4 globalOf(const LocalPose& pose, const Rig& rig, int32_t bone) {
    glm::mat4 m(1.0f);
    for (int32_t b = bone; b >= 0; b = rig.bones()[size_t(b)].parentIndex)
        m = pose.localTransforms[size_t(b)].matrix() * m;
    return m;
}

// Gives `bone` the global rotation `global`, its parent left as posed.
void setGlobalRotation(LocalPose& pose, const Rig& rig, int32_t bone, const glm::quat& global) {
    const int32_t parent = rig.bones()[size_t(bone)].parentIndex;
    const glm::quat above = parent >= 0 ? rotationOf(globalOf(pose, rig, parent)) : glm::quat(1, 0, 0, 0);
    pose.localTransforms[size_t(bone)].rotation = glm::normalize(glm::inverse(above) * global);
}

// The shortest rotation taking direction `from` onto direction `to`.
glm::quat rotationBetween(const glm::vec3& from, const glm::vec3& to) {
    const glm::vec3 f = glm::normalize(from), t = glm::normalize(to);
    const float cosine = glm::dot(f, t);
    if (cosine > 1.0f - kEpsilon) return glm::quat(1, 0, 0, 0);
    if (cosine < -1.0f + kEpsilon) {
        glm::vec3 axis = glm::cross(f, glm::vec3(1, 0, 0));
        if (glm::dot(axis, axis) < kEpsilon) axis = glm::cross(f, glm::vec3(0, 1, 0));
        return glm::angleAxis(glm::pi<float>(), glm::normalize(axis));
    }
    return glm::angleAxis(std::acos(std::clamp(cosine, -1.0f, 1.0f)), glm::normalize(glm::cross(f, t)));
}

float easing(float dt, float rate) { return 1.0f - std::exp(-rate * std::max(dt, 0.0f)); }

bool fits(const LocalPose& pose, const Rig& rig) { return pose.localTransforms.size() >= rig.boneCount(); }

} // namespace

// ── gaze ────────────────────────────────────────────────────────────────────

GazeModifier::GazeModifier(const Rig& rig, Settings settings) : settings_(std::move(settings)) {
    settings_.up = glm::normalize(settings_.up);
    // Forward is kept across `up`, so that yaw and pitch are the body's own.
    settings_.forward = glm::normalize(settings_.forward - settings_.up * glm::dot(settings_.forward, settings_.up));
    valid_ = !settings_.chain.empty();
    for (const Bone& b : settings_.chain) {
        Link link;
        link.bone = rig.findBoneIndex(b.name);
        if (link.bone < 0) {
            valid_ = false;
            continue;
        }
        // The bone's facing, in its own axes: where `forward` was when the
        // mesh was bound, which is the one pose every rig agrees on.
        const glm::quat bind = rotationOf(glm::inverse(rig.bones()[size_t(link.bone)].inverseBindMatrix));
        link.localForward = glm::normalize(glm::inverse(bind) * settings_.forward);
        link.share = std::clamp(b.share, 0.0f, 1.0f);
        link.maxAngle = std::max(0.0f, b.maxAngle);
        links_.push_back(link);
    }
}

void GazeModifier::lookAt(const glm::vec3& target, float weight) {
    // From rest, the gaze starts on the target and the weight carries it
    // there; easing the point too would sweep it in from wherever it last was.
    if (weight_ <= 1e-3f) current_ = target;
    wanted_ = target;
    wantedWeight_ = std::clamp(weight, 0.0f, 1.0f);
}

void GazeModifier::update(float dt) {
    const float k = easing(dt, settings_.response);
    weight_ += (wantedWeight_ - weight_) * k;
    current_ += (wanted_ - current_) * k;
    if (wantedWeight_ <= 0.0f && weight_ <= 1e-3f) weight_ = 0.0f;
    const glm::vec3 f = settings_.forward, r = glm::cross(settings_.up, f);
    const float yaw = std::atan2(glm::dot(current_, r), glm::dot(current_, f));
    if (std::abs(yaw) < kBehind) side_ = yaw < 0.0f ? -1.0f : 1.0f;
}

glm::vec3 GazeModifier::clampToBody(const glm::vec3& direction) const {
    const glm::vec3 f = settings_.forward, u = settings_.up, r = glm::cross(u, f);
    const float along = glm::dot(direction, f), across = glm::dot(direction, r);
    float yaw = std::atan2(across, along);
    if (std::abs(yaw) >= kBehind) yaw = side_ * std::abs(yaw);
    yaw = std::clamp(yaw, -settings_.maxYaw, settings_.maxYaw);
    const float pitch = std::clamp(std::atan2(glm::dot(direction, u), std::hypot(along, across)),
                                   -settings_.maxPitchDown, settings_.maxPitchUp);
    return std::cos(pitch) * (std::cos(yaw) * f + std::sin(yaw) * r) + std::sin(pitch) * u;
}

void GazeModifier::apply(LocalPose& pose, const Rig& rig) const {
    if (!active() || !fits(pose, rig)) return;
    for (const Link& link : links_) {
        const glm::mat4 global = globalOf(pose, rig, link.bone);
        const glm::vec3 toTarget = current_ - glm::vec3(global[3]);
        if (glm::dot(toTarget, toTarget) < kEpsilon) continue;
        const glm::quat rotation = rotationOf(global);
        const glm::quat full = rotationBetween(rotation * link.localForward, clampToBody(glm::normalize(toTarget)));
        const float angle = glm::angle(full);
        if (angle < 1e-5f) continue;
        const float turn = std::min(angle * link.share, link.maxAngle) * weight_;
        setGlobalRotation(pose, rig, link.bone, glm::angleAxis(turn, glm::axis(full)) * rotation);
    }
}

// ── impact ──────────────────────────────────────────────────────────────────

ImpactModifier::ImpactModifier(const Rig& rig, Settings settings) : settings_(std::move(settings)) {
    settings_.up = glm::normalize(settings_.up);
    valid_ = !settings_.chain.empty();
    for (const Bone& b : settings_.chain) {
        const int32_t bone = rig.findBoneIndex(b.name);
        if (bone < 0) {
            valid_ = false;
            continue;
        }
        links_.push_back({bone, b.share});
    }
}

void ImpactModifier::push(const glm::vec3& direction, float strength) {
    const glm::vec3 across = direction - settings_.up * glm::dot(direction, settings_.up);
    const float length = glm::length(across);
    if (length < kEpsilon || strength <= 0.0f) return;
    velocity_ += across / length * strength;
}

bool ImpactModifier::active() const {
    return valid_ && (glm::dot(angle_, angle_) > 1e-8f || glm::dot(velocity_, velocity_) > 1e-8f);
}

void ImpactModifier::update(float dt) {
    if (!active() || dt <= 0.0f) return;
    const float omega = 2.0f * glm::pi<float>() * settings_.frequency;
    const float stiffness = omega * omega, damping = 2.0f * settings_.damping * omega;
    // A spring stepped a frame at a time goes unstable once omega * dt nears
    // 2; sub-steps of 1/240 s keep it honest down to a few frames a second.
    const int steps = std::max(1, int(std::ceil(dt * 240.0f)));
    const float h = dt / float(steps);
    for (int i = 0; i < steps; ++i) {
        velocity_ += (-stiffness * angle_ - damping * velocity_) * h;
        angle_ += velocity_ * h;
        const float lean = glm::length(angle_);
        if (lean > settings_.maxAngle) {
            // Leaning as far as a body goes: what still pushed outward stops.
            const glm::vec3 out = angle_ / lean;
            angle_ = out * settings_.maxAngle;
            velocity_ -= out * std::max(0.0f, glm::dot(velocity_, out));
        }
    }
    // Settled: nothing is left to apply, and the Animator stops composing.
    if (glm::length(angle_) < 2e-4f && glm::length(velocity_) < 2e-3f) angle_ = velocity_ = glm::vec3(0.0f);
}

void ImpactModifier::apply(LocalPose& pose, const Rig& rig) const {
    const float lean = glm::length(angle_);
    if (!valid_ || lean < kEpsilon || !fits(pose, rig)) return;
    // Rotating `up` about up x d tips the chain's top toward d.
    const glm::vec3 axis = glm::normalize(glm::cross(settings_.up, angle_ / lean));
    for (const auto& [bone, share] : links_) {
        const glm::quat rotation = rotationOf(globalOf(pose, rig, bone));
        setGlobalRotation(pose, rig, bone, glm::angleAxis(lean * share, axis) * rotation);
    }
}

} // namespace saida
