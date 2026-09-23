#include "behaviours/LODGroupBehaviour.hpp"

#include "core/Reflection.hpp"
#include "scene/Node.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace saida {

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
    resolvedRevision_ = 0;
    active_ = -1;
}

void LODGroupBehaviour::onDisable() {
    // Resolve live paths, since a previously selected child may have been freed.
    if (node()) for (const auto& level : levels_)
        if (Node* child = node()->findByPath(level.path)) child->setVisible(true);
    resolvedRevision_ = 0;
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

void LODGroupBehaviour::updateForView(const glm::mat4& view, const glm::mat4& projection) {
    if (levels_.empty() || !node() || !node()->isVisibleInHierarchy()) return;
    if (resolvedRevision_ != node()->subtreeRevision() ||
        resolvedTransformRevision_ != node()->subtreeTransformRevision()) resolve();
    if (roots_.empty()) return;
    const float coverage = computeScreenCoverage(node()->worldTransform(), bounds_, view, projection);
    active_ = selectLodIndex(coverage, thresholds_, active_, .10f);
    for (size_t i = 0; i < roots_.size(); ++i) roots_[i]->setVisible(int(i) == active_);
    resolvedRevision_ = node()->subtreeRevision();
    resolvedTransformRevision_ = node()->subtreeTransformRevision();
}

void LODGroupBehaviour::save(nlohmann::json& out) const {
    if (levels_.empty()) return;
    out["levels"] = nlohmann::json::array();
    for (const auto& level : levels_) out["levels"].push_back({{"path", level.path}, {"minCoverage", level.minCoverage}});
}

void LODGroupBehaviour::load(const nlohmann::json& in) {
    std::vector<Level> levels;
    if (in.contains("levels"))
        for (const auto& level : in.at("levels"))
            levels.push_back({level.at("path").get<std::string>(), level.at("minCoverage").get<float>()});
    setLevels(std::move(levels));
}

void LODGroupBehaviour::describe(reflect::TypeBuilder<LODGroupBehaviour>& t) {
    t.doc("Selects one child representation by projected coverage with hysteresis. "
          "Child levels store path and minCoverage. An empty group exposes the "
          "MeshNode's own LOD chain in the editor.");
}

} // namespace saida
