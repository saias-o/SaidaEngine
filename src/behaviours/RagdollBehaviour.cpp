#include "behaviours/RagdollBehaviour.hpp"

#include "core/Log.hpp"
#include "physics/CollisionObjectNode.hpp"
#include "scene/Node.hpp"
#include "scene/Scene.hpp"
#include "scene/animation/Animator.hpp"
#include "scene/animation/PoseModifiers.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace saida {

namespace {

constexpr float kEpsilon = 1e-6f;
// Below this, the body's long axis counts as lying down rather than upright.
constexpr float kUprightCosine = 0.7f;

glm::quat rotationOf(const glm::mat4& m) {
    glm::mat3 r(m);
    for (int c = 0; c < 3; ++c) {
        const float length = glm::length(r[c]);
        if (length > kEpsilon) r[c] /= length;
    }
    return glm::normalize(glm::quat_cast(r));
}

Transform transformOf(const glm::mat4& m) {
    Transform t;
    t.position = glm::vec3(m[3]);
    t.scale = glm::vec3(glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2])));
    t.rotation = rotationOf(m);
    return t;
}

// The world matrix of `node`'s parent, composed from the live local
// transforms rather than a cache an update has not refreshed yet.
glm::mat4 parentWorld(const Node& node) {
    std::vector<const Node*> chain;
    for (const Node* p = node.parent(); p; p = p->parent()) chain.push_back(p);
    glm::mat4 world(1.0f);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) world *= (*it)->localMatrix();
    return world;
}

Scene* sceneOf(Node* node) {
    Scene* scene = nullptr;
    for (Node* n = node; n; n = n->parent())
        if (auto* candidate = dynamic_cast<Scene*>(n)) scene = candidate;
    return scene;
}

float smootherStep(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * x * (x * (x * 6.0f - 15.0f) + 10.0f);
}

class RagdollPoseModifier;

} // namespace

struct RagdollBehaviour::Shared {
    RagdollBehaviour* owner = nullptr;  // null once the behaviour is gone
    RagdollProfile profile;
    std::unique_ptr<Ragdoll> ragdoll;
    const Rig* rig = nullptr;
    Animator* animator = nullptr;
    bool refusalSaid = false;
    State state = State::Animated;
    LocalPose snapshot;
    glm::mat4 objectToWorld{1.0f};
    float rootLift = 0.0f;  // the root part's height above the node origin at activation
    float elapsed = 0.0f, blendSeconds = 0.0f, weight = 0.0f;
    bool settled = false;
    CollisionObjectNode::PhysicsSuspension animatedBodySuspension;

    Node* node() const { return owner ? owner->node() : nullptr; }

    glm::mat4 nodeWorld() const {
        Node* n = node();
        return n ? parentWorld(*n) * n->localMatrix() : glm::mat4(1.0f);
    }

    void refuse(const std::string& why) {
        if (refusalSaid) return;
        refusalSaid = true;
        Node* n = node();
        Log::error("Ragdoll '", n ? n->name() : std::string("?"), "': ", why);
    }

    // Puts the node's origin `lift` below the root part, keeping its rotation.
    void follow() {
        Node* n = node();
        if (!n || !ragdoll || !ragdoll->spawned()) return;
        const glm::vec3 root = ragdoll->rootPosition();
        const glm::mat4 above = parentWorld(*n);
        const glm::vec3 wanted(root.x, root.y - rootLift, root.z);
        n->transform().position = glm::vec3(glm::inverse(above) * glm::vec4(wanted, 1.0f));
    }

    void update(float dt) {
        if (!owner) return;
        switch (state) {
            case State::Simulated:
                if (!ragdoll || !ragdoll->spawned()) {
                    state = State::Animated; animatedBodySuspension.reset(); break;
                }
                elapsed += dt;
                settled = ragdoll->updateSettled(dt);
                if (owner->followRoot) follow();
                objectToWorld = nodeWorld();
                if (settled && owner->autoFreeze) freeze();
                break;
            case State::Blending:
                elapsed += dt;
                weight = blendSeconds > 0.0f ? 1.0f - smootherStep(elapsed / blendSeconds) : 0.0f;
                if (weight <= 0.0f) { state = State::Animated; animatedBodySuspension.reset(); }
                break;
            case State::Frozen:
            case State::Animated:
                break;
        }
    }

    // Where the body lay when it stopped being simulated, for blendOut().
    glm::vec3 frozenRoot{0.0f};
    glm::quat frozenRootRotation{1.0f, 0.0f, 0.0f, 0.0f};
    float frozenLowest = 0.0f;

    void freeze() {
        if (state != State::Simulated || !ragdoll) return;
        ragdoll->readPose(objectToWorld, snapshot);
        frozenRoot = ragdoll->rootPosition();
        frozenRootRotation = ragdoll->rootRotation();
        frozenLowest = frozenRoot.y;
        for (size_t i = 0; i < ragdoll->partCount(); ++i) frozenLowest = std::min(frozenLowest, ragdoll->partPosition(i).y);
        ragdoll->despawn();
        state = State::Frozen;
        weight = 1.0f;
    }

    void apply(LocalPose& pose) const {
        switch (state) {
            case State::Simulated:
                if (ragdoll) ragdoll->readPose(objectToWorld, pose);
                break;
            case State::Frozen:
                if (snapshot.localTransforms.size() == pose.localTransforms.size()) pose = snapshot;
                break;
            case State::Blending:
                if (snapshot.localTransforms.size() != pose.localTransforms.size()) break;
                for (size_t i = 0; i < pose.localTransforms.size(); ++i) {
                    Transform& t = pose.localTransforms[i];
                    const Transform& s = snapshot.localTransforms[i];
                    t.rotation = glm::normalize(glm::slerp(t.rotation, s.rotation, weight));
                    t.position = glm::mix(t.position, s.position, weight);
                    t.scale = glm::mix(t.scale, s.scale, weight);
                }
                break;
            case State::Animated:
                break;
        }
    }
};

namespace {

class RagdollPoseModifier : public PoseModifier {
public:
    explicit RagdollPoseModifier(std::shared_ptr<RagdollBehaviour::Shared> shared) : shared_(std::move(shared)) {}
    void update(float dt) override { shared_->update(dt); }
    bool active() const override {
        return shared_->owner && shared_->state != RagdollBehaviour::State::Animated;
    }
    void apply(LocalPose& pose, const Rig&) const override { shared_->apply(pose); }

private:
    std::shared_ptr<RagdollBehaviour::Shared> shared_;
};

} // namespace

RagdollBehaviour::RagdollBehaviour(RagdollProfile profile) : shared_(std::make_shared<Shared>()) {
    shared_->owner = this;
    shared_->profile = std::move(profile);
}

RagdollBehaviour::~RagdollBehaviour() {
    release();
    shared_->owner = nullptr;
}

void RagdollBehaviour::onDisable() { release(); }

void RagdollBehaviour::onHierarchyInactive() { release(); }

void RagdollBehaviour::onDestroy() { release(); }

void RagdollBehaviour::onRebase(const glm::vec3& translation, const glm::quat& rotation) {
    if (shared_->ragdoll) shared_->ragdoll->rebase(translation, rotation);
    shared_->objectToWorld = shared_->nodeWorld();
}

bool RagdollBehaviour::activateReplacing(CollisionObjectNode& animatedBody,
                                        const glm::vec3& velocity, const glm::vec3& angularVelocity) {
    if (!node() || sceneOf(node()) != sceneOf(&animatedBody)) {
        shared_->refuse("the animated collider must belong to the same scene");
        return false;
    }
    auto suspension = animatedBody.suspendPhysics();
    if (!activate(velocity, angularVelocity)) return false;
    shared_->animatedBodySuspension = std::move(suspension);
    return true;
}

bool RagdollBehaviour::activate(const glm::vec3& velocity, const glm::vec3& angularVelocity) {
    Shared& s = *shared_;
    Node* self = node();
    if (!self) return false;
    if (s.state == State::Simulated) return true;
    Animator* animator = self->getBehaviour<Animator>();
    if (!animator || !animator->rig()) { s.refuse("its node has no Animator with a rig"); return false; }
    if (!s.ragdoll || s.rig != animator->rig()) {
        s.rig = animator->rig();
        s.ragdoll = std::make_unique<Ragdoll>(*s.rig, s.profile);
    }
    if (!s.ragdoll->valid()) { s.refuse("the profile does not fit the rig: " + s.ragdoll->error()); return false; }
    if (s.animator != animator) {
        s.animator = animator;
        animator->addModifier<RagdollPoseModifier>(shared_);
    }
    Scene* scene = sceneOf(self);
    if (!scene) { s.refuse("its node is in no scene"); return false; }
#ifdef SAIDA_NO_PHYSICS
    s.refuse("this build has no physics");
    return false;
#else
    const GlobalPose& global = animator->globalPose();
    const Rig& rig = *s.rig;
    if (global.globalMatrices.size() < rig.boneCount()) { s.refuse("the Animator has not posed yet"); return false; }
    // The pose on screen this frame, frozen or blending included, as locals.
    LocalPose local;
    local.resize(rig.boneCount());
    for (size_t b = 0; b < rig.boneCount(); ++b) {
        const int32_t parent = rig.bones()[b].parentIndex;
        const glm::mat4 m = parent >= 0 ? glm::inverse(global.globalMatrices[size_t(parent)]) * global.globalMatrices[b]
                                        : global.globalMatrices[b];
        local.localTransforms[b] = transformOf(m);
    }
    s.objectToWorld = s.nodeWorld();
    if (!s.ragdoll->spawn(scene->ensurePhysics(), global, local, s.objectToWorld, velocity, angularVelocity)) {
        Log::error("Ragdoll '", self->name(), "': the physics world refused its bodies");
        return false;
    }
    s.rootLift = s.ragdoll->rootPosition().y - glm::vec3(s.objectToWorld[3]).y;
    s.state = State::Simulated;
    s.elapsed = 0.0f;
    s.settled = false;
    s.weight = 1.0f;
    return true;
#endif
}

void RagdollBehaviour::blendOut(float seconds, std::optional<float> groundHeight) {
    Shared& s = *shared_;
    Node* self = node();
    if (!self || s.state == State::Animated || s.state == State::Blending) return;
    s.freeze();
    if (s.state != State::Frozen || !s.rig) { release(); return; }
    // Stand where it lies, facing the way it would get up: along the spine
    // when lying, along the hips' front when still upright.
    const glm::mat4 above = parentWorld(*self);
    const glm::mat4 before = s.nodeWorld();
    const glm::quat nodeRotation = rotationOf(above) * self->transform().rotation;
    const glm::quat restRoot = rotationOf(glm::inverse(s.rig->bones()[size_t(s.rig->findBoneIndex(s.profile.parts[0].bone))].inverseBindMatrix));
    const glm::vec3 worldUp = nodeRotation * glm::normalize(up);
    const glm::vec3 spine = s.frozenRootRotation * (glm::inverse(restRoot) * glm::normalize(up));
    const glm::vec3 front = s.frozenRootRotation * (glm::inverse(restRoot) * glm::normalize(forward));
    glm::vec3 heading = std::abs(glm::dot(spine, worldUp)) < kUprightCosine ? spine : front;
    heading -= worldUp * glm::dot(heading, worldUp);
    glm::quat turned = nodeRotation;
    glm::vec3 facing = nodeRotation * glm::normalize(forward);
    facing -= worldUp * glm::dot(facing, worldUp);
    if (glm::dot(heading, heading) > kEpsilon && glm::dot(facing, facing) > kEpsilon) {
        const glm::vec3 f = glm::normalize(facing), h = glm::normalize(heading);
        const float angle = std::atan2(glm::dot(worldUp, glm::cross(f, h)), glm::dot(f, h));
        turned = glm::normalize(glm::angleAxis(angle, worldUp) * nodeRotation);
    }
    const glm::vec3 standing(s.frozenRoot.x, groundHeight ? *groundHeight : s.frozenLowest, s.frozenRoot.z);
    self->transform().position = glm::vec3(glm::inverse(above) * glm::vec4(standing, 1.0f));
    self->transform().rotation = glm::normalize(glm::inverse(rotationOf(above)) * turned);
    s.objectToWorld = s.nodeWorld();
    // The lying pose, unchanged on screen, seen from the node's new place:
    // only the rig's roots depend on the node's frame.
    const glm::mat4 rehome = glm::inverse(s.objectToWorld) * before;
    for (size_t b = 0; b < s.rig->boneCount() && b < s.snapshot.localTransforms.size(); ++b)
        if (s.rig->bones()[b].parentIndex < 0)
            s.snapshot.localTransforms[b] = transformOf(rehome * s.snapshot.localTransforms[b].matrix());
    s.state = State::Blending;
    s.elapsed = 0.0f;
    s.blendSeconds = std::max(seconds, 0.0f);
    s.weight = 1.0f;
}

void RagdollBehaviour::freeze() { shared_->freeze(); }

bool RagdollBehaviour::blendOutToParent(float seconds, const Transform& localRest,
                                       std::optional<float> groundHeight) {
    Node* self = node();
    if (!self || !self->parent() || dynamic_cast<Scene*>(self->parent()) ||
        state() == State::Animated || state() == State::Blending) return false;
    blendOut(seconds, groundHeight);
    if (state() != State::Blending) return false;
    Node& root = *self->parent();
    const glm::mat4 standing = shared_->nodeWorld() * glm::inverse(localRest.matrix());
    root.transform() = transformOf(glm::inverse(parentWorld(root)) * standing);
    self->transform() = localRest;
    shared_->objectToWorld = shared_->nodeWorld();
    return true;
}

void RagdollBehaviour::release() {
    if (shared_->ragdoll) shared_->ragdoll->despawn();
    shared_->state = State::Animated;
    shared_->weight = 0.0f;
    shared_->animatedBodySuspension.reset();
}

RagdollBehaviour::State RagdollBehaviour::state() const { return shared_->state; }
bool RagdollBehaviour::settled() const { return shared_->settled; }
float RagdollBehaviour::simulatedSeconds() const { return shared_->elapsed; }
Ragdoll* RagdollBehaviour::ragdoll() { return shared_->ragdoll.get(); }
const Ragdoll* RagdollBehaviour::ragdoll() const { return shared_->ragdoll.get(); }

void RagdollBehaviour::applyImpulse(const glm::vec3& impulse, const glm::vec3& worldPoint) {
    if (shared_->ragdoll && shared_->state == State::Simulated) shared_->ragdoll->applyImpulse(impulse, worldPoint);
}

} // namespace saida
