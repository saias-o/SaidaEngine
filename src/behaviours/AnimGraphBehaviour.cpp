#include "behaviours/AnimGraphBehaviour.hpp"

#include "core/Log.hpp"
#include "graphics/ResourceManager.hpp"
#include "project/AssetRegistry.hpp"
#include "scene/Node.hpp"
#include "scene/SceneTree.hpp"
#include "scene/animation/AnimGraphAsset.hpp"
#include "scene/animation/Animator.hpp"

namespace saida {

void AnimGraphBehaviour::onUpdate(float) {
    if (graph.empty() || applied_ || failed_) return;

    // The Animators appear with the imported model, which may arrive a frame or
    // more after this node: keep looking until at least one shows up.
    if (!searched_ || animators_.empty()) {
        animators_.clear();
        node()->findBehavioursInChildren<Animator>(animators_);
        searched_ = true;
    }
    if (animators_.empty()) return;

    if (assetId_ == kAssetInvalid) {
        assetId_ = tree()->resources().loadAnimGraph(tree()->resolveProjectPath(graph));
        if (assetId_ == kAssetInvalid) {
            failed_ = true;
            Log::warn("AnimGraph: cannot request '", graph, "'");
            return;
        }
    }

    ResourceManager& resources = tree()->resources();
    const AssetLoadState state = resources.animGraphLoadState(assetId_);
    if (state == AssetLoadState::Queued || state == AssetLoadState::Loading) return;
    if (state == AssetLoadState::Failed) {
        failed_ = true;
        const std::string error = resources.animGraphLoadError(assetId_);
        Log::warn("AnimGraph: cannot load '", graph, "'",
                  error.empty() ? "" : (": " + error));
        return;
    }

    const AnimGraphAsset* loaded = resources.getAnimGraph(assetId_);
    if (!loaded) {
        failed_ = true;
        Log::warn("AnimGraph: cannot load '", graph, "'");
        return;
    }

    for (Animator* a : animators_) {
        std::vector<AssetDiagnostic> diags;
        if (!a->setGraph(*loaded, &diags)) {
            failed_ = true;
            Log::warn("AnimGraph: cannot apply '", graph, "'",
                      diags.empty() ? "" : (": " + diags.front().message));
            return;
        }
    }

    applied_ = true;
    Log::info("AnimGraph: '", graph, "' driving ", animators_.size(),
              animators_.size() == 1 ? " animator" : " animators");
}

void AnimGraphBehaviour::describe(reflect::TypeBuilder<AnimGraphBehaviour>& t) {
    t.doc("Applies a .sgraph animation graph to every Animator below this node. "
          "Parameters are driven through the animation blackboard "
          "(setAnimFloat/setAnimBool/setAnimTrigger).");
    t.property("graph", &AnimGraphBehaviour::graph)
        .tooltip("project-relative .sgraph applied to the Animators below");
}

} // namespace saida
