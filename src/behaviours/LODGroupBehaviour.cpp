#include "behaviours/LODGroupBehaviour.hpp"

#include "core/Reflection.hpp"
#include "core/Time.hpp"
#include "nodes/MeshNode.hpp"
#include "scene/Node.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace saida {
namespace {
bool meshesLoaded(const Node& root) {
    if (root.mesh() && !root.mesh()->loaded()) return false;
    for (const auto& child : root.children())
        if (!meshesLoaded(*child)) return false;
    return true;
}
}

void LODGroupBehaviour::setBounds(const Aabb& local) {
    for (int axis = 0; axis < 3; ++axis)
        if (!std::isfinite(local.min[axis]) || !std::isfinite(local.max[axis]) ||
            local.min[axis] > local.max[axis])
            throw std::invalid_argument("LOD bounds require finite ordered local coordinates");
    bounds_ = local;
    explicitBounds_ = true;
    resolvedRevision_ = 0;
}

void LODGroupBehaviour::setLevelReady(int level, bool ready) {
    if (level < 0 || level >= int(ready_.size()))
        throw std::out_of_range("LOD readiness level is outside the configured chain");
    ready_[size_t(level)] = ready;
}

void LODGroupBehaviour::setLevels(std::vector<Level> levels) {
    float previous = 1.f;
    for (const auto& level : levels) {
        if (level.path.empty() || !std::isfinite(level.minCoverage) ||
            level.minCoverage < 0.f || level.minCoverage > previous)
            throw std::invalid_argument("LOD levels require paths and decreasing coverage in [0,1]");
        previous = level.minCoverage;
    }
    onDisable();
    levels_ = std::move(levels);
    thresholds_.clear();
    for (const auto& level : levels_) thresholds_.push_back({nullptr, nullptr, level.minCoverage});
    roots_.clear();
    ready_.assign(levels_.size(), true);
    loaded_.clear();
    pendingMeshes_ = false;
    resolvedRevision_ = 0;
    active_ = selected_ = desired_ = fading_ = -1;
    progress_ = 1.f;
}

void LODGroupBehaviour::applyFade(Node* root, float fade) {
    if (!root) return;
    if (root->mesh())
        if (auto* mesh = dynamic_cast<MeshNode*>(root)) mesh->setLodFade(fade);
    for (const auto& child : root->children()) applyFade(child.get(), fade);
}

int LODGroupBehaviour::shownLevel(int selected, int finest, int count) {
    if (count <= 0) return -1;
    return std::max(selected, std::min(finest, count - 1));
}

bool LODGroupBehaviour::levelLoaded(int level) const {
    return level >= 0 &&
           level < int(roots_.size()) && ready_[size_t(level)] && loaded_[size_t(level)];
}

bool LODGroupBehaviour::levelAvailable(int level) const {
    return level >= std::min(finest_, int(roots_.size()) - 1) && levelLoaded(level);
}

int LODGroupBehaviour::availableLevel() const {
    if (levelAvailable(desired_)) return desired_;
    // Keep the current representation while the requested upload is pending.
    if (levelAvailable(active_)) return active_;
    for (int level = desired_ + 1; level < int(roots_.size()); ++level)
        if (levelAvailable(level)) return level;
    for (int level = desired_ - 1; level >= 0; --level)
        if (levelAvailable(level)) return level;
    return -1;
}

void LODGroupBehaviour::onDisable() {
    // Resolve live paths, since a previously selected child may have been freed.
    if (node()) for (const auto& level : levels_)
        if (Node* child = node()->findByPath(level.path)) {
            child->setVisible(true);
            applyFade(child, 1.f);
        }
    resolvedRevision_ = 0;
    fading_ = -1;
    progress_ = 1.f;
}

void LODGroupBehaviour::resolve() {
    roots_.clear();
    for (const auto& level : levels_) {
        Node* root = node()->findByPath(level.path);
        if (!root || root->parent() != node() ||
            std::find(roots_.begin(), roots_.end(), root) != roots_.end())
            throw std::runtime_error("LOD level must name a distinct direct child: " + level.path);
        roots_.push_back(root);
    }
    loaded_.clear();
    pendingMeshes_ = false;
    for (const Node* root : roots_) {
        loaded_.push_back(meshesLoaded(*root));
        pendingMeshes_ |= !loaded_.back();
    }
    if (explicitBounds_) {
        resolvedRevision_ = node()->subtreeRevision();
        resolvedTransformRevision_ = node()->subtreeTransformRevision();
        return;
    }
    bounds_.min = glm::vec3(std::numeric_limits<float>::max());
    bounds_.max = -bounds_.min;
    bool found = false, pending = false;
    // LOD0's bounds define coverage for the whole group, irrespective of the
    // number of primitives or pivots of the lower-detail representations.
    roots_.front()->traverse([&](Node& n, const glm::mat4& local) {
        if (!n.mesh()) return;
        if (!n.mesh()->loaded()) { pending = true; return; }
        const auto& box = n.mesh()->bounds();
        for (unsigned corner = 0; corner < 8; ++corner) {
            glm::vec3 p{corner & 1 ? box.max.x : box.min.x,
                        corner & 2 ? box.max.y : box.min.y,
                        corner & 4 ? box.max.z : box.min.z};
            p = glm::vec3(local * glm::vec4(p, 1.f));
            bounds_.min = glm::min(bounds_.min, p);
            bounds_.max = glm::max(bounds_.max, p);
            found = true;
        }
    });
    if (!found || pending) { roots_.clear(); return; } // asynchronous meshes: retry after loading
    resolvedRevision_ = node()->subtreeRevision();
    resolvedTransformRevision_ = node()->subtreeTransformRevision();
}

void LODGroupBehaviour::updateForView(const glm::mat4& view, const glm::mat4& projection, uint64_t frame) {
    if (levels_.empty() || !node() || !node()->isVisibleInHierarchy()) return;
    // Seen without a break since the last frame: only then is a change of
    // level something the eye followed, and worth dissolving.
    const bool continuous = frame != 0 && lastFrame_ != 0 && frame == lastFrame_ + 1;
    lastFrame_ = frame;
    if (pendingMeshes_ || resolvedRevision_ != node()->subtreeRevision() ||
        resolvedTransformRevision_ != node()->subtreeTransformRevision()) resolve();
    if (roots_.empty()) return;
    const float coverage = computeScreenCoverage(node()->worldTransform(), bounds_, view, projection);
    // Hysteresis follows what coverage chose, not what the budget allowed, so
    // a group released by its caller returns at once to its own level.
    selected_ = selectLodIndex(coverage, thresholds_, selected_, .10f);
    desired_ = shownLevel(selected_, finest_, int(roots_.size()));
    const int was = active_;
    active_ = availableLevel();
    if (fading_ >= int(roots_.size())) fading_ = -1;
    if (fading_ >= 0 && (!continuous || !levelLoaded(fading_) || active_ < 0)) {
        for (Node* root : roots_) applyFade(root, 1.f);
        fading_ = -1;
        progress_ = 1.f;
    }
    if (was != active_) {
        // A stricter detail budget changes the incoming level, but the loaded
        // previous level remains valid for the short outgoing fade.
        if (crossFade_ > 0.f && continuous && levelLoaded(was) && active_ >= 0) {
            if (fading_ == active_) {
                // Turning back mid-fade: the level returning already covers the
                // share it had not yet given up.
                progress_ = 1.f - progress_;
            } else {
                // A third level: what was shown hands over from the start.
                if (fading_ >= 0) applyFade(roots_[size_t(fading_)], 1.f);
                progress_ = 0.f;
            }
            fading_ = was;
        } else {
            for (Node* root : roots_) applyFade(root, 1.f);
            fading_ = -1;
            progress_ = 1.f;
        }
    } else if (crossFade_ <= 0.f && fading_ >= 0) {
        progress_ = 1.f;
    }
    if (fading_ >= 0) {
        // Real seconds: a paused game still finishes the fade it shows.
        progress_ += Time::unscaledDelta() / std::max(crossFade_, 1e-3f);
        if (progress_ >= 1.f) {
            applyFade(roots_[size_t(fading_)], 1.f);
            applyFade(roots_[size_t(active_)], 1.f);
            fading_ = -1;
            progress_ = 1.f;
        } else {
            applyFade(roots_[size_t(active_)], incomingFade(progress_));
            applyFade(roots_[size_t(fading_)], outgoingFade(progress_));
        }
    }
    for (size_t i = 0; i < roots_.size(); ++i)
        roots_[i]->setVisible(int(i) == active_ || int(i) == fading_);
    resolvedRevision_ = node()->subtreeRevision();
    resolvedTransformRevision_ = node()->subtreeTransformRevision();
}

void LODGroupBehaviour::save(nlohmann::json& out) const {
    if (levels_.empty()) return;
    out["levels"] = nlohmann::json::array();
    for (const auto& level : levels_) out["levels"].push_back({{"path", level.path}, {"minCoverage", level.minCoverage}});
    if (crossFade_ > 0.f) out["crossFade"] = crossFade_;
}

void LODGroupBehaviour::load(const nlohmann::json& in) {
    std::vector<Level> levels;
    if (in.contains("levels"))
        for (const auto& level : in.at("levels"))
            levels.push_back({level.at("path").get<std::string>(), level.at("minCoverage").get<float>()});
    explicitBounds_ = false;
    setLevels(std::move(levels));
    setCrossFade(in.value("crossFade", 0.f));
}

void LODGroupBehaviour::describe(reflect::TypeBuilder<LODGroupBehaviour>& t) {
    t.doc("Selects one child representation by projected coverage with hysteresis. "
          "Child levels store path and minCoverage; crossFade (seconds) dissolves "
          "one level into the next through a screen door. An empty group exposes "
          "the MeshNode's own LOD chain in the editor.");
}

} // namespace saida
