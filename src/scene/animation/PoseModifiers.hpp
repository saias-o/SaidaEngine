#pragma once

// Pose modifiers: procedural corrections an Animator applies AFTER its graph,
// on whatever pose that graph produced -- play()-by-name, a .sgraph or a
// custom root alike (Animator::addModifier). Gameplay pushes a target or an
// impulse every frame without touching the graph, and a modifier with nothing
// to do costs nothing: the Animator skips it, and skips the extra composition.
//
//   GazeModifier   -- a chain of bones (spine to head, then the eyes) turns
//                     toward a point, each bone taking its share of what is
//                     left, within the body's limits, eased in and out.
//   ImpactModifier -- a push leans a chain of bones away on a damped spring
//                     that swings back: a flinch, a stagger, a shoulder bump.
//
// Both work in the rig's object space, the space the Animator's GlobalPose is
// in, and read a bone's facing off the rig's bind pose: nothing depends on how
// a given skeleton orients its bones' local axes.

#include "scene/animation/Pose.hpp"
#include "scene/animation/Rig.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <string>
#include <vector>

namespace saida {

class PoseModifier {
public:
    virtual ~PoseModifier() = default;
    // Every tick, whether or not the Animator resamples its pose.
    virtual void update(float dt) = 0;
    // Whether apply() would change the pose.
    virtual bool active() const = 0;
    // Corrects `pose`, the graph's local pose, in place.
    virtual void apply(LocalPose& pose, const Rig& rig) const = 0;
};

class GazeModifier : public PoseModifier {
public:
    struct Bone {
        std::string name;
        // Fraction of the turn still left when this bone is reached: 0.3 on a
        // neck turns it a third of the way, and the head after it takes the
        // rest. Two siblings (the eyes) each aim from their own position.
        float share = 1.0f;
        float maxAngle = 1.0f;  // radians this bone turns at most
    };
    struct Settings {
        std::vector<Bone> chain;         // parents before children
        glm::vec3 forward{0, 0, 1};     // the way the character faces, object space
        glm::vec3 up{0, 1, 0};
        // A target outside these (radians from `forward`) is looked at from
        // the edge: the head follows it that far and holds.
        float maxYaw = 1.4f;
        float maxPitchUp = 0.5f, maxPitchDown = 0.7f;
        float response = 6.0f;           // 1/s at which weight and target ease
    };

    GazeModifier(const Rig& rig, Settings settings);
    // Every bone of the chain exists in the rig.
    bool valid() const { return valid_; }

    // Looks at `target` (object space) at `weight`, both reached by easing.
    void lookAt(const glm::vec3& target, float weight = 1.0f);
    void release() { wantedWeight_ = 0.0f; }
    float weight() const { return weight_; }
    const glm::vec3& target() const { return current_; }

    void update(float dt) override;
    bool active() const override { return valid_ && (weight_ > 1e-3f || wantedWeight_ > 0.0f); }
    void apply(LocalPose& pose, const Rig& rig) const override;

private:
    struct Link { int32_t bone = -1; glm::vec3 localForward{0, 0, 1}; float share = 1, maxAngle = 1; };
    glm::vec3 clampToBody(const glm::vec3& direction) const;

    Settings settings_;
    std::vector<Link> links_;
    bool valid_ = false;
    glm::vec3 wanted_{0.0f}, current_{0.0f};
    float wantedWeight_ = 0.0f, weight_ = 0.0f;
    // The shoulder a target behind the body is looked at over: kept while it
    // stays behind, so every bone turns the same way and the head does not
    // flick across as the target crosses dead astern.
    float side_ = 1.0f;
};

class ImpactModifier : public PoseModifier {
public:
    struct Bone {
        std::string name;
        float share = 0.25f;  // of the lean; the shares down the chain add up
    };
    struct Settings {
        std::vector<Bone> chain;    // parents before children
        glm::vec3 up{0, 1, 0};
        float frequency = 1.6f;     // Hz of the swing back
        float damping = 0.45f;      // ratio to critical: below 1 it overshoots
        float maxAngle = 0.6f;      // radians the whole chain leans at most
    };

    ImpactModifier(const Rig& rig, Settings settings);
    bool valid() const { return valid_; }

    // A push toward `direction` (object space; only its part across `up`
    // counts) of `strength` radians per second of lean: the peak lean is
    // about strength / (2 pi frequency).
    void push(const glm::vec3& direction, float strength);
    // The lean now: its direction times its angle in radians.
    glm::vec3 lean() const { return angle_; }

    void update(float dt) override;
    bool active() const override;
    void apply(LocalPose& pose, const Rig& rig) const override;

private:
    Settings settings_;
    std::vector<std::pair<int32_t, float>> links_;
    bool valid_ = false;
    glm::vec3 angle_{0.0f}, velocity_{0.0f};
};

} // namespace saida
