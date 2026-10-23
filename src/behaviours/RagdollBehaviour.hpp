#pragma once

#include "physics/Ragdoll.hpp"
#include "scene/Behaviour.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <memory>
#include <optional>

namespace saida {
class CollisionObjectNode;
struct Transform;

// Turns the animated character on its node into a ragdoll and back. Add it
// beside the node's Animator (same node); it installs a pose modifier there,
// so while it holds the body the Animator draws the physics instead of the
// clip, and the clip resumes underneath without being touched.
//
//   Animated   -- the Animator's own pose; nothing simulated, nothing paid.
//   Simulated  -- activate(): eleven-odd bodies in the scene's physics world,
//                 posed from the frame's animation and moving at the given
//                 velocity. The node follows the root part (followRoot), so
//                 culling and LOD keep seeing the body where it fell.
//   Frozen     -- freeze(), or automatically once settled (autoFreeze): the
//                 bodies leave the world and the last pose is kept. A body at
//                 rest costs a skinned mesh and nothing else; activate() again
//                 to throw it once more.
//   Blending   -- blendOut(): the node stands where the root lies, facing
//                 along the body, and the pose blends back to the animation.
//
// Disabling the node takes the bodies out of the world (as it does for any
// body); a rebase (Scene::rebaseSubtree) carries them along. Runtime-only:
// not serialized. Refusals -- no Animator or rig, a profile that does not fit
// the rig, a full physics world -- are said in the log and return false.
class RagdollBehaviour : public Behaviour {
public:
    enum class State { Animated, Simulated, Frozen, Blending };

    explicit RagdollBehaviour(RagdollProfile profile);
    ~RagdollBehaviour() override;

    void onDisable() override;
    void onHierarchyInactive() override;
    void onDestroy() override;
    void onRebase(const glm::vec3& translation, const glm::quat& rotation) override;
    bool visibleInEditor() const override { return false; }

    bool activate(const glm::vec3& velocity = glm::vec3(0.0f),
                  const glm::vec3& angularVelocity = glm::vec3(0.0f));
    // Replace a character controller or animated collider transactionally.
    // Its physics resumes after recovery/release, including a refused activation.
    bool activateReplacing(CollisionObjectNode& animatedBody,
                           const glm::vec3& velocity = glm::vec3(0.0f),
                           const glm::vec3& angularVelocity = glm::vec3(0.0f));
    // Back to the animation over `seconds`. The node is placed under the root
    // part at `groundHeight` (world y) -- or on the lowest part when none is
    // given -- and turned to face along the body.
    void blendOut(float seconds, std::optional<float> groundHeight = std::nullopt);
    // Transfer the recovered pose to the direct parent (locomotion root),
    // restoring this visual node's local offset without moving its drawn pose.
    bool blendOutToParent(float seconds, const Transform& localRest,
                          std::optional<float> groundHeight = std::nullopt);
    void freeze();
    // Straight back to the animation, no blend.
    void release();

    State state() const;
    bool simulating() const { return state() == State::Simulated; }
    // Settled since activate() (RagdollProfile::settleSpeed/settleTime).
    bool settled() const;
    float simulatedSeconds() const;
    // The ragdoll once activate() has built it (null before, or if invalid).
    Ragdoll* ragdoll();
    const Ragdoll* ragdoll() const;
    void applyImpulse(const glm::vec3& impulse, const glm::vec3& worldPoint);

    // Object-space axes of the character: where it faces, and up.
    glm::vec3 forward{0.0f, 0.0f, 1.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};
    bool followRoot = true;
    bool autoFreeze = true;

    struct Shared;  // shared with the pose modifier it installs

private:
    std::shared_ptr<Shared> shared_;
};

} // namespace saida
