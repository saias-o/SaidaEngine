#include "core/Reflection.hpp"
#include "scene/animation/Animator.hpp"
#include "scene/animation/AnimGraphAsset.hpp"
#include "scene/animation/AnimationClip.hpp"
#include "scene/animation/ClipNode.hpp"
#include "scene/animation/ClipView.hpp"
#include "scene/Node.hpp"
#include "core/Log.hpp"
#include "core/Profiler.hpp"

#include <algorithm>
#include <cmath>

namespace saida {

namespace {

// Clip name from a sub-asset key: "model.glb#Run" -> "Run".
std::string clipNameFromKey(const std::string& key) {
    const size_t hash = key.rfind('#');
    return hash == std::string::npos ? key : key.substr(hash + 1);
}

} // namespace

void Animator::setRig(Rig* rig) {
    rig_ = rig;
    if (!rig_) {
        bindPose_.localTransforms.clear();
        currentLocalPose_.localTransforms.clear();
        globalPose_.globalMatrices.clear();
        globalPose_.skinningMatrices.clear();
        return;
    }

    std::string error;
    if (!rig_->finalized() && !rig_->finalize(&error)) {
        Log::error("Animator: invalid rig: ", error);
        rig_ = nullptr;
        return;
    }

    const size_t boneCount = rig_->boneCount();
    bindPose_.resize(boneCount);
    for (size_t i = 0; i < boneCount; ++i)
        bindPose_.localTransforms[i] = rig_->bones()[i].restLocal;
    currentLocalPose_.resize(boneCount);
    globalPose_.resize(boneCount);
}

void Animator::setRootNode(std::unique_ptr<AnimNode> rootNode) {
    rootNode_ = std::move(rootNode);
    playbackFsm_ = nullptr;  // a custom root replaces the by-name playback FSM
    authoredGraph_ = rootNode_ != nullptr;
    playStates_.clear();
    currentClip_.clear();
}

void Animator::setStateMachine(std::unique_ptr<AnimStateMachine> sm) {
    if (sm) sm->setBlackboard(&blackboard_);
    rootNode_ = std::move(sm);
    playbackFsm_ = nullptr;
    authoredGraph_ = rootNode_ != nullptr;
    playStates_.clear();
    currentClip_.clear();
}

void Animator::play(const std::string& name, bool loop, float crossfade) {
    if (name == currentClip_) return;
    auto it = clips_.find(name);
    if (it == clips_.end() || !it->second) {
        Log::warn("Animator::play: unknown clip '", name, "'");
        return;
    }
    if (!rig_) return;

    // Back play()-by-name with an internal state machine so we reuse its crossfade.
    if (!playbackFsm_) {
        auto sm = std::make_unique<AnimStateMachine>();
        sm->setBlackboard(&blackboard_);
        playbackFsm_ = sm.get();
        rootNode_ = std::move(sm);
        playStates_.clear();
    }

    if (playStates_.insert(name).second) {
        auto clip = std::make_unique<ClipNode>(it->second, *rig_,
                                               retarget_.empty() ? nullptr : &retarget_);
        clip->setLooping(loop);
        playbackFsm_->addState(std::make_unique<AnimState>(name, std::move(clip)));
    }
    playbackFsm_->transitionTo(name, crossfade);
    currentClip_ = name;
}

void Animator::playView(const ClipView& view, float crossfade) {
    if (!rig_) return;
    if (view.name.empty()) {
        Log::warn("Animator::playView: view has no name");
        return;
    }
    if (view.name == currentClip_) return;

    const std::string clipName = clipNameFromKey(view.source);
    auto it = clips_.find(clipName);
    if (it == clips_.end() || !it->second) {
        Log::warn("Animator::playView: unknown source clip '", clipName, "' for view '",
                  view.name, "'");
        return;
    }

    // Same internal FSM as play(): views and direct clips share the
    // state namespace.
    if (!playbackFsm_) {
        auto sm = std::make_unique<AnimStateMachine>();
        sm->setBlackboard(&blackboard_);
        playbackFsm_ = sm.get();
        rootNode_ = std::move(sm);
        playStates_.clear();
    }

    if (playStates_.insert(view.name).second) {
        playbackFsm_->addState(
            std::make_unique<AnimState>(view.name, view.instantiate(*it->second, *rig_)));
    }
    playbackFsm_->transitionTo(view.name, crossfade);
    currentClip_ = view.name;
}

bool Animator::setGraph(const AnimGraphAsset& graph,
                        std::vector<AssetDiagnostic>* diagnostics) {
    if (!rig_) {
        if (diagnostics)
            diagnostics->push_back({"animgraph.build.no_rig",
                                    AssetDiagnostic::Severity::Error, "",
                                    "animator has no rig"});
        return false;
    }

    auto sm = graph.build(
        [this](const std::string& key) -> const AnimationClip* {
            auto it = clips_.find(clipNameFromKey(key));
            return it != clips_.end() ? it->second : nullptr;
        },
        *rig_, diagnostics);
    if (!sm) return false;

    for (const auto& p : graph.parameters) blackboard_.setFloat(p.name, p.defaultValue);
    setStateMachine(std::move(sm));
    return true;
}

ClipNode* Animator::activeClipNode() const {
    if (auto* clip = dynamic_cast<ClipNode*>(rootNode_.get())) return clip;
    if (playbackFsm_ && playbackFsm_->currentState())
        return dynamic_cast<ClipNode*>(playbackFsm_->currentState()->node());
    return nullptr;
}

void Animator::setRetargetProfile(const RetargetProfile& profile) {
    retarget_ = profile.toRetargetMap();
    retargetCorrections_ =
        rig_ ? profile.compileCorrections(*rig_) : RetargetCorrections{};
}

glm::vec3 Animator::consumeRootMotion() {
    const glm::vec3 delta = pendingRootMotion_;
    pendingRootMotion_ = glm::vec3(0.0f);
    return delta;
}

void Animator::setPoseRate(float hz) {
    poseRate_ = std::max(0.0f, hz);
    poseAccumulator_ = 0.0f;
    sampledPosesPrimed_ = false;
}

// The mode can change after states are created: the active clip's
// configuration is realigned on every tick (trivial setter).
void Animator::refreshRootMotionExtraction() {
    ClipNode* clip = activeClipNode();
    if (!clip) return;
    int32_t rootBone = -1;
    if (rootMotionMode_ != RootMotionMode::Ignore) {
        for (size_t i = 0; i < rig_->boneCount(); ++i) {
            if (rig_->bones()[i].parentIndex < 0) {
                rootBone = int32_t(i);
                break;
            }
        }
    }
    clip->setRootMotionBone(rootBone);
}

void Animator::dispatchClipEvents() {
    ClipNode* clip = activeClipNode();
    if (!clip) return;
    for (uint32_t index : clip->firedEvents())
        animationEvent.emit(clip->events()[index].name);
    if (rootMotionMode_ != RootMotionMode::Ignore)
        pendingRootMotion_ += clip->takeRootMotionDelta();
}

// Control runs every tick, pose at a reduced rate: between two samples,
// the output interpolates the two latest evaluated poses.
void Animator::samplePose(float dt) {
    const float interval = poseRate_ > 0.0f ? 1.0f / poseRate_ : 0.0f;
    if (interval <= 0.0f) {
        rootNode_->evaluate(bindPose_, currentLocalPose_);
        retargetCorrections_.apply(currentLocalPose_);
        return;
    }

    poseAccumulator_ += dt;
    if (!sampledPosesPrimed_) {
        rootNode_->evaluate(bindPose_, lastSampledPose_);
        previousSampledPose_ = lastSampledPose_;
        sampledPosesPrimed_ = true;
        poseAccumulator_ = 0.0f;
    } else if (poseAccumulator_ >= interval) {
        std::swap(previousSampledPose_, lastSampledPose_);
        rootNode_->evaluate(bindPose_, lastSampledPose_);
        poseAccumulator_ = std::fmod(poseAccumulator_, interval);
    }

    const float alpha = std::clamp(poseAccumulator_ / interval, 0.0f, 1.0f);
    currentLocalPose_.resize(bindPose_.localTransforms.size());
    for (size_t i = 0; i < currentLocalPose_.localTransforms.size(); ++i) {
        const Transform& a = previousSampledPose_.localTransforms[i];
        const Transform& b = lastSampledPose_.localTransforms[i];
        Transform& out = currentLocalPose_.localTransforms[i];
        out.position = glm::mix(a.position, b.position, alpha);
        out.rotation = glm::slerp(a.rotation, b.rotation, alpha);
        out.scale = glm::mix(a.scale, b.scale, alpha);
    }
    retargetCorrections_.apply(currentLocalPose_);
}

void Animator::onUpdate(float dt) {
    SAIDA_PROFILE_SCOPE("Animation/AnimatorUpdate");
    SAIDA_PROFILE_COUNTER_ADD("Animation/Animators", 1);
    if (!rig_) return;

    if (rootNode_) {
        refreshRootMotionExtraction();
        rootNode_->update(dt);
        dispatchClipEvents();
        samplePose(dt);
    } else {
        currentLocalPose_ = bindPose_;  // no graph → rest pose
    }

    if (rootMotionMode_ == RootMotionMode::ApplyToNode && node()) {
        Transform& transform = node()->transform();
        transform.position += transform.rotation * consumeRootMotion();
    }

    // GlobalPose in object space (identity base): the renderer applies the entity
    // model matrix to the whole mesh after skinning.
    globalPose_.computeFrom(currentLocalPose_, *rig_, glm::mat4(1.0f));
}

void Animator::describe(reflect::TypeBuilder<Animator>& t) {
    t.doc("Skeletal animation player: clips, clip views and .sgraph graphs "
          "driven by the animation blackboard. Emits 'animationEvent' for each "
          "ClipView event crossed by the active clip.");
    t.signal("animationEvent", &Animator::animationEvent);
}

} // namespace saida
